// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/lists.h"

#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace domicile::content_blocking {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

constexpr char kAddress[] = "https://lists.example/easylist.txt";
constexpr char kNotUtf8[] = "||ads.example^\n\xff\xfe\n";

class ContentBlockingListsTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }

  base::FilePath Cache() const { return dir_.GetPath().AppendASCII("cache"); }

  std::string Write(std::string_view name, std::string_view text) {
    const base::FilePath path = dir_.GetPath().AppendASCII(name);
    EXPECT_TRUE(base::WriteFile(path, text));
    return path.AsUTF8Unsafe();
  }

  base::ScopedTempDir dir_;
};

TEST_F(ContentBlockingListsTest, APathIsReadAsItIs) {
  const std::string path = Write("own.txt", "||ads.example^\n");
  const ReadSources read = ReadFrom(Cache(), {path});
  EXPECT_EQ(read.texts.at(path).text, "||ads.example^\n");
  EXPECT_THAT(read.errors, IsEmpty());
}

TEST_F(ContentBlockingListsTest, APathThatCannotBeReadIsAnError) {
  const std::string path = dir_.GetPath().AppendASCII("gone.txt").AsUTF8Unsafe();
  const ReadSources read = ReadFrom(Cache(), {path});
  EXPECT_THAT(read.texts, IsEmpty());
  EXPECT_THAT(read.errors, ElementsAre(path + ", which cannot be read"));
}

TEST_F(ContentBlockingListsTest, AListThatIsNotUtf8IsAnError) {
  const std::string path = Write("binary.txt", kNotUtf8);
  const ReadSources read = ReadFrom(Cache(), {path});
  EXPECT_THAT(read.texts, IsEmpty());
  EXPECT_THAT(read.errors, ElementsAre(path + ", which is not UTF-8"));
}

TEST_F(ContentBlockingListsTest, AnAddressIsItsCachedCopy) {
  EXPECT_THAT(ReadFrom(Cache(), {kAddress}).texts, IsEmpty());
  EXPECT_THAT(ReadFrom(Cache(), {kAddress}).errors, IsEmpty());

  ASSERT_TRUE(KeepFetched(Cache(), kAddress, "||ads.example^\n"));
  EXPECT_EQ(ReadFrom(Cache(), {kAddress}).texts.at(kAddress).text,
            "||ads.example^\n");
}

TEST_F(ContentBlockingListsTest, AListLivesAsLongAsItsExpiresAllows) {
  EXPECT_EQ(Lifetime("! Expires: 4 days\n"), base::Days(4));
  EXPECT_EQ(Lifetime("||ads.example^\n"), base::Days(1));
  EXPECT_EQ(Lifetime("! Expires: 30 days\n"), base::Days(1));
}

TEST_F(ContentBlockingListsTest, TheListsEngineIsKeptForTheSameTexts) {
  const Built first = BuildBlocker(Cache(), {"||ads.example^\n"}, {}, "");
  EXPECT_TRUE(first.kept);
  std::string key;
  ASSERT_TRUE(base::ReadFileToString(Cache().AppendASCII("lists.key"), &key));

  BuildBlocker(Cache(), {"||ads.example^\n"}, {}, "");
  std::string same;
  ASSERT_TRUE(base::ReadFileToString(Cache().AppendASCII("lists.key"), &same));
  EXPECT_EQ(same, key);

  BuildBlocker(Cache(), {"||tracker.example^\n"}, {}, "");
  std::string other;
  ASSERT_TRUE(base::ReadFileToString(Cache().AppendASCII("lists.key"), &other));
  EXPECT_NE(other, key);
}

TEST_F(ContentBlockingListsTest, ResourcesThatAreNoResourceListAreNotUsed) {
  EXPECT_TRUE(BuildBlocker(Cache(), {}, {}, "").resources_used);
  EXPECT_TRUE(BuildBlocker(Cache(), {}, {}, "[]").resources_used);
  EXPECT_FALSE(BuildBlocker(Cache(), {}, {}, "not json").resources_used);
}

}  // namespace
}  // namespace domicile::content_blocking
