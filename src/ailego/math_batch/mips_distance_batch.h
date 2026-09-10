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
#pragma once

#include <cstddef>

namespace zvec::ailego::DistanceBatch {

// Localized spherical injection: 2 - 2 * dot(q, v) / max(norm2(q), norm2(v)).
// Both sides are unmodified FP32 vectors; no query preprocessing is required.
void MipsSquaredEuclideanFp32Batch(const float **vectors, const float *query,
                                   size_t count, size_t dimension, float *out);

}  // namespace zvec::ailego::DistanceBatch
