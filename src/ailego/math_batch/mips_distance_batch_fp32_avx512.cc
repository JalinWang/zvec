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
#if defined(__AVX512F__)
namespace {

template <size_t Batch>
void Compute(const float **vectors, const float *query, size_t dimension,
             float query_norm, float *out) {
  __m512 products[Batch], norms[Batch];
  for (size_t i = 0; i < Batch; ++i) {
    products[i] = norms[i] = _mm512_setzero_ps();
  }
  size_t d = 0;
  for (; d + 16 <= dimension; d += 16) {
    const __m512 q = _mm512_loadu_ps(query + d);
    for (size_t i = 0; i < Batch; ++i) {
      const __m512 v = _mm512_loadu_ps(vectors[i] + d);
      products[i] = _mm512_fmadd_ps(q, v, products[i]);
      norms[i] = _mm512_fmadd_ps(v, v, norms[i]);
    }
  }
  if (d < dimension) {
    const __mmask16 mask = static_cast<__mmask16>((1U << (dimension - d)) - 1);
    const __m512 q = _mm512_maskz_loadu_ps(mask, query + d);
    for (size_t i = 0; i < Batch; ++i) {
      const __m512 v = _mm512_maskz_loadu_ps(mask, vectors[i] + d);
      products[i] = _mm512_fmadd_ps(q, v, products[i]);
      norms[i] = _mm512_fmadd_ps(v, v, norms[i]);
    }
  }
  for (size_t i = 0; i < Batch; ++i) {
    out[i] = ComputeSphericalInjection(HorizontalAdd_FP32_V512(products[i]),
                                       query_norm,
                                       HorizontalAdd_FP32_V512(norms[i]), 0.0);
  }
}

}  // namespace

void MipsSquaredEuclideanFp32BatchAVX512(const float **vectors,
                                         const float *query, size_t count,
                                         size_t dimension, float *out) {
  float query_norm;
  SquaredNorm2Matrix<float, 1>::Compute(query, dimension, &query_norm);
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    Compute<8>(vectors + i, query, dimension, query_norm, out + i);
  }
  if (i + 4 <= count) {
    Compute<4>(vectors + i, query, dimension, query_norm, out + i);
    i += 4;
  }
  for (; i < count; ++i) {
    Compute<1>(vectors + i, query, dimension, query_norm, out + i);
  }
}
#endif
}  // namespace zvec::ailego::DistanceBatch
