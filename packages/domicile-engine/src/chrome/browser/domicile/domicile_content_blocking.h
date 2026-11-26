// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_BLOCKING_H_
#define CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_BLOCKING_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/domicile/content_blocking/blocker.h"
#include "components/domicile/content_blocking/settings.h"

class GURL;

namespace network {
class SimpleURLLoader;
}

namespace domicile {

// The engine's own ad and tracker blocking, for every profile. See
// docs/architecture/CONTENT-BLOCKING.md.
//
// The compositor's `content_blocking` sets it. Lists given by address are
// fetched without cookies through the browser's own network context, cached
// in <user-data-dir>/domicile-content-blocking, and fetched again when they
// expire. Engines are built on a worker and used on the UI thread, where
// requests are decided.
class ContentBlocker {
 public:
  static ContentBlocker& Get();

  ContentBlocker(const ContentBlocker&) = delete;
  ContentBlocker& operator=(const ContentBlocker&) = delete;

  void SetSettings(const content_blocking::Settings& settings);

  // Null while nothing blocks: before the first engines are built, and while
  // blocking is off.
  const content_blocking::Blocker* blocker() const { return blocker_.get(); }

  // Whether the config asks for blocking at all.
  bool Enabled() const { return settings_.enabled; }

  // The mode for a page whose top-level document is at `page`.
  content_blocking::Mode ModeFor(const GURL& page) const;

  // Whether Electron apps' pages are filtered too.
  bool FiltersElectronApps() const { return settings_.electron_apps; }

 private:
  friend class base::NoDestructor<ContentBlocker>;
  ContentBlocker();
  ~ContentBlocker();

  // One list or the resources: where it comes from, its cached text, and
  // when it was fetched.
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
  void Built(uint64_t generation,
             std::unique_ptr<content_blocking::Blocker> blocker);
  void ArmRefresh();
  void Refresh();

  base::FilePath CacheDirectory() const;

  content_blocking::Settings settings_;
  // Bumped on every SetSettings, so work for older settings is dropped.
  uint64_t generation_ = 0;
  std::map<std::string, Source> sources_;
  std::unique_ptr<content_blocking::Blocker> blocker_;
  base::OneShotTimer refresh_;
  base::WeakPtrFactory<ContentBlocker> weak_factory_{this};
};

// Hands the compositor's `content_blocking` to ContentBlocker. Once, on the
// UI thread, before any control channel binds.
void StartContentBlocking();

}  // namespace domicile

#endif  // CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_BLOCKING_H_
