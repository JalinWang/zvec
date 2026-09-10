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

#include <ailego/math/matrix_utility.i>
#include <ailego/math/mips_euclidean_distance_matrix.h>
#include <ailego/math/norm2_matrix.h>
#include "mips_distance_batch.h"

namespace zvec::ailego::DistanceBatch {
#if defined(__AVX2__)
namespace {

template <size_t Batch>
void Compute(const float **vectors, const float *query, size_t dimension,
             float query_norm, float *out) {
  __m256 products[Batch], norms[Batch];
  for (size_t i = 0; i < Batch; ++i) {
    products[i] = norms[i] = _mm256_setzero_ps();
  }
  size_t d = 0;
  for (; d + 8 <= dimension; d += 8) {
    const __m256 q = _mm256_loadu_ps(query + d);
    for (size_t i = 0; i < Batch; ++i) {
      const __m256 v = _mm256_loadu_ps(vectors[i] + d);
      products[i] = _mm256_fmadd_ps(q, v, products[i]);
      norms[i] = _mm256_fmadd_ps(v, v, norms[i]);
    }
  }
  for (size_t i = 0; i < Batch; ++i) {
    float product = HorizontalAdd_FP32_V256(products[i]);
    float norm = HorizontalAdd_FP32_V256(norms[i]);
    for (size_t tail = d; tail < dimension; ++tail) {
      product += query[tail] * vectors[i][tail];
      norm += vectors[i][tail] * vectors[i][tail];
    }
    out[i] = ComputeSphericalInjection(product, query_norm, norm, 0.0);
  }
}

}  // namespace

void MipsSquaredEuclideanFp32BatchAVX2(const float **vectors,
                                       const float *query, size_t count,
                                       size_t dimension, float *out) {
  float query_norm;
  SquaredNorm2Matrix<float, 1>::Compute(query, dimension, &query_norm);
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    Compute<4>(vectors + i, query, dimension, query_norm, out + i);
  }
  for (; i < count; ++i) {
    Compute<1>(vectors + i, query, dimension, query_norm, out + i);
  }
}
#endif
}  // namespace zvec::ailego::DistanceBatch
