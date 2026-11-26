// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_CONTENT_BLOCKING_BLOCKER_H_
#define COMPONENTS_DOMICILE_CONTENT_BLOCKING_BLOCKER_H_

#include <memory>
#include <string>

#include "components/domicile/content_blocking/engine.h"
#include "components/domicile/content_blocking/settings.h"
#include "url/gurl.h"

namespace domicile::content_blocking {

// What to do with one request.
struct Decision {
  enum class Kind {
    kAllow,
    kBlock,
    // Answer with `url`, a `data:` URL, instead of loading.
    kRedirect,
    // Load `url`, the request's URL less what `$removeparam` removes.
    kRewrite,
  };
  Kind kind = Kind::kAllow;
  GURL url;

  friend bool operator==(const Decision&, const Decision&) = default;
};

// The two engines the browser asks, as Brave's AdBlockEngineWrapper keeps
// them: `lists` from the config's lists, `filters` from the user's own rules.
// Standard mode drops `lists`' first-party matches but keeps its exceptions,
// and `filters` decides after it. Lives on the blocking sequence.
class Blocker {
 public:
  Blocker(std::unique_ptr<Engine> lists, std::unique_ptr<Engine> filters);
  Blocker(const Blocker&) = delete;
  Blocker& operator=(const Blocker&) = delete;
  ~Blocker();

  // `mode` is the page's, from ModeFor.
  Decision Decide(const Request& request, Mode mode) const;

  // The directives both engines' `$csp` rules add to a document, joined with
  // commas; empty for none or in kOff.
  std::string CspDirectives(const Request& request, Mode mode) const;

  Engine& lists() { return *lists_; }
  Engine& filters() { return *filters_; }
  const Engine& lists() const { return *lists_; }
  const Engine& filters() const { return *filters_; }

 private:
  std::unique_ptr<Engine> lists_;
  std::unique_ptr<Engine> filters_;
};

}  // namespace domicile::content_blocking

#endif  // COMPONENTS_DOMICILE_CONTENT_BLOCKING_BLOCKER_H_
