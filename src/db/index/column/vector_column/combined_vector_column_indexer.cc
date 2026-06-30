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
#include "combined_vector_column_indexer.h"
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <unordered_map>

namespace zvec {

CombinedVectorColumnIndexer::CombinedVectorColumnIndexer(
    const std::vector<VectorColumnIndexer::Ptr> &indexers,
    const std::vector<VectorColumnIndexer::Ptr> &normal_indexers,
    const FieldSchema &field_schema, const SegmentMeta &segment_meta,
    std::vector<BlockMeta> blocks, MetricType metric_type, bool is_quantized)
    : field_schema_(field_schema),
      indexers_(std::move(indexers)),
      normal_indexers_(std::move(normal_indexers)),
      blocks_(std::move(blocks)),
      metric_type_(metric_type),
      is_quantized_(is_quantized) {
  if (segment_meta.has_writing_forward_block()) {
    if (is_quantized_) {
      BlockMeta quant_block = segment_meta.writing_forward_block().value();
      quant_block.set_type(BlockType::VECTOR_INDEX_QUANTIZE);
      blocks_.push_back(std::move(quant_block));
    } else {
      BlockMeta block = segment_meta.writing_forward_block().value();
      block.set_type(BlockType::VECTOR_INDEX);
      blocks_.push_back(std::move(block));
    }
  }

  int block_offset = 0;
  for (size_t i = 0; i < indexers_.size(); ++i) {
    auto &block_meta = blocks_[i];
    block_offsets_.push_back(block_offset);
    block_offset += block_meta.doc_count_;
  }

  min_doc_id_ = segment_meta.min_doc_id();
}

Result<IndexResults::Ptr> CombinedVectorColumnIndexer::Search(
    const vector_column_params::VectorData &vector_data,
    const vector_column_params::QueryParams &query_params) {
  core::IndexDocumentList doc_list;
  std::vector<std::string> reverted_vector_list;
  std::vector<std::string> reverted_sparse_values_list;

  // For merging group_by results across blocks
  core::IndexGroupDocumentList merged_group_docs;
  std::unordered_map<std::string, size_t> group_merge_map;
  std::vector<std::vector<std::string>> merged_reverted_vector_list;
  std::vector<std::vector<std::string>> merged_reverted_sparse_values_list;

  auto append_group_values =
      [](std::vector<std::string> &merged_values,
         std::vector<std::vector<std::string>> &sub_values, size_t group_idx) {
        if (sub_values.empty() || group_idx >= sub_values.size()) {
          return;
        }
        auto &values = sub_values[group_idx];
        merged_values.insert(merged_values.end(),
                             std::make_move_iterator(values.begin()),
                             std::make_move_iterator(values.end()));
      };

  // query_params.bf_pks is segment level, here we need to convert it to block
  // level
  std::vector<std::vector<uint64_t>> block_bf_pks(indexers_.size());

  if (!query_params.bf_pks.empty()) {
    // dispatcher pks to corresponding block_bf_pks
    for (auto &pk : query_params.bf_pks[0]) {
      for (size_t i = 0; i < block_offsets_.size(); ++i) {
        if (pk >= block_offsets_[i] &&
            pk < block_offsets_[i] + blocks_[i].doc_count_) {
          block_bf_pks[i].push_back(
              static_cast<uint64_t>(pk - block_offsets_[i]));
          break;
        }
      }
    }
  }

  auto q_params = query_params.query_params;
  for (size_t i = 0; i < indexers_.size(); ++i) {
    if (!query_params.bf_pks.empty() && block_bf_pks[i].empty()) {
      LOG_DEBUG(
          "query_params has bf_pks, but block_bf_pks[%zu] is empty, just skip "
          "this indexer",
          i);
      continue;
    }
    zvec::Result<zvec::IndexResults::Ptr> result{nullptr};
    float scale_factor{};
    bool need_refine{false};
    if (q_params && q_params->is_using_refiner()) {
      if (normal_indexers_.size() != indexers_.size()) {
        return tl::make_unexpected(Status::InvalidArgument(
            "normal indexers size[", normal_indexers_.size(),
            "] not match indexers size[", indexers_.size(), "]"));
      }
      // query_params of HNSW doesn't have scale_factor
      if (q_params->type() == IndexType::FLAT) {
        scale_factor = std::dynamic_pointer_cast<FlatQueryParams>(q_params)
                           ->scale_factor();
      } else if (q_params->type() == IndexType::IVF) {
        scale_factor =
            std::dynamic_pointer_cast<IVFQueryParams>(q_params)->scale_factor();
      }
      need_refine = true;
    }

    const IndexFilter *filter{nullptr};
    auto per_block_filter =
        BlockOffsetFilter{query_params.filter, block_offsets_[i]};
    if (query_params.filter) {
      if (block_offsets_[i] > 0) {
        filter = &per_block_filter;
      } else {
        filter = query_params.filter;
      }
    }

    std::unique_ptr<vector_column_params::GroupByParams> group_by;
    if (query_params.group_by) {
      auto group_by_func = query_params.group_by->group_by;
      auto block_offset = block_offsets_[i];
      group_by = std::make_unique<vector_column_params::GroupByParams>(
          query_params.group_by->group_topk, query_params.group_by->group_count,
          [group_by_func = std::move(group_by_func),
           block_offset](uint64_t block_doc_id) {
            return group_by_func(block_doc_id + block_offset);
          });
    }

    vector_column_params::QueryParams modified_query_params{
        query_params.data_type,
        query_params.dimension,
        query_params.topk,
        filter,
        query_params.fetch_vector,
        query_params.query_params,
        std::move(group_by),
        {},
        need_refine ? std::shared_ptr<vector_column_params::RefinerParam>(
                          new vector_column_params::RefinerParam{
                              scale_factor, normal_indexers_[i]})
                    : nullptr,
        query_params.extra_params};

    if (!query_params.bf_pks.empty()) {
      modified_query_params.bf_pks.emplace_back(block_bf_pks[i]);
    }

    result = indexers_[i]->Search(vector_data, modified_query_params);
    if (!result) {
      return tl::make_unexpected(result.error());
    }

    auto index_results = result.value();

    // Handle group_by results
    GroupVectorIndexResults *group_index_results =
        dynamic_cast<GroupVectorIndexResults *>(index_results.get());
    if (group_index_results != nullptr) {
      // Merge group results from this block
      auto &sub_groups = group_index_results->groups();
      auto &sub_reverted_vector_list =
          group_index_results->reverted_vector_list();
      auto &sub_reverted_sparse_values_list =
          group_index_results->reverted_sparse_values_list();
      for (size_t group_idx = 0; group_idx < sub_groups.size(); ++group_idx) {
        auto &group = sub_groups[group_idx];
        // Adjust keys in group docs by block offset
        for (auto &doc : *group.mutable_docs()) {
          doc.set_key(block_offsets_[i] + doc.key());
        }
        // Merge into existing group or create new one
        auto it = group_merge_map.find(group.group_id());
        size_t merged_group_idx = 0;
        if (it != group_merge_map.end()) {
          merged_group_idx = it->second;
          auto &existing_docs =
              *merged_group_docs[merged_group_idx].mutable_docs();
          auto &new_docs = *group.mutable_docs();
          existing_docs.insert(existing_docs.end(),
                               std::make_move_iterator(new_docs.begin()),
                               std::make_move_iterator(new_docs.end()));
        } else {
          merged_group_idx = merged_group_docs.size();
          group_merge_map[group.group_id()] = merged_group_idx;
          merged_group_docs.emplace_back(std::move(group));
          merged_reverted_vector_list.emplace_back();
          merged_reverted_sparse_values_list.emplace_back();
        }
        append_group_values(merged_reverted_vector_list[merged_group_idx],
                            sub_reverted_vector_list, group_idx);
        append_group_values(
            merged_reverted_sparse_values_list[merged_group_idx],
            sub_reverted_sparse_values_list, group_idx);
      }
      continue;
    }

    VectorIndexResults *vector_index_results =
        dynamic_cast<VectorIndexResults *>(index_results.get());

    const auto &sub_docs = vector_index_results->docs();
    for (size_t j = 0; j < sub_docs.size(); ++j) {
      auto doc = sub_docs[j];
      doc.set_key(block_offsets_[i] + sub_docs[j].key());
      doc_list.emplace_back(std::move(doc));
    }

    auto &&temp_vector_list = vector_index_results->reverted_vector_list();
    reverted_vector_list.insert(
        reverted_vector_list.end(),
        std::make_move_iterator(temp_vector_list.begin()),
        std::make_move_iterator(temp_vector_list.end()));

    auto &&temp_sparse_list =
        vector_index_results->reverted_sparse_values_list();
    reverted_sparse_values_list.insert(
        reverted_sparse_values_list.end(),
        std::make_move_iterator(temp_sparse_list.begin()),
        std::make_move_iterator(temp_sparse_list.end()));
  }

  // Return merged group_by results if any
  if (!merged_group_docs.empty()) {
    // Sort docs within each group by score and truncate to group_topk
    bool lower_is_better =
        (metric_type_ == MetricType::L2 || metric_type_ == MetricType::COSINE);
    uint32_t group_topk =
        query_params.group_by ? query_params.group_by->group_topk : 0;
    for (size_t group_idx = 0; group_idx < merged_group_docs.size();
         ++group_idx) {
      auto &group = merged_group_docs[group_idx];
      auto &docs = *group.mutable_docs();
      auto &reverted_vectors = merged_reverted_vector_list[group_idx];
      auto &reverted_sparse_values =
          merged_reverted_sparse_values_list[group_idx];
      if (!reverted_vectors.empty() && reverted_vectors.size() != docs.size()) {
        reverted_vectors.clear();
      }
      if (!reverted_sparse_values.empty() &&
          reverted_sparse_values.size() != docs.size()) {
        reverted_sparse_values.clear();
      }

      std::vector<size_t> indices(docs.size());
      std::iota(indices.begin(), indices.end(), 0);
      std::sort(indices.begin(), indices.end(),
                [lower_is_better, &docs](size_t lhs, size_t rhs) {
                  return lower_is_better ? docs[lhs].score() < docs[rhs].score()
                                         : docs[lhs].score() > docs[rhs].score();
                });

      core::IndexDocumentList sorted_docs(docs.size());
      std::vector<std::string> sorted_reverted_vectors;
      std::vector<std::string> sorted_reverted_sparse_values;
      if (!reverted_vectors.empty()) {
        sorted_reverted_vectors.resize(reverted_vectors.size());
      }
      if (!reverted_sparse_values.empty()) {
        sorted_reverted_sparse_values.resize(reverted_sparse_values.size());
      }
      for (size_t i = 0; i < indices.size(); ++i) {
        sorted_docs[i] = std::move(docs[indices[i]]);
        if (!reverted_vectors.empty()) {
          sorted_reverted_vectors[i] =
              std::move(reverted_vectors[indices[i]]);
        }
        if (!reverted_sparse_values.empty()) {
          sorted_reverted_sparse_values[i] =
              std::move(reverted_sparse_values[indices[i]]);
        }
      }
      docs = std::move(sorted_docs);
      if (!sorted_reverted_vectors.empty()) {
        reverted_vectors = std::move(sorted_reverted_vectors);
      }
      if (!sorted_reverted_sparse_values.empty()) {
        reverted_sparse_values = std::move(sorted_reverted_sparse_values);
      }
      if (group_topk > 0 && docs.size() > group_topk) {
        docs.resize(group_topk);
        if (!reverted_vectors.empty()) {
          reverted_vectors.resize(group_topk);
        }
        if (!reverted_sparse_values.empty()) {
          reverted_sparse_values.resize(group_topk);
        }
      }
    }

    std::vector<size_t> group_indices(merged_group_docs.size());
    std::iota(group_indices.begin(), group_indices.end(), 0);
    std::sort(group_indices.begin(), group_indices.end(),
              [lower_is_better, &merged_group_docs](size_t lhs, size_t rhs) {
                const auto &lhs_docs = merged_group_docs[lhs].docs();
                const auto &rhs_docs = merged_group_docs[rhs].docs();
                if (lhs_docs.empty() || rhs_docs.empty()) {
                  return !lhs_docs.empty() && rhs_docs.empty();
                }
                if (lhs_docs[0].score() == rhs_docs[0].score()) {
                  return merged_group_docs[lhs].group_id() <
                         merged_group_docs[rhs].group_id();
                }
                return lower_is_better
                           ? lhs_docs[0].score() < rhs_docs[0].score()
                           : lhs_docs[0].score() > rhs_docs[0].score();
              });
    core::IndexGroupDocumentList sorted_group_docs(merged_group_docs.size());
    std::vector<std::vector<std::string>> sorted_reverted_vector_list(
        merged_reverted_vector_list.size());
    std::vector<std::vector<std::string>> sorted_reverted_sparse_values_list(
        merged_reverted_sparse_values_list.size());
    for (size_t i = 0; i < group_indices.size(); ++i) {
      sorted_group_docs[i] = std::move(merged_group_docs[group_indices[i]]);
      sorted_reverted_vector_list[i] =
          std::move(merged_reverted_vector_list[group_indices[i]]);
      sorted_reverted_sparse_values_list[i] =
          std::move(merged_reverted_sparse_values_list[group_indices[i]]);
    }
    merged_group_docs = std::move(sorted_group_docs);
    merged_reverted_vector_list = std::move(sorted_reverted_vector_list);
    merged_reverted_sparse_values_list =
        std::move(sorted_reverted_sparse_values_list);

    // Truncate to group_count
    uint32_t group_count =
        query_params.group_by ? query_params.group_by->group_count : 0;
    if (group_count > 0 && merged_group_docs.size() > group_count) {
      merged_group_docs.resize(group_count);
      merged_reverted_vector_list.resize(group_count);
      merged_reverted_sparse_values_list.resize(group_count);
    }
    auto has_reverted_values =
        [](const std::vector<std::vector<std::string>> &values) {
          return std::any_of(values.begin(), values.end(),
                             [](const auto &group_values) {
                               return !group_values.empty();
                             });
        };
    if (!has_reverted_values(merged_reverted_vector_list)) {
      merged_reverted_vector_list.clear();
    }
    if (!has_reverted_values(merged_reverted_sparse_values_list)) {
      merged_reverted_sparse_values_list.clear();
    }
    return std::make_unique<GroupVectorIndexResults>(
        std::move(merged_group_docs), std::move(merged_reverted_vector_list),
        std::move(merged_reverted_sparse_values_list));
  }

  if (doc_list.empty()) {
    // return empty result
    return std::make_unique<VectorIndexResults>(
        field_schema_.is_sparse_vector(), std::move(doc_list),
        std::move(reverted_vector_list),
        std::move(reverted_sparse_values_list));
  }

  std::vector<size_t> indices(doc_list.size());
  std::iota(indices.begin(), indices.end(), 0);

  std::sort(indices.begin(), indices.end(),
            [this, &doc_list](size_t lhs, size_t rhs) {
              const auto &lhs_doc = doc_list[lhs];
              const auto &rhs_doc = doc_list[rhs];

              if (this->metric_type_ == MetricType::L2) {
                return lhs_doc.score() < rhs_doc.score();
              } else if (this->metric_type_ == MetricType::IP) {
                return lhs_doc.score() > rhs_doc.score();
              } else if (this->metric_type_ == MetricType::COSINE) {
                return lhs_doc.score() < rhs_doc.score();
              } else {
                // default
                return lhs_doc.score() < rhs_doc.score();
              }
            });

  // doc_list
  std::vector<core::IndexDocument> sorted_doc_list(doc_list.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    sorted_doc_list[i] = std::move(doc_list[indices[i]]);
  }
  doc_list = std::move(sorted_doc_list);

  // reverted_vector_list
  if (!reverted_vector_list.empty()) {
    std::vector<std::string> sorted_reverted_vector_list(
        reverted_vector_list.size());
    for (size_t i = 0; i < indices.size(); ++i) {
      if (indices[i] < reverted_vector_list.size()) {
        sorted_reverted_vector_list[i] =
            std::move(reverted_vector_list[indices[i]]);
      }
    }
    reverted_vector_list = std::move(sorted_reverted_vector_list);
  }

  // reverted_sparse_values_list
  if (!reverted_sparse_values_list.empty()) {
    std::vector<std::string> sorted_reverted_sparse_vector_list(
        reverted_sparse_values_list.size());
    for (size_t i = 0; i < indices.size(); ++i) {
      if (indices[i] < reverted_sparse_values_list.size()) {
        sorted_reverted_sparse_vector_list[i] =
            std::move(reverted_sparse_values_list[indices[i]]);
      }
    }
    reverted_sparse_values_list = std::move(sorted_reverted_sparse_vector_list);
  }

  // truncate to topk
  if (doc_list.size() > query_params.topk) doc_list.resize(query_params.topk);
  if (reverted_vector_list.size() > query_params.topk)
    reverted_vector_list.resize(query_params.topk);
  if (reverted_sparse_values_list.size() > query_params.topk)
    reverted_sparse_values_list.resize(query_params.topk);

  return std::make_unique<VectorIndexResults>(
      field_schema_.is_sparse_vector(), std::move(doc_list),
      std::move(reverted_vector_list), std::move(reverted_sparse_values_list));
}

Result<vector_column_params::VectorDataBuffer>
CombinedVectorColumnIndexer::Fetch(uint32_t segment_doc_id) const {
  int32_t target_block_doc_id = -1;
  size_t target_block_idx = 0;

  uint32_t block_offset = 0;
  for (size_t i = 0; i < blocks_.size(); ++i) {
    auto &block_meta = blocks_[i];
    if (block_offset <= segment_doc_id &&
        segment_doc_id < block_offset + block_meta.doc_count_) {
      target_block_doc_id = segment_doc_id - block_offset;
      target_block_idx = i;
      break;
    }
    block_offset += block_meta.doc_count_;
  }

  if (target_block_doc_id == -1) {
    LOG_ERROR("Can't find block for doc_id[%u]", segment_doc_id);
    return tl::make_unexpected(
        Status::NotFound("Can't find block for doc_id:", segment_doc_id));
  }

  auto indexer = indexers_[target_block_idx];
  return indexer->Fetch(target_block_doc_id);
}

}  // namespace zvec
