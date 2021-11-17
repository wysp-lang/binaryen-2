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

#include <algorithm>
#include <iomanip>
#include <ir/module-utils.h>
#include <pass.h>
#include <support/colors.h>
#include <wasm-binary.h>
#include <wasm.h>

using namespace std;

namespace wasm {

// Each value we count is either a size_t size, or some other kind of metric
// that is stored in a double.
typedef map<const char*, std::variant<size_t, double>> Counts;

static Counts lastCounts;

static size_t getDepth(HeapType type) {
  if (auto super = type.getSuperType()) {
    return getDepth(*super) + 1;
  }
  return 0;
}

// Prints metrics between optimization passes.
struct Metrics
  : public WalkerPass<PostWalker<Metrics, UnifiedExpressionVisitor<Metrics>>> {
  bool modifiesBinaryenIR() override { return false; }

  bool byFunction;

  Counts counts;

  size_t refExpressionDepths = 0;
  size_t refExpressions = 0;

  Metrics(bool byFunction) : byFunction(byFunction) {}

  void visitExpression(Expression* curr) {
    auto name = getExpressionName(curr);
    std::get<size_t>(counts[name])++;
    if (curr->type.isRef()) {
      refExpressionDepths += getDepth(curr->type.getHeapType());
      refExpressions++;
    }
  }

  void doWalkModule(Module* module) {
    ImportInfo imports(*module);

    // global things
    for (auto& curr : module->exports) {
      visitExport(curr.get());
    }
    ModuleUtils::iterDefinedGlobals(*module,
                                    [&](Global* curr) { walkGlobal(curr); });
    walkMemory(&module->memory);

    // add imports / funcs / globals / exports / tables
    counts["[imports]"] = imports.getNumImports();
    counts["[funcs]"] = imports.getNumDefinedFunctions();
    counts["[globals]"] = imports.getNumDefinedGlobals();
    counts["[tags]"] = imports.getNumDefinedTags();
    counts["[exports]"] = module->exports.size();
    counts["[tables]"] = imports.getNumDefinedTables();
    // add memory and table
    if (module->memory.exists) {
      Index size = 0;
      for (auto& segment : module->memory.segments) {
        size += segment.data.size();
      }
      counts["[memory-data]"] = size;
    }

    if (module->features.hasGC()) {
      // Find the types and add some stats for them.
      std::vector<HeapType> types;
      std::unordered_map<HeapType, Index> typeIndices;
      ModuleUtils::collectHeapTypes(*module, types, typeIndices);
      counts["[types]"] = types.size();
    }

    Index size = 0;
    ModuleUtils::iterActiveElementSegments(
      *module, [&](ElementSegment* segment) { size += segment->data.size(); });
    for (auto& table : module->tables) {
      walkTable(table.get());
    }
    for (auto& segment : module->elementSegments) {
      walkElementSegment(segment.get());
    }
    if (!module->tables.empty()) {
      counts["[table-data]"] = size;
    }

    if (byFunction) {
      // print global
      printCounts("global");
      // compute binary info, so we know function sizes
      BufferWithRandomAccess buffer;
      WasmBinaryWriter writer(module, buffer);
      writer.write();
      // print for each function
      Index binaryIndex = 0;
      ModuleUtils::iterDefinedFunctions(*module, [&](Function* func) {
        counts.clear();
        walkFunction(func);
        counts["[vars]"] = func->getNumVars();
        counts["[binary-bytes]"] =
          writer.tableOfContents.functionBodies[binaryIndex++].size;
        printCounts(std::string("func: ") + func->name.str);
      });
      // print for each export how much code size is due to it, i.e.,
      // how much the module could shrink without it.
      auto sizeAfterGlobalCleanup = [](Module* module) {
        PassRunner runner(module,
                          PassOptions::getWithDefaultOptimizationOptions());
        runner.setIsNested(true);
        runner.addDefaultGlobalOptimizationPostPasses(); // remove stuff
        runner.run();
        BufferWithRandomAccess buffer;
        WasmBinaryWriter writer(module, buffer);
        writer.write();
        return buffer.size();
      };
      size_t baseline;
      {
        Module test;
        ModuleUtils::copyModule(*module, test);
        baseline = sizeAfterGlobalCleanup(&test);
      }
      for (auto& exp : module->exports) {
        // create a test module where we remove the export and then see how much
        // can be removed thanks to that
        Module test;
        ModuleUtils::copyModule(*module, test);
        test.removeExport(exp->name);
        counts.clear();
        counts["[removable-bytes-without-it]"] =
          baseline - sizeAfterGlobalCleanup(&test);
        printCounts(std::string("export: ") + exp->name.str + " (" +
                    exp->value.str + ')');
      }
      // check how much size depends on the start method
      if (!module->start.isNull()) {
        Module test;
        ModuleUtils::copyModule(*module, test);
        test.start = Name();
        counts.clear();
        counts["[removable-bytes-without-it]"] =
          baseline - sizeAfterGlobalCleanup(&test);
        printCounts(std::string("start: ") + module->start.str);
      }
      // can't compare detailed info between passes yet
      lastCounts.clear();
    } else {
      // add function info
      size_t vars = 0;
      ModuleUtils::iterDefinedFunctions(*module, [&](Function* func) {
        walkFunction(func);
        vars += func->getNumVars();
      });
      counts["[vars]"] = vars;
      counts["[type-depth]"] = double(refExpressionDepths) / double(refExpressions);
      // print
      printCounts("total");
      // compare to next time
      lastCounts = counts;
    }
  }

  void printCounts(std::string title) {
    ostream& o = cout;
    vector<const char*> keys;
    // add total
    size_t total = 0;
    for (auto i : counts) {
      keys.push_back(i.first);
      // total is of all the normal sizes, not the special [things] or other
      // stuff.
      if (i.first[0] != '[') {
        if (auto size = std::get_if<size_t>(&i.second)) {
          total += *size;
        }
      }
    }
    keys.push_back("[total]");
    counts["[total]"] = total;
    // sort
    sort(keys.begin(), keys.end(), [](const char* a, const char* b) -> bool {
      // Sort the [..] ones first.
      if (a[0] == '[' && b[0] != '[') {
        return true;
      }
      if (a[0] != '[' && b[0] == '[') {
        return false;
      }
      return strcmp(b, a) > 0;
    });
    o << title << "\n";
    for (auto* key : keys) {
      if (auto size = std::get_if<size_t>(&counts[key])) {
        auto value = *size;
        if (value == 0 && key[0] != '[') {
          continue;
        }
        o << " " << left << setw(15) << key << ": " << setw(8) << value;
        if (lastCounts.count(key)) {
          size_t before = std::get<size_t>(lastCounts[key]);
          size_t after = value;
          if (after - before) {
            if (after > before) {
              Colors::red(o);
            } else {
              Colors::green(o);
            }
            o << right << setw(8);
            o << showpos << after - before << noshowpos;
            Colors::normal(o);
          }
        }
      } else {
        auto value = std::get<double>(counts[key]);
        o << " " << left << setw(15) << key << ": " << setw(8) << value;
      }
      o << "\n";
    }
  }
};

Pass* createMetricsPass() { return new Metrics(false); }

Pass* createFunctionMetricsPass() { return new Metrics(true); }

} // namespace wasm
