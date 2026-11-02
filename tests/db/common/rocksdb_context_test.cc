// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0

#include "db/common/rocksdb_context.h"
#include <gtest/gtest.h>
#include <rocksdb/merge_operator.h>
#include "tests/test_util.h"

using namespace zvec;

namespace {
class AppendMerger : public rocksdb::AssociativeMergeOperator {
 public:
  explicit AppendMerger(char separator) : separator_(separator) {}
  const char *Name() const override {
    return "Issue743AppendMerger";
  }
  bool Merge(const rocksdb::Slice &, const rocksdb::Slice *existing,
             const rocksdb::Slice &operand, std::string *result,
             rocksdb::Logger *) const override {
    *result = existing ? existing->ToString() + separator_ : "";
    result->append(operand.data(), operand.size());
    return true;
  }

 private:
  char separator_;
};
}  // namespace

class RocksdbContextTest : public testing::Test {
 protected:
  void SetUp() override {
    test_util::RemoveTestPath(path_);
  }
  void TearDown() override {
    if (ctx_.db_) {
      EXPECT_TRUE(ctx_.close().ok());
    }
    test_util::RemoveTestPath(path_);
  }
  const std::string path_ = "./rocksdb_context_test_dir";
  RocksdbContext ctx_;
};

TEST_F(RocksdbContextTest, BatchPreservesMergeOperatorsAndDefaultFamily) {
  RocksdbContext::Args args{path_,
                            {"alpha", "default", "beta"},
                            std::make_shared<AppendMerger>(','),
                            {{"beta", std::make_shared<AppendMerger>('|')}}};
  ASSERT_TRUE(ctx_.create(args).ok());
  ASSERT_EQ(ctx_.cf_handles_.size(), 3u);
  for (const auto &name : {"alpha", "default", "beta"}) {
    auto cf = ctx_.get_cf(name);
    ASSERT_NE(cf, nullptr);
    ASSERT_TRUE(ctx_.db_->Put(ctx_.write_opts_, cf, "key", "a").ok());
    ASSERT_TRUE(ctx_.db_->Merge(ctx_.write_opts_, cf, "key", "b").ok());
  }
  ASSERT_TRUE(ctx_.close().ok());
  ASSERT_TRUE(ctx_.open(args, false).ok());
  for (const auto &name : {"alpha", "default", "beta"}) {
    std::string value;
    ASSERT_TRUE(
        ctx_.db_->Get(ctx_.read_opts_, ctx_.get_cf(name), "key", &value).ok());
    EXPECT_EQ(value, std::string(name) == "beta" ? "a|b" : "a,b");
  }
}

TEST_F(RocksdbContextTest, FailedBatchReleasesPartialHandles) {
  ASSERT_FALSE(ctx_.create(path_, {"first", "first"}).ok());
  EXPECT_FALSE(ctx_.db_);
  EXPECT_TRUE(ctx_.cf_handles_.empty());
  // The first CF may have been created before the duplicate was rejected.
  ASSERT_TRUE(ctx_.open(path_).ok());
  ASSERT_NE(ctx_.get_cf("first"), nullptr);
  ASSERT_NE(ctx_.get_cf("default"), nullptr);
}

TEST_F(RocksdbContextTest, DefaultOnlyCreatesNoExtraFamilies) {
  ASSERT_TRUE(ctx_.create(path_).ok());
  ASSERT_EQ(ctx_.cf_handles_.size(), 1u);
  EXPECT_EQ(ctx_.cf_handles_[0]->GetName(), "default");
}
