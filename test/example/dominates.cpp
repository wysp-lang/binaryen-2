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

  // An CFG with just an entry.
  {
    CFG cfg;
    auto* entry = cfg.add();
    auto* first = entry->addItem(builder.makeNop());
    auto* second = entry->addItem(builder.makeNop());
    auto* third = entry->addItem(builder.makeNop());

    cfg::DominationChecker<BasicBlock> checker(cfg);

    // Things dominate themselves.
    assert(checker.dominates(first, first));
    assert(checker.dominates(second, second));
    assert(checker.dominates(third, third));

    // Things dominate those after them.
    assert(checker.dominates(first, second));
    assert(checker.dominates(first, third));
    assert(checker.dominates(second, third));

    // Things do *not* dominate those before them.
    assert(!checker.dominates(second, first));
    assert(!checker.dominates(third, first));
    assert(!checker.dominates(third, second));
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
    assert(checker.dominates(entryA, entryA));
    assert(checker.dominates(entryB, entryB));
    assert(checker.dominates(nextA, nextA));
    assert(checker.dominates(nextB, nextB));

    // Things dominate things after them in the same block.
    assert(checker.dominates(entryA, entryB));
    assert(checker.dominates(nextA, nextB));
    assert(!checker.dominates(entryB, entryA));
    assert(!checker.dominates(nextB, nextA));

    // The entry block items dominate items in the next block.
    assert(checker.dominates(entryA, nextA));
    assert(checker.dominates(entryA, nextB));
    assert(checker.dominates(entryB, nextA));
    assert(checker.dominates(entryB, nextB));
    assert(!checker.dominates(nextA, entryA));
    assert(!checker.dominates(nextA, entryB));
    assert(!checker.dominates(nextB, entryA));
    assert(!checker.dominates(nextB, entryB));
  }

  std::cout << "success.\n";
}
