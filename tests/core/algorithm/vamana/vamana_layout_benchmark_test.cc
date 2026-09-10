// Copyright 2025-present the zvec project
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <type_traits>
#include <gtest/gtest.h>
#include "tests/test_util.h"
#include "vamana_algorithm.h"
#include "vamana_streamer.h"

#ifdef _WIN32
#include <windows.h>

namespace zvec::core {
namespace {
using Clock = std::chrono::steady_clock;
double Seconds(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// Experimental FP32 snapshot ONLY: intentionally separate from production
// formats. Preserves IDs, keys, entry point and adjacency without rebuilding.
// Production requires versioned metadata, checksums and all vector APIs.
class MappedSnapshot : public VamanaContiguousStreamerEntity {
 public:
  using VamanaContiguousStreamerEntity::VamanaContiguousStreamerEntity;
  const void *get_vector(node_id_t id) const override {
    return id < graph_capacity_ ? vector_base_ + size_t(id) * vector_stride_
                                : nullptr;
  }
  static bool Dump(const VamanaMmapStreamerEntity &source,
                   const std::string &path) {
    if (source.vector_size() != 4096 || source.extra_values_size() != 0)
      return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    VamanaHeader hd;
    hd.graph.version = 0x504F4301;  // Test snapshot, not a production version.
    hd.graph.doc_count = source.doc_cnt();
    hd.graph.vector_size = source.vector_size();
    hd.graph.node_size = source.node_size();
    hd.graph.max_degree = source.max_degree();
    hd.graph.search_list_size = source.search_list_size();
    hd.graph.max_occlusion_size = source.max_occlusion_size();
    hd.graph.entry_point = source.entry_point();
    hd.graph.alpha = source.alpha();
    out.write(reinterpret_cast<const char *>(&hd), sizeof(hd));
    std::vector<char> zeros(64, 0);
    const size_t padding = (64 - sizeof(hd) % 64) % 64;
    out.write(zeros.data(), padding);
    for (node_id_t i = 0; i < source.doc_cnt(); ++i) {
      out.write(static_cast<const char *>(source.get_vector_ptr(i)),
                source.vector_size());
    }
    std::vector<node_id_t> row(source.max_degree(), 0);
    for (node_id_t i = 0; i < source.doc_cnt(); ++i) {
      const auto key = source.get_key(i);
      const auto neighbors = source.get_neighbors(i);
      uint32_t degree = neighbors.size();
      std::fill(row.begin(), row.end(), 0);
      for (uint32_t j = 0; j < degree; ++j) row[j] = neighbors[j];
      out.write(reinterpret_cast<const char *>(&key), sizeof(key));
      out.write(reinterpret_cast<const char *>(&degree), sizeof(degree));
      out.write(reinterpret_cast<const char *>(row.data()), row.size() * 4);
    }
    out.close();
    if (!out) return false;
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
  }

  bool Load(const std::string &path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < sizeof(VamanaHeader)) {
      CloseHandle(file);
      return false;
    }
    HANDLE mapping =
        CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) return false;
    char *base =
        static_cast<char *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (!base) return false;
    vector_memory_ =
        std::shared_ptr<char>(base, [](char *p) { UnmapViewOfFile(p); });
    graph_memory_ = vector_memory_;
    std::memcpy(mutable_header(), base, sizeof(VamanaHeader));
    if (header().graph.version != 0x504F4301 || vector_size() != 4096 ||
        max_degree() != 64 || entry_point() >= doc_cnt() ||
        uint64_t(size.QuadPart) !=
            ((sizeof(VamanaHeader) + 63) & ~size_t(63)) +
                uint64_t(doc_cnt()) *
                    (4096 + sizeof(key_t) + neighbors_size())) {
      vector_memory_.reset();
      graph_memory_.reset();
      return false;
    }
    vector_stride_ = vector_size();
    vector_base_ = base + ((sizeof(VamanaHeader) + 63) & ~size_t(63));
    graph_base_ = vector_base_ + size_t(doc_cnt()) * vector_stride_;
    graph_stride_ = sizeof(key_t) + neighbors_size();
    graph_capacity_ = doc_cnt();
    set_use_key_info_map(true);
    return true;
  }
};

template <class Entity>
void Search(const std::shared_ptr<Entity> &entity,
            const IndexMetric::Pointer &metric,
            const std::vector<float> &queries,
            std::vector<std::pair<uint64_t, float>> *results, double *seconds) {
  // Instantiate the production template, also for the mapped subclass.
  using AlgorithmEntity =
      std::conditional_t<std::is_same_v<Entity, VamanaMmapStreamerEntity>,
                         VamanaMmapStreamerEntity,
                         VamanaContiguousStreamerEntity>;
  VamanaAlgorithm<AlgorithmEntity> algorithm(*entity);
  VamanaContext context(1024, metric, entity);
  context.set_ef(200);
  context.set_topk(10);
  ASSERT_EQ(0, context.init(VamanaContext::kStreamerContext));
  if (auto qm = metric->query_metric()) {
    context.update_dist_calculator_distance(qm->distance(),
                                            qm->batch_distance());
  }
  results->clear();
  const auto start = Clock::now();
  for (size_t i = 0; i < queries.size(); i += 1024) {
    context.clear();
    context.reset_query(queries.data() + i);
    ASSERT_EQ(0, algorithm.search(&context));
    context.topk_to_result();
    ASSERT_EQ(10U, context.result().size());
    for (const auto &doc : context.result())
      results->emplace_back(doc.key(), doc.score());
  }
  *seconds = Seconds(start);
}
}  // namespace

TEST(VamanaLayoutBenchmark, DISABLED_WindowsFp32_1024) {
  const char *n = std::getenv("VAMANA_BENCH_COUNT");
  const uint32_t count = n ? std::stoul(n) : 20000;
  ASSERT_GT(count, 1000U);
  const std::string path = "vamana_layout_benchmark";
  const bool reuse = std::getenv("VAMANA_BENCH_REUSE") != nullptr;
  if (!reuse) test_util::RemoveTestPath(path);
  IndexMeta meta(IndexMeta::DataType::DT_FP32, 1024);
  meta.set_metric("MipsSquaredEuclidean", 0, ailego::Params());
  auto storage = IndexFactory::CreateStorage("MMapFileStorage");
  ASSERT_EQ(0, storage->init(ailego::Params()));
  ASSERT_EQ(0, storage->open(path + "/stream", !reuse));
  auto builder = IndexFactory::CreateStreamer("VamanaStreamer");
  ailego::Params params;
  params.set(PARAM_VAMANA_STREAMER_USE_ID_MAP, true);
  if (!reuse) {
    ASSERT_EQ(0, builder->init(meta, params));
    ASSERT_EQ(0, builder->open(storage));
  }
  std::mt19937 rng(20260907);
  std::normal_distribution<float> normal;
  std::vector<float> vector(1024), queries(200 * 1024);
  IndexQueryMeta qmeta(IndexMeta::DataType::DT_FP32, 1024);
  IndexContext::Pointer ctx;
  if (!reuse) ctx = builder->create_context();
  auto start = Clock::now();
  for (uint32_t i = 0; i < count; ++i) {
    for (auto &v : vector) v = normal(rng);
    if (!reuse)
      ASSERT_EQ(0,
                builder->add_impl(1000000ULL + i, vector.data(), qmeta, ctx));
  }
  if (!reuse) {
    ASSERT_EQ(0, builder->flush(0));
    printf("VAMANA_BUILD count=%u seconds=%.6f\n", count, Seconds(start));
  }
  ctx.reset();
  if (!reuse) ASSERT_EQ(0, builder->close());
  builder.reset();
  for (auto &v : queries) v = normal(rng);
  IndexStreamer::Stats stats;
  auto source = std::make_shared<VamanaMmapStreamerEntity>(stats);
  auto contiguous = std::make_shared<VamanaContiguousStreamerEntity>(stats);
  for (auto entity : {static_cast<VamanaStreamerEntity *>(source.get()),
                      static_cast<VamanaStreamerEntity *>(contiguous.get())}) {
    entity->set_vector_size(4096);
    entity->set_max_degree(64);
    entity->set_search_list_size(100);
    entity->set_max_occlusion_size(750);
    entity->set_use_key_info_map(true);
    ASSERT_EQ(0, entity->init(count));
    ASSERT_EQ(0, entity->open(storage, 0, false));
    ASSERT_EQ(count, entity->doc_cnt());
  }
  start = Clock::now();
  ASSERT_EQ(0, contiguous->build_contiguous_memory());
  printf("VAMANA_CONVERT seconds=%.6f\n", Seconds(start));
  start = Clock::now();
  ASSERT_TRUE(MappedSnapshot::Dump(*source, path + "/snapshot"));
  printf("VAMANA_DUMP seconds=%.6f\n", Seconds(start));
  auto mapped = std::make_shared<MappedSnapshot>(stats);
  start = Clock::now();
  ASSERT_TRUE(mapped->Load(path + "/snapshot"));
  printf("VAMANA_MAP seconds=%.6f\n", Seconds(start));
  // Validate every record and edge, not just search output.
  for (uint32_t i = 0; i < count; ++i) {
    ASSERT_EQ(source->get_key(i), mapped->get_key(i));
    ASSERT_EQ(0, std::memcmp(source->get_vector_ptr(i),
                             mapped->get_vector_ptr(i), 4096));
    auto a = source->get_neighbors(i), b = mapped->get_neighbors(i);
    ASSERT_EQ(a.size(), b.size());
    for (uint32_t j = 0; j < a.size(); ++j) ASSERT_EQ(a[j], b[j]);
  }
  auto metric = IndexFactory::CreateMetric(meta.metric_name());
  printf("VAMANA_RECORDS_VERIFIED count=%u\n", count);
  ASSERT_EQ(0, metric->init(meta, meta.metric_params()));
  std::vector<std::pair<uint64_t, float>> reference, actual;
  double elapsed;
  Search(source, metric, queries, &reference, &elapsed);
  // Warm each layout, then rotate order across five measured rounds.
  Search(contiguous, metric, queries, &actual, &elapsed);
  ASSERT_EQ(reference, actual);
  Search(mapped, metric, queries, &actual, &elapsed);
  ASSERT_EQ(reference, actual);
  for (int round = 0; round < 5; ++round) {
    for (int offset = 0; offset < 3; ++offset) {
      const int mode = (round + offset) % 3;
      if (mode == 0) Search(source, metric, queries, &actual, &elapsed);
      if (mode == 1) Search(contiguous, metric, queries, &actual, &elapsed);
      if (mode == 2) Search(mapped, metric, queries, &actual, &elapsed);
      ASSERT_EQ(reference, actual);
      printf("VAMANA_SEARCH round=%d mode=%d query_ms=%.6f\n", round, mode,
             elapsed * 5);
    }
  }
  mapped.reset();
  ASSERT_EQ(0, contiguous->close());
  ASSERT_EQ(0, source->close());
  contiguous.reset();
  source.reset();
  storage.reset();
  if (!std::getenv("VAMANA_BENCH_KEEP")) test_util::RemoveTestPath(path);
}
}  // namespace zvec::core
#endif
