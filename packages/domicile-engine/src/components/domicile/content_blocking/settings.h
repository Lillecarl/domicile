// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_CONTENT_BLOCKING_SETTINGS_H_
#define COMPONENTS_DOMICILE_CONTENT_BLOCKING_SETTINGS_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"

class GURL;

namespace domicile::content_blocking {

// How much is blocked on a site. See docs/architecture/CONTENT-BLOCKING.md.
enum class Mode {
  kOff,
  // Other sites' ads and trackers, and the site's own `$important` ones.
  kStandard,
  // The site's own too.
  kAggressive,
};

// The config's `content_blocking`, as the compositor sends it on the control
// channel (`content_blocking`).
struct Settings {
  bool enabled = false;
  // Addresses or absolute paths.
  std::vector<std::string> lists;
  std::string resources;
  // The user's own rules.
  std::vector<std::string> filters;
  Mode mode = Mode::kStandard;
  // By host, or `*.host` for it and its subdomains.
  std::map<std::string, Mode> sites;
  bool electron_apps = false;

  friend bool operator==(const Settings&, const Settings&) = default;
};

// A `content_blocking` message's settings, or nullopt when a field is missing
// or of the wrong kind.
std::optional<Settings> SettingsFromMessage(const base::DictValue& message);

// The mode for a page at `page`: the most specific `sites` entry, a host
// before `*.host` and a longer `*.host` before a shorter one, else `mode`.
// kOff while blocking is not enabled.
Mode ModeFor(const Settings& settings, const GURL& page);

}  // namespace domicile::content_blocking

#endif  // COMPONENTS_DOMICILE_CONTENT_BLOCKING_SETTINGS_H_
