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

//
// Write the module to binary, and load it from there. This is useful in
// testing to check for the effects of roundtripping in a single wasm-opt
// parameter.
//

#include "ir/module-utils.h"
#include "pass.h"
#include "support/process.h"
#include "wasm-io.h"
#include "wasm.h"

namespace wasm {

struct LLVMOpt : public Pass {
  void run(PassRunner* runner, Module* module) override {
    std::string base = runner->options.getArgument(
      "llvm-opt",
      "LLVMOpt usage:  wasm-opt --llvm-opt=BASENAME\n"
      "  BASENAME will be used as the base name for the temporary files that\n"
      "  we create (BASENAME.1.wasm, BASENAME.2.c, BASENAME.3.wasm");
    // Save features, which would not otherwise make it through a round trip if
    // the target features section has been stripped.
    auto features = module->features;
    // Write the module to a temp file.
    std::string tempWasmA = base + ".1.wasm";
    ModuleWriter().writeBinary(*module, tempWasmA);
    // Compile the wasm to C.
    std::string tempC = base + ".2.c";
    if (ProgramResult("emcc " + tempWasmA + " -o " + tempC + " -s WASM2C").failed()) {
      Fatal() << "LLVMOpt: failed to convert to C";
    }
    // Compile the C to wasm.
    // TODO: optimize!
    std::string tempWasmB = base + ".3.c";
    if (ProgramResult("emcc " + tempC + " -o " + tempWasmB).failed()) {
      Fatal() << "LLVMOpt: failed to convert to wasm";
    }
    // Clear the module in preparation for reading, and read it.
    ModuleUtils::clearModule(*module);
    ModuleReader().readBinary(tempWasmB, *module);
    // Reapply features
    module->features = features;
  }
};

Pass* createLLVMOptPass() { return new LLVMOpt(); }

} // namespace wasm
