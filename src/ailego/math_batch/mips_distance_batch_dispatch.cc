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

#include <ailego/internal/cpu_features.h>
#include <ailego/math/mips_euclidean_distance_matrix.h>
#include "mips_distance_batch.h"

namespace zvec::ailego::DistanceBatch {

#if defined(__AVX512F__)
void MipsSquaredEuclideanFp32BatchAVX512(const float **vectors,
                                         const float *query, size_t count,
                                         size_t dimension, float *out);
#endif
#if defined(__AVX2__)
void MipsSquaredEuclideanFp32BatchAVX2(const float **vectors,
                                       const float *query, size_t count,
                                       size_t dimension, float *out);
#endif

void MipsSquaredEuclideanFp32Batch(const float **vectors, const float *query,
                                   size_t count, size_t dimension, float *out) {
  if (count == 0) return;
#if defined(__AVX512F__)
  if (internal::CpuFeatures::static_flags_.AVX512F) {
    return MipsSquaredEuclideanFp32BatchAVX512(vectors, query, count, dimension,
                                               out);
  }
#endif
#if defined(__AVX2__)
  if (internal::CpuFeatures::static_flags_.AVX2 &&
      internal::CpuFeatures::static_flags_.FMA) {
    return MipsSquaredEuclideanFp32BatchAVX2(vectors, query, count, dimension,
                                             out);
  }
#endif
  for (size_t i = 0; i < count; ++i) {
    MipsSquaredEuclideanDistanceMatrix<float, 1, 1>::Compute(
        vectors[i], query, dimension, 0.0f, out + i);
  }
}

}  // namespace zvec::ailego::DistanceBatch
