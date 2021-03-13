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
// DCE the types in the program, removing unneeded fields of structs which can
// then lead to removal of more types and so forth.
//
// For convenience at the cost of efficiency this operates on the text format.
// That format has all types in a single place and allows types to be modified,
// unlike Binaryen IR which is designed to assume types are immutable.
//
// This works in a simple brute-force manner, removing a field and then seeing
// if the wasm parses and validates. If we altered subtyping in a way that
// matters then that will fail, and we'll try something else. Note that we
// should not be changing runtime behavior, as RTT subtyping is a subset of
// static subtyping, so our preserving the latter ensures identical RTT
// behavior.
//

#include "ir/module-utils.h"
#include "ir/names.h"
#include "pass.h"
#include "support/colors.h"
#include "wasm-binary.h"
#include "wasm-s-parser.h"
#include "wasm.h"

using namespace std;

namespace wasm {

struct TypeDCE : public Pass {
  // The list of type names in the initial wasm (after we ensure all types are
  // named).
  // TODO: use this
  std::set<Name> typeNames;
  
  void run(PassRunner* runner, Module* module) override {
    // Find all the types.
    std::vector<HeapType> types;
    std::unordered_map<HeapType, Index> typeIndices;
    ModuleUtils::collectHeapTypes(*module, types, typeIndices);

    // Ensure all types and fields are named, so that we can refer to them
    // easily in the text format.
    ensureNames(*module, types);

    // Note all the names.
    for (auto type : types) {
      typeNames.insert(module->typeNames.at(type).name);
    }

    // Keep DCEing while we can.
    while (dceAllTypes(*module)) {}
  }

  // A single pass on all the types in the module. Returns true if we managed
  // to remove anything - if so, another pass on them all may find even more.
  bool dceAllTypes(Module& wasm) {
std::cout << "dceAllTypes\n";

    bool dced = false;

    // Current state of what is the next type.field we will attempt to DCE.
    Name nextType;
    Index nextField;

    while (1) {
      // Each loop iteration starts by printing the module, which by itself is a
      // procedure that DCEs unneeded types, as only used types are printed.
      // This takes a lot of work, but ensures we do not wait to perform such a
      // DCE until later, which may be even slower.
      // TODO: this could avoid the work in the case the pruning fails, by
      //       undoing the pruning

      // Generate the text format and parse it.
      std::stringstream stream;
      Colors::setEnabled(false);
      stream << wasm;
      Colors::setEnabled(true);
std::cout << "iter1\n";
      SExpressionParser parser(const_cast<char*>(stream.str().c_str()));
std::cout << "iter2\n";
      Element& root = *(*parser.root)[0];

      // Prune the next field.
      if (!pruneNext(root, nextType, nextField)) {
        // Nothing to prune, so we are all done here.
        return dced;
      }

      Module pruned;
std::cout << "iter3 " << wasm.features << "\n";
      pruned.features = wasm.features;
std::cout << "  pruned s: " << root << '\n';
      try {
        SExpressionWasmBuilder builder(pruned, root, IRProfile::Normal);
      } catch (ParseException& p) {
std::cout << "parse error\n";
p.dump(std::cerr);
        // The module does not parse, continue to the next field.
        nextField++;
        continue;
      }

      if (!wasm::WasmValidator().validate(pruned, WasmValidator::Quiet)) {
        // The module does not validate, continue to the next field.
std::cout << "novalidate\n";
        nextField++;
        continue;
      }

      // Success! Swap to the pruned module.
std::cout << "success! " << root << "\n";
      ModuleUtils::clearModule(wasm);
      ModuleUtils::copyModule(pruned, wasm);
      dced = true;

      // Do not increment nextField - we just pruned at the current index, so
      // there will be a new field in that position.
    }
  }

  void ensureNames(Module& wasm, std::vector<HeapType>& types) {
    // Type names.
    std::unordered_set<Name> used;
    for (auto type : types) {
      auto name = wasm.typeNames[type].name;
      if (name.is()) {
        used.insert(name);
      }
    }
    for (auto type : types) {
      auto& name = wasm.typeNames[type].name;
      if (!name.is()) {
        name = Names::getValidName("type", [&](Name test) {
          return used.count(test) == 0;
        });
        used.insert(name);
      }
    }

    // Field names.
    for (auto type : types) {
      if (!type.isStruct()) {
        continue;
      }
      auto& fieldNames = wasm.typeNames[type].fieldNames;
      std::unordered_set<Name> used;
      for (auto& kv : fieldNames) {
        used.insert(kv.second);
      }
      auto& fields = type.getStruct().fields;
      for (Index i = 0; i < fields.size(); i++) {
        if (fieldNames.count(i) == 0) {
          fieldNames[i] = Names::getValidName("field", [&](Name test) {
            return used.count(test) == 0;
          });
          used.insert(fieldNames[i]);
        }
      }
    }
  }

  // Finds and prunes the next thing. Returns true if successful and false if
  // nothing could be found to prune. Updates nextType/nextField accordingly.
  bool pruneNext(Element& root, Name& nextType, Index& nextField) {
std::cout << "pruneNext " << nextType << " : " << nextField << '\n';
    // Look for nextType.nextField. It is possible the type does not exist, if
    // it was optimized out; it is also possible the next field is one past the
    // end; in both cases simply continue forward in order. On the very first
    // call here nextType is not even set, so look for the first type to begin
    // our process.
    Element* found = nullptr;
    Name foundName;
    for (Index i = 0; i < root.size(); i++) {
      auto& item = *root[i];
      // Look for (type ..)
      if (!item.isList() || !item.size() || *item[0] != TYPE) {
        continue;
      }
      Name name = item[1]->str();
      // If we know what to look for (this is not the very first iteration), and
      // this is too small, ignore it. We want the first item >= that the
      // target.
      if (nextType.is() && name < nextType) {
        continue;
      }
      if (nextType == name && nextField == Index(-1)) {
        // We were told to skip this struct and look for the next.
        continue;
      }
      // Look for the name we want, or the first after it.
      if (!nextType.is() || !found || name < foundName) {
        // "item" is a type declaration, something like this:
        //      (type $struct.A (struct (field ..) (field ..) ..))
        // Look for (struct ..), and not (func ..) etc. in the inner part.
        auto& inner = *item[2];
        if (inner[0]->str() == "struct") {
          found = &inner;
          foundName = name;
        }
      }
    }
    if (!found) {
      return false;
    }
std::cout << "  found! " << foundName << " : " << *found << "\n";
    if (!nextType.is() || nextType < foundName || nextField == Index(-1)) {
      // We did not find the exact type, but one after it, or this is the very
      // first iteration; so reset the field.
      nextField = 0;
    }
    // We can now focus on the name we found.
    nextType = foundName;
    // "found" is a structure, something like this:
    //      (struct (field ..) (field ..) ..)
    // Note how the fields start at index 1.
    auto& struct_ = *found;
    assert(struct_[0]->str() == "struct");
    auto& list = struct_.list();
    auto numFields = list.size() - 1;
    if (nextField >= numFields) {
      // We must proceed to the next struct. This can happen because we came to
      // the end of the current struct, or we are on a new struct and it happens
      // to have size 0. To find the next one's name we must scan the list once
      // more, which we do recursively to keep this code simple. We mark
      // "nextField" as -1 so that the loop can easily know to ignore the struct
      // with an exact name match, and look for one after.
      nextField = -1;
      return pruneNext(root, nextType, nextField);
    }
    // We have something to prune!
    list.erase(list.begin() + 1 + nextField);
    return true;
  }
};

Pass* createTypeDCEPass() { return new TypeDCE(); }

} // namespace wasm
