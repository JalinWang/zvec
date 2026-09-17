// Copyright 2025-present the centaurdb project
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
// limitations under the License

#include "core/algorithm/hnsw_rabitq/hnsw_rabitq_dist_calculator.h"
#include "zvec/core/framework/index_error.h"

namespace zvec::core {

int HnswRabitqAddDistCalculator::get_vectors(
    const node_id_t *ids, uint32_t count,
    std::vector<IndexStorage::MemoryBlock> &vec_blocks) const {
  std::vector<key_t> keys(count);
  for (uint32_t i = 0; i < count; ++i) {
    keys[i] = entity_->get_key(ids[i]);
    if (keys[i] == kInvalidKey) {
      return IndexError_NoExist;
    }
  }
  return provider_->get_vectors(keys.data(), count, vec_blocks);
}

}  // namespace zvec::core
