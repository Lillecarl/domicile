// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/settings.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "url/gurl.h"

namespace domicile::content_blocking {

namespace {

std::optional<std::vector<std::string>> Strings(const base::DictValue& message,
                                                std::string_view key) {
  const base::ListValue* list = message.FindList(key);
  if (list == nullptr) {
    return std::nullopt;
  }
  std::vector<std::string> strings;
  for (const base::Value& value : *list) {
    if (!value.is_string()) {
      return std::nullopt;
    }
    strings.push_back(value.GetString());
  }
  return strings;
}

bool IsLabel(std::string_view label) {
  return !label.empty() && label.size() <= 63 && label.front() != '-' &&
         label.back() != '-' &&
         std::ranges::all_of(label, [](char c) {
           return base::IsAsciiLower(c) || base::IsAsciiDigit(c) || c == '-';
         });
}

}  // namespace

std::optional<Mode> ModeNamed(std::string_view name) {
  if (name == "off") {
    return Mode::kOff;
  }
  if (name == "standard") {
    return Mode::kStandard;
  }
  if (name == "aggressive") {
    return Mode::kAggressive;
  }
  return std::nullopt;
}

std::string_view ModeName(Mode mode) {
  switch (mode) {
    case Mode::kOff:
      return "off";
    case Mode::kStandard:
      return "standard";
    case Mode::kAggressive:
      return "aggressive";
  }
}

bool IsBlockingSite(std::string_view site) {
  const bool wildcard = site.starts_with("*.");
  const std::string_view host = wildcard ? site.substr(2) : site;
  const std::vector<std::string_view> labels = base::SplitStringPiece(
      host, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (host.empty() || !std::ranges::all_of(labels, IsLabel)) {
    return false;
  }
  // The last label is a name, not a number, unless the whole is an address.
  const bool address =
      labels.size() == 4 &&
      std::ranges::all_of(labels, [](std::string_view label) {
        return std::ranges::all_of(label, base::IsAsciiDigit<char>);
      });
  return address ? !wildcard
                 : !std::ranges::all_of(labels.back(),
                                        base::IsAsciiDigit<char>);
}

Settings WithUserSites(Settings config,
                       const std::map<std::string, Mode>& sites) {
  for (const auto& [site, mode] : sites) {
    config.sites.emplace(site, mode);
  }
  return config;
}

std::optional<Settings> SettingsFromMessage(const base::DictValue& message) {
  const std::optional<bool> enabled = message.FindBool("enabled");
  const std::optional<bool> electron_apps = message.FindBool("electron_apps");
  std::optional<std::vector<std::string>> lists = Strings(message, "lists");
  std::optional<std::vector<std::string>> filters =
      Strings(message, "filters");
  const std::string* resources = message.FindString("resources");
  const std::string* mode_name = message.FindString("mode");
  const base::DictValue* site_modes = message.FindDict("sites");
  if (!enabled || !electron_apps || !lists || !filters || !resources ||
      !mode_name || !site_modes) {
    return std::nullopt;
  }
  const std::optional<Mode> mode = ModeNamed(*mode_name);
  if (!mode) {
    return std::nullopt;
  }
  std::map<std::string, Mode> sites;
  for (const auto [site, value] : *site_modes) {
    const std::optional<Mode> site_mode =
        value.is_string() ? ModeNamed(value.GetString()) : std::nullopt;
    if (!site_mode) {
      return std::nullopt;
    }
    sites.emplace(site, *site_mode);
  }
  return Settings{
      .enabled = *enabled,
      .lists = std::move(*lists),
      .resources = *resources,
      .filters = std::move(*filters),
      .mode = *mode,
      .sites = std::move(sites),
      .electron_apps = *electron_apps,
  };
}

Mode ModeFor(const Settings& settings, const GURL& page) {
  if (!settings.enabled) {
    return Mode::kOff;
  }
  const std::string_view host = page.host();
  if (auto exact = settings.sites.find(std::string(host));
      exact != settings.sites.end()) {
    return exact->second;
  }
  // `*.example.com` covers example.com and its subdomains; try the longest
  // suffix of `host` first.
  for (std::string_view rest = host; !rest.empty();) {
    if (auto covering = settings.sites.find("*." + std::string(rest));
        covering != settings.sites.end()) {
      return covering->second;
    }
    const size_t dot = rest.find('.');
    if (dot == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(dot + 1);
  }
  return settings.mode;
}

}  // namespace domicile::content_blocking
