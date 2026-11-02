// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <zvec/db/collection.h>

using namespace zvec;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}
static void check(const Status &s) {
  if (!s.ok()) throw std::runtime_error(s.message());
}

// Each path must be new. No pre-existing user data is removed.
int main(int argc, char **argv) try {
  if (argc < 2)
    throw std::runtime_error(
        "usage: issue743_optimize NEW_ROOT [iterations=5] [indexed=1] "
        "[range_optimization=1]");
  const std::filesystem::path root = std::filesystem::absolute(argv[1]);
  if (!std::filesystem::create_directory(root))
    throw std::runtime_error("root already exists");
  const int iterations = argc > 2 ? std::stoi(argv[2]) : 5;
  const bool indexed = argc < 4 || std::stoi(argv[3]) != 0;
  const bool ranges = argc < 5 || std::stoi(argv[4]) != 0;
  CollectionSchema schema("minimal");
  check(schema.add_field(std::make_shared<FieldSchema>(
      "file_id", DataType::STRING, false,
      indexed ? std::make_shared<InvertIndexParams>(ranges) : nullptr)));
  std::cout << "iteration,concurrency,indexed,ranges,create_ms,upsert_ms,"
               "optimize_ms,fetch_ms,noop_optimize_ms,close_ms,cleanup_ms\n";
  for (int i = 0; i < iterations; ++i)
    for (int j = 0; j < 2; ++j) {
      const int concurrency = (i + j) % 2;
      const auto path =
          root / (std::to_string(i) + "_" + std::to_string(concurrency));
      auto t0 = Clock::now();
      auto result =
          Collection::CreateAndOpen(path.string(), schema, CollectionOptions{});
      auto t1 = Clock::now();
      if (!result) throw std::runtime_error(result.error().message());
      auto collection = std::move(result.value());
      std::vector<Doc> docs(1);
      docs[0].set_pk("one");
      docs[0].set("file_id", std::string("alpha.ts"));
      auto w0 = Clock::now();
      auto written = collection->upsert(docs);
      auto w1 = Clock::now();
      if (!written) throw std::runtime_error(written.error().message());
      if (written->size() != 1) throw std::runtime_error("wrong write count");
      check(written->at(0));
      auto o0 = Clock::now();
      check(collection->optimize(OptimizeOptions{concurrency}));
      auto o1 = Clock::now();
      auto found = collection->fetch({"one"});
      auto f1 = Clock::now();
      if (!found || found->size() != 1 || !found->at("one") ||
          found->at("one")->get<std::string>("file_id") !=
              std::optional<std::string>("alpha.ts"))
        throw std::runtime_error("fetch mismatch");
      auto n0 = Clock::now();
      check(collection->optimize(OptimizeOptions{concurrency}));
      auto n1 = Clock::now();
      check(collection->close());
      collection.reset();
      auto c1 = Clock::now();
      std::filesystem::remove_all(path);
      auto d1 = Clock::now();
      std::cout << std::fixed << std::setprecision(3) << i << ',' << concurrency
                << ',' << indexed << ',' << ranges << ',' << ms(t0, t1) << ','
                << ms(w0, w1) << ',' << ms(o0, o1) << ',' << ms(o1, f1) << ','
                << ms(n0, n1) << ',' << ms(n1, c1) << ',' << ms(c1, d1)
                << std::endl;
    }
  std::filesystem::remove(root);
  return 0;
} catch (const std::exception &e) {
  std::cerr << e.what() << std::endl;
  return 1;
}
