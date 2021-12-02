#include <cassert>
#include <iostream>

#include <cfg/dominates.h>
#include <wasm.h>

using namespace wasm;

struct BasicBlock {
  std::vector<BasicBlock*> in;

  struct Contents {
    std::vector<Expression*> list;
  } contents;

  void addPred(BasicBlock* pred) { in.push_back(pred); }

  Expression* addItem(Expression* item) {
    contents.list.push_back(item);
    return item;
  }
};

struct CFG : public std::vector<std::unique_ptr<BasicBlock>> {
  BasicBlock* add() {
    emplace_back(std::make_unique<BasicBlock>());
    return back().get();
  }

  void connect(BasicBlock* pred, BasicBlock* succ) { succ->addPred(pred); }
};

int main() {
  Module temp;
  Builder builder(temp);

  // Do a check of
  //   assert( check(left, right) );
  // and also check the reverse (right, left) combination. If left == right then
  // that must assert as true, the same as unflipped. Otherwise, it must check
  // as the reverse, as domination of different items is antisymmetrical.
  #define CHECK_SYMMETRIC(check, left, right) { \
    assert(check(left, right)); \
    if (left == right) { \
      assert(check(right, left)); \
    } else { \
      assert(!check(right, left)); \
    } \
  }

  // An CFG with just an entry.
  {
    CFG cfg;
    auto* entry = cfg.add();
    auto* first = entry->addItem(builder.makeNop());
    auto* second = entry->addItem(builder.makeNop());
    auto* third = entry->addItem(builder.makeNop());

    cfg::DominationChecker<BasicBlock> checker(cfg);

    // Things dominate themselves.
    CHECK_SYMMETRIC(checker.dominates, first, first);
    CHECK_SYMMETRIC(checker.dominates, second, second);
    CHECK_SYMMETRIC(checker.dominates, third, third);

    // Things dominate those after them.
    CHECK_SYMMETRIC(checker.dominates, first, second);
    CHECK_SYMMETRIC(checker.dominates, first, third);
    CHECK_SYMMETRIC(checker.dominates, second, third);
  }

  // entry => next, with items in both.
  {
    CFG cfg;
    auto* entry = cfg.add();
    auto* next = cfg.add();
    cfg.connect(entry, next);
    auto* entryA = entry->addItem(builder.makeNop());
    auto* entryB = entry->addItem(builder.makeNop());
    auto* nextA = next->addItem(builder.makeNop());
    auto* nextB = next->addItem(builder.makeNop());

    cfg::DominationChecker<BasicBlock> checker(cfg);

    // Things dominate themselves in all blocks.
    CHECK_SYMMETRIC(checker.dominates, entryA, entryA);
    CHECK_SYMMETRIC(checker.dominates, entryB, entryB);
    CHECK_SYMMETRIC(checker.dominates, nextA, nextA);
    CHECK_SYMMETRIC(checker.dominates, nextB, nextB);

    // Things dominate things after them in the same block.
    CHECK_SYMMETRIC(checker.dominates, entryA, entryB);
    CHECK_SYMMETRIC(checker.dominates, nextA, nextB);

    // The entry block items dominate items in the next block.
    CHECK_SYMMETRIC(checker.dominates, entryA, nextA);
    CHECK_SYMMETRIC(checker.dominates, entryA, nextB);
    CHECK_SYMMETRIC(checker.dominates, entryB, nextA);
    CHECK_SYMMETRIC(checker.dominates, entryB, nextB);
  }

  std::cout << "success.\n";
}
