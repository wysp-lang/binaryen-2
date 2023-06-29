/*
 * Copyright 2023 WebAssembly Community Group participants
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
// Packs string constant imports. This is meant to help with the imported JS
// String API,
//
// https://github.com/WebAssembly/design/issues/1480
//
// In that approach string functions (like string concatenation) are imported
// functions from JavaScript that basically provide JS string abilities to wasm.
// String constants, however, are a little trickier to manage, which is what
// this pass is for. Specifically, a natural way to handle string constants is
// like this:
//
//  JS:
//    WebAssembly.compile(.. { // import object
//      "string.const": ["hello", "world", "!"]
//    });
//
//  Wasm:
//
//    (import "string.const" "0" (global $hello (ref extern)))
//    (import "string.const" "1" (global $world (ref extern)))
//    (import "string.const" "2" (global $! (ref extern)))
//
// Each string constant is imported in the wasm, but all arrive from a single
// JS "store" of strings. A challenge then happens if we want to *optimize* this
// module, and say that $world becomes unused. Then we want to get rid of it on
// the JS side as well. That is what this pass is for.
//
// What this pass does is input a JSON file that contains that array of strings
// (["hello", ..] from the example above). Based on which string constants are
// still in use, it will compact that array, removing unused ones and decreasing
// the indexes of others above them. The wasm module is modified to refer to the
// new indexes, and we emit the new array of strings for JS to use.
//
// This pass does *not* optimize the module in any way. You should optimize the
// module before running it, so that unused imports are removed.
//
// Usage:
//
//   --pack-strings=module,infile,outfile
//
// "module" is "string.const" in the above example, that is, it is the module
// name we import from (optimized builds might use something like "a" as opposed
// to the more verbose "string.const" - that appears once per string constant,
// so it can affect binary size noticeably). "infile" is the input JSON file,
// and "outfile" is the output JSON file.
//

#include <string>

#include "pass.h"
#include "support/file.h"
#include "support/json.h"
#include "support/string.h"
#include "wasm.h"

namespace wasm {

struct PackStrings : public Pass {
  void run(Module* module) override {
    auto rawArgs = getPassOptions().getArgumentOrDefault("pack-strings", "");
    String::Split args(rawArgs, ",");
    if (args.size() != 3) {
      Fatal() << "Usage: --pack-strings=module,infile,outfile";
    }

    Name moduleName = args[0];
    auto inFile = args[1];
    auto outFile = args[2];

    // Process the imports, finding which indexes are used, and using packed
    // (consecutive) indexes in the new mapping.
    std::set<Index> usedOldIndexes;

    for (auto& import : module->globals) {
      if (import->module != moduleName) {
        continue;
      }

      auto oldIndex = std::stoi(import->base.toString());
      usedOldIndexes.insert(oldIndex);

      auto newIndex = usedOldIndexes.size();
      import->base = std::to_string(newIndex);
    }

    // Read the input and parse it.
    auto jsonText = read_file<std::vector<char>>(inFile, Flags::Text);
    json::Value json;
    json.parse(jsonText.data());
    if (!json.isArray()) {
      Fatal() << "Input file does not contain valid JSON";
    }

    // Write the packed output.
    Output output(outFile, Flags::Text);
    auto& o = output.getStream();
    o << "[";
    bool first = true;
    for (auto index : usedOldIndexes) {
      if (first) {
        first = false;
      } else {
        o << ", ";
      }
      if (index >= json.size()) {
        Fatal() << "Input JSON is not large enough";
      }
      if (!json[index]->isString()) {
        Fatal() << "Input JSON does not contain only strings";
      }
      o << json[index]->getCString();
    }
    o << "]";
  }
};

Pass* createPackStringsPass() { return new PackStrings(); }

} // namespace wasm
