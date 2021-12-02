#include <cassert>
#include <iostream>

#include <cfg/domtree.h>
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

    DominationChecker<BasicBlock> checker(cfg);

    // Things dominate themselves.
    assert(checker.dominates(first, first));
    assert(checker.dominates(second, second));
    assert(checker.dominates(third, third));

    // Things dominate those after them.
    assert(checker.dominates(first, second));
    assert(checker.dominates(first, third));
    assert(checker.dominates(second, third));

    // Things do not dominate those before them.
    assert(checker.dominates(second, first));
    assert(checker.dominates(third, first));
    assert(checker.dominates(third, second));
  }

  std::cout << "success.\n";
}
