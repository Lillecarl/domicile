// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/engine.h"

#include <string>
#include <vector>

#include "base/strings/string_util.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace domicile::content_blocking {
namespace {

constexpr char kPage[] = "https://site.example/article";

Request Script(std::string_view url, std::string_view page = kPage) {
  return {.url = GURL(url), .source = GURL(page), .type = RequestType::kScript};
}

std::unique_ptr<Engine> EngineOf(std::string list) {
  return Engine::FromLists({std::move(list)});
}

TEST(ContentBlockingEngineTest, BlocksAThirdPartyRequestARuleNames) {
  auto engine = EngineOf("||ads.example^$third-party\n");
  EXPECT_TRUE(
      engine->Check(Script("https://ads.example/ad.js")).ShouldBlock());
  EXPECT_FALSE(
      engine->Check(Script("https://cdn.example/app.js")).ShouldBlock());
  EXPECT_FALSE(engine
                   ->Check(Script("https://ads.example/ad.js",
                                  "https://ads.example/home"))
                   .ShouldBlock());
}

TEST(ContentBlockingEngineTest, AnExceptionLetsARequestThrough) {
  auto engine = EngineOf("||ads.example^\n@@||ads.example/ok.js\n");
  const Verdict verdict = engine->Check(Script("https://ads.example/ok.js"));
  EXPECT_TRUE(verdict.matched);
  EXPECT_TRUE(verdict.exception);
  EXPECT_FALSE(verdict.ShouldBlock());
}

TEST(ContentBlockingEngineTest, AnImportantRuleOutranksAnException) {
  auto engine =
      EngineOf("||ads.example^$important\n@@||ads.example/ok.js\n");
  const Verdict verdict = engine->Check(Script("https://ads.example/ok.js"));
  EXPECT_TRUE(verdict.important);
  EXPECT_TRUE(verdict.ShouldBlock());
}

TEST(ContentBlockingEngineTest, ARedirectAnswersWithTheResource) {
  auto engine = EngineOf("||ads.example/ad.js$script,redirect=noop.js\n");
  ASSERT_TRUE(engine->UseResources(
      R"([{"name": "noop.js", "aliases": [],
           "kind": {"mime": "application/javascript"},
           "content": "KGZ1bmN0aW9uKCl7fSkoKTs="}])"));
  const Verdict verdict = engine->Check(Script("https://ads.example/ad.js"));
  EXPECT_TRUE(verdict.ShouldBlock());
  ASSERT_TRUE(verdict.redirect);
  EXPECT_TRUE(verdict.redirect->SchemeIs("data"));
  EXPECT_TRUE(base::StartsWith(verdict.redirect->spec(),
                               "data:application/javascript;base64,"));
}

TEST(ContentBlockingEngineTest, ResourcesThatDoNotParseAreRefused) {
  auto engine = EngineOf("");
  EXPECT_FALSE(engine->UseResources("not json"));
}

TEST(ContentBlockingEngineTest, RemoveparamRewritesTheUrl) {
  auto engine = EngineOf("||shop.example^$removeparam=utm_source\n");
  const Verdict verdict = engine->Check(
      {.url = GURL("https://shop.example/item?id=4&utm_source=mail"),
       .source = GURL(kPage),
       .type = RequestType::kSubdocument});
  EXPECT_FALSE(verdict.ShouldBlock());
  ASSERT_TRUE(verdict.rewritten_url);
  EXPECT_EQ(verdict.rewritten_url->spec(), "https://shop.example/item?id=4");
}

TEST(ContentBlockingEngineTest, ASerializedEngineBlocksTheSame) {
  auto engine = EngineOf("||ads.example^\n");
  const std::vector<uint8_t> data = engine->Serialize();
  ASSERT_FALSE(data.empty());
  auto read = Engine::FromSerialized(data);
  ASSERT_TRUE(read);
  EXPECT_TRUE(read->Check(Script("https://ads.example/ad.js")).ShouldBlock());
}

TEST(ContentBlockingEngineTest, BytesThatAreNoEngineReadAsNone) {
  const std::vector<uint8_t> data = {1, 2, 3, 4};
  EXPECT_FALSE(Engine::FromSerialized(data));
}

TEST(ContentBlockingEngineTest, AListSaysWhenItExpires) {
  EXPECT_EQ(ListExpiresHours("! Title: A\n! Expires: 4 days\n||a.example^\n"),
            96);
  EXPECT_EQ(ListExpiresHours("! Expires: 12 hours\n"), 12);
  EXPECT_EQ(ListExpiresHours("||a.example^\n"), std::nullopt);
}

TEST(ContentBlockingEngineTest, ASitesHideRulesAreItsCosmeticResources) {
  auto engine = EngineOf("site.example##.banner\n");
  EXPECT_THAT(engine->CosmeticResources(GURL(kPage)),
              testing::HasSubstr(".banner"));
  EXPECT_THAT(engine->CosmeticResources(GURL("https://other.example/")),
              testing::Not(testing::HasSubstr(".banner")));
}

TEST(ContentBlockingEngineTest, AGenericRuleHidesTheClassesItNames) {
  auto engine = EngineOf("##.ad-box\n");
  EXPECT_THAT(engine->HiddenClassIdSelectors({"ad-box", "story"}, {}, {}),
              testing::ElementsAre(".ad-box"));
  EXPECT_THAT(
      engine->HiddenClassIdSelectors({"ad-box"}, {}, {".ad-box"}),
      testing::IsEmpty());
}

TEST(ContentBlockingEngineTest, ACspRuleAddsDirectivesToADocument) {
  auto engine = EngineOf("||site.example^$csp=script-src 'none'\n");
  EXPECT_EQ(engine->CspDirectives({.url = GURL(kPage),
                                   .source = GURL(kPage),
                                   .type = RequestType::kDocument}),
            "script-src 'none'");
}

TEST(ContentBlockingEngineTest, APrivateRegistryCountsAsSites) {
  EXPECT_TRUE(IsThirdParty(GURL("https://a.github.io/"),
                           GURL("https://b.github.io/")));
  EXPECT_FALSE(IsThirdParty(GURL("https://cdn.site.example/"),
                            GURL("https://www.site.example/")));
}

}  // namespace
}  // namespace domicile::content_blocking
