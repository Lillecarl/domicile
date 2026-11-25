// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/lists.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "components/domicile/content_blocking/engine.h"
#include "crypto/sha2.h"

namespace domicile::content_blocking {

namespace {

// The most a list on disk may hold. A fetched one may hold
// SimpleURLLoader::kMaxBoundedStringDownloadSize, 5M; EasyList is under 2M.
constexpr size_t kMostListBytes = 32 * 1024 * 1024;
constexpr size_t kMostEngineBytes = 256 * 1024 * 1024;

// adblock-rust's serialized format changes with its version, so the cache key
// holds crates/CRATES's: bump it with the crate.
constexpr char kEngineFormat[] = "adblock 0.13.6\n";

std::string CacheName(std::string_view source) {
  return base::ToLowerASCII(
             base::HexEncode(crypto::SHA256HashString(source)).substr(0, 32)) +
         ".txt";
}

base::FilePath PathOf(const base::FilePath& cache, const std::string& source) {
  return IsAddress(source) ? cache.AppendASCII(CacheName(source))
                           : base::FilePath(source);
}

// The lists' engine, from `cache` when its key matches, else built and kept.
std::pair<std::unique_ptr<Engine>, bool> ListsEngine(
    const base::FilePath& cache,
    const std::vector<std::string>& lists) {
  std::string key = kEngineFormat;
  for (const std::string& list : lists) {
    key += crypto::SHA256HashString(list);
  }
  key = base::HexEncode(crypto::SHA256HashString(key));
  const base::FilePath path = cache.AppendASCII("lists.dat");
  const base::FilePath key_path = cache.AppendASCII("lists.key");
  std::string stored_key;
  std::string stored;
  if (base::ReadFileToString(key_path, &stored_key) && stored_key == key &&
      base::ReadFileToStringWithMaxSize(path, &stored, kMostEngineBytes)) {
    if (std::unique_ptr<Engine> engine =
            Engine::FromSerialized(base::as_byte_span(stored))) {
      return {std::move(engine), true};
    }
  }
  std::unique_ptr<Engine> engine = Engine::FromLists(lists);
  const bool kept = base::CreateDirectory(cache) &&
                    base::WriteFile(path, engine->Serialize()) &&
                    base::WriteFile(key_path, key);
  return {std::move(engine), kept};
}

}  // namespace

bool IsAddress(std::string_view source) {
  return base::StartsWith(source, "https://") ||
         base::StartsWith(source, "http://");
}

base::TimeDelta Lifetime(std::string_view list) {
  return base::Hours(ListExpiresHours(list).value_or(24));
}

bool IsListText(std::string_view text) {
  return base::IsStringUTF8AllowingNoncharacters(text);
}

ReadSources ReadFrom(const base::FilePath& cache,
                     const std::vector<std::string>& sources) {
  ReadSources read;
  for (const std::string& source : sources) {
    const base::FilePath path = PathOf(cache, source);
    SourceText entry;
    base::File::Info info;
    if (!base::GetFileInfo(path, &info) ||
        !base::ReadFileToStringWithMaxSize(path, &entry.text,
                                           kMostListBytes)) {
      if (!IsAddress(source)) {
        read.errors.push_back(source + ", which cannot be read");
      }
      continue;
    }
    if (!IsListText(entry.text)) {
      read.errors.push_back(source + ", which is not UTF-8");
      continue;
    }
    entry.modified = info.last_modified;
    read.texts.emplace(source, std::move(entry));
  }
  return read;
}

bool KeepFetched(const base::FilePath& cache,
                 std::string_view source,
                 std::string_view text) {
  return base::CreateDirectory(cache) &&
         base::WriteFile(cache.AppendASCII(CacheName(source)), text);
}

Built BuildBlocker(const base::FilePath& cache,
                   const std::vector<std::string>& lists,
                   const std::vector<std::string>& filters,
                   std::string_view resources) {
  auto [listed, kept] = ListsEngine(cache, lists);
  auto own = Engine::FromLists({base::JoinString(filters, "\n")});
  const bool resources_used =
      resources.empty() ||
      (listed->UseResources(resources) && own->UseResources(resources));
  return {
      .blocker = std::make_unique<Blocker>(std::move(listed), std::move(own)),
      .kept = kept,
      .resources_used = resources_used,
  };
}

}  // namespace domicile::content_blocking
