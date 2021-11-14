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

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <unordered_map>

#include "entropy.h"
#include "mixed_arena.h"

namespace wasm {

namespace Entropy {

// Given a vector of frequencies of items, compute their entropy. That is, if
// given [50, 50] then each index has equal probability, so this is like a fair
// coin, and the entropy is 1. The result is measured in bits.
#if 0
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
#endif

static double estimateCompressedBytesInternal(const std::vector<uint8_t>& data) {
  if (data.size() <= 1) {
    return 1.0;
  }

  // Allocate everything in an arena for speed.
  MixedArena arena;

  double totalBits = 0;

  // brotli will actally pick the optimal window out of 1K-16MB. gzip uses 32K.
  // Use 64k which represents gzip++;
  size_t WindowSize = 64 * 1024;

  // Deflate/gzip have this as the maximum pattern size. Brotli can have up to
  // 16MB.
  size_t MaxPatternSize = 258;

  // Tracks the frequency of each byte we emitted, of the bytes we emit when we
  // fail to find a pattern. Start with equal probability of all bytes.
  // TODO: we need to clear out old bytes (older than the window) somehow -
  //       track them, or randomly?
  std::vector<size_t> byteFreqs(1 << 8, 1);
  size_t totalByteFreqs = 256;

  // We track the patterns seen so far using a trie.
  struct Node {
    // How many times the pattern ending in this node has been seen.
    size_t num; // FIXME do we need this?

    // The index in |data| of the last time we saw this pattern. This marks the
    // first byte in that appearance.
    size_t lastStart;

    // A map of bytes to another node that continues the pattern with that
    // particular byte;
    std::vector<Node*> children;

    Node() {
      children.resize(256, 0);
    }
    Node(MixedArena& allocator) : Node() {}
  };

  // The |lastStart| of the root is meaningless.
  const size_t MeaninglessIndex = -1;
  Node root;
  root.lastStart = MeaninglessIndex;

  size_t i = 0;
  while (i < data.size()) {
//std::cout << "seek pattern from " << i << '\n';
    // Starting at the i-th byte, see how many bytes forward we can look while
    // still finding something in the trie.
    Node* node = &root;
    size_t j = i;

    // We will note the last time we saw the pattern that we found to be
    // repeating.
    size_t previousLastStart = MeaninglessIndex;
    bool emitByte = true;

    while (1) {
      if (j == data.size()) {
        // Nothing to do, and not even a new byte to emit.
        emitByte = false;
        break;
      }

      if (j - i == MaxPatternSize) {
        // This pattern is unreasonably-large; stop and emit a new byte without
        // adding a pattern this size.
        break;
      }

      // Add j to the pattern and see if we have seen that as well.
      if (auto* child = node->children[data[j]]) {
        node = child;

        // Note the last start of the parent before we look at the child. If we
        // end up stopping at the child, then the repeating pattern is in the
        // parent, and its |lastStart| is when last we saw the pattern.
        previousLastStart = node->lastStart;
////std::cout << "prev last: " << previousLastStart << '\n';

        // Mark that we have seen this pattern starting at i. We need to do that
        // regardless of our actions later: this is either one we've seen too
        // long ago, and so it counts as new, or it is actually new. Either way,
        // |i| is the lastStart for it.
        node->lastStart = i;
////std::cout << "set node's last to " << i << '\n';
        if (i - node->lastStart >= WindowSize) {
          // This is a too-old pattern. We must emit a byte for it as if it is
          // a new pattern here.
          break;
        }

        // Otherwise, we saw the pattern recently enough, and can proceed.
        j++;
        continue;
      }

      // Otherwise, we failed to find the pattern in the trie: this extra
      // character at data[j] is new. Add a node, emit a byte, and stop.
      auto* newNode = node->children[data[j]] = arena.alloc<Node>();
      newNode->lastStart = i;
////std::cout << "created new node with last of " << i << '\n';
      break;
    }

    if (previousLastStart != MeaninglessIndex) {
      // We proceeded past the root, that is, we actually found at least one
      // repeating byte of some pattern before we stopped to emit a new byte.
      // To represent the size of that pattern in the compressed output,
      // estimate it as if emitting the optimal number of bits for the distance.
assert(i != previousLastStart);
//std::cout << "emit ref to known pattern: " << (i - previousLastStart) << " , of size " << (j - i) << '\n';
      totalBits += log2(i - previousLastStart);
//std::cout << "atotal " << totalBits << '\n';

      // Also we estimate the bits for the size in a similar way.
assert(j != i);
      totalBits += log2(j - i);
//std::cout << "btotal " << totalBits << '\n';
    }

    if (emitByte) {
      auto byte = data[j];
//std::cout << "emit byte " << int(byte) << '\n';
      // To estimate how many bits we need to emit the new byte, use the factor
      // that the byte would have when computing the entropy.
      totalBits += -log2(double(byteFreqs[byte]) / double(totalByteFreqs));
//std::cout << "ctotal " << totalBits << '\n';

      // Update the frequency of the byte we are emitting.
      byteFreqs[byte]++;
      totalByteFreqs++;
    }

    // Continue after this pattern. TODO: fill in lastStarts of subpatterns?
    i = j + 1;
  }

  return totalBits / 8;
}

double estimateCompressedBytes(const std::vector<uint8_t>& data) {
#if 0
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
//std::cout << "final final: " << (ratios / double(chunks)) << '\n';
  return ratios / double(chunks);
#else
  return estimateCompressedBytesInternal(data);
#endif
}

} // namespace Entropy

} // namespace wasm
