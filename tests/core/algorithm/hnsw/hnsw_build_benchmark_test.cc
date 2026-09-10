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

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <random>
#include <set>
#include <thread>
#include <gtest/gtest.h>
#include "tests/test_util.h"
#include "hnsw_streamer.h"

namespace zvec::core {

// Opt-in benchmark: HNSW_BENCH_COUNT, HNSW_BENCH_THREADS and
// HNSW_BENCH_METRIC (SquaredEuclidean or MipsSquaredEuclidean).
// Measures the insertion stage used by compact, separately from storage flush.
TEST(HnswBuildBenchmark, DISABLED_Fp32_1024) {
  auto number = [](const char *name, uint32_t fallback) {
    const char *value = std::getenv(name);
    return value ? static_cast<uint32_t>(std::stoul(value)) : fallback;
  };
  const uint32_t count = number("HNSW_BENCH_COUNT", 5000);
  const uint32_t threads = number("HNSW_BENCH_THREADS", 1);
  ASSERT_GE(count, 10U);
  ASSERT_GT(threads, 0U);
  constexpr uint32_t dimension = 1024, queries = 100, topk = 10;
  const char *metric_env = std::getenv("HNSW_BENCH_METRIC");
  const std::string metric = metric_env ? metric_env : "MipsSquaredEuclidean";
  const std::string path = "hnsw_build_benchmark/index";
  test_util::RemoveTestPath("hnsw_build_benchmark");

  std::mt19937 rng(20260907);
  std::normal_distribution<float> normal;
  std::vector<float> vectors(static_cast<size_t>(count + queries) * dimension);
  for (auto &value : vectors) value = normal(rng);
  IndexMeta meta(IndexMeta::DataType::DT_FP32, dimension);
  meta.set_metric(metric, 0, ailego::Params());
  auto streamer = IndexFactory::CreateStreamer("HnswStreamer");
  ASSERT_NE(nullptr, streamer);
  ailego::Params params;
  params.set("proxima.hnsw.streamer.max_neighbor_count", 50U);
  params.set("proxima.hnsw.streamer.efconstruction", 500U);
  params.set("proxima.hnsw.streamer.ef", 300U);
  ASSERT_EQ(0, streamer->init(meta, params));
  auto storage = IndexFactory::CreateStorage("MMapFileStorage");
  ASSERT_NE(nullptr, storage);
  ASSERT_EQ(0, storage->init(ailego::Params()));
  ASSERT_EQ(0, storage->open(path, true));
  ASSERT_EQ(0, streamer->open(storage));
  IndexQueryMeta qmeta(IndexMeta::DataType::DT_FP32, dimension);
  std::atomic<uint32_t> next{0};
  std::atomic<int> error{0};
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  for (uint32_t t = 0; t < threads; ++t) {
    workers.emplace_back([&]() {
      auto context = streamer->create_context();
      for (uint32_t id = next.fetch_add(1); id < count;
           id = next.fetch_add(1)) {
        int ret = streamer->add_with_id_impl(
            id, vectors.data() + static_cast<size_t>(id) * dimension, qmeta,
            context);
        if (ret != 0) {
          error.store(ret);
          break;
        }
      }
    });
  }
  for (auto &worker : workers) worker.join();
  const auto built = std::chrono::steady_clock::now();
  ASSERT_EQ(0, error.load());
  ASSERT_EQ(0, streamer->flush(0));
  const auto flushed = std::chrono::steady_clock::now();

  auto context = streamer->create_context();
  auto exact = streamer->create_context();
  context->set_topk(topk);
  exact->set_topk(topk);
  size_t hits = 0;
  double search_ms = 0;
  for (uint32_t i = 0; i < queries; ++i) {
    const float *query =
        vectors.data() + static_cast<size_t>(count + i) * dimension;
    const auto query_start = std::chrono::steady_clock::now();
    ASSERT_EQ(0, streamer->search_impl(query, qmeta, context));
    search_ms += std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - query_start)
                     .count();
    ASSERT_EQ(0, streamer->search_bf_impl(query, qmeta, exact));
    ASSERT_EQ(topk, context->result().size());
    ASSERT_EQ(topk, exact->result().size());
    std::set<uint64_t> truth;
    for (const auto &doc : exact->result()) truth.insert(doc.key());
    for (const auto &doc : context->result()) hits += truth.count(doc.key());
  }
  printf(
      "HNSW_BENCH metric=%s count=%u dim=%u m=50 efc=500 threads=%u "
      "build_s=%.6f flush_s=%.6f recall10=%.6f search_ms=%.6f\n",
      metric.c_str(), count, dimension, threads,
      std::chrono::duration<double>(built - start).count(),
      std::chrono::duration<double>(flushed - built).count(),
      static_cast<double>(hits) / (queries * topk), search_ms / queries);
  context.reset();
  exact.reset();
  ASSERT_EQ(0, streamer->close());
  streamer.reset();
  storage.reset();
  test_util::RemoveTestPath("hnsw_build_benchmark");
}

}  // namespace zvec::core
