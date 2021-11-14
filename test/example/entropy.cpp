#include <cassert>
#include <iostream>

#include <support/entropy.h>

using namespace wasm::Entropy;

int main() {
  std::cout << "estimating compressibility of various sequences of 14 bytes:\n";
  std::cout << "1, 1, 1, ...:\n\t" <<
    estimateCompressedBytes({ 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1 })
    << '\n';
  std::cout << "1, 2, 1, 2,... (should be higher):\n\t" <<
    estimateCompressedBytes({ 1, 2, 1, 2, 1, 2, 1, 2, 1,  2,  1,  2,  1,  2 })
    << '\n';
  std::cout << "1, 2, ..7, 1, 2, .. 7 (should be higher):\n\t" <<
    estimateCompressedBytes({ 1, 2, 3, 4, 5, 6, 7, 1, 2, 3, 4, 5, 6, 7 })
    << '\n';
  std::cout << "1, 2, .,, 14 with no repetition (should be close to the full size of 14):\n\t" <<
    estimateCompressedBytes({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 })
    << '\n';
  std::cout << "done.\n";
}
