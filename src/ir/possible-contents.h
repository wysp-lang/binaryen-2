/*
 * Copyright 2022 WebAssembly Community Group participants
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

#ifndef wasm_ir_possible_contents_h
#define wasm_ir_possible_contents_h

#include <variant>

#include "ir/possible-constant.h"
#include "ir/subtypes.h"
#include "support/hash.h"
#include "support/insert_ordered.h"
#include "support/small_vector.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

//
// PossibleContents represents the possible contents at a particular location
// (such as in a local or in a function parameter). This is a little similar to
// PossibleConstantValues, but considers more types of contents than constant
// values - in particular, it can track types to some extent.
//
// The specific contents this can vary over are:
//
//  * None:            No possible value.
//
//  * Literal:         One possible constant value like an i32 of 42.
//
//  * Global:          The name of a global whose value is here. We do not know
//                     the actual value at compile time, but we know it is equal
//                     to that global. Typically we can only infer this for
//                     immutable globals.
//
//  * ConeType:        Any possible value of a particular type, and a possible
//                     "cone" of a certain depth below it. If the depth is 0
//                     then only the exact type is possible; if the depth is 1
//                     then either that type or its immediate subtypes, and so
//                     forth.
//                     A depth of -1 means unlimited: all subtypes are allowed.
//                     If the type here is nullable then null is also allowed.
//                     TODO: Add ConeTypePlusContents or such, which would be
//                           used on e.g. a struct.new with an immutable field
//                           to which we assign a constant: not only do we know
//                           the type, but also certain field's values.
//
//  * Many:            Anything else. Many things are possible here, and we do
//                     not track what they might be, so we must assume the worst
//                     in the calling code.
//
class PossibleContents {
  struct None : public std::monostate {};

  struct GlobalInfo {
    Name name;
    // The type of contents. Note that this may not match the type of the
    // global, if we were filtered. For example:
    //
    //  (ref.as_non_null
    //    (global.get $nullable-global)
    //  )
    //
    // The contents flowing out will be a Global, but of a non-nullable type,
    // unlike the original global.
    Type type;
    bool operator==(const GlobalInfo& other) const {
      return name == other.name && type == other.type;
    }
  };

  struct ConeType {
    Type type;
    Index depth;
    bool operator==(const ConeType& other) const {
      return type == other.type && depth == other.depth;
    }
  };

  struct Many : public std::monostate {};

  // TODO: This is similar to the variant in PossibleConstantValues, and perhaps
  //       we could share code, but extending a variant using template magic may
  //       not be worthwhile. Another option might be to make PCV inherit from
  //       this and disallow ConeType etc., but PCV might get slower.
  using Variant = std::variant<None, Literal, GlobalInfo, ConeType, Many>;
  Variant value;

  // Internal convenience for creating a cone type with depth 0, i.e,, an exact
  // type.
  static ConeType ExactType(Type type) { return ConeType{type, 0}; }

  static constexpr Index FullDepth = -1;

  // Internal convenience for creating a cone type of unbounded depth, i.e., the
  // full cone of all subtypes for that type.
  static ConeType FullConeType(Type type) { return ConeType{type, FullDepth}; }

  template<typename T> PossibleContents(T value) : value(value) {}

public:
  PossibleContents() : value(None()) {}
  PossibleContents(const PossibleContents& other) = default;

  // Most users will use one of the following static functions to construct a
  // new instance:

  static PossibleContents none() { return PossibleContents{None()}; }
  static PossibleContents literal(Literal c) { return PossibleContents{c}; }
  static PossibleContents global(Name name, Type type) {
    return PossibleContents{GlobalInfo{name, type}};
  }
  // Helper for a cone type with depth 0, i.e., an exact type.
  static PossibleContents exactType(Type type) {
    return PossibleContents{ExactType(type)};
  }
  // Helper for a cone with unbounded depth, i.e., the full cone of all subtypes
  // for that type.
  static PossibleContents fullConeType(Type type) {
    return PossibleContents{FullConeType(type)};
  }
  static PossibleContents coneType(Type type, Index depth) {
    return PossibleContents{ConeType{type, depth}};
  }
  static PossibleContents many() { return PossibleContents{Many()}; }

  // Helper for creating a PossibleContents based on a wasm type, that is, where
  // all we know is the wasm type.
  static PossibleContents fromType(Type type) {
    assert(type != Type::none);

    if (type.isRef()) {
      // For a reference, subtyping matters.
      return fullConeType(type);
    }

    if (type == Type::unreachable) {
      // Nothing is possible here.
      return none();
    }

    // Otherwise, this is a concrete MVP type.
    assert(type.isConcrete());
    return exactType(type);
  }

  PossibleContents& operator=(const PossibleContents& other) = default;

  bool operator==(const PossibleContents& other) const {
    return value == other.value;
  }

  bool operator!=(const PossibleContents& other) const {
    return !(*this == other);
  }

  // Combine the information in a given PossibleContents to this one. The
  // contents here will then include whatever content was possible in |other|.
  [[nodiscard]] static PossibleContents combine(const PossibleContents& a,
                                                const PossibleContents& b);

  void combine(const PossibleContents& other) {
    *this = PossibleContents::combine(*this, other);
  }

  // Removes anything not in |other| from this object, so that it ends up with
  // only their intersection. Currently this only handles an intersection with a
  // full cone.
  void intersectWithFullCone(const PossibleContents& other);

  bool isNone() const { return std::get_if<None>(&value); }
  bool isLiteral() const { return std::get_if<Literal>(&value); }
  bool isGlobal() const { return std::get_if<GlobalInfo>(&value); }
  bool isConeType() const { return std::get_if<ConeType>(&value); }
  bool isMany() const { return std::get_if<Many>(&value); }

  Literal getLiteral() const {
    assert(isLiteral());
    return std::get<Literal>(value);
  }

  Name getGlobal() const {
    assert(isGlobal());
    return std::get<GlobalInfo>(value).name;
  }

  bool isNull() const { return isLiteral() && getLiteral().isNull(); }

  // Return the relevant type here. Note that the *meaning* of the type varies
  // by the contents: type $foo of a global means that type or any subtype, as a
  // subtype might be written to it, while type $foo of a Literal or a ConeType
  // with depth zero means that type and nothing else, etc. (see also
  // hasExactType).
  //
  // If no type is possible, return unreachable; if many types are, return none.
  Type getType() const {
    if (auto* literal = std::get_if<Literal>(&value)) {
      return literal->type;
    } else if (auto* global = std::get_if<GlobalInfo>(&value)) {
      return global->type;
    } else if (auto* coneType = std::get_if<ConeType>(&value)) {
      return coneType->type;
    } else if (std::get_if<None>(&value)) {
      return Type::unreachable;
    } else if (std::get_if<Many>(&value)) {
      return Type::none;
    } else {
      WASM_UNREACHABLE("bad value");
    }
  }

  // Returns cone type info. This can be called on non-cone types as well, and
  // it returns a cone that best describes them. That is, this is like getType()
  // but it also provides an indication about the depth, if relevant. (If cone
  // info is not relevant, like when getType() returns none or unreachable, the
  // depth is set to 0.)
  ConeType getCone() const {
    if (auto* literal = std::get_if<Literal>(&value)) {
      return ExactType(literal->type);
    } else if (auto* global = std::get_if<GlobalInfo>(&value)) {
      return FullConeType(global->type);
    } else if (auto* coneType = std::get_if<ConeType>(&value)) {
      return *coneType;
    } else if (std::get_if<None>(&value)) {
      return ExactType(Type::unreachable);
    } else if (std::get_if<Many>(&value)) {
      return ExactType(Type::none);
    } else {
      WASM_UNREACHABLE("bad value");
    }
  }

  // Returns whether the relevant cone for this, as computed by getCone(), is of
  // full size, that is, includes all subtypes.
  bool hasFullCone() const { return getCone().depth == FullDepth; }

  // Returns whether this is a cone type and also is of full size. This differs
  // from hasFullCone() in that the former can return true for a global, for
  // example, while this cannot (a global is not a cone type, but the
  // information we have about its cone is that it is full).
  bool isFullConeType() const { return isConeType() && hasFullCone(); }

  // Returns whether the type we can report here is exact, that is, nothing of a
  // strict subtype might show up - the contents here have an exact type.
  //
  // This returns false for None and Many, for whom it is not well-defined.
  bool hasExactType() const {
    if (isLiteral()) {
      return true;
    }

    if (auto* coneType = std::get_if<ConeType>(&value)) {
      return coneType->depth == 0;
    }

    return false;
  }

  // Returns whether the given contents have any intersection, that is, whether
  // some value exists that can appear in both |a| and |b|. For example, if
  // either is None, or if they are different literals, then they have no
  // intersection.
  static bool haveIntersection(const PossibleContents& a,
                               const PossibleContents& b);

  // Returns whether |a| is a subset of |b|, that is, all possible contents of
  // |a| are also possible in |b|.
  static bool isSubContents(const PossibleContents& a,
                            const PossibleContents& b);

  // Whether we can make an Expression* for this containing the proper contents.
  // We can do that for a Literal (emitting a Const or RefFunc etc.) or a
  // Global (emitting a GlobalGet), but not for anything else yet.
  bool canMakeExpression() const { return isLiteral() || isGlobal(); }

  Expression* makeExpression(Module& wasm) {
    assert(canMakeExpression());
    Builder builder(wasm);
    if (isLiteral()) {
      return builder.makeConstantExpression(getLiteral());
    } else {
      auto name = getGlobal();
      // Note that we load the type from the module, rather than use the type
      // in the GlobalInfo, as that type may not match the global (see comment
      // in the GlobalInfo declaration above).
      return builder.makeGlobalGet(name, wasm.getGlobal(name)->type);
    }
  }

  size_t hash() const {
    // First hash the index of the variant, then add the internals for each.
    size_t ret = std::hash<size_t>()(value.index());
    if (isNone() || isMany()) {
      // Nothing to add.
    } else if (isLiteral()) {
      rehash(ret, getLiteral());
    } else if (isGlobal()) {
      rehash(ret, getGlobal());
    } else if (auto* coneType = std::get_if<ConeType>(&value)) {
      rehash(ret, coneType->type);
      rehash(ret, coneType->depth);
    } else {
      WASM_UNREACHABLE("bad variant");
    }
    return ret;
  }

  void dump(std::ostream& o, Module* wasm = nullptr) const {
    o << '[';
    if (isNone()) {
      o << "None";
    } else if (isLiteral()) {
      o << "Literal " << getLiteral();
      auto t = getType();
      if (t.isRef()) {
        auto h = t.getHeapType();
        o << " HT: " << h;
      }
    } else if (isGlobal()) {
      o << "GlobalInfo $" << getGlobal() << " T: " << getType();
    } else if (auto* coneType = std::get_if<ConeType>(&value)) {
      auto t = coneType->type;
      o << "ConeType " << t;
      if (coneType->depth == 0) {
        o << " exact";
      } else {
        o << " depth=" << coneType->depth;
      }
      if (t.isRef()) {
        auto h = t.getHeapType();
        o << " HT: " << h;
        if (wasm && wasm->typeNames.count(h)) {
          o << " $" << wasm->typeNames[h].name;
        }
        if (t.isNullable()) {
          o << " null";
        }
      }
    } else if (isMany()) {
      o << "Many";
    } else {
      WASM_UNREACHABLE("bad variant");
    }
    o << ']';
  }
};

// The various *Location structs (ExpressionLocation, ResultLocation, etc.)
// describe particular locations where content can appear.

// The location of a specific IR expression.
struct ExpressionLocation {
  Expression* expr;
  // If this expression contains a tuple then each index in the tuple will have
  // its own location with a corresponding tupleIndex. If this is not a tuple
  // then we only use tupleIndex 0.
  Index tupleIndex;
  bool operator==(const ExpressionLocation& other) const {
    return expr == other.expr && tupleIndex == other.tupleIndex;
  }
};

// The location of one of the parameters of a function.
struct ParamLocation {
  Function* func;
  Index index;
  bool operator==(const ParamLocation& other) const {
    return func == other.func && index == other.index;
  }
};

// The location of one of the results of a function.
struct ResultLocation {
  Function* func;
  Index index;
  bool operator==(const ResultLocation& other) const {
    return func == other.func && index == other.index;
  }
};

// The location of a break target in a function, identified by its name.
struct BreakTargetLocation {
  Function* func;
  Name target;
  // As in ExpressionLocation, the index inside the tuple, or 0 if not a tuple.
  // That is, if the branch target has a tuple type, then each branch to that
  // location sends a tuple, and we'll have a separate BreakTargetLocation for
  // each, indexed by the index in the tuple that the branch sends.
  Index tupleIndex;
  bool operator==(const BreakTargetLocation& other) const {
    return func == other.func && target == other.target &&
           tupleIndex == other.tupleIndex;
  }
};

// The location of a global in the module.
struct GlobalLocation {
  Name name;
  bool operator==(const GlobalLocation& other) const {
    return name == other.name;
  }
};

// The location of one of the parameters in a function signature.
struct SignatureParamLocation {
  HeapType type;
  Index index;
  bool operator==(const SignatureParamLocation& other) const {
    return type == other.type && index == other.index;
  }
};

// The location of one of the results in a function signature.
struct SignatureResultLocation {
  HeapType type;
  Index index;
  bool operator==(const SignatureResultLocation& other) const {
    return type == other.type && index == other.index;
  }
};

// The location of contents in a struct or array (i.e., things that can fit in a
// dataref). Note that this is specific to this type - it does not include data
// about subtypes or supertypes.
struct DataLocation {
  HeapType type;
  // The index of the field in a struct, or 0 for an array (where we do not
  // attempt to differentiate by index).
  Index index;
  bool operator==(const DataLocation& other) const {
    return type == other.type && index == other.index;
  }
};

// The location of anything written to a particular tag.
struct TagLocation {
  Name tag;
  // If the tag has more than one element, we'll have a separate TagLocation for
  // each, with corresponding indexes. If the tag has just one element we'll
  // only have one TagLocation with index 0.
  Index tupleIndex;
  bool operator==(const TagLocation& other) const {
    return tag == other.tag && tupleIndex == other.tupleIndex;
  }
};

// A null value. This is used as the location of the default value of a var in a
// function, a null written to a struct field in struct.new_with_default, etc.
struct NullLocation {
  Type type;
  bool operator==(const NullLocation& other) const {
    return type == other.type;
  }
};

// A special type of location that does not refer to something concrete in the
// wasm, but is used to optimize the graph. A "cone read" is a struct.get or
// array.get of a type that is not exact, so it can read from either that type
// of some of the subtypes (up to a particular subtype depth).
//
// In general a read of a cone type + depth (as opposed to an exact type) will
// require N incoming links, from each of the N subtypes - and we need that
// for each struct.get of a cone. If there are M such gets then we have N * M
// edges for this. Instead, we make a single canonical "cone read" location, and
// add a single link to it from each get, which is only N + M (plus the cost
// of adding "latency" in requiring an additional step along the way for the
// data to flow along).
struct ConeReadLocation {
  HeapType type;
  // As in PossibleContents, this represents the how deep we go with subtypes.
  // 0 means an exact type, 1 means immediate subtypes, etc. (Note that 0 is not
  // needed since that is what DataLocation already is.)
  Index depth;
  // The index of the field in a struct, or 0 for an array (where we do not
  // attempt to differentiate by index).
  Index index;
  bool operator==(const ConeReadLocation& other) const {
    return type == other.type && depth == other.depth && index == other.index;
  }
};

// A location is a variant over all the possible flavors of locations that we
// have.
using Location = std::variant<ExpressionLocation,
                            ParamLocation,
                            ResultLocation,
                            BreakTargetLocation,
                            GlobalLocation,
                            SignatureParamLocation,
                            SignatureResultLocation,
                            DataLocation,
                            TagLocation,
                            NullLocation,
                            ConeReadLocation>;

// We are going to do a very large flow operation, potentially, as we create
// a Location for every interesting part in the entire wasm, and some of those
// places will have lots of links (like a struct field may link out to every
// single struct.get of that type), so we must make the data structures here
// as efficient as possible. Towards that goal, we work with location
// *indexes* where possible, which are small (32 bits) and do not require any
// complex hashing when we use them in sets or maps.
//
// Note that we do not use indexes everywhere, since the initial analysis is
// done in parallel, and we do not have a fixed indexing of locations yet. When
// we merge the parallel data we create that indexing, and use indexes from then
// on.
using LocationIndex = uint32_t;

// A link indicates a flow of content from one location to another. For
// example, if we do a local.get and return that value from a function, then
// we have a link from the ExpressionLocation of that local.get to a
// ResultLocation.
template<typename T> struct PossibleContentLink {
  T from;
  T to;

  bool operator==(const PossibleContentLink<T>& other) const {
    return from == other.from && to == other.to;
  }
};

using LocationLink = PossibleContentLink<Location>;
using IndexLink = PossibleContentLink<LocationIndex>;

} // namespace wasm

namespace std {

std::ostream& operator<<(std::ostream& stream,
                         const wasm::PossibleContents& contents);

template<> struct hash<wasm::PossibleContents> {
  size_t operator()(const wasm::PossibleContents& contents) const {
    return contents.hash();
  }
};

// Define hashes of all the *Location flavors so that Location itself is
// hashable and we can use it in unordered maps and sets.

template<> struct hash<wasm::ExpressionLocation> {
  size_t operator()(const wasm::ExpressionLocation& loc) const {
    return std::hash<std::pair<size_t, wasm::Index>>{}(
      {size_t(loc.expr), loc.tupleIndex});
  }
};

template<> struct hash<wasm::ParamLocation> {
  size_t operator()(const wasm::ParamLocation& loc) const {
    return std::hash<std::pair<size_t, wasm::Index>>{}(
      {size_t(loc.func), loc.index});
  }
};

template<> struct hash<wasm::ResultLocation> {
  size_t operator()(const wasm::ResultLocation& loc) const {
    return std::hash<std::pair<size_t, wasm::Index>>{}(
      {size_t(loc.func), loc.index});
  }
};

template<> struct hash<wasm::BreakTargetLocation> {
  size_t operator()(const wasm::BreakTargetLocation& loc) const {
    return std::hash<std::tuple<size_t, wasm::Name, wasm::Index>>{}(
      {size_t(loc.func), loc.target, loc.tupleIndex});
  }
};

template<> struct hash<wasm::GlobalLocation> {
  size_t operator()(const wasm::GlobalLocation& loc) const {
    return std::hash<wasm::Name>{}(loc.name);
  }
};

template<> struct hash<wasm::SignatureParamLocation> {
  size_t operator()(const wasm::SignatureParamLocation& loc) const {
    return std::hash<std::pair<wasm::HeapType, wasm::Index>>{}(
      {loc.type, loc.index});
  }
};

template<> struct hash<wasm::SignatureResultLocation> {
  size_t operator()(const wasm::SignatureResultLocation& loc) const {
    return std::hash<std::pair<wasm::HeapType, wasm::Index>>{}(
      {loc.type, loc.index});
  }
};

template<> struct hash<wasm::DataLocation> {
  size_t operator()(const wasm::DataLocation& loc) const {
    return std::hash<std::pair<wasm::HeapType, wasm::Index>>{}(
      {loc.type, loc.index});
  }
};

template<> struct hash<wasm::TagLocation> {
  size_t operator()(const wasm::TagLocation& loc) const {
    return std::hash<std::pair<wasm::Name, wasm::Index>>{}(
      {loc.tag, loc.tupleIndex});
  }
};

template<> struct hash<wasm::NullLocation> {
  size_t operator()(const wasm::NullLocation& loc) const {
    return std::hash<wasm::Type>{}(loc.type);
  }
};

template<> struct hash<wasm::ConeReadLocation> {
  size_t operator()(const wasm::ConeReadLocation& loc) const {
    return std::hash<std::tuple<wasm::HeapType, wasm::Index, wasm::Index>>{}(
      {loc.type, loc.depth, loc.index});
  }
};

template<> struct hash<wasm::LocationLink> {
  size_t operator()(const wasm::LocationLink& loc) const {
    return std::hash<std::pair<wasm::Location, wasm::Location>>{}(
      {loc.from, loc.to});
  }
};

template<> struct hash<wasm::IndexLink> {
  size_t operator()(const wasm::IndexLink& loc) const {
    return std::hash<std::pair<wasm::LocationIndex, wasm::LocationIndex>>{}(
      {loc.from, loc.to});
  }
};

} // namespace std

namespace wasm {

// Builds a graph that represents the flow of PossibleContent in an entire wasm
// module. Optionally also flows the data through the graph.
struct PossibleContentsGraph {
  Module& wasm;

  // The constructor builds the graph, filling in the public data below.
  PossibleContentsGraph(Module& wasm);

  // Flow content through the graph. This must be done before calling
  // getContents().
  void flow();

  // Each LocationIndex will have one LocationInfo that contains the relevant
  // information we need for each location.
  struct LocationInfo {
    // The location at this index.
    Location location;

    // The possible contents in that location.
    PossibleContents contents;

    // A list of the target locations to which this location sends content.
    // TODO: benchmark SmallVector<1> here, as commonly there may be a single
    //       target (an expression has one parent)
    std::vector<LocationIndex> targets;

    LocationInfo(Location location) : location(location) {}
  };

  // Maps location indexes to the info stored there, as just described above.
  std::vector<LocationInfo> locations;

  // Reverse mapping of locations to their indexes.
  std::unordered_map<Location, LocationIndex> locationIndexes;

  const Location& getLocation(LocationIndex index) {
    assert(index < locations.size());
    return locations[index].location;
  }

  PossibleContents& getContents(LocationIndex index) {
    assert(index < locations.size());
    return locations[index].contents;
  }

private:
  std::vector<LocationIndex>& getTargets(LocationIndex index) {
    assert(index < locations.size());
    return locations[index].targets;
  }

  // Convert the data into the efficient LocationIndex form we will use during
  // the flow analysis. This method returns the index of a location, allocating
  // one if this is the first time we see it.
  LocationIndex getIndex(const Location& location) {
    auto iter = locationIndexes.find(location);
    if (iter != locationIndexes.end()) {
      return iter->second;
    }

    // Allocate a new index here.
    size_t index = locations.size();
#if defined(POSSIBLE_CONTENTS_DEBUG) && POSSIBLE_CONTENTS_DEBUG >= 2
    std::cout << "  new index " << index << " for ";
    dump(location);
#endif
    if (index >= std::numeric_limits<LocationIndex>::max()) {
      // 32 bits should be enough since each location takes at least one byte
      // in the binary, and we don't have 4GB wasm binaries yet... do we?
      Fatal() << "Too many locations for 32 bits";
    }
    locations.emplace_back(location);
    locationIndexes[location] = index;

    return index;
  }

  bool hasIndex(const Location& location) {
    return locationIndexes.find(location) != locationIndexes.end();
  }

  IndexLink getIndexes(const LocationLink& link) {
    return {getIndex(link.from), getIndex(link.to)};
  }

  // See the comment on CollectedFuncInfo::childParents. This is the merged info
  // from all the functions and the global scope.
  std::unordered_map<LocationIndex, LocationIndex> childParents;

  // The work remaining to do during the flow: locations that we need to flow
  // content from, after new content reached them.
  //
  // Using a set here is efficient as multiple updates may arrive to a location
  // before we get to processing it.
  //
  // The items here could be {location, newContents}, but it is more efficient
  // to have already written the new contents to the main data structure. That
  // avoids larger data here, and also, updating the contents as early as
  // possible is helpful as anything reading them meanwhile (before we get to
  // their work item in the queue) will see the newer value, possibly avoiding
  // flowing an old value that would later be overwritten.
  //
  // This must be ordered to avoid nondeterminism. The problem is that our
  // operations are imprecise and so the transitive property does not hold:
  // (AvB)vC may differ from Av(BvC). Likewise (AvB)^C may differ from
  // (A^C)v(B^C). An example of the latter is if a location is sent a null func
  // and an i31, and the location can only contain funcref. If the null func
  // arrives first, then later we'd merge null func + i31 which ends up as Many,
  // and then we filter that to funcref and get funcref. But if the i31 arrived
  // first, we'd filter it into nothing, and then the null func that arrives
  // later would be the final result. This would not happen if our operations
  // were precise, but we only make approximations here to avoid unacceptable
  // overhead, such as cone types but not arbitrary unions, etc.
  InsertOrderedSet<LocationIndex> workQueue;

  // All existing links in the graph. We keep this to know when a link we want
  // to add is new or not.
  std::unordered_set<IndexLink> links;

  // Update a location with new contents that are added to everything already
  // present there. If the update changes the contents at that location (if
  // there was anything new) then we also need to flow from there, which we will
  // do by adding the location to the work queue, and eventually flowAfterUpdate
  // will be called on this location.
  //
  // Returns whether it is worth sending new contents to this location in the
  // future. If we return false, the sending location never needs to do that
  // ever again.
  bool updateContents(LocationIndex locationIndex,
                      PossibleContents newContents);

  // Slow helper that converts a Location to a LocationIndex. This should be
  // avoided. TODO: remove the remaining uses of this.
  bool updateContents(const Location& location,
                      const PossibleContents& newContents) {
    return updateContents(getIndex(location), newContents);
  }

  // Flow contents from a location where a change occurred. This sends the new
  // contents to all the normal targets of this location (using
  // flowToTargetsAfterUpdate), and also handles special cases of flow after.
  void flowAfterUpdate(LocationIndex locationIndex);

  // Internal part of flowAfterUpdate that handles sending new values to the
  // given location index's normal targets (that is, the ones listed in the
  // |targets| vector).
  void flowToTargetsAfterUpdate(LocationIndex locationIndex,
                                const PossibleContents& contents);

  // Add a new connection while the flow is happening. If the link already
  // exists it is not added.
  void connectDuringFlow(Location from, Location to);

  // Contents sent to certain locations can be filtered in a special way during
  // the flow, which is handled in these helpers. These may update
  // |worthSendingMore| which is whether it is worth sending any more content to
  // this location in the future.
  void filterExpressionContents(PossibleContents& contents,
                                const ExpressionLocation& exprLoc,
                                bool& worthSendingMore);
  void filterGlobalContents(PossibleContents& contents,
                            const GlobalLocation& globalLoc);
  void filterDataContents(PossibleContents& contents,
                          const DataLocation& dataLoc);

  // Reads from GC data: a struct.get or array.get. This is given the type of
  // the read operation, the field that is read on that type, the known contents
  // in the reference the read receives, and the read instruction itself. We
  // compute where we need to read from based on the type and the ref contents
  // and get that data, adding new links in the graph as needed.
  void readFromData(Type declaredType,
                    Index fieldIndex,
                    const PossibleContents& refContents,
                    Expression* read);

  // Similar to readFromData, but does a write for a struct.set or array.set.
  void writeToData(Expression* ref, Expression* value, Index fieldIndex);

  // We will need subtypes during the flow, so compute them once ahead of time.
  std::unique_ptr<SubTypes> subTypes;

  // The depth of children for each type. This is 0 if the type has no
  // subtypes, 1 if it has subtypes but none of those have subtypes themselves,
  // and so forth.
  std::unordered_map<HeapType, Index> maxDepths;

  // Given a ConeType, return the normalized depth, that is, the canonical depth
  // given the actual children it has. If this is a full cone, then we can
  // always pick the actual maximal depth and use that instead of FullDepth==-1.
  // For a non-full cone, we also reduce the depth as much as possible, so it is
  // equal to the maximum depth of an existing subtype.
  Index getNormalizedConeDepth(Type type, Index depth) {
    return std::min(depth, maxDepths[type.getHeapType()]);
  }

  void normalizeConeType(PossibleContents& cone) {
    assert(cone.isConeType());
    auto type = cone.getType();
    auto before = cone.getCone().depth;
    auto normalized = getNormalizedConeDepth(type, before);
    if (normalized != before) {
      cone = PossibleContents::coneType(type, normalized);
    }
  }

#if defined(POSSIBLE_CONTENTS_DEBUG) && POSSIBLE_CONTENTS_DEBUG >= 2
  // Dump out a location for debug purposes.
  void dump(Location location);
#endif
};

// Analyze the entire wasm file to find which contents are possible in which
// locations. This assumes a closed world and starts from roots - newly created
// values - and propagates them to the locations they reach. After the
// analysis the user of this class can ask which contents are possible at any
// location.
//
// This focuses on useful information for the typical user of this API.
// Specifically, we find out:
//
//  1. What locations have no content reaching them at all. That means the code
//     is unreachable. (Other passes may handle this, but ContentOracle does it
//     for all things, so it might catch situations other passes do not cover;
//     and, it takes no effort to support this here).
//  2. For all locations, we try to find when they must contain a constant value
//     like i32(42) or ref.func(foo).
//  3. For locations that contain references, information about the subtypes
//     possible there. For example, if something has wasm type anyref in the IR,
//     we might find it must contain an exact type of something specific.
//
// Note that there is not much use in providing type info for locations that are
// *not* references. If a local is i32, for example, then it cannot contain any
// subtype anyhow, since i32 is not a reference and has no subtypes. And we know
// the type i32 from the wasm anyhow, that is, the caller will know it.
// Therefore the only useful information we can provide on top of the info
// already in the wasm is either that nothing can be there (1, above), or that a
// constant must be there (2, above), and so we do not make an effort to track
// non-reference types here. This makes the internals of ContentOracle simpler
// and faster. A noticeable outcome of that is that querying the contents of an
// i32 local will return Many and not ConeType{i32, 0} (assuming we could not
// infer either that there must be nothing there, or a constant). Again, the
// caller is assumed to know the wasm IR type anyhow, and also other
// optimization passes work on the types in the IR, so we do not focus on that
// here.
class ContentOracle {
  Module& wasm;

  void analyze();

public:
  ContentOracle(Module& wasm) : wasm(wasm) { analyze(); }

  // Get the contents possible at a location.
  PossibleContents getContents(Location location) {
    auto iter = locationContents.find(location);
    if (iter == locationContents.end()) {
      // We know of no possible contents here.
      return PossibleContents::none();
    }
    return iter->second;
  }

  // Helper for the common case of an expression location that is not a
  // multivalue.
  PossibleContents getContents(Expression* curr) {
    assert(curr->type.size() == 1);
    return getContents(ExpressionLocation{curr, 0});
  }

private:
  std::unordered_map<Location, PossibleContents> locationContents;
};

} // namespace wasm

#endif // wasm_ir_possible_contents_h
