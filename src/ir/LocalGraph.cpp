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

#include <cfg/cfg-traversal.h>
#include <ir/find_all.h>
#include <ir/local-graph.h>
#include <wasm-builder.h>

namespace wasm {

// UseDefAnalysis implementation

template<typename Use, typename Def>
UseDefAnalysis::UseDefAnalysis(Function* func, AnalysisParams params) {
  // Information about a basic block.
  struct Info {
    // Actions occurring in this block: uses and defs.
    std::vector<Expression*> actions;
    // For each lane, the last def for it.
    std::unordered_map<Index, Def*> lastDefs;
  };

  // Flow helper class: flows the gets to their sets.

  struct Flower : public CFGWalker<Flower, Visitor<Flower>, Info> {
    UseDefAnalysis<Use, Def>::UseDefs& useDefs;
    UseDefAnalysis<Use, Def>::Locations& locations;

    Flower(UseDefAnalysis<Use, Def>::UseDefs& useDefs,
           UseDefAnalysis<Use, Def>::Locations& locations,
           Function* func)
      : useDefs(useDefs), locations(locations) {
      setFunction(func);
      // create the CFG by walking the IR
      CFGWalker<Flower, Visitor<Flower>, Info>::doWalkFunction(func);
      // flow gets across blocks
      flow(func);
    }

    BasicBlock* makeBasicBlock() { return new BasicBlock(); }

    // cfg traversal work

    static void doVisitLocalGet(Flower* self, Expression** currp) {
      auto* curr = (*currp)->cast<Use>();
      // if in unreachable code, skip
      if (!self->currBasicBlock) {
        return;
      }
      self->currBasicBlock->contents.actions.emplace_back(curr);
      self->locations[curr] = currp;
    }

    static void doVisitLocalSet(Flower* self, Expression** currp) {
      auto* curr = (*currp)->cast<Def>();
      // if in unreachable code, skip
      if (!self->currBasicBlock) {
        return;
      }
      self->currBasicBlock->contents.actions.emplace_back(curr);
      self->currBasicBlock->contents.lastDefs[curr->index] = curr;
      self->locations[curr] = currp;
    }

    void flow(Function* func) {
      // This block struct is optimized for this flow process (Minimal
      // information, iteration index).
      struct FlowBlock {
        // Last Traversed Iteration: This value helps us to find if this block
        // has been seen while traversing blocks. We compare this value to the
        // current iteration index in order to determine if we already process
        // this block in the current iteration. This speeds up the processing
        // compared to unordered_set or other struct usage. (No need to reset
        // internal values, lookup into container, ...)
        size_t lastTraversedIteration;
        std::vector<Expression*> actions;
        std::vector<FlowBlock*> in;
        // Sor each index, the last local.set for it
        // The unordered_map from BasicBlock.Info is converted into a vector
        // This speeds up search as there are usually few sets in a block, so
        // just scanning them linearly is efficient, avoiding hash computations
        // (while in Info, it's convenient to have a map so we can assign them
        // easily, where the last one seen overwrites the previous; and, we do
        // that O(1)).
        std::vector<std::pair<Index, Def*>> lastDefs;
      };

      auto numLocals = func->getNumLocals();
      std::vector<std::vector<Use*>> allGets;
      allGets.resize(numLocals);
      std::vector<FlowBlock*> work;

      // Convert input blocks (basicBlocks) into more efficient flow blocks to
      // improve memory access.
      std::vector<FlowBlock> flowBlocks;
      flowBlocks.resize(basicBlocks.size());

      // Init mapping between basicblocks and flowBlocks
      std::unordered_map<BasicBlock*, FlowBlock*> basicToFlowMap;
      for (Index i = 0; i < basicBlocks.size(); ++i) {
        basicToFlowMap[basicBlocks[i].get()] = &flowBlocks[i];
      }

      const size_t NULL_ITERATION = -1;

      FlowBlock* entryFlowBlock = nullptr;
      for (Index i = 0; i < flowBlocks.size(); ++i) {
        auto& block = basicBlocks[i];
        auto& flowBlock = flowBlocks[i];
        // Get the equivalent block to entry in the flow list
        if (block.get() == entry) {
          entryFlowBlock = &flowBlock;
        }
        flowBlock.lastTraversedIteration = NULL_ITERATION;
        flowBlock.actions.swap(block->contents.actions);
        // Map in block to flow blocks
        auto& in = block->in;
        flowBlock.in.resize(in.size());
        std::transform(
          in.begin(), in.end(), flowBlock.in.begin(), [&](BasicBlock* block) {
            return basicToFlowMap[block];
          });
        // Convert unordered_map to vector.
        flowBlock.lastDefs.reserve(block->contents.lastDefs.size());
        for (auto set : block->contents.lastDefs) {
          flowBlock.lastDefs.emplace_back(
            std::make_pair(set.first, set.second));
        }
      }
      assert(entryFlowBlock != nullptr);

      size_t currentIteration = 0;
      for (auto& block : flowBlocks) {
#ifdef LOCAL_GRAPH_DEBUG
        std::cout << "basic block " << block.get() << " :\n";
        for (auto& action : block->contents.actions) {
          std::cout << "  action: " << *action << '\n';
        }
        for (auto* lastSet : block->contents.lastDefs) {
          std::cout << "  last set " << lastSet << '\n';
        }
#endif
        // go through the block, finding each get and adding it to its index,
        // and seeing how sets affect that
        auto& actions = block.actions;
        // move towards the front, handling things as we go
        for (int i = int(actions.size()) - 1; i >= 0; i--) {
          auto* action = actions[i];
          if (auto* get = action->dynCast<Use>()) {
            allGets[get->index].push_back(get);
          } else {
            // This set is the only set for all those gets.
            auto* set = action->cast<Def>();
            auto& gets = allGets[set->index];
            for (auto* get : gets) {
              useDefs[get].insert(set);
            }
            gets.clear();
          }
        }
        // If anything is left, we must flow it back through other blocks. we
        // can do that for all gets as a whole, they will get the same results.
        for (Index index = 0; index < numLocals; index++) {
          auto& gets = allGets[index];
          if (gets.empty()) {
            continue;
          }
          work.push_back(&block);
          // Note that we may need to revisit the later parts of this initial
          // block, if we are in a loop, so don't mark it as seen.
          while (!work.empty()) {
            auto* curr = work.back();
            work.pop_back();
            // We have gone through this block; now we must handle flowing to
            // the inputs.
            if (curr->in.empty()) {
              if (curr == entryFlowBlock) {
                // These receive a param or zero init value.
                for (auto* get : gets) {
                  useDefs[get].insert(nullptr);
                }
              }
            } else {
              for (auto* pred : curr->in) {
                if (pred->lastTraversedIteration == currentIteration) {
                  // We've already seen pred in this iteration.
                  continue;
                }
                pred->lastTraversedIteration = currentIteration;
                auto lastSet = std::find_if(pred->lastDefs.begin(),
                                            pred->lastDefs.end(),
                                            [&](std::pair<Index, Def*>& value) {
                                              return value.first == index;
                                            });
                if (lastSet != pred->lastDefs.end()) {
                  // There is a set here, apply it, and stop the flow.
                  for (auto* get : gets) {
                    useDefs[get].insert(lastSet->second);
                  }
                } else {
                  // Keep on flowing.
                  work.push_back(pred);
                }
              }
            }
          }
          gets.clear();
          currentIteration++;
        }
      }
    }
  };

  UseDefInternal::Flower flower(useDefs, locations, func);

#ifdef LOCAL_GRAPH_DEBUG
  std::cout << "UseDefAnalysis::dump\n";
  for (auto& pair : useDefs) {
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

void UseDefAnalysis::computeInfluences() {
  for (auto& pair : locations) {
    auto* curr = pair.first;
    if (auto* set = curr->dynCast<Def>()) {
      FindAll<Use> findAll(set->value);
      for (auto* get : findAll.list) {
        getInfluences[get].insert(set);
      }
    } else {
      auto* get = curr->cast<Use>();
      for (auto* set : useDefs[get]) {
        setInfluences[set].insert(get);
      }
    }
  }
}

// LocalGraph implementation

void LocalGraph::LocalGraph(Function* func)
  : UseDefAnalysis<LocalGet, LocalSet>(
      func,
      {// A use for us is a local.get.
       [](Expression* curr) { return curr->is<LocalGet>(); },
       // A definition for us is a local.set.
       [](Expression* curr) { return curr->is<LocalSet>(); },
       // A "lane" is the local index.
       [](Expression* curr) {
         if (auto* get = curr->dynCasts<LocalGet>()) {
           return get->index;
         } else if (auto* set = curr->dynCasts<LocalSet>()) {
           return set->index;
         }
         WASM_UNREACHABLE("bad use-def expr");
       }}) {}

void LocalGraph::computeSSAIndexes() {
  std::unordered_map<Index, std::set<LocalSet*>> indexSets;
  for (auto& pair : useDefs) {
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
