#include <iostream>
#include <cassert>
#include <vector>

#include "support/small_set.h"

using namespace wasm;

template<typename T, typename U>
void assertContents(T& t, const std::vector<U>& contents) {
  assert(t.size() == contents.size());
  for (auto item : contents) {
    assert(t.count(item) == 1);
  }
}

template<typename T>
void test() {
  {
    T t;

    // build up with no duplicates
    assert(t.empty());
    assert(t.size() == 0);
    t.insert(1);
    assert(!t.empty());
    assert(t.size() == 1);
    t.insert(2);
    assert(!t.empty());
    assert(t.size() == 2);
    t.insert(3);
    assert(!t.empty());

    // unwind
    assert(t.size() == 3);
    t.erase(3);
    assert(t.size() == 2);
    t.erase(2);
    assert(t.size() == 1);
    t.erase(1);
    assert(t.size() == 0);
    assert(t.empty());
  }
  {
    T t;

    // build up with duplicates
    t.insert(1);
    t.insert(2);
    t.insert(2);
    t.insert(3);
    assert(t.size() == 3);

    // unwind by erasing (in the opposite direction from before)
    assert(t.count(1) == 1);
    assert(t.count(2) == 1);
    assert(t.count(3) == 1);
    assert(t.count(1337) == 0);

    t.erase(1);
    assert(t.count(1) == 0);

    assert(t.size() == 2);

    assert(t.count(2) == 1);
    t.erase(2);
    assert(t.count(2) == 0);

    assert(t.size() == 1);

    assert(t.count(3) == 1);
    t.erase(3);

    assert(t.count(1) == 0);
    assert(t.count(2) == 0);
    assert(t.count(3) == 0);
    assert(t.count(1337) == 0);

    assert(t.size() == 0);
  }
  {
    T t;

    // build up
    t.insert(1);
    t.insert(2);
    t.insert(3);

    // unwind by clearing
    t.clear();
    assert(t.size() == 0);
    assert(t.empty());
  }
  {
    T t, u;
    // comparisons
    assert(t == u);
    t.insert(1);
    assert(t != u);
    u.insert(1);
    assert(t == u);
    u.erase(1);
    assert(t != u);
    u.insert(2);
    assert(t != u);
  }
}

int main() {
  test<SmallSet<int, 0>>();
  test<SmallSet<int, 1>>();
  test<SmallSet<int, 2>>();
  test<SmallSet<int, 10>>();
  std::cout << "ok.\n";
}

