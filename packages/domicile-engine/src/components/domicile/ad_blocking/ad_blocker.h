// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_AD_BLOCKING_AD_BLOCKER_H_
#define COMPONENTS_DOMICILE_AD_BLOCKING_AD_BLOCKER_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "url/gurl.h"

namespace domicile {

// adblock-rust's name for a request's type, as filter options such as
// `$script` and `$image` name it, from what the request fetches.
std::string_view AdBlockRequestType(network::mojom::RequestDestination);

// How long a filter list says to keep it before it is fetched again, by its
// `! Expires:` (`4 days`, `12 hours`), or nothing when it names none or one
// adblock-rust does not read. `text` is valid UTF-8.
std::optional<base::TimeDelta> BlockListExpiry(std::string_view text);

// Blocks ads and trackers as Brave does, with Brave's filter engine
// (adblock-rust) over filter lists in Adblock Plus and uBlock Origin syntax.
// See docs/architecture/AD-BLOCKING.md.
//
// Two engines, as Brave keeps them: one for the lists and one for the user's
// own rules, so a mode that lets a site's own requests through can still
// apply the user's rules to them, and a rule of the user's can make an
// exception to a list's.
//
// Immutable once built. Building is slow (tens of milliseconds for EasyList,
// EasyPrivacy and uBlock Origin's filters), so it is built on a worker and
// asked where requests start.
class AdBlocker {
 public:
  // What to do with one request. See Decide.
  struct Decision {
    enum class Kind {
      kAllow,
      kBlock,
      // Answer with `url`, a `data:` URL of a `$redirect` rule's stand-in,
      // instead of loading.
      kRedirect,
      // Load `url`, the request's less what `$removeparam` rules remove.
      kRewrite,
    };

    // Whether the request is kept from the network.
    bool blocks() const {
      return kind == Kind::kBlock || kind == Kind::kRedirect;
    }

    friend bool operator==(const Decision&, const Decision&) = default;

    Kind kind = Kind::kAllow;
    GURL url;
  };

  // What a page at one address hides by its elements (cosmetic filters, as
  // `##.ad` and `example.com##.promo`), before its classes and ids are known.
  struct Cosmetics {
    Cosmetics();
    Cosmetics(Cosmetics&&);
    Cosmetics& operator=(Cosmetics&&);
    ~Cosmetics();

    // CSS selectors to hide, from the lists and the user's rules: the site's
    // own and the generic ones that aren't a bare class or id. In order,
    // without repeats.
    std::vector<std::string> hide_selectors;
    // Class and id selectors (`.ad`) the site's rules except from the
    // generic ones, for HiddenClassIdSelectors.
    std::vector<std::string> exceptions;
    // A `$generichide` exception matched: no generic rule applies, so the
    // page's classes and ids need no asking about.
    bool generichide = false;
  };

  // `lists` are filter lists' texts, each valid UTF-8; `rules` the user's own
  // rules, one per entry; `resources` the scriptlets and stand-in files the
  // rules name, as adblock-rust's JSON, or empty for none.
  static std::unique_ptr<AdBlocker> Build(const std::vector<std::string>& lists,
                                          const std::vector<std::string>& rules,
                                          std::string_view resources);

  AdBlocker(const AdBlocker&) = delete;
  AdBlocker& operator=(const AdBlocker&) = delete;
  ~AdBlocker();

  // Whether the engines took the resources Build was given: false for none,
  // and for a text that is not adblock-rust's resource list.
  bool has_resources() const { return has_resources_; }

  // Whether to block a `method` request for `url`, of AdBlockRequestType
  // `type`, from the page at `page`, the top-level document's address.
  //
  // A request to another site than the page's (by registrable domain, as
  // cookies count sites) is blocked when a list or the user's rules name it.
  // One to the page's own site is blocked when the user's rules name it, a
  // list's `$important` rule does, or `block_first_party` (Strict) is set and
  // a list does. No exception lifts an `$important` rule; the user's
  // exceptions lift the lists' other rules.
  bool ShouldBlock(const GURL& url,
                   const GURL& page,
                   std::string_view type,
                   std::string_view method,
                   bool block_first_party) const;

  // ShouldBlock's verdict, with what a blocking rule's `$redirect` answers
  // instead (when the blocker has resources) and what an allowed request's
  // `$removeparam` rules remove. A list's `$removeparam` applies where a
  // list's block would; the user's everywhere. Only a GET or HEAD is
  // rewritten.
  Decision Decide(const GURL& url,
                  const GURL& page,
                  std::string_view type,
                  std::string_view method,
                  bool block_first_party) const;

  // The `Content-Security-Policy` the lists' and the user's `$csp` rules add
  // to the response for `url`, a document or frame of AdBlockRequestType
  // `type` in the page at `page`, or empty for none. In every mode that
  // blocks, as Brave adds it.
  std::string CspDirectives(const GURL& url,
                            const GURL& page,
                            std::string_view type,
                            std::string_view method) const;

  // What the page at `page` hides. See docs/architecture/AD-BLOCKING.md.
  Cosmetics CosmeticsFor(const GURL& page) const;

  // The generic rules' selectors for a page's `classes` and `ids`, without
  // their `.` or `#`, but those in `exceptions` (Cosmetics::exceptions). In
  // order, without repeats.
  std::vector<std::string> HiddenClassIdSelectors(
      const std::vector<std::string>& classes,
      const std::vector<std::string>& ids,
      const std::vector<std::string>& exceptions) const;

 private:
  struct Engines;
  AdBlocker(std::unique_ptr<Engines> engines, bool has_resources);

  const std::unique_ptr<Engines> engines_;
  const bool has_resources_;
};

}  // namespace domicile

#endif  // COMPONENTS_DOMICILE_AD_BLOCKING_AD_BLOCKER_H_
