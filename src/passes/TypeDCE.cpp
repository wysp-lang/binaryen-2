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
// then lead to removal of more types and so forth. Note that just a simple
// round-trip operation (through either text or binary) will "DCE" types in that
// no unused type will be emit; what this pass does on top of that is to also
// remove fields that are not actually needed. Removing fields can allow much
// more DCEing as a type may only be used in an unused field.
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
// TODO: Signature params/results too and not just Struct fields.

#include "ir/find_all.h"
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
  // named). These names will remain valid even after we prune fields and the
  // types change.
  // In addition to holding the names, this maps them to their index in the
  // list of names.
  std::map<Name, Index> typeNameIndexes;

  // It is useful to know which fields are referenced in the code. These are
  // stored here as pairs <type name, field name>
  using ReferencedFields = std::unordered_set<std::pair<Name, Name>>;
  ReferencedFields referencedFields;

  Index totalIterations = 0;

  void run(PassRunner* runner, Module* module) override {
    // Find all the types.
    std::vector<HeapType> types;
    std::unordered_map<HeapType, Index> typeIndices;
    ModuleUtils::collectHeapTypes(*module, types, typeIndices);

    // Ensure all types and fields are named, so that we can refer to them
    // easily in the text format.
    ensureNames(*module, types);

    // Note all the fields that are referenced. Those definitely cannot be
    // pruned.

    ModuleUtils::ParallelFunctionAnalysis<ReferencedFields> analysis(
      *module, [&](Function* func, ReferencedFields& info) {
        if (func->imported()) {
          return;
        }
        auto note = [&](Expression* ref, Index index) {
          auto& typeNameInfo = module->typeNames.at(ref->type.getHeapType());
          auto typeName = typeNameInfo.name;
          auto fieldName = typeNameInfo.fieldNames.at(index);
          info.insert({typeName, fieldName});
        };
        // TODO: mark all struct.new fields?
        for (auto* get : FindAll<StructGet>(func->body).list) {
          note(get->ref, get->index);
        }
        for (auto* set : FindAll<StructSet>(func->body).list) {
          note(set->ref, set->index);
        }
      });

    for (auto& kv : analysis.map) {
      auto& currReferencedFields = kv.second;
      for (auto pair : currReferencedFields) {
        referencedFields.insert(pair);
      }
    }

    // Keep DCEing while we can.
    while (iteration(*module)) {
      totalIterations++;
    }
  }

  // A single pass on all the types in the module. Returns true if we managed
  // to remove anything - if so, another pass on them all may find even more.
  bool iteration(Module& wasm) {
    std::cout << "iteration\n";

    bool dced = false;

    // Current state of what is the next type.field we will attempt to DCE.
    Name nextType;
    Index nextField;

    while (1) {
      // Updating the names is not necessary, but improves logging. Updating
      // each iteration ensures we notice each type that is removed.
      updateNamesForLogging(wasm);

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
      std::cout << "iter " << typeNameIndexes[nextType] << " / "
                << typeNameIndexes.size() << " [" << totalIterations << "]\n";
      SExpressionParser parser(const_cast<char*>(stream.str().c_str()));
      // std::cout << "iter2\n";
      Element& root = *(*parser.root)[0];
      // std::cout << root << '\n';

      // Prune the next field.
      if (!pruneNext(root, nextType, nextField)) {
        // Nothing to prune, so we are all done here.
        return dced;
      }

      Module pruned;
      // std::cout << "iter3 " << wasm.features << "\n";
      pruned.features = wasm.features;
      // std::cout << "  pruned s: " << root << '\n';
      try {
        SExpressionWasmBuilder builder(pruned, root, IRProfile::Normal);
      } catch (ParseException& p) {
        // std::cout << "parse error\n";
        // The module does not parse, continue to the next field.
        nextField++;
        continue;
      }

      if (!wasm::WasmValidator().validate(
            pruned, WasmValidator::Globally | WasmValidator::Quiet)) {
        // The module does not validate, continue to the next field.
        // std::cout << "novalidate\n";
        nextField++;
        continue;
      }

      // Success! Swap to the pruned module.
      std::cout << "success! "
                << "\n";
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
        name = Names::getValidName(
          "type", [&](Name test) { return used.count(test) == 0; });
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
          fieldNames[i] = Names::getValidName(
            "field", [&](Name test) { return used.count(test) == 0; });
          used.insert(fieldNames[i]);
        }
      }
    }
  }

  // Update the global list of names.
  void updateNamesForLogging(Module& wasm) {
    std::set<Name> allNames;
    for (auto& kv : wasm.typeNames) {
      allNames.insert(kv.second.name);
    }
    typeNameIndexes.clear();
    Index i = 0;
    for (auto name : allNames) {
      typeNameIndexes[name] = i++;
    }
  }

  // Finds and prunes the next thing. Returns true if successful and false if
  // nothing could be found to prune. Updates nextType/nextField accordingly.
  bool pruneNext(Element& root, Name& nextType, Index& nextField) {
    // std::cout << "pruneNext " << nextType << " : " << nextField << '\n';
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
    // std::cout << "  found! " << foundName << " : " << *found << "\n";
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
    // Look for a relevant field. We must skip any fields that are known to be
    // referenced, as those are definitely impossible to remove.
    auto numFields = list.size() - 1;
    while (nextField < numFields) {
      auto& field = *list[1 + nextField];
      Name fieldName = field[1]->str();
      if (referencedFields.count({foundName, fieldName})) {
        // Skip this referenced field.
        nextField++;
        continue;
      }
      // We have something to prune!
      list.erase(list.begin() + 1 + nextField);
      // std::cout << "try to prune " << foundName << " : " << fieldName <<
      // '\n';
      return true;
    }
    // We must proceed to the next struct. This can happen because we came to
    // the end of the current struct, or we are on a new struct and it happens
    // to have size 0. To find the next one's name we must scan the list once
    // more, which we do recursively to keep this code simple. We mark
    // "nextField" as -1 so that the loop can easily know to ignore the struct
    // with an exact name match, and look for one after.
    // (This should never deeply recurse, but FIXME)
    nextField = -1;
    return pruneNext(root, nextType, nextField);
  }
};

Pass* createTypeDCEPass() { return new TypeDCE(); }

} // namespace wasm
