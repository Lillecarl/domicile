// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/blocker.h"

#include <string>

#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace domicile::content_blocking {
namespace {

constexpr char kSite[] = "https://site.example/article";

Blocker BlockerOf(std::string lists, std::string filters = "") {
  return Blocker(Engine::FromLists({std::move(lists)}),
                 Engine::FromLists({std::move(filters)}));
}

Request Script(std::string_view url) {
  return {.url = GURL(url), .source = GURL(kSite), .type = RequestType::kScript};
}

Decision Allow() {
  return {};
}
constexpr Decision::Kind kBlock = Decision::Kind::kBlock;

TEST(ContentBlockingBlockerTest, StandardModeBlocksOtherSitesOnly) {
  const Blocker blocker = BlockerOf("||ads.example^\n||site.example/ads/\n");
  EXPECT_EQ(blocker.Decide(Script("https://ads.example/a.js"), Mode::kStandard)
                .kind,
            kBlock);
  EXPECT_EQ(
      blocker.Decide(Script("https://site.example/ads/a.js"), Mode::kStandard),
      Allow());
}

TEST(ContentBlockingBlockerTest, AnImportantRuleBlocksASitesOwnContent) {
  const Blocker blocker = BlockerOf("||site.example/ads/$important\n");
  EXPECT_EQ(
      blocker.Decide(Script("https://site.example/ads/a.js"), Mode::kStandard)
          .kind,
      kBlock);
}

TEST(ContentBlockingBlockerTest, AggressiveModeBlocksASitesOwnContent) {
  const Blocker blocker = BlockerOf("||site.example/ads/\n");
  EXPECT_EQ(
      blocker
          .Decide(Script("https://site.example/ads/a.js"), Mode::kAggressive)
          .kind,
      kBlock);
}

TEST(ContentBlockingBlockerTest, OffBlocksNothing) {
  const Blocker blocker = BlockerOf("||ads.example^$important\n");
  EXPECT_EQ(blocker.Decide(Script("https://ads.example/a.js"), Mode::kOff),
            Allow());
}

TEST(ContentBlockingBlockerTest, TheUsersOwnRulesBlockASitesOwnContent) {
  const Blocker blocker = BlockerOf("", "||site.example/ads/\n");
  EXPECT_EQ(
      blocker.Decide(Script("https://site.example/ads/a.js"), Mode::kStandard)
          .kind,
      kBlock);
}

TEST(ContentBlockingBlockerTest, TheUsersExceptionLetsAListedRequestThrough) {
  const Blocker blocker =
      BlockerOf("||ads.example^\n", "@@||ads.example/needed.js\n");
  EXPECT_EQ(
      blocker.Decide(Script("https://ads.example/needed.js"), Mode::kStandard),
      Allow());
  EXPECT_EQ(blocker.Decide(Script("https://ads.example/a.js"), Mode::kStandard)
                .kind,
            kBlock);
}

TEST(ContentBlockingBlockerTest, ARemovedParameterRewritesAGetOnly) {
  const Blocker blocker =
      BlockerOf("||shop.example^$removeparam=utm_source\n");
  Request request{.url = GURL("https://shop.example/x?utm_source=a&id=1"),
                  .source = GURL(kSite),
                  .type = RequestType::kXmlHttpRequest};
  EXPECT_EQ(blocker.Decide(request, Mode::kStandard),
            (Decision{Decision::Kind::kRewrite,
                      GURL("https://shop.example/x?id=1")}));
  request.method = "POST";
  EXPECT_EQ(blocker.Decide(request, Mode::kStandard), Allow());
}

// adblock-rust, like uBlock Origin, gives a `$removeparam` with no type
// documents, frames and fetches only.
TEST(ContentBlockingBlockerTest, AnImageLosesAParameterOnlyForATypedRule) {
  const Request request{
      .url = GURL("http://localhost:44453/pixel.gif?utm_source=guard&id=1"),
      .source = GURL("http://127.0.0.1:44453/test"),
      .type = RequestType::kImage};
  EXPECT_EQ(BlockerOf("||localhost^$removeparam=utm_source\n")
                .Decide(request, Mode::kStandard),
            Allow());
  EXPECT_EQ(BlockerOf("||localhost^$image,removeparam=utm_source\n")
                .Decide(request, Mode::kStandard),
            (Decision{Decision::Kind::kRewrite,
                      GURL("http://localhost:44453/pixel.gif?id=1")}));
}

TEST(ContentBlockingBlockerTest, CspDirectivesComeFromBothEngines) {
  const Blocker blocker = BlockerOf("||site.example^$csp=script-src 'none'\n",
                                    "||site.example^$csp=img-src 'none'\n");
  const Request document{.url = GURL(kSite),
                         .source = GURL(kSite),
                         .type = RequestType::kDocument};
  EXPECT_EQ(blocker.CspDirectives(document, Mode::kStandard),
            "script-src 'none',img-src 'none'");
  EXPECT_EQ(blocker.CspDirectives(document, Mode::kOff), "");
}

TEST(ContentBlockingBlockerTest, ASitesCosmeticRulesComeFromBothEngines) {
  const Blocker blocker =
      BlockerOf("site.example##.banner\nsite.example#@#.ad-box\n",
                "site.example##.mine\n");
  const std::optional<CosmeticRules> rules =
      blocker.Cosmetic(GURL(kSite), Mode::kStandard);
  ASSERT_TRUE(rules);
  EXPECT_THAT(rules->hide_selectors, testing::ElementsAre(".banner"));
  EXPECT_THAT(rules->force_hide_selectors, testing::ElementsAre(".mine"));
  EXPECT_THAT(rules->exceptions, testing::ElementsAre(".ad-box"));
  EXPECT_FALSE(rules->generichide);
  EXPECT_FALSE(rules->hide_first_party);
  EXPECT_EQ(blocker.Cosmetic(GURL(kSite), Mode::kOff), std::nullopt);
}

TEST(ContentBlockingBlockerTest, AScriptletRunsTheResourceItNames) {
  Blocker blocker = BlockerOf("site.example##+js(guard-mark)\n");
  ASSERT_TRUE(blocker.lists().UseResources(
      R"([{"name": "guard-mark.js", "aliases": ["guard-mark"],
           "kind": "template",
           "content": "d2luZG93Lm1hcmtlZCA9IHRydWU7"}])"));
  const std::optional<CosmeticRules> rules =
      blocker.Cosmetic(GURL(kSite), Mode::kStandard);
  ASSERT_TRUE(rules);
  EXPECT_THAT(rules->injected_script,
              testing::HasSubstr("window.marked = true;"));
}

TEST(ContentBlockingBlockerTest, GenericSelectorsComeFromBothEngines) {
  const Blocker blocker = BlockerOf("##.ad-box\n", "##.my-box\n");
  EXPECT_EQ(blocker.GenericSelectors({"ad-box", "my-box", "story"}, {}, {}),
            (GenericHide{.hide_selectors = {".ad-box"},
                         .force_hide_selectors = {".my-box"}}));
}

TEST(ContentBlockingBlockerTest, StandardModeKeepsOnlyPlainSelectorFilters) {
  const Blocker blocker = BlockerOf(
      "site.example##.box:has(.ad)\n"
      "site.example##.promo:has-text(Sponsored)\n"
      "site.example##.sticky:style(position: static !important)\n");
  const std::optional<CosmeticRules> standard =
      blocker.Cosmetic(GURL(kSite), Mode::kStandard);
  const std::optional<CosmeticRules> aggressive =
      blocker.Cosmetic(GURL(kSite), Mode::kAggressive);
  ASSERT_TRUE(standard && aggressive);
  EXPECT_TRUE(aggressive->hide_first_party);
  EXPECT_LT(standard->hide_selectors.size() +
                standard->procedural_actions.size(),
            aggressive->hide_selectors.size() +
                aggressive->procedural_actions.size());
  EXPECT_EQ(standard->procedural_actions.size(), 1u);
  EXPECT_THAT(standard->procedural_actions.front(),
              testing::HasSubstr("position: static"));
}

}  // namespace
}  // namespace domicile::content_blocking
