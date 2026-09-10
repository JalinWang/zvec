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

#include <array>
#include <cmath>
#include <random>
#include <vector>
#include <gtest/gtest.h>
#include <zvec/core/framework/index_factory.h>
#include "hnsw_context.h"

namespace zvec {
namespace core {
namespace {

constexpr size_t kVectorDataSize = sizeof(float);
constexpr size_t kExtraValuesSize = sizeof(uint32_t);
constexpr size_t kRecordSize = kVectorDataSize + kExtraValuesSize;

class InlineVectorHnswEntity final : public HnswEntity {
 public:
  using Record = std::array<uint8_t, kRecordSize>;

  explicit InlineVectorHnswEntity(std::vector<Record> records)
      : records_(std::move(records)) {
    set_vector_size(kRecordSize);
  }

  key_t get_key(node_id_t id) const override {
    return id < records_.size() ? id : kInvalidKey;
  }

  const void *get_vector(node_id_t id) const override {
    return id < records_.size() ? records_[id].data() : nullptr;
  }

  int get_vector(const node_id_t *ids, uint32_t count,
                 const void **vecs) const override {
    for (uint32_t i = 0; i < count; ++i) {
      vecs[i] = get_vector(ids[i]);
      if (vecs[i] == nullptr) return IndexError_NoExist;
    }
    return 0;
  }

  int get_vector(node_id_t id,
                 IndexStorage::MemoryBlock &block) const override {
    const void *vector = get_vector(id);
    if (vector == nullptr) return IndexError_NoExist;
    block.reset(const_cast<void *>(vector));
    return 0;
  }

  int get_vector(
      const node_id_t *ids, uint32_t count,
      std::vector<IndexStorage::MemoryBlock> &vec_blocks) const override {
    vec_blocks.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      int ret = get_vector(ids[i], vec_blocks[i]);
      if (ret != 0) return ret;
    }
    return 0;
  }

  const Neighbors get_neighbors(level_t /*level*/,
                                node_id_t /*id*/) const override {
    return {};
  }

 private:
  std::vector<Record> records_;
};

IndexMetric::Pointer CreateMetric() {
  IndexMeta meta(IndexMeta::DataType::DT_FP32, 1);
  meta.set_metric("SquaredEuclidean", 0, ailego::Params());
  auto metric = IndexFactory::CreateMetric("SquaredEuclidean");
  EXPECT_NE(nullptr, metric);
  EXPECT_EQ(0, metric->init(meta, meta.metric_params()));
  return metric;
}

TEST(HnswDistCalculatorTest, ForwardsCallerProvidedExtraValues) {
  InlineVectorHnswEntity::Record first{};
  InlineVectorHnswEntity::Record second{};
  first[kVectorDataSize] = 11;
  second[kVectorDataSize] = 22;
  InlineVectorHnswEntity entity({first, second});
  auto metric = CreateMetric();
  ASSERT_NE(nullptr, metric);

  HnswDistCalculator calculator(&entity, metric, 1);
  bool called = false;
  IndexMetric::MatrixBatchDistance batch_distance =
      [&](const void **vectors, const void * /*query*/, size_t count,
          size_t /*dimension*/, float *distances, const void **extra_values) {
        called = true;
        ASSERT_EQ(2U, count);
        ASSERT_NE(nullptr, extra_values);
        EXPECT_EQ(static_cast<const uint8_t *>(vectors[0]) + kVectorDataSize,
                  extra_values[0]);
        EXPECT_EQ(static_cast<const uint8_t *>(vectors[1]) + kVectorDataSize,
                  extra_values[1]);
        distances[0] = *static_cast<const uint8_t *>(extra_values[0]);
        distances[1] = *static_cast<const uint8_t *>(extra_values[1]);
      };
  calculator.update_distance(metric->distance(), batch_distance);

  const void *vectors[] = {entity.get_vector(0), entity.get_vector(1)};
  const void *extra_values[] = {
      static_cast<const uint8_t *>(vectors[0]) + kVectorDataSize,
      static_cast<const uint8_t *>(vectors[1]) + kVectorDataSize};
  float distances[2]{};
  float query = 0;
  calculator.reset_query(&query);
  calculator.batch_dist(vectors, 2, distances, extra_values);

  EXPECT_TRUE(called);
  EXPECT_FLOAT_EQ(11.0f, distances[0]);
  EXPECT_FLOAT_EQ(22.0f, distances[1]);
}

TEST(HnswDistCalculatorTest, ContextPassesInlineTailForSingleNodeBatch) {
  InlineVectorHnswEntity::Record record{};
  record[kVectorDataSize] = 37;
  auto entity = std::make_shared<InlineVectorHnswEntity>(
      std::vector<InlineVectorHnswEntity::Record>{record});
  auto metric = CreateMetric();
  ASSERT_NE(nullptr, metric);

  IndexMetric::MatrixBatchDistance batch_distance =
      [&](const void **vector, const void * /*query*/, size_t count,
          size_t /*dimension*/, float *distance, const void **extra_values) {
        ASSERT_EQ(1U, count);
        ASSERT_NE(nullptr, extra_values);
        EXPECT_EQ(static_cast<const uint8_t *>(vector[0]) + kVectorDataSize,
                  extra_values[0]);
        *distance = *static_cast<const uint8_t *>(extra_values[0]);
      };
  HnswContext context(metric, entity);
  context.bind_dist_space(metric->distance(), batch_distance, nullptr,
                          kRecordSize, kExtraValuesSize);

  float query = 0;
  context.dist_calculator().reset_query(&query);
  EXPECT_FLOAT_EQ(37.0f, context.batch_dist(0));
}

TEST(HnswDistCalculatorTest, PassesNullWhenMetricHasNoExtraValues) {
  InlineVectorHnswEntity entity({{}});
  auto metric = CreateMetric();
  ASSERT_NE(nullptr, metric);

  HnswDistCalculator calculator(&entity, metric, 1);
  IndexMetric::MatrixBatchDistance batch_distance =
      [](const void ** /*vectors*/, const void * /*query*/, size_t count,
         size_t /*dimension*/, float *distances, const void **extra_values) {
        EXPECT_EQ(nullptr, extra_values);
        for (size_t i = 0; i < count; ++i) distances[i] = 0;
      };
  calculator.update_distance(metric->distance(), batch_distance);

  const void *vector = entity.get_vector(0);
  float distance = 1;
  float query = 0;
  calculator.reset_query(&query);
  calculator.batch_dist(&vector, 1, &distance, nullptr);
  EXPECT_FLOAT_EQ(0.0f, distance);
}

TEST(HnswDistCalculatorTest, StoredBatchMatchesPairwiseAndPreservesQuery) {
  std::mt19937 rng(20260907);
  std::normal_distribution<float> normal;
  for (const char *name :
       {"SquaredEuclidean", "Cosine", "MipsSquaredEuclidean"}) {
    for (size_t base_dim : {1U, 7U, 16U, 1024U, 1025U}) {
      // Cosine's stored representation has a trailing norm value.
      size_t dimension = base_dim + (std::string(name) == "Cosine" ? 1 : 0);
      IndexMeta meta(IndexMeta::DataType::DT_FP32, dimension);
      auto metric = IndexFactory::CreateMetric(name);
      ASSERT_NE(nullptr, metric);
      ASSERT_EQ(0, metric->init(meta, ailego::Params()));
      ASSERT_TRUE(metric->stored_batch_distance());
      HnswDistCalculator calculator(nullptr, metric,
                                    static_cast<uint32_t>(dimension));
      std::vector<float> data(102 * dimension);
      for (auto &value : data) value = normal(rng);
      if (std::string(name) == "Cosine") {
        for (size_t row = 0; row < 102; ++row) {
          float *record = data.data() + row * dimension;
          double norm = 0;
          for (size_t d = 0; d < base_dim; ++d) norm += record[d] * record[d];
          for (size_t d = 0; d < base_dim; ++d) record[d] /= std::sqrt(norm);
          record[base_dim] = 1.0f;
        }
      }
      const float *center = data.data() + 100 * dimension;
      const float *query = data.data() + 101 * dimension;
      calculator.reset_query(query);
      std::vector<const void *> vectors(100);
      for (size_t i = 0; i < vectors.size(); ++i) {
        vectors[i] = data.data() + i * dimension;
      }
      std::vector<float> out(100);
      for (size_t count : {0U, 1U, 11U, 12U, 13U, 100U}) {
        SCOPED_TRACE(std::string(name) + " dim=" + std::to_string(dimension) +
                     " count=" + std::to_string(count));
        calculator.clear();
        ASSERT_TRUE(calculator.batch_dist_between_vectors(
            center, vectors.data(), count, out.data()));
        EXPECT_EQ(count, calculator.compare_cnt());
        for (size_t i = 0; i < count; ++i) {
          float expected = metric->distance(center, vectors[i], dimension);
          EXPECT_NEAR(expected, out[i],
                      1e-5f * std::max(1.0f, std::abs(expected)));
        }
        EXPECT_FLOAT_EQ(metric->distance(center, query, dimension),
                        calculator.dist(center));
        EXPECT_EQ(count + 1, calculator.compare_cnt());
        EXPECT_FALSE(calculator.error());
      }
    }
  }
}

TEST(HnswDistCalculatorTest, RebindingClearsStoredBatchAndUsesPairwiseOrder) {
  auto metric = CreateMetric();
  HnswDistCalculator calculator(nullptr, metric, 1);
  bool query_batch_called = false;
  // An asymmetric fallback catches accidental swapping of the two inputs.
  IndexMetric::MatrixDistance pairwise = [](const void *lhs, const void *rhs,
                                            size_t, float *out) {
    *out =
        *static_cast<const float *>(lhs) + 2 * *static_cast<const float *>(rhs);
  };
  IndexMetric::MatrixBatchDistance query_batch =
      [&](const void **, const void *, size_t, size_t, float *, const void **) {
        query_batch_called = true;
      };
  calculator.update_distance(pairwise, query_batch);
  float center = 10, candidate = 3, out = 0;
  const void *vectors[] = {&candidate};
  ASSERT_TRUE(calculator.batch_dist_between_vectors(&center, vectors, 1, &out));
  EXPECT_FLOAT_EQ(16, out);
  EXPECT_EQ(1U, calculator.compare_cnt());
  EXPECT_FALSE(query_batch_called);
  EXPECT_FLOAT_EQ(16, calculator.dist_between_vectors(&center, &candidate));
  EXPECT_EQ(2U, calculator.compare_cnt());
}

TEST(HnswDistCalculatorTest, StoredBatchRejectsMissingVectors) {
  auto metric = CreateMetric();
  HnswDistCalculator calculator(nullptr, metric, 1);
  float center = 1, out = 123;
  const void *vectors[] = {nullptr};
  EXPECT_FALSE(
      calculator.batch_dist_between_vectors(&center, vectors, 1, &out));
  EXPECT_TRUE(calculator.error());
  EXPECT_FLOAT_EQ(123, out);
  calculator.clear();
  EXPECT_TRUE(
      calculator.batch_dist_between_vectors(nullptr, nullptr, 0, nullptr));
  EXPECT_FALSE(calculator.error());
}

TEST(HnswDistCalculatorTest, QuantizedMetricsDoNotOptIntoStoredBatch) {
  for (const char *name :
       {"SquaredEuclidean", "Cosine", "MipsSquaredEuclidean"}) {
    IndexMeta meta(IndexMeta::DataType::DT_FP16, 16);
    auto metric = IndexFactory::CreateMetric(name);
    ASSERT_NE(nullptr, metric);
    ASSERT_EQ(0, metric->init(meta, ailego::Params()));
    EXPECT_FALSE(metric->stored_batch_distance());
  }
}

}  // namespace
}  // namespace core
}  // namespace zvec
