// Copyright 2025-present the zvec project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cmath>
#include <random>
#include <vector>
#include <ailego/internal/cpu_features.h>
#include <ailego/math_batch/mips_distance_batch.h>
#include <ailego/pattern/defer.h>
#include <gtest/gtest.h>

namespace zvec::ailego {

TEST(MipsDistanceBatchTest, DispatchTailsAndUnalignedInputs) {
  const auto saved_flags = internal::CpuFeatures::static_flags_;
  AILEGO_DEFER([&]() { internal::CpuFeatures::static_flags_ = saved_flags; });
  std::mt19937 rng(20260907);
  std::normal_distribution<float> normal;
  // Never enable a feature the machine lacks. Exercise AVX512, AVX2 and
  // pairwise fallback dispatch on capable hosts, with one test thread only.
  for (int mode = 0; mode < 3; ++mode) {
    internal::CpuFeatures::static_flags_ = saved_flags;
    if (mode >= 1) internal::CpuFeatures::static_flags_.AVX512F = false;
    if (mode >= 2) internal::CpuFeatures::static_flags_.AVX2 = false;
    for (size_t dimension : {1U, 3U, 7U, 8U, 15U, 16U, 17U, 1024U, 1025U}) {
      const size_t stride = dimension + 3;
      std::vector<float> data(130 * stride + 1);
      for (auto &value : data) value = normal(rng);
      const float *query = data.data() + 1;
      std::vector<const float *> vectors(128);
      for (size_t i = 0; i < vectors.size(); ++i) {
        vectors[i] = data.data() + 1 + i * stride;
      }
      // Include a zero vector and q itself; these expose norm/denominator
      // mistakes that random unrelated pairs can hide.
      std::fill(data.begin() + 1 + stride,
                data.begin() + 1 + stride + dimension, 0.0f);
      std::vector<float> out(129, -123.0f);
      for (size_t count : {0U, 1U, 3U, 4U, 5U, 7U, 8U, 9U, 17U, 128U}) {
        SCOPED_TRACE("mode=" + std::to_string(mode) +
                     " dim=" + std::to_string(dimension) +
                     " count=" + std::to_string(count));
        std::fill(out.begin(), out.end(), -123.0f);
        DistanceBatch::MipsSquaredEuclideanFp32Batch(
            vectors.data(), query, count, dimension, out.data());
        for (size_t i = 0; i < count; ++i) {
          double product = 0, query_norm = 0, vector_norm = 0;
          for (size_t d = 0; d < dimension; ++d) {
            const double q = query[d], v = vectors[i][d];
            product += q * v;
            query_norm += q * q;
            vector_norm += v * v;
          }
          const double expected =
              2 - 2 * product / std::max(query_norm, vector_norm);
          EXPECT_NEAR(expected, out[i], 2e-5);
        }
        EXPECT_FLOAT_EQ(-123.0f, out[count]);
      }
    }
  }
}

TEST(MipsDistanceBatchTest, EmptyBatchAndZeroNorm) {
  DistanceBatch::MipsSquaredEuclideanFp32Batch(nullptr, nullptr, 0, 0, nullptr);
  const float zero[17]{};
  const float *vectors[] = {zero};
  float out = 0;
  DistanceBatch::MipsSquaredEuclideanFp32Batch(vectors, zero, 1, 17, &out);
  // Match the existing localized spherical metric's 0/0 behavior.
  EXPECT_TRUE(std::isnan(out));
}

}  // namespace zvec::ailego
