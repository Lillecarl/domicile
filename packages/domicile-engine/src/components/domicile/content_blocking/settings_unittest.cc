// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/settings.h"

#include "base/json/json_reader.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace domicile::content_blocking {
namespace {

base::DictValue Message(std::string_view json) {
  return base::JSONReader::Read(json, base::JSON_PARSE_RFC)
      ->GetDict()
      .Clone();
}

constexpr char kMessage[] = R"({
  "type": "content_blocking", "enabled": true,
  "lists": ["https://easylist.to/easylist/easylist.txt"],
  "resources": "/etc/domicile/resources.json",
  "filters": ["||ads.example^"], "mode": "standard",
  "sites": {"bank.example": "off", "*.news.example": "aggressive",
            "*.example": "standard"},
  "electron_apps": false
})";

TEST(ContentBlockingSettingsTest, AMessageReadsAsSettings) {
  const std::optional<Settings> settings =
      SettingsFromMessage(Message(kMessage));
  ASSERT_TRUE(settings);
  EXPECT_TRUE(settings->enabled);
  EXPECT_EQ(settings->lists,
            std::vector<std::string>{
                "https://easylist.to/easylist/easylist.txt"});
  EXPECT_EQ(settings->resources, "/etc/domicile/resources.json");
  EXPECT_EQ(settings->filters, std::vector<std::string>{"||ads.example^"});
  EXPECT_EQ(settings->mode, Mode::kStandard);
  EXPECT_EQ(settings->sites.at("bank.example"), Mode::kOff);
  EXPECT_FALSE(settings->electron_apps);
}

TEST(ContentBlockingSettingsTest, AMessageWithAnUnknownModeIsRefused) {
  EXPECT_FALSE(SettingsFromMessage(Message(R"({
    "enabled": true, "lists": [], "resources": "", "filters": [],
    "mode": "maximum", "sites": {}, "electron_apps": false})")));
  EXPECT_FALSE(SettingsFromMessage(Message(R"({
    "enabled": true, "lists": [], "resources": "", "filters": [],
    "mode": "standard", "sites": {"a.example": 1}, "electron_apps": false})")));
}

TEST(ContentBlockingSettingsTest, AMessageMissingAFieldIsRefused) {
  EXPECT_FALSE(SettingsFromMessage(Message(R"({
    "enabled": true, "lists": [1], "resources": "", "filters": [],
    "mode": "standard", "sites": {}, "electron_apps": false})")));
  EXPECT_FALSE(SettingsFromMessage(Message(R"({"enabled": true})")));
}

TEST(ContentBlockingSettingsTest, TheMostSpecificSiteGivesTheMode) {
  const Settings settings = *SettingsFromMessage(Message(kMessage));
  EXPECT_EQ(ModeFor(settings, GURL("https://bank.example/login")),
            Mode::kOff);
  EXPECT_EQ(ModeFor(settings, GURL("https://www.news.example/")),
            Mode::kAggressive);
  EXPECT_EQ(ModeFor(settings, GURL("https://news.example/")),
            Mode::kAggressive);
  EXPECT_EQ(ModeFor(settings, GURL("https://shop.example/")),
            Mode::kStandard);
  EXPECT_EQ(ModeFor(settings, GURL("https://unlisted.org/")),
            Mode::kStandard);
}

TEST(ContentBlockingSettingsTest, NothingIsBlockedUntilEnabled) {
  Settings settings = *SettingsFromMessage(Message(kMessage));
  settings.enabled = false;
  EXPECT_EQ(ModeFor(settings, GURL("https://unlisted.org/")), Mode::kOff);
}

TEST(ContentBlockingSettingsTest, ASiteIsAHostOrAnAddress) {
  EXPECT_TRUE(IsBlockingSite("news.example"));
  EXPECT_TRUE(IsBlockingSite("*.news.example"));
  EXPECT_TRUE(IsBlockingSite("127.0.0.1"));
  EXPECT_FALSE(IsBlockingSite("*.127.0.0.1"));
  EXPECT_FALSE(IsBlockingSite("News.example"));
  EXPECT_FALSE(IsBlockingSite("news.example/path"));
  EXPECT_FALSE(IsBlockingSite("news..example"));
  EXPECT_FALSE(IsBlockingSite(""));
}

TEST(ContentBlockingSettingsTest, TheConfigsSitesWinOverTheUsers) {
  const Settings settings = WithUserSites(
      *SettingsFromMessage(Message(kMessage)),
      {{"bank.example", Mode::kStandard}, {"blog.org", Mode::kOff}});
  EXPECT_EQ(ModeFor(settings, GURL("https://bank.example/")), Mode::kOff);
  EXPECT_EQ(ModeFor(settings, GURL("https://blog.org/")), Mode::kOff);
}

TEST(ContentBlockingSettingsTest, ModesHaveNames) {
  for (Mode mode : {Mode::kOff, Mode::kStandard, Mode::kAggressive}) {
    EXPECT_EQ(ModeNamed(ModeName(mode)), mode);
  }
  EXPECT_EQ(ModeNamed("maximum"), std::nullopt);
}

}  // namespace
}  // namespace domicile::content_blocking
