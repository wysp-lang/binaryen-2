/*
 * Copyright 2021 WebAssembly Community Group participants
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

#ifndef locations_h
#define locations_h

#include "cfg-traversal.h"
#include "wasm.h"

namespace wasm::cfg {

//
// Given a list of basic blocks, generates location info for each item: a map
// from items to their basic block and index in that block.
//
// This assumes that the BasicBlock type has .contents.list where the list is a
// vector of Expression*.
//
template<typename BasicBlock> struct BlockLocations {
  struct Location {
    // The index of the basic block this is in.
    Index blockIndex;

    // The index in the block.
    Index positionIndex;
  };

  std::unordered_map<Expression*, Location> locations;

  BlockLocations(const std::vector<std::unique_ptr<BasicBlock>>& blocks) {
    for (Index blockIndex = 0; blockIndex < blocks.size(); blockIndex++) {
      auto& list = blocks[blockIndex]->contents.list;
      for (Index positionIndex = 0; positionIndex < list.size(); positionIndex++) {
        locations.emplace(list[positionIndex], Location{blockIndex, positionIndex});
      }
    }
  }
};

} // namespace wasm::cfg

#endif // locations_h
