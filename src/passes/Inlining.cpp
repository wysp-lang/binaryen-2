/*
 * Copyright 2016 WebAssembly Community Group participants
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
// Inlining.
//
// Two versions are provided: inlining and inlining-optimizing. You
// probably want the optimizing version, which will optimize locations
// we inlined into, as inlining by itself creates a block to house the
// inlined code, some temp locals, etc., which can usually be removed
// by optimizations. Note that the two versions use the same heuristics,
// so we don't take into account the overhead if you don't optimize
// afterwards. The non-optimizing version is mainly useful for debugging,
// or if you intend to run a full set of optimizations anyhow on
// everything later.
//

#include <atomic>

#include "ir/cost.h"
#include "ir/debug.h"
#include "ir/find_all.h"
#include "ir/literal-utils.h"
#include "ir/module-utils.h"
#include "ir/utils.h"
#include "parsing.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

namespace {

// Useful into on a function, helping us decide if we can inline it
struct FunctionInfo {
  std::atomic<Index> refs;
  Index size;
  bool hasCalls;
  bool hasLoops;
  bool usedGlobally; // in a table or export

  FunctionInfo() {
    refs = 0;
    size = 0;
    hasCalls = false;
    hasLoops = false;
    usedGlobally = false;
  }

  // Check if we should inline a function.
  bool worthInlining(const PassOptions& options) const {
    // See pass.h for how defaults for these options were chosen.

    // If it's small enough that we always want to inline such things, do so.
    if (size <= options.inlining.alwaysInlineMaxSize) {
      return true;
    }
    // If it has one use, then inlining it would likely reduce code size, at
    // least for reasonable function sizes.
    if (refs == 1 && !usedGlobally &&
        size <= options.inlining.oneCallerInlineMaxSize) {
      return true;
    }
    // If it's so big that we have no flexible options that could allow it,
    // do not inline.
    if (size > options.inlining.flexibleInlineMaxSize) {
      return false;
    }
    // More than one use, so we can't eliminate it after inlining,
    // so only worth it if we really care about speed and don't care
    // about size. First, check if it has calls. In that case it is not
    // likely to speed us up, and also if we want to inline such
    // functions we would need to be careful to avoid infinite recursion.
    if (hasCalls) {
      return false;
    }
    return options.optimizeLevel >= 3 && options.shrinkLevel == 0 &&
           (!hasLoops || options.inlining.allowFunctionsWithLoops);
  }

  // Check if we should speculatively inline a function.
  // Note that it only makes sense to speculatively optimize if we are
  // optimizing: if we are not willing to run the optimizer after each
  // inlining, that exactly precludes speculation.
  bool speculativelyWorthInlining(const PassOptions& options,
                                  bool optimize) const {
    PassOptions speculativeOptions = options;
    // To speculate, we must optimize, and we must be optimizing heavily for
    // speed or size or both.
    if (optimize && (options.optimizeLevel >= 3 || options.shrinkLevel)) {
      auto speculate = [&](Index& value) {
        value = (value * options.inlining.speculativePercent) / 100;
      };
      speculate(speculativeOptions.inlining.alwaysInlineMaxSize);
      speculate(speculativeOptions.inlining.oneCallerInlineMaxSize);
      speculate(speculativeOptions.inlining.flexibleInlineMaxSize);
    }
    return worthInlining(speculativeOptions);
  }
};

typedef std::unordered_map<Name, FunctionInfo> NameInfoMap;

struct FunctionInfoScanner
  : public WalkerPass<PostWalker<FunctionInfoScanner>> {
  bool isFunctionParallel() override { return true; }

  FunctionInfoScanner(NameInfoMap* infos) : infos(infos) {}

  FunctionInfoScanner* create() override {
    return new FunctionInfoScanner(infos);
  }

  void visitLoop(Loop* curr) {
    // having a loop
    (*infos)[getFunction()->name].hasLoops = true;
  }

  void visitCall(Call* curr) {
    // can't add a new element in parallel
    assert(infos->count(curr->target) > 0);
    (*infos)[curr->target].refs++;
    // having a call
    (*infos)[getFunction()->name].hasCalls = true;
  }

  void visitRefFunc(RefFunc* curr) {
    assert(infos->count(curr->func) > 0);
    (*infos)[curr->func].refs++;
  }

  void visitFunction(Function* curr) {
    (*infos)[curr->name].size = Measurer::measure(curr->body);
  }

private:
  NameInfoMap* infos;
};

struct InliningAction {
  // The target function to inline into.
  Function* target;
  // The call in the target function that we will inline onto.
  Expression** callSite;
  // The source contents to be inlined, that is, the source function.
  Function* source;
};

struct InliningState {
  // The set of all functions that may be worth inlining. This includes ones
  // that we are sure about, and ones that we will consider speculatively.
  std::unordered_set<Name> maybeWorthInlining;
  // function name => actions that can be performed in it
  std::unordered_map<Name, std::vector<InliningAction>> actionsForFunction;
};

struct Planner : public WalkerPass<PostWalker<Planner>> {
  bool isFunctionParallel() override { return true; }

  Planner(InliningState* state) : state(state) {}

  Planner* create() override { return new Planner(state); }

  void visitCall(Call* curr) {
    // plan to inline if we know this is valid to inline, and if the call is
    // actually performed - if it is dead code, it's pointless to inline.
    // we also cannot inline ourselves.
    bool isUnreachable;
    if (curr->isReturn) {
      // Tail calls are only actually unreachable if an argument is
      isUnreachable = std::any_of(
        curr->operands.begin(), curr->operands.end(), [](Expression* op) {
          return op->type == Type::unreachable;
        });
    } else {
      isUnreachable = curr->type == Type::unreachable;
    }
    if (state->maybeWorthInlining.count(curr->target) && !isUnreachable &&
        curr->target != getFunction()->name) {
      // nest the call in a block. that way the location of the pointer to the
      // call will not change even if we inline multiple times into the same
      // function, otherwise call1(call2()) might be a problem
      auto* block = Builder(*getModule()).makeBlock(curr);
      replaceCurrent(block);
      // can't add a new element in parallel
      assert(state->actionsForFunction.count(getFunction()->name) > 0);
      state->actionsForFunction[getFunction()->name].emplace_back(
        getFunction(), &block->list[0], getModule()->getFunction(curr->target));
    }
  }

private:
  InliningState* state;
};

struct Updater : public PostWalker<Updater> {
  Module* module;
  std::map<Index, Index> localMapping;
  Name returnName;
  Builder* builder;
  void visitReturn(Return* curr) {
    replaceCurrent(builder->makeBreak(returnName, curr->value));
  }
  // Return calls in inlined functions should only break out of the scope of
  // the inlined code, not the entire function they are being inlined into. To
  // achieve this, make the call a non-return call and add a break. This does
  // not cause unbounded stack growth because inlining and return calling both
  // avoid creating a new stack frame.
  template<typename T> void handleReturnCall(T* curr, Type targetType) {
    curr->isReturn = false;
    curr->type = targetType;
    if (targetType.isConcrete()) {
      replaceCurrent(builder->makeBreak(returnName, curr));
    } else {
      replaceCurrent(builder->blockify(curr, builder->makeBreak(returnName)));
    }
  }
  void visitCall(Call* curr) {
    if (curr->isReturn) {
      handleReturnCall(curr, module->getFunction(curr->target)->sig.results);
    }
  }
  void visitCallIndirect(CallIndirect* curr) {
    if (curr->isReturn) {
      handleReturnCall(curr, curr->sig.results);
    }
  }
  void visitLocalGet(LocalGet* curr) {
    curr->index = localMapping[curr->index];
  }
  void visitLocalSet(LocalSet* curr) {
    curr->index = localMapping[curr->index];
  }
};

// Core inlining logic. Modifies the outside function (adding locals as
// needed), and returns the inlined code.
static void doInlining(Module* module, const InliningAction& action) {
  Function* target = action.target;
  Function* source = action.source;
#ifdef INLINING_DEBUG
  std::cout << "inline " << source->name << " into " << target->name << '\n';
#endif
  auto* call = (*action.callSite)->cast<Call>();
  // Works for return_call, too
  Type retType = source->sig.results;
  Builder builder(*module);
  auto* block = builder.makeBlock();
  block->name = Name(std::string("__inlined_func$") + source->name.str);
  if (call->isReturn) {
    if (retType.isConcrete()) {
      *action.callSite = builder.makeReturn(block);
    } else {
      *action.callSite = builder.makeSequence(block, builder.makeReturn());
    }
  } else {
    *action.callSite = block;
  }
  // Prepare to update the inlined code's locals and other things.
  Updater updater;
  updater.module = module;
  updater.returnName = block->name;
  updater.builder = &builder;
  // Set up a locals mapping
  for (Index i = 0; i < source->getNumLocals(); i++) {
    updater.localMapping[i] = builder.addVar(target, source->getLocalType(i));
  }
  // Assign the operands into the params
  for (Index i = 0; i < source->sig.params.size(); i++) {
    block->list.push_back(
      builder.makeLocalSet(updater.localMapping[i], call->operands[i]));
  }
  // Zero out the vars (as we may be in a loop, and may depend on their
  // zero-init value
  for (Index i = 0; i < source->vars.size(); i++) {
    block->list.push_back(
      builder.makeLocalSet(updater.localMapping[source->getVarIndexBase() + i],
                           LiteralUtils::makeZero(source->vars[i], *module)));
  }
  // Generate and update the inlined contents
  auto* contents = ExpressionManipulator::copy(source->body, *module);
  if (!source->debugLocations.empty()) {
    debug::copyDebugInfo(source->body, contents, source, target);
  }
  updater.walk(contents);
  block->list.push_back(contents);
  block->type = retType;
  // If the function returned a value, we just set the block containing the
  // inlined code to have that type. or, if the function was void and
  // contained void, that is fine too. a bad case is a void function in which
  // we have unreachable code, so we would be replacing a void call with an
  // unreachable.
  if (contents->type == Type::unreachable && block->type == Type::none) {
    // Make the block reachable by adding a break to it
    block->list.push_back(builder.makeBreak(block->name));
  }
  // After inlining weo may have non-unique label names, fix those up.
  wasm::UniqueNameMapper::uniquify(target->body);
}

// Given a thing and its copy, find the corresponding call in the copy to a call
// in the original.
static Expression**
getCorrespondingCallInCopy(Call* call, Expression* original, Expression* copy) {
  // Traverse them both, and use the fact that the walk is a deterministic
  // order.
  // TODO: Add a way to not need to do these traversal, by noting the
  //       correspondence while copying.
  FindAllPointers<Call> originalCalls(original), copyCalls(copy);
  assert(originalCalls.list.size() == copyCalls.list.size());
  for (Index i = 0; i < originalCalls.list.size(); i++) {
    if (*originalCalls.list[i] == call) {
      return copyCalls.list[i];
    }
  }
  WASM_UNREACHABLE("copy is not a copy of original");
}

static void
doOptimize(Function* func, Module* module, const PassOptions& options) {
  PassRunner runner(module, options);
  runner.setIsNested(true);
  runner.setValidateGlobally(false); // not a full valid module
  // this is especially useful after inlining
  runner.add("precompute-propagate");
  runner.addDefaultFunctionOptimizationPasses(); // do all the usual stuff
  runner.runOnFunction(func);
}

// Returns whether we inlined.
static bool doSpeculativeInlining(Module* module,
                                  const InliningAction& action,
                                  const FunctionInfo& targetInfo,
                                  const FunctionInfo& sourceInfo,
                                  const PassOptions& options) {
  Function* target = action.target;
  Function* source = action.source;
#ifdef INLINING_DEBUG
  std::cout << "maybe inline " << source->name << " into " << target->name
            << '\n';
#endif
  abort(); // TODO
  // We were not certain, but since we were called, that means we can at least
  // speculatively inline it.
  assert(sourceInfo.speculativelyWorthInlining(options, true));

  // Create a temporary setup to inline into, and perform inlining and
  // optimization there.
  Module tempModule;
  Function* tempFunc = ModuleUtils::copyFunction(target, tempModule);
  InliningAction tempAction = action;
  auto* targetCall = (*action.callSite)->cast<Call>();
  tempAction.callSite =
    getCorrespondingCallInCopy(targetCall, target->body, tempFunc->body);
  assert(tempAction.callSite);
  doInlining(&tempModule, tempAction);
  doOptimize(target, module, options);

  // Check if the result is worthwhile. We look for a strict reduction in
  // the thing we are trying to minimize, which guarantees no cycles (like a
  // Lyapunov function https://en.wikipedia.org/wiki/Lyapunov_function).
  bool keepResults;
  if (options.shrinkLevel) {
    // Check for a decrease in code size.
    auto newSize = Measurer::measure(tempFunc->body);
    if (sourceInfo.refs == 1 && !sourceInfo.usedGlobally) {
      // The inlined function has no other references, so we will remove it
      // after the inlining. Compare to the previous total size of the inlined
      // function and the function we inlined into.
      // (Note that just by removing a function we are saving a few bytes at
      // least, so make the comparison <=.)
      keepResults = newSize <= targetInfo.size + sourceInfo.size;
    } else {
      // There are other references, so we need a strict decrease in size in
      // the function we inline to.
      keepResults = newSize < targetInfo.size;
    }
  } else if (options.optimizeLevel >= 3) {
    // Check for a decrease in computational cost.
    auto oldCost = CostAnalyzer(target->body).cost;
    auto newCost = CostAnalyzer(tempFunc->body).cost;
    if (!sourceInfo.hasCalls) {
      // The source function has no calls in it. That means that we can tell
      // exactly what is going on, without a call that might do more work (or
      // even recurse). The previous code that ran is the target function plus
      // the source function, and after inlining, we have the new target
      // function, so compare those.
      auto oldSourceCost = CostAnalyzer(source->body).cost;
      keepResults = newCost <= oldCost + oldSourceCost;
    } else {
      // The source function has a call. In this case we must be careful, and
      // look for a strict decrease in the target cost, as if the inlined code
      // has a call, we don't know how much work that does.
      keepResults = newCost < oldCost;
    }
  } else {
    WASM_UNREACHABLE("invalid options when speculatively optimizing");
  }
  if (!keepResults) {
    // This speculation has sadly not worked out.
    return false;
  }

  // This is worth keeping; copy it over!
  target->body = ExpressionManipulator::copy(tempFunc->body, *module);
  return true;
}

// Schedules inlinings for a list of possible ones. We need to schedule because
// we may not be able to do them all: after we inline A into B, it may not be
// worth inlining B into anything else, for example, so even if we planned to
// inline B, we must not do so, at least not in this iteration.
//
// Note that we need to do this scheduling in a deterministic manner, but we
// also want to run the actual inlinings and optimizations in parallel, as the
// optimization in particular can be costly.
struct Scheduler {
  bool optimize;

  std::vector<InliningAction> possibleActions;

  bool inlined = false;

  Scheduler(Module* module, const InliningState& state, bool optimize)
    : optimize(optimize) {
    // Accumulate all the possible actions.
    for (auto& pair : state.actionsForFunction) {
      for (auto& action : pair.second) {
        possibleActions.push_back(action);
      }
    }

    run();
  }

  virtual void run();
};

// A scheduler for inlinings we definitely want to perform, i.e., that require
// no speculation.
struct DefiniteScheduler : public Scheduler {
  void schedule() {
    // Scheduling is fairly simple here, as we definitely want to do each
    // action. Schedule a new action unless it interferes with another.
    // Note that it is ok to inline multiple times into the same target, but we
    // do want to avoid optimizing that target function more than once, so we
    // optimize once at the end for each. Which functions were inlined or
    // inlined into.

    // How many times we inlined a source. Using this count we can tell if we
    // inlined into all the calls to the function (which may leave it with no
    // more uses).
    std::unordered_map<Function*, Index> sourcesInlinedFrom;
    // The actions we'll run for each target function, each representing an
    // inlining into it.
    std::map<Function*, std::vector<InliningAction>> actionsForTarget;

    for (auto& action : possibleActions) {
      // If we'll inline the target into something else, then there is a "race"
      // here, and potentially this inlining is not necessary (for example, the
      // function may have no more uses), so leave it for later.
      if (actionsForTarget.count(action.target)) {
        continue;
      }
      // If we'll inline into the source, then it has been modified, and may
      // not be worth inlining as it can be larger, so leave it for later.
      if (actionsForTarget.count(action.source)) {
        continue;
      }
      // This is an action we can do!
      actionsForTarget[action.target].push_back(action);
      sourcesInlinedFrom[action.source->name]++;
    }

    if (actionsForTarget.empty()) {
      inlined = false;
      return;
    }

    // We found things to inline!
    inlined = true;

    ParallelFunctionAnalysis(
      *module,
      [&](Function* target, const std::vector<InliningAction>& actions) {
        for (auto& action : actions) {
          assert(action.target == target);
          doInlining(module, action);
        }
        if (optimize) {
          doOptimize(target, module, runner->options);
        }
      });
  }
};

// remove functions that we no longer need after inlining
module->removeFunctions([&](Function* func) {
  auto name = func->name;
  auto& info = infos[name];
  return sourcesInlinedFrom.count(name) &&
         sourcesInlinedFrom[name] == info.refs && !info.usedGlobally;
});
// return whether we did any work
return inlinedUses.size() > 0;
} // namespace

} // namespace wasm

struct Inlining : public Pass {
  // whether to optimize where we inline
  bool optimize = false;

  // the information for each function. recomputed in each iteraction
  NameInfoMap infos;

  Index iterationNumber;

  void run(PassRunner* runner, Module* module) override {
    Index numFunctions = module->functions.size();
    // keep going while we inline, to handle nesting. TODO: optimize
    iterationNumber = 0;
    // no point to do more iterations than the number of functions, as
    // it means we infinitely recursing (which should
    // be very rare in practice, but it is possible that a recursive call
    // can look like it is worth inlining)
    while (iterationNumber <= numFunctions) {
#ifdef INLINING_DEBUG
      std::cout << "inlining loop iter " << iterationNumber
                << " (numFunctions: " << numFunctions << ")\n";
#endif
      calculateInfos(module);
      if (!iteration(runner, module)) {
        return;
      }
      iterationNumber++;
    }
  }

  void calculateInfos(Module* module) {
    infos.clear();
    // fill in info, as we operate on it in parallel (each function to its own
    // entry)
    for (auto& func : module->functions) {
      infos[func->name];
    }
    PassRunner runner(module);
    FunctionInfoScanner(&infos).run(&runner, module);
    // fill in global uses
    for (auto& ex : module->exports) {
      if (ex->kind == ExternalKind::Function) {
        infos[ex->value].usedGlobally = true;
      }
    }
    for (auto& segment : module->table.segments) {
      for (auto name : segment.data) {
        infos[name].usedGlobally = true;
      }
    }
    for (auto& global : module->globals) {
      if (!global->imported()) {
        for (auto* ref : FindAll<RefFunc>(global->init).list) {
          infos[ref->func].usedGlobally = true;
        }
      }
    }
    if (module->start.is()) {
      infos[module->start].usedGlobally = true;
    }
  }

  bool iteration(PassRunner* runner, Module* module) {
    // Find functions potentially worth inlining.
    InliningState state;
    ModuleUtils::iterDefinedFunctions(*module, [&](Function* func) {
      if (infos[func->name].speculativelyWorthInlining(runner->options,
                                                       optimize)) {
        state.maybeWorthInlining.insert(func->name);
      }
    });
    if (state.maybeWorthInlining.size() == 0) {
      return false;
    }
    // Fill in actionsForFunction, as we operate on it in parallel (each
    // function to its own entry).
    for (auto& func : module->functions) {
      state.actionsForFunction[func->name];
    }
    // Find all possible inlinings.
    Planner(&state).run(runner, module);

    // Start with definitely-worth inlinings.
    DefiniteScheduler definiteScheduler(module, state, optimize);
    // Don't do definite and speculative inlinings in the same iteration, to
    // keep things simple.
    if (definiteScheduler.inlined) {
      return true;
    }
    // If we can, try speculative inlinings.
    if (!optimize) {
      // Speculation requires optimization.
      return false;
    }
    return false; // XXX TODO
    /*
    while (1) {
      // In each loop iteration, run all the actions we can, and accumulate
      // deferred actions to a later loop iteration. We need this looping
      // because with speculative inlining we can't tell if we inline until we
      // actually try.
      std::vector<InliningAction> laterActions;
      inlined = pickAndExecute(actions,
        // Pick
        [&](const InliningAction& action) {
          // TODO FIXME: don't try more than once, except in a new
    uber-iteration? return
    infos[action.source].speculativelyWorthInlining(runner->options, true);
        },
        // Defer
        [&](const InliningAction& action) {
          laterActions.push_back(action);
        },
        // Execute
        [](const InliningAction& action) {
          return doSpeculativeInlining(module, action);
        });
      if (inlined) {
        // TODO:
        return true;
      }
      actions.swap(laterActions);
    }*/
  }
};

Pass* createInliningPass() { return new Inlining(); }

Pass* createInliningOptimizingPass() {
  auto* ret = new Inlining();
  ret->optimize = true;
  return ret;
}

static const char* MAIN = "main";
static const char* ORIGINAL_MAIN = "__original_main";

// Inline __original_main into main, if they exist. This works around the odd
// thing that clang/llvm currently do, where __original_main contains the user's
// actual main (this is done as a workaround for main having two different
// possible signatures).
struct InlineMainPass : public Pass {
  void run(PassRunner* runner, Module* module) override {
    auto* main = module->getFunctionOrNull(MAIN);
    auto* originalMain = module->getFunctionOrNull(ORIGINAL_MAIN);
    if (!main || main->imported() || !originalMain ||
        originalMain->imported()) {
      return;
    }
    FindAllPointers<Call> calls(main->body);
    Expression** callSite = nullptr;
    for (auto* call : calls.list) {
      if ((*call)->cast<Call>()->target == ORIGINAL_MAIN) {
        if (callSite) {
          // More than one call site.
          return;
        }
        callSite = call;
      }
    }
    if (!callSite) {
      // No call at all.
      return;
    }
    doInlining(module, main, InliningAction(callSite, originalMain));
  }
};

Pass* createInlineMainPass() { return new InlineMainPass(); }

} // namespace wasm
