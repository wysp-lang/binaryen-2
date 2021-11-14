#include <cassert>
#include <iostream>

#include <support/entropy.h>

using namespace wasm;

int main() {
  {
    std::cout << "constant char compressibility: " << Entropy::estimateCompressedRatio({ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }) << '\n';
  }
  std::cout << "success.\n";
}
