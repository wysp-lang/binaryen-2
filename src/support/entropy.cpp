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

#include <math>
#include <numeric>
#include <vector>

#include "entropy.h"

namespace wasm {

namespace Entropy {

// Given a vector of frequencies of items, compute their entropy. That is, if
// given [50, 50] then each index has equal probability, so this is like a fair
// coin, and the entropy is 1. The result is measured in bits.
static double computeEntropy(const std::vector<size_t>& freqs) {
  size_t totalFreq = 0;
  double sum = 0;
  for (auto freq : freqs) {
    sum += -double(freq) * log2(freq);
    totalFreq += freq;
  }
  return sum / double(totalFreq);
}

double estimateCompressedRatio(const std::vector<uint8_t>& data) {
  // Brotli, gzip, etc. do not do well on small sizes anyhow.
  if (data.size() < 128) {
    return 1.0;
  }

  // First, compute the entropy of single byres.
  std::vector<size_t> singleCounts(1 << 8, 0);
  for (auto single : data) {
    singleCounts[single]++;
  }
  auto singleEntropy = computeEntropy(singleCounts);

  // Second, compute the entropy of pairs of bytes.
  std::vector<size_t> pairCounts(1 << 16, 0);
  for (size_t i = 0; i < data.size() - 1; i++) {
    auto pair = (uint16_t(data[i]) << 8) | uint16_t(data[i + 1]);
    pairCounts[pair]++;
  }
  auto pairEntropy = computeEntropy(pairCounts);

  // We estimate the compressed ratio using the product of the normalized single
  // and pair entropies. The rationale is that a compression scheme can use either
  // patterns of size 1 or above, and it chooses the better as it sees, so we
  // do not want a simple mean: if one of the normalized entropies is very near 0, then our
  // final estimate of the relevant entropy should also be zero. Likewise, if
  // one of the normalized entropies is very near 1, then it should not affect
  // the result at all: there is no information there, but hopefully there is in
  // the other measurement.
  //
  // This does not take into account larger structure than pairs. TODO
  //
  // This does not take into account that when using pairs we replace type
  // bytes each time, that is, it is twice as efficient as single codes. TODO
  auto totalBits = double(data.size()) * 8;
  auto normalizedSingleEntropy = singleEntropy / totalBits;
  auto normalizedPairEntropy = pairEntropy / totalBits;
  auto combined = normalizedSingleEntropy * normalizedPairEntropy;
  return combined;
}

} // namespace Entropy

} // namespace wasm
