// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "db/index/column/inverted_column/inverted_column_indexer.h"
#include "db/index/column/inverted_column/inverted_rocksdb_merger.h"
#include "tests/test_util.h"

using namespace zvec;

class SmallInvertedIndexTest : public testing::Test {
 protected:
  void SetUp() override {
    test_util::RemoveTestPath(path_);
    ASSERT_TRUE(
        ctx_.create(path_, {terms_, ranges_, INVERT_CDF}, merger_).ok());
    indexer_ = InvertedColumnIndexer::CreateAndOpen("small", field_, ctx_);
    ASSERT_TRUE(indexer_);
  }

  void TearDown() override {
    indexer_.reset();
    if (ctx_.db_) {
      EXPECT_TRUE(ctx_.close().ok());
    }
    test_util::RemoveTestPath(path_);
  }

  std::string value(uint32_t id) {
    return std::to_string(100000 + id);
  }

  void verify(uint32_t count, bool all_null = false) {
    const uint32_t pivot = count / 2;
    for (auto op : {CompareOp::EQ, CompareOp::LT, CompareOp::LE, CompareOp::GT,
                    CompareOp::GE}) {
      auto result = indexer_->search(value(pivot), op);
      ASSERT_TRUE(result);
      uint32_t expected_count = 0;
      for (uint32_t id = 0; id < count; ++id) {
        bool match = !all_null && ((op == CompareOp::EQ && id == pivot) ||
                                   (op == CompareOp::LT && id < pivot) ||
                                   (op == CompareOp::LE && id <= pivot) ||
                                   (op == CompareOp::GT && id > pivot) ||
                                   (op == CompareOp::GE && id >= pivot));
        EXPECT_EQ(result->contains(id), match);
        expected_count += match;
      }
      EXPECT_EQ(result->count(), expected_count);
    }
    // CDF statistics must survive even when no range slots are stored.
    uint64_t total = 0, matching = 0;
    ASSERT_TRUE(
        indexer_->evaluate_ratio("999999", CompareOp::LT, &total, &matching)
            .ok());
    EXPECT_EQ(total, all_null ? 0 : count);
    EXPECT_EQ(matching, total);
    auto nulls = indexer_->search_null();
    ASSERT_TRUE(nulls);
    EXPECT_EQ(nulls->count(), all_null ? count : 0);
  }

  void reopen() {
    indexer_.reset();
    ASSERT_TRUE(ctx_.close().ok());
    ASSERT_TRUE(ctx_.open(path_, true, merger_).ok());
    indexer_ =
        InvertedColumnIndexer::CreateAndOpen("small", field_, ctx_, true);
    ASSERT_TRUE(indexer_);
    ASSERT_TRUE(indexer_->is_sealed());
  }

  const std::string path_ = "./inverted_column_indexer_small_dir";
  const std::string terms_ = "file_id" + INVERT_SUFFIX_TERMS;
  const std::string ranges_ = "file_id" + INVERT_SUFFIX_RANGES;
  FieldSchema field_{"file_id", DataType::STRING, true,
                     std::make_shared<InvertIndexParams>()};
  std::shared_ptr<InvertedRocksdbValueMerger> merger_ =
      std::make_shared<InvertedRocksdbValueMerger>();
  RocksdbContext ctx_;
  InvertedColumnIndexer::Ptr indexer_;
};

class RangeSlotBoundaryTest : public SmallInvertedIndexTest,
                              public testing::WithParamInterface<uint32_t> {};

TEST_P(RangeSlotBoundaryTest, SealAndReopen) {
  const auto count = GetParam();
  for (uint32_t id = 0; id < count; ++id) {
    ASSERT_TRUE(indexer_->insert(id, value(id)).ok());
  }
  const auto cf_id = ctx_.get_cf(ranges_)->GetID();
  ASSERT_TRUE(indexer_->seal().ok());
  // A fresh range CF must not be replaced just to seal a segment.
  EXPECT_EQ(ctx_.get_cf(ranges_)->GetID(), cf_id);
  {
    std::unique_ptr<rocksdb::Iterator> it(
        ctx_.db_->NewIterator(ctx_.read_opts_, ctx_.get_cf(ranges_)));
    it->SeekToFirst();
    EXPECT_EQ(it->Valid(), count >= 2000);
    EXPECT_TRUE(it->status().ok());
  }
  ASSERT_NO_FATAL_FAILURE(verify(count));
  ASSERT_NO_FATAL_FAILURE(reopen());
  ASSERT_NO_FATAL_FAILURE(verify(count));
}

INSTANTIATE_TEST_SUITE_P(SmallAndUsefulRanges, RangeSlotBoundaryTest,
                         testing::Values(0u, 1u, 99u, 999u, 1000u, 1999u, 2000u,
                                         2001u));

TEST_F(SmallInvertedIndexTest, AllNullStatisticsSurviveReopen) {
  for (uint32_t id = 0; id < 11; ++id) {
    ASSERT_TRUE(indexer_->insert_null(id).ok());
  }
  const auto cf_id = ctx_.get_cf(ranges_)->GetID();
  ASSERT_TRUE(indexer_->seal().ok());
  EXPECT_EQ(ctx_.get_cf(ranges_)->GetID(), cf_id);
  ASSERT_NO_FATAL_FAILURE(verify(11, true));
  ASSERT_NO_FATAL_FAILURE(reopen());
  ASSERT_NO_FATAL_FAILURE(verify(11, true));
}

TEST_F(SmallInvertedIndexTest, ClearsPartialRangeStatisticsBeforeSealing) {
  ASSERT_TRUE(indexer_->insert(0, value(0)).ok());
  auto cf = ctx_.get_cf(ranges_);
  const auto old_id = cf->GetID();
  // Simulate data persisted by an interrupted, unsealed range-index build.
  ASSERT_TRUE(ctx_.db_->Put(ctx_.write_opts_, cf, "partial", "stale").ok());
  ASSERT_TRUE(ctx_.flush().ok());
  indexer_.reset();
  ASSERT_TRUE(ctx_.close().ok());
  ASSERT_TRUE(ctx_.open(path_, false, merger_).ok());
  indexer_ = InvertedColumnIndexer::CreateAndOpen("small", field_, ctx_);
  ASSERT_TRUE(indexer_);
  ASSERT_TRUE(indexer_->seal().ok());
  EXPECT_NE(ctx_.get_cf(ranges_)->GetID(), old_id);
  std::string stale;
  EXPECT_TRUE(
      ctx_.db_->Get(ctx_.read_opts_, ctx_.get_cf(ranges_), "partial", &stale)
          .IsNotFound());
  ASSERT_NO_FATAL_FAILURE(verify(1));
  ASSERT_NO_FATAL_FAILURE(reopen());
  ASSERT_NO_FATAL_FAILURE(verify(1));
}
