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
// Find struct fields that are always written to with a constant value, and
// replace gets of them with that value.
//
// For example, if we have a vtable of type T, and we always create it with one
// of the fields containing a ref.func of the same function F, and there is no
// write to that field of a different value (even using a subtype of T), then
// anywhere we see a get of that field we can place a ref.func of F.
//
// FIXME: This pass assumes a closed world. When we start to allow multi-module
//        wasm GC programs we need to check for type escaping.
//

#include "ir/module-utils.h"
#include "ir/properties.h"
#include "ir/struct-utils.h"
#include "ir/type-updating.h"
#include "ir/utils.h"
#include "pass.h"
#include "support/unique_deferring_queue.h"
#include "wasm-builder.h"
#include "wasm-traversal.h"
#include "wasm.h"

namespace wasm {

namespace {

// Represents data about what constant values are possible in a particular
// place. There may be no values, or one, or many, or if a non-constant value is
// possible, then all we can say is that the value is "unknown" - it can be
// anything.
struct PossibleConstantValues {
  // The maximum amount of constant values we are willing to tolerate. Anything
  // above this causes us to say that the value is unknown.
  static const size_t MaxConstantValues = 3;

  // Note a written value as we see it, and update our internal knowledge based
  // on it and all previous values noted.
  //
  // Returns whether we changed anything.
  bool note(Literal curr) {
    if (!noted) {
      // This is the first value.
      values.insert(curr);
      static_assert(MaxConstantValues >= 1, "invalid max values");
      noted = true;
      return true;
    }

    // If this was already non-constant, it stays that way.
    if (!isConstant()) {
      return false;
    }

    // This is a subsequent value. Perhaps we have seen it before; if so, we
    // have nothing else to do.
    if (values.count(curr)) {
      return false;
    }

    // If this pushed us past the limit of the number of values, then mark us as
    // unknown.
    if (values.size() == MaxConstantValues) {
      noteUnknown();
    } else {
      values.insert(curr);
    }
    return true;
  }

  // Notes a value that is unknown - it can be anything. We have failed to
  // identify a constant value here.
  void noteUnknown() {
    values.clear();
    noted = true;
  }

  // Combine the information in a given PossibleConstantValues to this one. This
  // is the same as if we have called note*() on us with all the history of
  // calls to that other object.
  //
  // Returns whether we changed anything.
  bool combine(const PossibleConstantValues& other) {
    if (!other.noted) {
      return false;
    }
    if (!noted) {
      *this = other;
      return other.noted;
    }
    if (!isConstant()) {
      return false;
    }
    if (!other.isConstant()) {
      noteUnknown();
      return true;
    }

    // Both have constant values. Add the values from the other to this one,
    // looking for a change (which may be a new value, or may be that we become
    // non-constant due to too many values).
    for (auto otherValue : other.values) {
      if (note(otherValue)) {
        return true;
      }
    }
    return false;
  }

  // Check if all the values are identical and constant.
  bool isConstant() const { return noted && !values.empty(); }

  // Returns the single constant value.
  std::unordered_set<Literal> getConstantValues() const {
    assert(isConstant());
    return values;
  }

  // Returns whether we have ever noted a value.
  bool hasNoted() const { return noted; }

  void dump(std::ostream& o) {
    o << '[';
    if (!hasNoted()) {
      o << "unwritten";
    } else if (!isConstant()) {
      o << "unknown";
    } else {
      for (auto value : values) {
        o << value << ' ';
      }
    }
    o << ']';
  }

private:
  // Whether we have noted any values at all.
  bool noted = false;

  // The constant values we have seen. If |noted| is false, then this will be
  // empty. Once |noted| is true, this will contain the values, or, if we found
  // too many constant values or a non-constant value, this will be empty to
  // indicate that.
  // TODO: use a SmallSet?
  std::unordered_set<Literal> values;
};

using PCVStructValuesMap = StructValuesMap<PossibleConstantValues>;
using PCVFunctionStructValuesMap =
  FunctionStructValuesMap<PossibleConstantValues>;

// Optimize struct gets based on what we've learned about writes.
//
// TODO Aside from writes, we could use information like whether any struct of
//      this type has even been created (to handle the case of struct.sets but
//      no struct.news).
struct FunctionOptimizer : public WalkerPass<PostWalker<FunctionOptimizer>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new FunctionOptimizer(infos); }

  FunctionOptimizer(PCVStructValuesMap& infos) : infos(infos) {}

  void visitStructGet(StructGet* curr) {
    auto type = curr->ref->type;
    if (type == Type::unreachable) {
      return;
    }

    Builder builder(*getModule());

    // Find the info for this field, and see if we can optimize. First, see if
    // there is any information for this heap type at all. If there isn't, it is
    // as if nothing was ever noted for that field.
    PossibleConstantValues info;
    assert(!info.hasNoted());
    auto iter = infos.find(type.getHeapType());
    if (iter != infos.end()) {
      // There is information on this type, fetch it.
      info = iter->second[curr->index];
    }

    if (!info.hasNoted()) {
      // This field is never written at all. That means that we do not even
      // construct any data of this type, and so it is a logic error to reach
      // this location in the code. (Unless we are in an open-world
      // situation, which we assume we are not in.) Replace this get with a
      // trap. Note that we do not need to care about the nullability of the
      // reference, as if it should have trapped, we are replacing it with
      // another trap, which we allow to reorder (but we do need to care about
      // side effects in the reference, so keep it around).
      replaceCurrent(builder.makeSequence(builder.makeDrop(curr->ref),
                                          builder.makeUnreachable()));
      changed = true;
      return;
    }

    // If the value is not a constant, then it is unknown and we must give up.
    if (!info.isConstant()) {
      return;
    }

    // TODO: in -Os, avoid multiple constant values here?

    // Looks like we can do this! Replace the get with a trap on a null reference using a
    // ref.as_non_null (we need to trap as the get would have done so), plus the
    // constant value. (Leave it to further optimizations to get rid of the
    // ref.)
    auto* constantExpression = makeConstantExpression(info, curr, builder);
    if (constantExpression) {
      replaceCurrent(builder.makeSequence(
        builder.makeDrop(builder.makeRefAs(RefAsNonNull, curr->ref)),
        constantExpression));
      changed = true;
    }
  }

  void doWalkFunction(Function* func) {
    WalkerPass<PostWalker<FunctionOptimizer>>::doWalkFunction(func);

    // If we changed anything, we need to update parent types as types may have
    // changed.
    if (changed) {
      ReFinalize().walkFunctionInModule(func, getModule());
    }
  }

private:
  PCVStructValuesMap& infos;

  bool changed = false;

  Expression* makeConstantExpression(const PossibleConstantValues& info,
                                     StructGet* get,
                                     Builder& builder) {
    auto values = info.getConstantValues();
    if (values.size() == 1) {
      // Simply return the single constant value here.
      return builder.makeConstantExpression(*values.begin());
    }

    // There is more than one constant value. Emit a "select chain" for it, like
    // this:
    //
    //    (struct.get (ref))
    // =>
    //    (select
    //      (value1)
    //      (value2)
    //      (are.equal (struct.get) (value1)))
    //
    // If there are more values then two, then we recurse into the select's
    // "else" arm:
    //
    //    (select
    //      (value1)
    //      (select
    //        (value2)
    //        (value3)
    //        (are.equal (struct.get) (value2)))
    //      (are.equal (struct.get) (value1)))

    auto type = get->type;

    // We will need to make comparisons in order to pick the right value, so if
    // the type prevents that, give up.
    // TODO: Perhaps modify the field to contain an enum here and use a table.
    if (!Type::isSubType(type, Type::eqref)) {
std::cout << "so sad " << type << '\n'; // function ref types are not eqref :( so we MUST use an enum
      return nullptr;
    }

    // If there are more than 2 values, then we will need the value of the
    // struct.get more than once. If we can't use a local for that, give up. 
    if (values.size() > 2 && !TypeUpdating::canHandleAsLocal(type)) {
      return nullptr;
    }

    // If we do need a local, we will use this type. Note that we don't need to
    // do anything more than this: if the type was non-nullable, then we just
    // turned it into a nullable type, but that is fine: we don't need to turn
    // it back into a non-nullable one because we know exactly what the uses of
    // the type will be, which are simple comparisons in the select chain.
    auto localType = TypeUpdating::getValidLocalType(type, getModule()->features);

    // Given a constant expression, make a check for it, that is, that returns
    // i32:1 if an input value is indeed that value.
    auto makeCheck = [&](Expression* constant, Expression* input) -> Expression* {
      if (type.isInteger() || type.isFloat()) {
        return builder.makeBinary(Abstract::getBinary(type, Abstract::Eq), input, constant);
      }
      if (type.isRef()) {
        return builder.makeRefEq(input, constant);
      }
      WASM_UNREACHABLE("bad type for makeCheck");
    };

    Expression* ret = nullptr;
    Index tempLocal;
    auto iter = values.begin();
    for (Index i = 0; i < values.size(); i++, iter++) {
      auto value = *iter;
      auto* currExpr = builder.makeConstantExpression(value);
      if (i == 0) {
        // This is the first item.
        ret = currExpr;
        continue;
      }

      // This is a subsequent item, create a select. First, create the input
      // value for the select. This is the value that the struct.get returns,
      // but if we need it more than once we'll end up using a local.
      Expression* input = nullptr;
      if (i == 1) {
        input = get;
        if (values.size() > 2) {
          // We will have further uses of the input. Tee it to a local.
          // TODO: nullability and defaultability.
          tempLocal = builder.addVar(getFunction(), localType);
          input = builder.makeLocalTee(tempLocal, input, localType);
        }
      } else {
        // This is a reuse of the original input.
        input = builder.makeLocalGet(tempLocal, localType);
      }
      ret = builder.makeSelect(makeCheck(currExpr, input),
                               currExpr,
                               ret);
    }
    return ret;
  }
};

struct PCVScanner : public Scanner<PossibleConstantValues, PCVScanner> {
  Pass* create() override {
    return new PCVScanner(functionNewInfos, functionSetInfos);
  }

  PCVScanner(FunctionStructValuesMap<PossibleConstantValues>& functionNewInfos,
             FunctionStructValuesMap<PossibleConstantValues>& functionSetInfos)
    : Scanner<PossibleConstantValues, PCVScanner>(functionNewInfos,
                                                  functionSetInfos) {}

  void noteExpression(Expression* expr,
                      HeapType type,
                      Index index,
                      PossibleConstantValues& info) {

    if (!Properties::isConstantExpression(expr)) {
      info.noteUnknown();
    } else {
      info.note(Properties::getLiteral(expr));
    }
  }

  void noteDefault(Type fieldType,
                   HeapType type,
                   Index index,
                   PossibleConstantValues& info) {
    info.note(Literal::makeZero(fieldType));
  }

  void noteCopy(HeapType type, Index index, PossibleConstantValues& info) {
    // Ignore copies: when we set a value to a field from that same field, no
    // new values are actually introduced.
    //
    // Note that this is only sound by virtue of the overall analysis in this
    // pass: the object read from may be of a subclass, and so subclass values
    // may be actually written here. But as our analysis considers subclass
    // values too (as it must) then that is safe. That is, if a subclass of $A
    // adds a value X that can be loaded from (struct.get $A $b), then consider
    // a copy
    //
    //   (struct.set $A $b (struct.get $A $b))
    //
    // Our analysis will figure out that X can appear in that copy's get, and so
    // the copy itself does not add any information about values.
    //
    // TODO: This may be extensible to a copy from a subtype by the above
    //       analysis (but this is already entering the realm of diminishing
    //       returns).
  }
};

struct ConstantFieldPropagation : public Pass {
  void run(PassRunner* runner, Module* module) override {
    if (getTypeSystem() != TypeSystem::Nominal) {
      Fatal() << "ConstantFieldPropagation requires nominal typing";
    }

    // Find and analyze all writes inside each function.
    PCVFunctionStructValuesMap functionNewInfos(*module),
      functionSetInfos(*module);
    PCVScanner scanner(functionNewInfos, functionSetInfos);
    scanner.run(runner, module);
    scanner.walkModuleCode(module);

    // Combine the data from the functions.
    PCVStructValuesMap combinedNewInfos, combinedSetInfos;
    functionNewInfos.combineInto(combinedNewInfos);
    functionSetInfos.combineInto(combinedSetInfos);

    // Handle subtyping. |combinedInfo| so far contains data that represents
    // each struct.new and struct.set's operation on the struct type used in
    // that instruction. That is, if we do a struct.set to type T, the value was
    // noted for type T. But our actual goal is to answer questions about
    // struct.gets. Specifically, when later we see:
    //
    //  (struct.get $A x (REF-1))
    //
    // Then we want to be aware of all the relevant struct.sets, that is, the
    // sets that can write data that this get reads. Given a set
    //
    //  (struct.set $B x (REF-2) (..value..))
    //
    // then
    //
    //  1. If $B is a subtype of $A, it is relevant: the get might read from a
    //     struct of type $B (i.e., REF-1 and REF-2 might be identical, and both
    //     be a struct of type $B).
    //  2. If $B is a supertype of $A that still has the field x then it may
    //     also be relevant: since $A is a subtype of $B, the set may write to a
    //     struct of type $A (and again, REF-1 and REF-2 may be identical).
    //
    // Thus, if either $A <: $B or $B <: $A then we must consider the get and
    // set to be relevant to each other. To make our later lookups for gets
    // efficient, we therefore propagate information about the possible values
    // in each field to both subtypes and supertypes.
    //
    // struct.new on the other hand knows exactly what type is being written to,
    // and so given a get of $A and a new of $B, the new is relevant for the get
    // iff $A is a subtype of $B, so we only need to propagate in one direction
    // there, to supertypes.

    TypeHierarchyPropagator<PossibleConstantValues> propagator(*module);
    propagator.propagateToSuperTypes(combinedNewInfos);
    propagator.propagateToSuperAndSubTypes(combinedSetInfos);

    // Combine both sources of information to the final information that gets
    // care about.
    PCVStructValuesMap combinedInfos = std::move(combinedNewInfos);
    combinedSetInfos.combineInto(combinedInfos);

    // Optimize.
    // TODO: Skip this if we cannot optimize anything
    FunctionOptimizer(combinedInfos).run(runner, module);

    // TODO: Actually remove the field from the type, where possible? That might
    //       be best in another pass.
  }
};

} // anonymous namespace

Pass* createConstantFieldPropagationPass() {
  return new ConstantFieldPropagation();
}

} // namespace wasm
