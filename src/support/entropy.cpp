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

#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>

#include "entropy.h"

namespace wasm {

namespace Entropy {

// Given a vector of frequencies of items, compute their entropy. That is, if
// given [50, 50] then each index has equal probability, so this is like a fair
// coin, and the entropy is 1. The result is measured in bits.
static double computeEntropy(const std::vector<size_t>& freqs) {
  double totalFreq = 0;
  for (auto freq : freqs) {
    totalFreq += freq;
  }
  if (totalFreq == 0) {
    return 0;
  }

  double mean = 0;
  for (auto freq : freqs) {
    auto probability = double(freq) / totalFreq;

    // A probability close to 0 does not contribute to the mean.
    if (probability >= 0.001) {
      mean += -double(probability) * log2(probability);
    }
  }

  // We have computed the entropy, which is the mean bits per item. To get
  // the total number of bits, multiply by the total frequency.
  return mean * totalFreq;
}

static double estimateCompressedRatioInternal(const std::vector<uint8_t>& data) {
//use more than pairs?

std::cout << "data size " << data.size() << '\n';
  // As we will look at pairs, we need at least two items.
  //
  // Note that real compression will have a header and other overhead, so there
  // is a minimum size (128 bytes in brotli, for example) that we should
  // probably ignore anything under, but leave that for the caller.
  if (data.size() < 2) {
    return 1.0;
  }

  // First, compute the entropy of single byres.
  std::vector<size_t> singleCounts(1 << 8, 0);
  for (auto single : data) {
    singleCounts[single]++;
  }
  auto singleEntropy = computeEntropy(singleCounts);
std::cout << "  single entropy: " << singleEntropy << '\n';

  // Second, compute the entropy of pairs of bytes.
  std::vector<size_t> pairCounts(1 << 24, 0);
  for (size_t i = 0; i < data.size() - 2; i++) {
    auto pair = uint16_t(data[i]) | (uint16_t(data[i + 1]) << 8) | (uint16_t(data[i + 2]) << 16);
    pairCounts[pair]++;
  }
  auto pairEntropy = computeEntropy(pairCounts);
std::cout << "  pair entropy: " << pairEntropy << '\n';

  // We estimate the compressed ratio using the product of the normalized single
  // and pair entropies. The rationale is that a compression scheme can use either
  // patterns of size 1 or above, and it chooses the better as it sees, so we
  // do not want a simple mean: if one of the normalized entropies is very near 0, then our
  // final estimate of the relevant entropy should also be zero. Likewise, if
  // one of the normalized entropies is very near 1, then it should not affect
  // the result at all: there is no information there, but hopefully there is in
  // the other measurement.
  //
  // To keep the product itself normalized in terms of the input values, we
  // use the geometric mean.
  //
  // This does not take into account larger structure than pairs. TODO
  //
  // This does not take into account that when using pairs we replace type
  // bytes each time, that is, it is twice as efficient as single codes. TODO
  auto totalBits = double(data.size()) * 8;
  auto normalizedSingleEntropy = singleEntropy / totalBits;
  auto normalizedPairEntropy = pairEntropy / totalBits;
  auto combined = normalizedSingleEntropy;// pow(normalizedSingleEntropy, 0.333) * pow(normalizedPairEntropy, 0.666);
std::cout << "  final: " << normalizedSingleEntropy << " & " << normalizedPairEntropy << " => " << combined << '\n';
  return combined;
}

double estimateCompressedRatio(const std::vector<uint8_t>& data) {
  // brotli will actally pick the optimal window out of 1K-16MB. gzip uses 32K.
  // Use 64k which represents gzip + overlaps.
  size_t ChunkSize = 64 * 1024;
  size_t start = 0;
  std::vector<uint8_t> temp;
  size_t chunks = 0;
  double ratios = 0;
  while (start < data.size()) {
    auto end = std::min(start + ChunkSize, data.size());
    temp.resize(end - start);
    std::copy(data.begin() + start, data.begin() + end, temp.begin());
    auto ratio = estimateCompressedRatioInternal(temp);
    ratios += ratio;
    chunks++;
    start += ChunkSize / 2; // overlap like the window slides
  }
std::cout << "final final: " << (ratios / double(chunks)) << '\n';
  return ratios / double(chunks);
}

} // namespace Entropy

} // namespace wasm
