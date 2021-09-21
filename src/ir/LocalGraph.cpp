/*
 * Copyright 2017 WebAssembly Community Group participants
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

#include <iterator>

#include <support/unique_deferring_queue.h>
#include <cfg/cfg-traversal.h>
#include <ir/find_all.h>
#include <ir/local-graph.h>
#include <wasm-builder.h>

namespace wasm {

namespace LocalGraphInternal {

// Information about a basic block.
struct Info {
  // actions occurring in this block: local.gets and local.sets
  std::vector<Expression*> actions;
};

// flow helper class. flows the gets to their sets

struct Flower : public CFGWalker<Flower, Visitor<Flower>, Info> {
  LocalGraph::GetSetses& getSetses;
  LocalGraph::Locations& locations;

  Flower(LocalGraph::GetSetses& getSetses,
         LocalGraph::Locations& locations,
         Function* func)
    : getSetses(getSetses), locations(locations) {
    setFunction(func);
    // create the CFG by walking the IR
    CFGWalker<Flower, Visitor<Flower>, Info>::doWalkFunction(func);
    // flow gets across blocks
    flow(func);
  }

  BasicBlock* makeBasicBlock() { return new BasicBlock(); }

  // cfg traversal work

  static void doVisitLocalGet(Flower* self, Expression** currp) {
    // if in unreachable code, skip
    if (!self->currBasicBlock) {
      return;
    }

    auto* curr = (*currp)->cast<LocalGet>();
    self->currBasicBlock->contents.actions.emplace_back(curr);
    self->locations[curr] = currp;
  }

  static void doVisitLocalSet(Flower* self, Expression** currp) {
    // if in unreachable code, skip
    if (!self->currBasicBlock) {
      return;
    }

    auto* curr = (*currp)->cast<LocalSet>();
    self->currBasicBlock->contents.actions.emplace_back(curr);
    self->currBasicBlock->contents.lastSets[curr->index] = curr;
    self->locations[curr] = currp;
  }

  void flow(Function* func) {
    // The flow logic here is based on the fact that wasm is in structured form,
    // and that the CFG walker's blocks are in reverse postorder. Using that, we
    // can do basically the same type of flow as a fast SSA generation approach
    // would do, as follows:
    //
    //  * Flow a map if  index => vector of Sets to that index. This flows to
    //    the end of a block and then to the successors.
    //  * When we reach a Get, its sets are the vector of Sets for its index.
    //  * The only thing requiring special handling here are backedges, that is,
    //    loop tops. They are the only case in which we have not yet seen all
    //    the predecessors (otherwise, reverse postorder handles that for us).
    //    To handle loops, we create a "Phi" for each index at the loop top. The
    //    Phi looks like a Set, and is added to the normal flow. After we finish
    //    everything, we can finalize Phis and apply their actual values.

    auto numLocals = func->getNumLocals();

    // Map basic blocks to their indices.
    std::unordered_map<BasicBlock*, Index> blockIndices;
    for (Index i = 0; i < numBlocks; i++) {
      blockIndices[blocks[i].get()] = i;
    }

    // Create a Phi for each loop top and for each index. Placing them all in a
    // single vector makes it easy to check if something is in fact a Phi.
    std::vector<LocalSet> phis(loopTops.size() * numLocals);

    // Maps a phi index to the list of sets for it.
    std::vector<std::vector<LocalSet*>> phiSets;

    auto isPhi = [&](LocalSet* set) {
      return set >= &phis.front() && set < &phis.back();
    };

    auto getPhiIndex = [&](LocalSet* phi) {
      assert(isPhi(phi));
      return (phi - &phis[0]) / sizeof(LocalSet);
    };

    // Use a convenient data structure for querying if something is a loop top.
    std::unordered_set<BasicBlock*> loopTopSet(loopTops.begin(), loopTops.end());

    // The base index in |phis| where the phis for each loop resides.
    std::unordered_map<BasicBlock*, Index> loopTopPhiIndex;
    Index next phiIndex = 0;

    // Track the flowing sets as a map from local indexes to the sets for that
    // index. (We could also use a vector here, but the number of locals may be
    // very large - TODO experiment.)
    using FlowingSets = std::map<Index, std::vector<LocalSet*>>;

    // For each basic block, the flow at the end of it (which is what should
    // then flow to its successors).
    std::vector<FlowingSets> blockFlows(basicBlocks.size());

    for (Index blockIndex = 0; blockIndex < basicBlocks.size(); blockIndex++) {
      auto* block = basicBlocks[blockIndex].get();
      auto& blockFlow = blockFlows[blockIndex];
      bool loopTop = false;

      // The initial flow is the union of all the things that flow into this
      // block.
      for (auto* in : block->in) {
        auto inIndex = blockIndices[in];
        if (inIndex >= blockIndex) {
          // This is a backedge, which means this is a loop top.
          loopTop = true;

          // We have not yet traversed this predecessor, and so we can do
          // nothing for it now. Later down we will add phis for it, so that we
          // can fix things up later.
          continue;
        }

        // This predecessor has already been traversed. Add it's info to ours.
        auto& inFlow = blockFlows[inIndex];
        for (Index i = 0; i < numLocals; i++) {
          std::copy(inFlow[i].begin(), inFlow[i].end(),
                    std::back_inserter(blockFlow[i]));
        }
      }

      if (loopTop) {
        // This is a loop top. Add the phis for it.
        assert(phiIndex < phis.size());
        loopTopPhiIndex[block] = phiIndex;
        for (Index i = 0; i < numLocals; i++) {
          blockFlow[i].push_back(&phis[phiIndex + i]);
        }
        phiIndex += numLocals;
      }

      // Traverse through the block.
      auto& actions = block->contents.actions;
      for (auto* action : actions) {
        if (auto* get = action->dynCast<LocalGet>()) {
          // This get's sets are all the sets for its index.
          getSetses[get] = blockFlow[get->index];
        } else {
          // This set is now the only set for this index.
          auto* set = action->cast<LocalSet>();
          blockFlow[set->index] = {set};
        }
      }

      // If we have a successor that is before us, then that is a backedge to a
      // loop top, and we can update phi info.
      // TODO: test for a loop to itself, the <= and >= here and above
      for (auto* loopTop : block->out) {
        auto loopTopIndex = blockIndices[loopTop];
        if (loopTopIndex <= blockIndex) {
          // This is indeed a backedge, and loopTop is a loop top. Update the
          // phi info.
          auto phiIndex = loopTopPhiIndex[loopTop];
          for (Index i = 0; i < numLocals; i++) {
            std::copy(blockFlow[i].begin(), blockFlow[i].end(),
                      std::back_inserter(phiSets[phiIndex + i]));
          }
        }
      }
    }

    // To finish the flow, expand phis. A phi may contain a reference to another
    // phi, in which case, we can expand out that phi and so forth, until we are
    // left with only actual Sets and not phis.
    for (Index phiIndex = 0; phiIndex < phis.size(); phiIndex++) {
      auto& sets = phiSets[phiIndex];

      // Perform multiple iterations, while we still find placeholders. Note
      // that once we see a phi, we never need to expand it again recursively.
      UniqueNonrepeatingDeferredQueue<LocalSet*> removedPhis;
      while (1) {
        sets.erase(std::remove_if(sets.begin(), 
                                  sets.end(),
                                  [&](LocalSet* set) {
                                    if (isPhi(set)) {
                                      removedPhis.push(set);
                                      return true;
                                    }
                                    return false;
                                  }),
                   sets.end());
        if (removedPhis.empty()) {
          break;
        }
        while (!removedPhis.empty()) {
          auto* otherPhi = removedPhis.pop();
          auto& otherPhiSets = phiSets[getPhiIndex(otherPhi)];
          std::copy(otherPhiSets.begin(), otherPhiSets.end(),
                    std::back_inserter(sets));
        }
      }
    }

    // Now that phis are expanded, we can replace them in the getSetses.
    for (auto& kv : getSetes) {
      auto* sets : kv.second;

      UniqueDeferredQueue<LocalSet*> removedPhis;
      sets.erase(std::remove_if(sets.begin(), 
                                sets.end(),
                                [&](LocalSet* set) {
                                  if (isPhi(set)) {
                                    removedPhis.push(set);
                                    return true;
                                  }
                                  return false;
                                }),
                 sets.end());
      while (!removedPhis.empty()) {
        auto* phi = removedPhis.pop();
        auto& phiSets = phiSets[getPhiIndex(phi)];
        std::copy(phiSets.begin(), phiSets.end(),
                  std::back_inserter(sets));
      }
    }

    // TODO: "phi" is wrong. placeholder. or "backedge placeholder"

    // TODO: SmallVectors. Or, use "Sets" which is a set of sets, and is defined in our class?
    //       Or: dedup Flows at the end. Faster that way?
  }
};

} // namespace LocalGraphInternal

// LocalGraph implementation

LocalGraph::LocalGraph(Function* func) : func(func) {
  LocalGraphInternal::Flower flower(getSetses, locations, func);

#ifdef LOCAL_GRAPH_DEBUG
  std::cout << "LocalGraph::dump\n";
  for (auto& pair : getSetses) {
    auto* get = pair.first;
    auto& sets = pair.second;
    std::cout << "GET\n" << get << " is influenced by\n";
    for (auto* set : sets) {
      std::cout << set << '\n';
    }
  }
  std::cout << "total locations: " << locations.size() << '\n';
#endif
}

bool LocalGraph::equivalent(LocalGet* a, LocalGet* b) {
  auto& aSets = getSetses[a];
  auto& bSets = getSetses[b];
  // The simple case of one set dominating two gets easily proves that they must
  // have the same value. (Note that we can infer dominance from the fact that
  // there is a single set: if the set did not dominate one of the gets then
  // there would definitely be another set for that get, the zero initialization
  // at the function entry, if nothing else.)
  if (aSets.size() != 1 || bSets.size() != 1) {
    // TODO: use a LinearExecutionWalker to find trivially equal gets in basic
    //       blocks. that plus the above should handle 80% of cases.
    // TODO: handle chains, merges and other situations
    return false;
  }
  auto* aSet = *aSets.begin();
  auto* bSet = *bSets.begin();
  if (aSet != bSet) {
    return false;
  }
  if (!aSet) {
    // They are both nullptr, indicating the implicit value for a parameter
    // or the zero for a local.
    if (func->isParam(a->index)) {
      // For parameters to be equivalent they must have the exact same
      // index.
      return a->index == b->index;
    } else {
      // As locals, they are both of value zero, but must have the right
      // type as well.
      return func->getLocalType(a->index) == func->getLocalType(b->index);
    }
  } else {
    // They are both the same actual set.
    return true;
  }
}

void LocalGraph::computeSetInfluences() {
  for (auto& pair : locations) {
    auto* curr = pair.first;
    if (auto* get = curr->dynCast<LocalGet>()) {
      for (auto* set : getSetses[get]) {
        setInfluences[set].insert(get);
      }
    }
  }
}

void LocalGraph::computeGetInfluences() {
  for (auto& pair : locations) {
    auto* curr = pair.first;
    if (auto* set = curr->dynCast<LocalSet>()) {
      FindAll<LocalGet> findAll(set->value);
      for (auto* get : findAll.list) {
        getInfluences[get].insert(set);
      }
    }
  }
}

void LocalGraph::computeSSAIndexes() {
  std::unordered_map<Index, std::set<LocalSet*>> indexSets;
  for (auto& pair : getSetses) {
    auto* get = pair.first;
    auto& sets = pair.second;
    for (auto* set : sets) {
      indexSets[get->index].insert(set);
    }
  }
  for (auto& pair : locations) {
    auto* curr = pair.first;
    if (auto* set = curr->dynCast<LocalSet>()) {
      auto& sets = indexSets[set->index];
      if (sets.size() == 1 && *sets.begin() != curr) {
        // While it has just one set, it is not the right one (us),
        // so mark it invalid.
        sets.clear();
      }
    }
  }
  for (auto& pair : indexSets) {
    auto index = pair.first;
    auto& sets = pair.second;
    if (sets.size() == 1) {
      SSAIndexes.insert(index);
    }
  }
}

bool LocalGraph::isSSA(Index x) { return SSAIndexes.count(x); }

} // namespace wasm
