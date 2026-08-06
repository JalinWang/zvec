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

// Regression tests for issue #626: a collection stored under a non-ASCII path
// has to work whatever ANSI code page (ACP) the process runs with.
//
// On Windows a narrow std::string handed to std::filesystem::path is not
// interpreted as UTF-8 but decoded with the process ACP. For the CJK
// double-byte code pages that means one of two things:
//
//   * the UTF-8 bytes happen to form a valid sequence in that code page and
//     silently resolve to a completely different path, or
//   * they do not, and the conversion throws
//     std::system_error(ERROR_NO_UNICODE_TRANSLATION), which pybind11 turns
//     into the RuntimeError from the issue: "No mapping for the Unicode
//     character exists in the target multi-byte code page."
//
// Which of the two happens depends on the byte pattern, not on the language,
// which is why only some non-ASCII paths appeared broken in the report.
//
// The ACP is fixed when the process starts, so each code page gets its own test
// binary with an embedded manifest that pins it (see CMakeLists.txt). One
// machine therefore covers the code pages used in China, Japan, Korea and
// Taiwan without touching the system locale.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <gtest/gtest.h>
#include <zvec/ailego/utility/file_helper.h>
#include "db/common/file_helper.h"
#include "index/utils/utils.h"
#include "zvec/db/collection.h"
#include "zvec/db/options.h"
#include "zvec/db/schema.h"
#include "zvec/db/status.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef DeleteFile
#undef RemoveDirectory
#undef CreateFile
#undef DELETE

#ifndef ZVEC_TEST_EXPECTED_ACP
#error "ZVEC_TEST_EXPECTED_ACP has to be defined by the build"
#endif

using namespace zvec;
using namespace zvec::test;

namespace {

const char *AcpDescription(unsigned int code_page) {
  switch (code_page) {
    case 932:
      return "Japanese, Shift-JIS (double byte)";
    case 936:
      return "Chinese Simplified, GBK (double byte)";
    case 949:
      return "Korean, UHC (double byte)";
    case 950:
      return "Chinese Traditional, Big5 (double byte)";
    case 65001:
      return "Unicode, UTF-8";
    default:
      return "single byte or unknown";
  }
}

struct PathCase {
  const char *label;
  const char *utf8_name;
};

// Directory names as raw UTF-8 bytes so that the encoding this source file
// happens to be stored in cannot influence the test. The last entry is taken
// verbatim from issue #626.
const PathCase kPathCases[] = {
    // 中文测试
    {"zh-Hans", "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95"},
    // テストパス
    {"ja", "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe3\x83\x91\xe3\x82\xb9"},
    // 테스트 경로 (contains a space, which is not a valid trail byte)
    {"ko", "\xed\x85\x8c\xec\x8a\xa4\xed\x8a\xb8 \xea\xb2\xbd\xeb\xa1\x9c"},
    // 繁體中文
    {"zh-Hant", "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87"},
    // di22222ci。。-cmy
    {"issue-626", "di22222ci\xe3\x80\x82\xe3\x80\x82-cmy"},
};

// What building a path out of narrow UTF-8 bytes does under the current ACP.
enum class NarrowConversion {
  kExact,     // decoded to the same path, the ACP is UTF-8 compatible
  kMojibake,  // decoded without an error but to a different path
  kThrows,    // could not be decoded at all
};

NarrowConversion ClassifyNarrowConversion(const std::string &utf8,
                                          std::string *message) {
  const std::filesystem::path expected = ailego::FileHelper::PathFromUtf8(utf8);
  try {
    // Exactly what std::filesystem::exists(const std::string &) used to do in
    // SegmentImpl::recover() and SegmentImpl::open_wal_file().
    const std::filesystem::path narrow(utf8);
    return narrow.native() == expected.native() ? NarrowConversion::kExact
                                                : NarrowConversion::kMojibake;
  } catch (const std::exception &e) {
    if (message != nullptr) {
      *message = e.what();
    }
    return NarrowConversion::kThrows;
  }
}

class Utf8AcpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    acp_ = ::GetACP();
    if (acp_ != ZVEC_TEST_EXPECTED_ACP) {
      GTEST_SKIP() << "process ACP is " << acp_ << " but this binary targets "
                   << ZVEC_TEST_EXPECTED_ACP
                   << "; selecting a code page by locale name needs Windows 11 "
                      "or Server 2022 or newer, so the case is skipped rather "
                      "than silently run against the wrong code page";
    }
  }

  // Keeps the variants from sharing a directory and stays non-ASCII.
  static std::string DirFor(const PathCase &path_case) {
    return "utf8_acp" + std::to_string(ZVEC_TEST_EXPECTED_ACP) + "_" +
           path_case.utf8_name;
  }

  static void Remove(const std::string &dir) {
    ailego::FileHelper::RemovePath(dir.c_str());
  }

  unsigned int acp_ = 0;
};

// Documents how this code page treats UTF-8 bytes, and pins the property the
// fix relies on: the UTF-8 aware conversion is exact no matter what the ACP is.
TEST_F(Utf8AcpTest, PathConversionIsAcpIndependent) {
  std::cout << "ACP " << acp_ << " (" << AcpDescription(acp_) << ")"
            << std::endl;

  int hazardous = 0;
  for (const auto &path_case : kPathCases) {
    const std::string name = DirFor(path_case);
    SCOPED_TRACE(std::string(path_case.label) + " -> " + name);

    const std::filesystem::path converted =
        ailego::FileHelper::PathFromUtf8(name);
    EXPECT_EQ(ailego::FileHelper::PathToUtf8(converted), name);

    std::string message;
    switch (ClassifyNarrowConversion(name, &message)) {
      case NarrowConversion::kExact:
        std::cout << "  [exact   ] " << path_case.label << std::endl;
        break;
      case NarrowConversion::kMojibake:
        ++hazardous;
        std::cout << "  [mojibake] " << path_case.label
                  << ": narrow conversion silently resolves another path"
                  << std::endl;
        break;
      case NarrowConversion::kThrows:
        ++hazardous;
        std::cout << "  [throws  ] " << path_case.label << ": " << message
                  << std::endl;
        break;
    }
  }

  if (acp_ == CP_UTF8) {
    EXPECT_EQ(hazardous, 0)
        << "with a UTF-8 ACP the narrow and the wide conversion have to agree";
  } else {
    EXPECT_GT(hazardous, 0)
        << "none of the cases is affected by this code page, so this binary "
           "cannot catch a regression of issue #626 and needs a better sample";
  }
}

// The workflow from the issue. Before the fix the insert threw under a double
// byte ACP as soon as the WAL path could not be decoded with it.
TEST_F(Utf8AcpTest, CollectionInsertAndReopenUnderNonAsciiPath) {
  constexpr int kDocCount = 10;

  for (const auto &path_case : kPathCases) {
    const std::string dir = DirFor(path_case);
    SCOPED_TRACE(std::string(path_case.label) + " -> " + dir);
    Remove(dir);

    CollectionOptions options;
    options.read_only_ = false;
    options.enable_mmap_ = true;
    auto schema = TestHelper::CreateNormalSchema();

    auto created = Collection::CreateAndOpen(dir, *schema, options);
    ASSERT_TRUE(created.has_value()) << created.error().message();
    auto collection = std::move(created).value();

    // Appending to the WAL is what builds the path that used to break.
    auto status = TestHelper::CollectionInsertDoc(collection, 0, kDocCount);
    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_TRUE(collection->Flush().ok());
    ASSERT_EQ(collection->Stats().value().doc_count, kDocCount);
    collection.reset();

    // Reopening replays the WAL, inserting again reopens it for append.
    auto reopened = Collection::Open(dir, options);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().message();
    auto collection2 = std::move(reopened).value();
    ASSERT_EQ(collection2->Stats().value().doc_count, kDocCount);

    status =
        TestHelper::CollectionInsertDoc(collection2, kDocCount, kDocCount * 2);
    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_TRUE(collection2->Flush().ok());
    ASSERT_EQ(collection2->Stats().value().doc_count, kDocCount * 2);

    // Everything written before and after the reopen has to be readable.
    auto schema_value = collection2->Schema().value();
    for (int i = 0; i < kDocCount * 2; i++) {
      auto expected = TestHelper::CreateDoc(i, schema_value);
      auto fetched = collection2->Fetch({expected.pk()});
      ASSERT_TRUE(fetched.has_value()) << fetched.error().message();
      ASSERT_EQ(fetched.value().count(expected.pk()), 1u)
          << "missing doc " << i;
    }
    collection2.reset();

    Remove(dir);
  }
}

// Guards the helper the fix switched to: it has to answer for the UTF-8 path
// and not for whatever the ACP decodes those bytes into.
TEST_F(Utf8AcpTest, WalPathExistenceCheckIsAcpIndependent) {
  for (const auto &path_case : kPathCases) {
    const std::string dir = DirFor(path_case);
    SCOPED_TRACE(std::string(path_case.label) + " -> " + dir);
    Remove(dir);

    const std::string wal_path = FileHelper::MakeWalPath(dir, 0, 0);
    const std::filesystem::path wide =
        ailego::FileHelper::PathFromUtf8(wal_path);

    // Ground truth is established through the wide API only, so it does not
    // depend on the code being tested.
    std::filesystem::create_directories(wide.parent_path());
    ASSERT_FALSE(std::filesystem::exists(wide));
    EXPECT_FALSE(FileHelper::FileExists(wal_path))
        << "reported as existing before it was created: " << wal_path;

    { std::ofstream ofs(wide, std::ios::binary); }
    ASSERT_TRUE(std::filesystem::exists(wide));
    EXPECT_TRUE(FileHelper::FileExists(wal_path))
        << "reported as missing although it exists: " << wal_path;

    Remove(dir);
  }
}

}  // namespace
