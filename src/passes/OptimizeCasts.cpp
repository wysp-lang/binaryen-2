/*
 * Copyright 2022 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Refine uses of locals where possible. For example, consider this:
//
//  (some.operation
//    (ref.cast .. (local.get $ref))
//    (local.get $ref)
//  )
//
// The second use might as well use the refined/cast value as well:
//
//  (some.operation
//    (local.tee $temp
//      (ref.cast .. (local.get $ref))
//    )
//    (local.get $temp)
//  )
//
// This change adds a local but it switches some local.gets to use a local of a
// more refined type. That can help other optimizations later.
//
// An example of an important pattern this handles are itable calls:
//
//  (call_ref
//    (ref.cast $actual.type
//      (local.get $object)
//    )
//    (struct.get $vtable ..
//      (ref.cast $vtable
//        (struct.get $itable ..
//          (local.get $object)
//        )
//      )
//    )
//  )
//
// We cast to the actual type for the |this| parameter, but we technically do
// not need to do so for reading its itable - since the itable may be of a
// generic type, and we cast the vtable afterwards anyhow. But since we cast
// |this|, we can use the cast value for the itable get, which may then lead to
// removing the vtable cast after we refine the itable type. And that can lead
// to devirtualization later.
//
// Closely related things appear in other passes:
//
//  * SimplifyLocals will find locals already containing a more refined type and
//    switch to them. RedundantSetElimination does the same across basic blocks.
//    In theory one of them could be extended to also add new locals, and then
//    they would be doing something similar to this pass.
//  * LocalCSE finds repeated expressions and stores them in locals for use
//    later. In theory that pass could be extended to look not for exact copies
//    but for equivalent things through a cast, and then it would be doing
//    something similar to this pass.
//
// However, while those other passes could be extended to cover what this pass
// does, we will have further cast-specific optimizations to add, which make
// sense in new pass anyhow, and things should be simpler overall to keep such
// casts all in one pass, here.
//
// TODO: Move casts earlier in a basic block as well, at least in traps-never-
//       happen mode where we can assume they never fail.
// TODO: Look past individual basic blocks?
// TODO: Look at LocalSet as well and not just Get. That would add some overlap
//       with the other passes mentioned above, but once we do things like
//       moving casts earlier as in the other TODO, we'd be doing uniquely
//       useful things with LocalSet here.
//

#include "ir/linear-execution.h"
#include "ir/properties.h"
#include "ir/utils.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

namespace {

// Find the best casted verisons of local.gets: other local.gets with the same
// value, but cast to a more refined type.
struct BestCastFinder : public LinearExecutionWalker<BestCastFinder> {

  PassOptions options;

  // Map local indices to all gets seen for that index. These are either literal
  // local.gets, or casts of local.gets, so the list can contain things like
  // (ref.cast (local.get ..)). That is, all the things in each vector here
  // contain the same local.get value, but perhaps casted.
  //
  // This is tracked in each basic block, and cleared between them.
  std::unordered_map<Index, std::vector<Expression*>> getsForIndex;

  // The findings: things we can optimize. Each is a sequence of local.gets, all
  // of whom can use the most-casted value, and that best cast which we want to
  // use for all of them. The key in the map is the first of the local.gets, who
  // we will apply the cast to, then all others will read from it.
  //
  // This is tracked until the end of the entire function, and contains the
  // information we need to optimize later. That is, entries here are things we
  // want to apply.
  struct Finding {
    std::vector<LocalGet*> gets;
    Expression* bestCast;
  };
  std::unordered_map<Expression*, Finding> findings;

  static void doNoteNonLinear(BestCastFinder* self, Expression** currp) {
    // This basic block is ending. Make decisions about all we've seen.
    self->decideAboutAllGets();
    self->getsForIndex.clear();
  }

  void visitLocalSet(LocalSet* curr) {
    // This index has a new value here, so make a decision up to here, and
    // clear the state.
    auto index = curr->index;
    decideAboutGets(index, getsForIndex[index]);
    getsForIndex.erase(index);
  }

  void visitLocalGet(LocalGet* curr) {
    getsForIndex[curr->index].push_back(curr);
  }

  void visitRefAs(RefAs* curr) { handleRefinement(curr); }

  void visitRefCast(RefCast* curr) { handleRefinement(curr); }

  void handleRefinement(Expression* curr) {
    auto* fallthrough = Properties::getFallthrough(curr, options, *getModule());
    if (auto* get = fallthrough->dynCast<LocalGet>()) {
      getsForIndex[get->index].push_back(curr);
    }
  }

  void visitFunction(Function* curr) {
    // The last basic block ended.
    decideAboutAllGets();
  }

  void decideAboutAllGets() {
    for (auto& [index, gets] : getsForIndex) {
      decideAboutGets(index, gets);
    }
  }

  void decideAboutGets(Index index, const std::vector<Expression*>& gets) {
    // Find the most-cast get in the list.
    Expression* bestCast = nullptr;
    for (auto* get : gets) {
      if (get->is<LocalGet>()) {
        // A plain local.get, without a cast, cannot be better than any other
        // local.get.
        continue;
      }

      if (!bestCast) {
        // This is the first.
        bestCast = curr;
        continue;
      }

      // See if we are better than the current best.
      if (get->type != bestCast->type &&
          Type::isSubType(get->type, bestCast->type)) {
        bestCast = get;
      }
    }

    // See if we can make an actual improvement: a get exists with a less-
    // refined type than the best. We can improve that one, if so.
    auto canImprove = false;
    Finding finding;
    for (auto* get : gets) {
      if (get->type != bestCast->type) {
        canImprove = true;
      }

      // Only push the actual LocalGets, not casts of them, to the finding's
      // vector. We'll make them all use the more-casted value (then the casts
      // of them will be optimized away, when possible, by other passes).
      // XXX if we store them all, we can optimize the case where the first get
      // is the cast laready.
      if (auto* localGet = get->dynCast<LocalGet>()) {
        finding.gets.push_back(localGet);
      }
    }
    if (canImprove) {
      finding.bestCast = bestCast;
      findings[gets[0]] = finding;
    }
  }
};

// Given a set of best casts, apply them: save each best cast in a local and use
// it in the places that want to.
//
// It is simpler to do this in another pass after BestCastFinder so that we do
// not need to worry about corner cases with invalidation of pointers in things
// we've already walked past.
struct FindingApplier : public PostWalker<FindingApplier> {
  BestCastFinder& finder;

  FindingApplier(BestCastFinder& finder) : finder(finder) {}

  void visitLocalGet(waka..
  void visitRefAs(RefAs* curr) { handleRefinement(curr); }

  void visitRefCast(RefCast* curr) { handleRefinement(curr); }

  void handleRefinement(Expression* curr) {
    auto iter = finder.findings.find(curr);
    if (iter == finder.findings.end()) {
      return;
    }

    auto& finding = iter->second;
    Builder builder(*getModule());

    // This expression was the first local.get in a sequence of them, for whom
    // we can make an improvement. Put the cast here, save the cast value to a
    // new local, and use it in all the others.
    auto* cast = ExpressionManipulator::copy(finding.bestCast, *getModule());
    ChildIterator(cast).getChild(0) = curr;
    auto var = Builder::addVar(getFunction(), curr->type);
    for (auto* get : finding.gets) {
      get->index = var;
      get->type = curr->type;
    }

    // Replace ourselves with a tee.
    replaceCurrent(builder.makeLocalTee(var, curr, curr->type));
  }
};

} // anonymous namespace

struct OptimizeCasts : public WalkerPass<PostWalker<OptimizeCasts>> {
  bool isFunctionParallel() override { return true; }

  std::unique_ptr<Pass> create() override {
    return std::make_unique<OptimizeCasts>();
  }

  void doWalkFunction(Function* func) {
    if (!getModule()->features.hasGC()) {
      return;
    }

    // First, find the best casts that we want to use.
    BestCastFinder finder;
    finder.options = getPassOptions();
    finder.walkFunctionInModule(func, getModule());

    if (finder.findings.empty()) {
      // Nothing to do.
      return;
    }

    // Apply the requests: use the best casts.
    FindingApplier applier(finder);
    applier.walkFunctionInModule(func, getModule());

    // LocalGet type changes must be propagated.
    ReFinalize().walkFunctionInModule(func, getModule());
  }
};

Pass* createOptimizeCastsPass() { return new OptimizeCasts(); }

} // namespace wasm
