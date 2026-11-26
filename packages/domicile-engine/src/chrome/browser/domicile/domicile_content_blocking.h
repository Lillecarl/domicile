// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_BLOCKING_H_
#define CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_BLOCKING_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "components/domicile/content_blocking/blocker.h"
#include "components/domicile/content_blocking/lists.h"
#include "components/domicile/content_blocking/settings.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

class GURL;
class PrefRegistrySimple;
class PrefService;

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

namespace content {
class Page;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace network {
class SimpleURLLoader;
}

namespace domicile {

// The engine's own ad and tracker blocking, for every profile, as the
// compositor's `content_blocking` sets it. See
// docs/architecture/CONTENT-BLOCKING.md.
class ContentBlocker {
 public:
  static ContentBlocker& Get();

  ContentBlocker(const ContentBlocker&) = delete;
  ContentBlocker& operator=(const ContentBlocker&) = delete;

  void SetSettings(const content_blocking::Settings& settings);

  // Reads and keeps the user's own site modes in `prefs`, the shell
  // profile's.
  void UsePrefs(PrefService& prefs);

  // The mode `page`'s site has, for the shell: kOff while nothing blocks.
  content_blocking::Mode ModeForShell(const GURL& page) const;

  // Sets `site`'s mode, kept in the shell profile; nullopt removes it. An
  // error for a site that is no host or `*.host`, one the config's sites
  // name, and while the config blocks nothing.
  base::expected<void, std::string> SetSite(
      const std::string& site,
      std::optional<content_blocking::Mode> mode);

  // Null while nothing blocks: before the first engines are built, and while
  // blocking is off.
  const content_blocking::Blocker* blocker() const { return blocker_.get(); }

  bool Enabled() const { return settings_.enabled; }

  // The mode for a page whose top-level document is at `page`.
  content_blocking::Mode ModeFor(const GURL& page) const;

  bool FiltersElectronApps() const { return settings_.electron_apps; }

 private:
  friend class base::NoDestructor<ContentBlocker>;
  ContentBlocker();
  ~ContentBlocker();

  // One list or the resources: its text, and when it was fetched.
  struct Source {
    std::string text;
    base::Time fetched;
    bool read = false;
  };

  void ReadCached(uint64_t generation);
  void Fetch(uint64_t generation, const std::string& source);
  void Fetched(uint64_t generation,
               std::string source,
               std::unique_ptr<network::SimpleURLLoader> loader,
               std::optional<std::string> body);
  void BuildWhenRead(uint64_t generation);
  void Built(uint64_t generation, content_blocking::Built built);
  void ArmRefresh();
  void Refresh();
  // `settings_` with the user's sites, which ModeFor reads.
  void Merge();

  base::FilePath CacheDirectory() const;

  // The config's, as the compositor sent them.
  content_blocking::Settings settings_;
  // With the user's sites below the config's.
  content_blocking::Settings merged_;
  raw_ptr<PrefService> prefs_ = nullptr;
  // Bumped on every SetSettings, so work for older settings is dropped.
  uint64_t generation_ = 0;
  std::map<std::string, Source> sources_;
  std::unique_ptr<content_blocking::Blocker> blocker_;
  base::OneShotTimer refresh_;
  base::WeakPtrFactory<ContentBlocker> weak_factory_{this};
};

// The requests ContentBlocker blocked for one tab's current page, for the
// shell's `blockedRequests`. Starts again at each page the tab commits.
class BlockedRequests final
    : public content::WebContentsUserData<BlockedRequests>,
      public content::WebContentsObserver {
 public:
  // Counts one more for the page `frame` is in.
  static void Count(content::RenderFrameHost& frame);

  // How many `contents`' page has had blocked.
  static uint32_t Of(content::WebContents& contents);

  ~BlockedRequests() override;

  // `changed` runs after the count moves, at most every quarter second.
  base::CallbackListSubscription Watch(base::RepeatingClosure changed);

 private:
  friend class content::WebContentsUserData<BlockedRequests>;
  explicit BlockedRequests(content::WebContents* contents);

  // content::WebContentsObserver:
  void PrimaryPageChanged(content::Page& page) override;

  void Moved();

  uint32_t count_ = 0;
  base::OneShotTimer settle_;
  base::RepeatingClosureList changed_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

// Hands the compositor's `content_blocking` to ContentBlocker, which keeps the
// user's site modes in `shell`'s prefs. Once, on the UI thread, before any
// control channel binds.
void StartContentBlocking(PrefService& shell);

// The shell profile's pref for the user's site modes.
void RegisterContentBlockingPrefs(PrefRegistrySimple* registry);

}  // namespace domicile

#endif  // CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_BLOCKING_H_
