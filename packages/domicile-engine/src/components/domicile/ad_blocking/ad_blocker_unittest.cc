// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/ad_blocking/ad_blocker.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace domicile {
namespace {

using network::mojom::RequestDestination;

// adblock-rust's resource list with one stand-in script, `noop.js`, whose
// content is `(function() {})();` in base64.
constexpr char kResources[] =
    R"([{"name":"noop.js","aliases":["noopjs"],)"
    R"("kind":{"mime":"application/javascript"},)"
    R"("content":"KGZ1bmN0aW9uKCkge30pKCk7"}])";

constexpr bool kStandard = false;
constexpr bool kStrict = true;

class AdBlockerTest : public testing::Test {
 protected:
  void Build(std::vector<std::string> lists, std::vector<std::string> rules) {
    blocker_ = AdBlocker::Build(lists, rules, /*resources=*/"");
  }

  bool Blocks(const char* url,
              const char* page,
              bool strict = kStandard,
              const char* type = "script") {
    return blocker_->ShouldBlock(GURL(url), GURL(page), type, "GET", strict);
  }

  std::unique_ptr<AdBlocker> blocker_;
};

TEST(AdBlockerResourcesTest, TakesAdblockRustsResourceList) {
  EXPECT_TRUE(AdBlocker::Build({"||ads.example^\n"}, {}, kResources)
                  ->has_resources());
}

TEST(AdBlockerResourcesTest, HasNoneForNoneOrAnotherText) {
  EXPECT_FALSE(AdBlocker::Build({}, {}, "")->has_resources());
  EXPECT_FALSE(AdBlocker::Build({}, {}, "{\"not\": \"a list\"}")
                   ->has_resources());
  EXPECT_FALSE(AdBlocker::Build({}, {}, "\xff\xfe")->has_resources());
}

using Kind = AdBlocker::Decision::Kind;

// The `$removeparam` rules here name `$script`: an untyped one applies to
// documents, frames and fetches only, as in uBlock Origin.

AdBlocker::Decision Decide(const AdBlocker& blocker,
                           const char* url,
                           const char* page,
                           bool strict = kStandard,
                           const char* method = "GET") {
  return blocker.Decide(GURL(url), GURL(page), "script", method, strict);
}

TEST(AdBlockerDecideTest, AnswersARedirectWithItsStandIn) {
  std::unique_ptr<AdBlocker> blocker = AdBlocker::Build(
      {"||ads.example^$script,redirect=noop.js\n"}, {}, kResources);
  const AdBlocker::Decision decision =
      Decide(*blocker, "https://ads.example/a.js", "https://news.example/");
  EXPECT_EQ(decision.kind, Kind::kRedirect);
  EXPECT_EQ(decision.url.spec(),
            "data:application/javascript;base64,KGZ1bmN0aW9uKCkge30pKCk7");
}

TEST(AdBlockerDecideTest, BlocksARedirectWithoutResources) {
  std::unique_ptr<AdBlocker> blocker = AdBlocker::Build(
      {"||ads.example^$script,redirect=noop.js\n"}, {}, "");
  EXPECT_EQ(
      Decide(*blocker, "https://ads.example/a.js", "https://news.example/")
          .kind,
      Kind::kBlock);
}

TEST(AdBlockerDecideTest, RemovesTheParametersAListNames) {
  std::unique_ptr<AdBlocker> blocker = AdBlocker::Build(
      {"||cdn.example^$script,removeparam=utm_source\n"}, {}, "");
  EXPECT_EQ(Decide(*blocker, "https://cdn.example/a.js?utm_source=x&b=1",
                   "https://news.example/"),
            (AdBlocker::Decision{.kind = Kind::kRewrite,
                                 .url = GURL("https://cdn.example/a.js?b=1")}));
}

TEST(AdBlockerDecideTest, LeavesAPostsParameters) {
  std::unique_ptr<AdBlocker> blocker = AdBlocker::Build(
      {"||cdn.example^$script,removeparam=utm_source\n"}, {}, "");
  EXPECT_EQ(Decide(*blocker, "https://cdn.example/a.js?utm_source=x",
                   "https://news.example/", kStandard, "POST")
                .kind,
            Kind::kAllow);
}

TEST(AdBlockerDecideTest, RemovesAListsParametersFromASitesOwnOnlyInStrict) {
  std::unique_ptr<AdBlocker> blocker =
      AdBlocker::Build({"$script,removeparam=utm_source\n"}, {}, "");
  const char* own = "https://news.example/a.js?utm_source=x";
  EXPECT_EQ(Decide(*blocker, own, "https://news.example/").kind,
            Kind::kAllow);
  EXPECT_EQ(Decide(*blocker, own, "https://news.example/", kStrict).kind,
            Kind::kRewrite);
}

TEST(AdBlockerDecideTest, RemovesTheUsersParametersEverywhere) {
  std::unique_ptr<AdBlocker> blocker =
      AdBlocker::Build({}, {"$script,removeparam=utm_source"}, "");
  EXPECT_EQ(Decide(*blocker, "https://news.example/a.js?utm_source=x",
                   "https://news.example/")
                .kind,
            Kind::kRewrite);
}

TEST(AdBlockerCspTest, AddsTheListsAndTheUsersPolicies) {
  std::unique_ptr<AdBlocker> blocker =
      AdBlocker::Build({"||news.example^$csp=script-src 'none'\n"},
                       {"||news.example^$csp=img-src 'self'"}, "");
  EXPECT_EQ(blocker->CspDirectives(GURL("https://news.example/"),
                                   GURL("https://news.example/"), "document",
                                   "GET"),
            "script-src 'none',img-src 'self'");
}

TEST(AdBlockerCspTest, IsEmptyWithoutACspRule) {
  std::unique_ptr<AdBlocker> blocker =
      AdBlocker::Build({"||ads.example^\n"}, {}, "");
  EXPECT_EQ(blocker->CspDirectives(GURL("https://news.example/"),
                                   GURL("https://news.example/"), "document",
                                   "GET"),
            "");
}

TEST(AdBlockerCosmeticsTest, GivesASitesProceduralFilters) {
  std::unique_ptr<AdBlocker> blocker = AdBlocker::Build(
      {"news.example##div:has-text(Sponsored)\n"}, {}, "");
  const AdBlocker::Cosmetics cosmetics =
      blocker->CosmeticsFor(GURL("https://news.example/"));
  ASSERT_EQ(cosmetics.procedural_filters.size(), 1u);
  EXPECT_NE(cosmetics.procedural_filters[0].find("Sponsored"),
            std::string::npos);
  EXPECT_TRUE(blocker->CosmeticsFor(GURL("https://other.example/"))
                  .procedural_filters.empty());
}

TEST(AdBlockerCosmeticsTest, GivesASitesScriptletsWithResources) {
  // A scriptlet is a template resource, `{{1}}` its first argument:
  // `window.flag = '{{1}}';` in base64.
  constexpr char kTemplate[] =
      R"([{"name":"set-flag.js","kind":"template",)"
      R"("content":"d2luZG93LmZsYWcgPSAne3sxfX0nOw=="}])";
  constexpr char kScriptlet[] = "news.example##+js(set-flag, on)\n";
  EXPECT_NE(AdBlocker::Build({kScriptlet}, {}, kTemplate)
                ->CosmeticsFor(GURL("https://news.example/"))
                .scriptlets.find("window.flag = 'on';"),
            std::string::npos);
  EXPECT_EQ(AdBlocker::Build({kScriptlet}, {}, "")
                ->CosmeticsFor(GURL("https://news.example/"))
                .scriptlets,
            "");
}

TEST(BlockListExpiryTest, ReadsTheListsExpires) {
  EXPECT_EQ(BlockListExpiry("! Title: A\n! Expires: 4 days\n||ads.example^\n"),
            base::Days(4));
  EXPECT_EQ(BlockListExpiry("! Expires: 12 hours (update frequency)\n"),
            base::Hours(12));
}

TEST(BlockListExpiryTest, IsNothingWithoutOne) {
  EXPECT_EQ(BlockListExpiry("! Title: A\n||ads.example^\n"), std::nullopt);
  EXPECT_EQ(BlockListExpiry("! Expires: soon\n"), std::nullopt);
  EXPECT_EQ(BlockListExpiry(""), std::nullopt);
}

TEST_F(AdBlockerTest, BlocksAnotherSitesRequestAListNames) {
  Build({"/ads/*\n"}, {});
  EXPECT_TRUE(Blocks("https://cdn.example/ads/a.js", "https://news.example/"));
  EXPECT_TRUE(Blocks("https://cdn.example/ads/a.js", "https://news.example/",
                     kStrict));
  EXPECT_FALSE(Blocks("https://cdn.example/app.js", "https://news.example/"));
}

TEST_F(AdBlockerTest, StandardLetsASitesOwnRequestThroughAndStrictDoesNot) {
  Build({"/ads/*\n"}, {});
  // Another subdomain is the same site: Chromium's registry decides.
  EXPECT_FALSE(
      Blocks("https://static.news.example/ads/a.js", "https://www.news.example/"));
  EXPECT_TRUE(Blocks("https://static.news.example/ads/a.js",
                     "https://www.news.example/", kStrict));
  // A private registry counts too: two github.io sites are different sites.
  EXPECT_TRUE(
      Blocks("https://ads.github.io/ads/a.js", "https://news.github.io/"));
}

TEST_F(AdBlockerTest, AnImportantRuleBlocksASitesOwnRequestOnStandard) {
  Build({"/ads/*$important\n@@/ads/*\n"}, {});
  EXPECT_TRUE(Blocks("https://news.example/ads/a.js", "https://news.example/"));
}

TEST_F(AdBlockerTest, AListsExceptionLetsARequestThrough) {
  Build({"/ads/*\n@@/ads/allowed.js\n"}, {});
  EXPECT_FALSE(
      Blocks("https://cdn.example/ads/allowed.js", "https://news.example/"));
  EXPECT_TRUE(Blocks("https://cdn.example/ads/a.js", "https://news.example/"));
}

TEST_F(AdBlockerTest, YourOwnRulesBlockASitesOwnRequestOnStandard) {
  Build({}, {"/mine/*"});
  EXPECT_TRUE(Blocks("https://news.example/mine/a.js", "https://news.example/"));
}

TEST_F(AdBlockerTest, YourOwnExceptionLiftsAListsRule) {
  Build({"||cdn.example^\n"}, {"@@||cdn.example^$script"});
  EXPECT_FALSE(Blocks("https://cdn.example/a.js", "https://news.example/"));
  EXPECT_TRUE(Blocks("https://cdn.example/a.png", "https://news.example/",
                     kStandard, "image"));
}

TEST_F(AdBlockerTest, ARuleForOneTypeLeavesTheOthers) {
  Build({"||pixel.example^$image\n"}, {});
  EXPECT_TRUE(Blocks("https://pixel.example/p", "https://news.example/",
                     kStandard, "image"));
  EXPECT_FALSE(Blocks("https://pixel.example/p", "https://news.example/"));
}

TEST_F(AdBlockerTest, ADomainOptionReadsThePage) {
  Build({"||widget.example^$domain=news.example\n"}, {});
  EXPECT_TRUE(Blocks("https://widget.example/w.js", "https://www.news.example/"));
  EXPECT_FALSE(Blocks("https://widget.example/w.js", "https://shop.example/"));
}

TEST_F(AdBlockerTest, SeveralListsAllApply) {
  Build({"||one.example^\n", "||two.example^\n"}, {});
  EXPECT_TRUE(Blocks("https://one.example/a.js", "https://news.example/"));
  EXPECT_TRUE(Blocks("https://two.example/a.js", "https://news.example/"));
}

TEST_F(AdBlockerTest, NoListsAndNoRulesBlockNothing) {
  Build({}, {});
  EXPECT_FALSE(Blocks("https://cdn.example/ads/a.js", "https://news.example/",
                      kStrict));
}

TEST_F(AdBlockerTest, HidesWhatASitesRulesAndTheGenericOnesName) {
  Build({"example.com##.site-ad\n##div[data-ad]\nother.example##.theirs\n"},
        {"example.com##.mine\n"});
  const AdBlocker::Cosmetics cosmetics =
      blocker_->CosmeticsFor(GURL("https://www.example.com/page"));
  EXPECT_EQ(cosmetics.hide_selectors,
            (std::vector<std::string>{".mine", ".site-ad", "div[data-ad]"}));
  EXPECT_FALSE(cosmetics.generichide);
}

TEST_F(AdBlockerTest, HidesAPagesClassesAndIdsTheGenericRulesName) {
  Build({"##.ad-banner\n###sponsor\n##.other-ad\n"}, {"##.my-ad\n"});
  const AdBlocker::Cosmetics cosmetics =
      blocker_->CosmeticsFor(GURL("https://news.example/"));
  EXPECT_EQ(blocker_->HiddenClassIdSelectors({"ad-banner", "my-ad", "story"},
                                              {"sponsor"},
                                              cosmetics.exceptions),
            (std::vector<std::string>{"#sponsor", ".ad-banner", ".my-ad"}));
}

TEST_F(AdBlockerTest, ASitesExceptionKeepsAGenericRuleOffItsPages) {
  Build({"##.ad-banner\nexample.com#@#.ad-banner\n"}, {});
  const AdBlocker::Cosmetics cosmetics =
      blocker_->CosmeticsFor(GURL("https://example.com/"));
  EXPECT_EQ(cosmetics.exceptions, (std::vector<std::string>{".ad-banner"}));
  EXPECT_TRUE(blocker_
                  ->HiddenClassIdSelectors({"ad-banner"}, {},
                                           cosmetics.exceptions)
                  .empty());
}

TEST_F(AdBlockerTest, AGenerichideExceptionLeavesOnlyTheSitesOwn) {
  Build({"##div[data-ad]\n@@||example.com^$generichide\n"
         "example.com##.site-ad\n"},
        {});
  const AdBlocker::Cosmetics cosmetics =
      blocker_->CosmeticsFor(GURL("https://example.com/"));
  EXPECT_TRUE(cosmetics.generichide);
  EXPECT_EQ(cosmetics.hide_selectors,
            (std::vector<std::string>{".site-ad"}));
}

TEST_F(AdBlockerTest, APageThatIsNotAWebsiteHidesNothing) {
  Build({"##div[data-ad]\n"}, {});
  EXPECT_TRUE(blocker_->CosmeticsFor(GURL("domicile://shell/"))
                  .hide_selectors.empty());
}

TEST(AdBlockRequestTypeTest, NamesWhatARequestFetchesAsFiltersDo) {
  EXPECT_EQ(AdBlockRequestType(RequestDestination::kScript), "script");
  EXPECT_EQ(AdBlockRequestType(RequestDestination::kServiceWorker), "script");
  EXPECT_EQ(AdBlockRequestType(RequestDestination::kImage), "image");
  EXPECT_EQ(AdBlockRequestType(RequestDestination::kStyle), "stylesheet");
  EXPECT_EQ(AdBlockRequestType(RequestDestination::kIframe), "subdocument");
  EXPECT_EQ(AdBlockRequestType(RequestDestination::kVideo), "media");
  EXPECT_EQ(AdBlockRequestType(RequestDestination::kEmpty), "xmlhttprequest");
  EXPECT_EQ(AdBlockRequestType(RequestDestination::kManifest), "other");
}

}  // namespace
}  // namespace domicile
