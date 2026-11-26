// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "chrome/browser/domicile/domicile_content_blocking.h"

#include <algorithm>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/chrome_paths.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/domicile/browser/control_channel.h"
#include "components/domicile/content_blocking/engine.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "crypto/sha2.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace domicile {

namespace {

using content_blocking::Blocker;
using content_blocking::Engine;
using content_blocking::Mode;
using content_blocking::Settings;

// The most a list on disk may hold. A fetched one may hold
// SimpleURLLoader::kMaxBoundedStringDownloadSize, 5M; EasyList is under 2M.
constexpr size_t kMostBytes = 32 * 1024 * 1024;

// A list's `! Expires:`, within the hour to 14 days Adblock Plus allows, and
// a day for a list that names none.
base::TimeDelta Lifetime(std::string_view text) {
  const int hours = std::clamp(
      content_blocking::ListExpiresHours(text).value_or(24), 1, 14 * 24);
  return base::Hours(hours);
}

bool IsAddress(std::string_view source) {
  return base::StartsWith(source, "https://") ||
         base::StartsWith(source, "http://");
}

std::string CacheName(std::string_view source) {
  return base::ToLowerASCII(
             base::HexEncode(crypto::SHA256HashString(source)).substr(0, 32)) +
         ".txt";
}

struct Read {
  std::string text;
  base::Time modified;
};

// The text of each source as it is on disk: a path itself, an address its
// cached copy. One that cannot be read is left out.
std::map<std::string, Read> ReadSources(base::FilePath cache,
                                        std::vector<std::string> sources) {
  std::map<std::string, Read> read;
  for (const std::string& source : sources) {
    const base::FilePath path = IsAddress(source)
                                    ? cache.AppendASCII(CacheName(source))
                                    : base::FilePath(source);
    Read entry;
    base::File::Info info;
    if (base::GetFileInfo(path, &info) &&
        base::ReadFileToStringWithMaxSize(path, &entry.text, kMostBytes)) {
      entry.modified = info.last_modified;
      read.emplace(source, std::move(entry));
    }
  }
  return read;
}

void WriteCached(base::FilePath cache, std::string source, std::string text) {
  if (!base::CreateDirectory(cache) ||
      !base::WriteFile(cache.AppendASCII(CacheName(source)), text)) {
    LOG(WARNING) << "domicile: content blocking could not cache " << source
                 << " in " << cache;
  }
}

// An engine for `lists`, read from `cache` when it holds one built from the
// same texts, else built and written there. Parsing EasyList and uBlock
// Origin's lists takes about a second; reading the serialized engine much
// less. adblock-rust's format changes with its version, and a file it cannot
// read is built again.
std::unique_ptr<Engine> Cached(const base::FilePath& cache,
                               std::string_view name,
                               const std::vector<std::string>& lists) {
  std::string key = "adblock 0.13.6\n";
  for (const std::string& list : lists) {
    key += crypto::SHA256HashString(list);
  }
  key = base::HexEncode(crypto::SHA256HashString(key));
  const base::FilePath path = cache.AppendASCII(std::string(name) + ".dat");
  const base::FilePath key_path =
      cache.AppendASCII(std::string(name) + ".key");
  std::string stored_key;
  std::string stored;
  if (base::ReadFileToString(key_path, &stored_key) && stored_key == key &&
      base::ReadFileToStringWithMaxSize(path, &stored, 256 * 1024 * 1024)) {
    if (std::unique_ptr<Engine> engine =
            Engine::FromSerialized(base::as_byte_span(stored))) {
      return engine;
    }
  }
  std::unique_ptr<Engine> engine = Engine::FromLists(lists);
  const std::vector<uint8_t> written = engine->Serialize();
  if (!written.empty() && base::CreateDirectory(cache) &&
      base::WriteFile(path, written) && base::WriteFile(key_path, key)) {
    return engine;
  }
  LOG(WARNING) << "domicile: content blocking could not keep its " << name
               << " engine in " << cache << "; it is built on each start.";
  return engine;
}

std::unique_ptr<Blocker> Build(base::FilePath cache,
                               std::vector<std::string> lists,
                               std::vector<std::string> filters,
                               std::string resources) {
  auto listed = Cached(cache, "lists", lists);
  auto own = Engine::FromLists({base::JoinString(filters, "\n")});
  if (!resources.empty() &&
      !(listed->UseResources(resources) && own->UseResources(resources))) {
    LOG(WARNING) << "domicile: content blocking's resources are not "
                    "adblock-rust's resource list; ##+js() and $redirect "
                    "rules find none.";
  }
  return std::make_unique<Blocker>(std::move(listed), std::move(own));
}

const net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("domicile_content_blocking", R"(
      semantics {
        sender: "Domicile content blocking"
        description:
          "Downloads the filter lists and resources the desk's config names "
          "for blocking ads and trackers."
        trigger: "The config's content_blocking, and each list's expiry."
        data: "None; the request carries no cookies."
        destination: OTHER
      }
      policy {
        cookies_allowed: NO
        setting: "Leave content_blocking out of the desk's config."
      })");

}  // namespace

// static
ContentBlocker& ContentBlocker::Get() {
  static base::NoDestructor<ContentBlocker> blocker;
  return *blocker;
}

ContentBlocker::ContentBlocker() = default;
ContentBlocker::~ContentBlocker() = default;

// The user's site modes, by site, as mode names.
constexpr char kSitesPref[] = "domicile.content_blocking.sites";

Mode ContentBlocker::ModeFor(const GURL& page) const {
  return content_blocking::ModeFor(merged_, page);
}

Mode ContentBlocker::ModeForShell(const GURL& page) const {
  return blocker_ ? ModeFor(page) : Mode::kOff;
}

void ContentBlocker::UsePrefs(PrefService& prefs) {
  prefs_ = &prefs;
  Merge();
}

void ContentBlocker::Merge() {
  std::map<std::string, Mode> sites;
  if (prefs_) {
    for (const auto [site, value] : prefs_->GetDict(kSitesPref)) {
      std::optional<Mode> mode =
          value.is_string() ? content_blocking::ModeNamed(value.GetString())
                            : std::nullopt;
      if (mode && content_blocking::IsBlockingSite(site)) {
        sites.emplace(site, *mode);
      }
    }
  }
  merged_ = content_blocking::WithUserSites(settings_, sites);
}

base::expected<void, std::string> ContentBlocker::SetSite(
    const std::string& site,
    std::optional<Mode> mode) {
  if (!settings_.enabled) {
    return base::unexpected(
        "the desk's config blocks nothing, so no site has a mode to set");
  }
  if (!content_blocking::IsBlockingSite(site)) {
    return base::unexpected(site +
                            " is not a site such as `github.com` or "
                            "`*.github.com`");
  }
  if (settings_.sites.contains(site)) {
    return base::unexpected(site +
                            " has its mode from the config's "
                            "content_blocking.sites, which only the config "
                            "changes");
  }
  if (prefs_ == nullptr) {
    return base::unexpected("the shell's profile has not loaded");
  }
  ScopedDictPrefUpdate sites(prefs_, kSitesPref);
  if (mode) {
    sites->Set(site, content_blocking::ModeName(*mode));
  } else {
    sites->Remove(site);
  }
  Merge();
  return base::ok();
}

base::FilePath ContentBlocker::CacheDirectory() const {
  base::FilePath data;
  base::PathService::Get(chrome::DIR_USER_DATA, &data);
  return data.AppendASCII("domicile-content-blocking");
}

void ContentBlocker::SetSettings(const Settings& settings) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (settings == settings_) {
    return;
  }
  settings_ = settings;
  Merge();
  ++generation_;
  refresh_.Stop();
  sources_.clear();
  if (!settings_.enabled) {
    blocker_.reset();
    return;
  }
  for (const std::string& list : settings_.lists) {
    sources_.emplace(list, Source());
  }
  if (!settings_.resources.empty()) {
    sources_.emplace(settings_.resources, Source());
  }
  ReadCached(generation_);
}

void ContentBlocker::ReadCached(uint64_t generation) {
  std::vector<std::string> names;
  for (const auto& [name, source] : sources_) {
    names.push_back(name);
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadSources, CacheDirectory(), std::move(names)),
      base::BindOnce(
          [](base::WeakPtr<ContentBlocker> self, uint64_t generation,
             std::map<std::string, Read> read) {
            if (!self || generation != self->generation_) {
              return;
            }
            const base::Time now = base::Time::Now();
            for (auto& [name, source] : self->sources_) {
              auto found = read.find(name);
              if (found != read.end()) {
                source.text = std::move(found->second.text);
                source.fetched = found->second.modified;
              }
              const bool stale =
                  found == read.end() ||
                  source.fetched + Lifetime(source.text) < now;
              if (IsAddress(name) && stale) {
                self->Fetch(generation, name);
              } else {
                source.read = true;
              }
            }
            self->BuildWhenRead(generation);
          },
          weak_factory_.GetWeakPtr(), generation));
}

void ContentBlocker::Fetch(uint64_t generation, const std::string& source) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(source);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  auto loader = network::SimpleURLLoader::Create(std::move(request),
                                                 kTrafficAnnotation);
  loader->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE |
             network::SimpleURLLoader::RETRY_ON_5XX);
  network::SimpleURLLoader* raw = loader.get();
  raw->DownloadToString(
      g_browser_process->shared_url_loader_factory().get(),
      base::BindOnce(&ContentBlocker::Fetched, weak_factory_.GetWeakPtr(),
                     generation, source, std::move(loader)),
      network::SimpleURLLoader::kMaxBoundedStringDownloadSize);
}

void ContentBlocker::Fetched(uint64_t generation,
                             std::string source,
                             std::unique_ptr<network::SimpleURLLoader> loader,
                             std::optional<std::string> body) {
  if (generation != generation_) {
    return;
  }
  auto entry = sources_.find(source);
  if (entry == sources_.end()) {
    return;
  }
  if (body) {
    entry->second.text = *body;
    entry->second.fetched = base::Time::Now();
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
         base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
        base::BindOnce(&WriteCached, CacheDirectory(), source,
                       std::move(*body)));
  } else {
    // A cached copy, if any, stays in use, and is fetched again later.
    LOG(WARNING) << "domicile: content blocking could not fetch " << source
                 << ": " << net::ErrorToString(loader->NetError());
  }
  entry->second.read = true;
  BuildWhenRead(generation);
}

void ContentBlocker::BuildWhenRead(uint64_t generation) {
  if (std::ranges::any_of(sources_, [](const auto& entry) {
        return !entry.second.read;
      })) {
    return;
  }
  std::vector<std::string> lists;
  for (const std::string& list : settings_.lists) {
    lists.push_back(sources_[list].text);
  }
  std::string resources = settings_.resources.empty()
                              ? std::string()
                              : sources_[settings_.resources].text;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&Build, CacheDirectory(), std::move(lists),
                     settings_.filters, std::move(resources)),
      base::BindOnce(&ContentBlocker::Built, weak_factory_.GetWeakPtr(),
                     generation));
}

void ContentBlocker::Built(uint64_t generation,
                           std::unique_ptr<Blocker> blocker) {
  if (generation != generation_) {
    return;
  }
  blocker_ = std::move(blocker);
  VLOG(1) << "domicile: content blocking built from "
          << settings_.lists.size() << " lists";
  ArmRefresh();
}

void ContentBlocker::ArmRefresh() {
  std::optional<base::Time> next;
  for (const auto& [name, source] : sources_) {
    if (IsAddress(name)) {
      const base::Time due = source.fetched + Lifetime(source.text);
      next = next ? std::min(*next, due) : due;
    }
  }
  if (next) {
    refresh_.Start(FROM_HERE,
                   std::max(*next - base::Time::Now(), base::Minutes(1)),
                   base::BindOnce(&ContentBlocker::Refresh,
                                  base::Unretained(this)));
  }
}

void ContentBlocker::Refresh() {
  const base::Time now = base::Time::Now();
  bool fetching = false;
  for (auto& [name, source] : sources_) {
    if (IsAddress(name) && source.fetched + Lifetime(source.text) <= now) {
      source.read = false;
      fetching = true;
      Fetch(generation_, name);
    }
  }
  if (!fetching) {
    ArmRefresh();
  }
}

BlockedRequests::BlockedRequests(content::WebContents* contents)
    : content::WebContentsUserData<BlockedRequests>(*contents),
      content::WebContentsObserver(contents) {}

BlockedRequests::~BlockedRequests() = default;

// static
void BlockedRequests::Count(content::RenderFrameHost& frame) {
  content::WebContents* contents =
      content::WebContents::FromRenderFrameHost(&frame);
  if (contents == nullptr) {
    return;
  }
  BlockedRequests* blocked = GetOrCreateForWebContents(contents);
  ++blocked->count_;
  blocked->Moved();
}

// static
uint32_t BlockedRequests::Of(content::WebContents& contents) {
  BlockedRequests* blocked = FromWebContents(&contents);
  return blocked ? blocked->count_ : 0;
}

base::CallbackListSubscription BlockedRequests::Watch(
    base::RepeatingClosure changed) {
  return changed_.Add(std::move(changed));
}

void BlockedRequests::PrimaryPageChanged(content::Page&) {
  if (count_ != 0) {
    count_ = 0;
    Moved();
  }
}

void BlockedRequests::Moved() {
  // A page loses dozens of requests at once; the shell hears the total.
  if (!settle_.IsRunning()) {
    settle_.Start(FROM_HERE, base::Milliseconds(250),
                  base::BindOnce(
                      [](BlockedRequests* self) { self->changed_.Notify(); },
                      base::Unretained(this)));
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(BlockedRequests);

void RegisterContentBlockingPrefs(PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(kSitesPref);
}

void StartContentBlocking(PrefService& shell) {
  ContentBlocker::Get().UsePrefs(shell);
  SetContentBlockingSink(base::BindPostTask(
      content::GetUIThreadTaskRunner({}),
      base::BindRepeating([](const content_blocking::Settings& settings) {
        ContentBlocker::Get().SetSettings(settings);
      })));
}

}  // namespace domicile
