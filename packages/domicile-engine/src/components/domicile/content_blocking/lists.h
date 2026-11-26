// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_CONTENT_BLOCKING_LISTS_H_
#define COMPONENTS_DOMICILE_CONTENT_BLOCKING_LISTS_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "components/domicile/content_blocking/blocker.h"

// The config's lists and resources on disk: cached copies of fetched ones,
// and the serialized lists' engine.
namespace domicile::content_blocking {

// Whether `source`, a list or the resources in the config, is fetched rather
// than read from a path.
bool IsAddress(std::string_view source);

// How long a fetched list stays fresh: its `! Expires:`, and a day for a list
// that names none. adblock-rust reads an hour to 14 days, as Adblock Plus
// does, and no other.
base::TimeDelta Lifetime(std::string_view list);

// Whether adblock-rust can take `text`: UTF-8, as Rust's `str` is.
bool IsListText(std::string_view text);

struct SourceText {
  std::string text;
  base::Time modified;
};

struct ReadSources {
  // A path's own text, an address's cached copy.
  std::map<std::string, SourceText> texts;
  // One line for each source `texts` leaves out, but an address with no
  // cached copy yet.
  std::vector<std::string> errors;
};

// Each of `sources` as `cache` and the disk hold it.
ReadSources ReadFrom(const base::FilePath& cache,
                     const std::vector<std::string>& sources);

// Keeps `text`, fetched from `source`, in `cache`. False when it cannot.
bool KeepFetched(const base::FilePath& cache,
                 std::string_view source,
                 std::string_view text);

struct Built {
  std::unique_ptr<Blocker> blocker;
  // False when the lists' engine could not be kept in the cache, so the next
  // start builds it again.
  bool kept = true;
  // False when the resources are not adblock-rust's resource list, so
  // `##+js()` and `$redirect` find none.
  bool resources_used = true;
};

// A blocker for `lists` and the user's `filters`. The lists' engine is read
// from `cache` when it holds one built from the same texts, else built and
// kept there: parsing EasyList and uBlock Origin's lists takes about a
// second, reading the serialized engine much less. Each text is UTF-8.
Built BuildBlocker(const base::FilePath& cache,
                   const std::vector<std::string>& lists,
                   const std::vector<std::string>& filters,
                   std::string_view resources);

}  // namespace domicile::content_blocking

#endif  // COMPONENTS_DOMICILE_CONTENT_BLOCKING_LISTS_H_
