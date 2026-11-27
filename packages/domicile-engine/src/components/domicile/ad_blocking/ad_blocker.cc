// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/ad_blocking/ad_blocker.h"

#include <set>
#include <utility>

#include "base/check.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_util.h"
#include "components/domicile/ad_blocking/filter_engine.rs.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"

namespace domicile {

namespace {

using network::mojom::RequestDestination;

rust::Str Str(std::string_view text) {
  return rust::Str(text.data(), text.size());
}

rust::Vec<rust::String> RustStrings(const std::vector<std::string>& strings) {
  rust::Vec<rust::String> out;
  out.reserve(strings.size());
  for (const std::string& each : strings) {
    // Class and id names come from a page, which may hold anything; rust
    // strings must be UTF-8, and a selector that isn't matches no rule.
    if (base::IsStringUTF8(each)) {
      out.push_back(rust::String(each));
    }
  }
  return out;
}

void AddAll(std::set<std::string>& into,
            const rust::Vec<rust::String>& from) {
  for (const rust::String& each : from) {
    into.insert(std::string(each));
  }
}

// Whether `url` is another site's than `page`'s, by registrable domain, as
// cookies count sites.
bool IsThirdParty(const GURL& url, const GURL& page) {
  return !net::registry_controlled_domains::SameDomainOrHost(
      url, page, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

rust::Box<ad_blocking::FilterEngine> EngineOf(
    const std::vector<std::string>& texts) {
  rust::Box<ad_blocking::FilterSetBuilder> builder =
      ad_blocking::new_filter_set();
  for (const std::string& text : texts) {
    // rust::Str throws on a string that is not UTF-8, which the caller
    // checked: a list that is not is left out where it was read.
    CHECK(base::IsStringUTF8(text));
    builder->add_list(Str(text));
  }
  return ad_blocking::build(std::move(builder));
}

}  // namespace

std::string_view AdBlockRequestType(RequestDestination destination) {
  switch (destination) {
    case RequestDestination::kEmpty:
      // fetch() and XMLHttpRequest, and sendBeacon.
      return "xmlhttprequest";
    case RequestDestination::kDocument:
      return "document";
    case RequestDestination::kFrame:
    case RequestDestination::kIframe:
    case RequestDestination::kFencedframe:
      return "subdocument";
    case RequestDestination::kScript:
    case RequestDestination::kWorker:
    case RequestDestination::kSharedWorker:
    case RequestDestination::kServiceWorker:
    case RequestDestination::kAudioWorklet:
    case RequestDestination::kPaintWorklet:
      return "script";
    case RequestDestination::kStyle:
      return "stylesheet";
    case RequestDestination::kImage:
      return "image";
    case RequestDestination::kFont:
      return "font";
    case RequestDestination::kAudio:
    case RequestDestination::kVideo:
    case RequestDestination::kTrack:
      return "media";
    case RequestDestination::kEmbed:
    case RequestDestination::kObject:
      return "object";
    case RequestDestination::kReport:
      return "ping";
    default:
      return "other";
  }
}

std::optional<base::TimeDelta> BlockListExpiry(std::string_view text) {
  CHECK(base::IsStringUTF8(text));
  const uint32_t hours = ad_blocking::list_expires_hours(Str(text));
  if (hours == 0) {
    return std::nullopt;
  }
  return base::Hours(hours);
}

struct AdBlocker::Engines {
  rust::Box<ad_blocking::FilterEngine> lists;
  rust::Box<ad_blocking::FilterEngine> rules;
};

// static
std::unique_ptr<AdBlocker> AdBlocker::Build(
    const std::vector<std::string>& lists,
    const std::vector<std::string>& rules,
    std::string_view resources) {
  std::string own;
  for (const std::string& rule : rules) {
    own += rule;
    own += '\n';
  }
  auto engines = std::make_unique<Engines>(
      Engines{.lists = EngineOf(lists), .rules = EngineOf({own})});
  // Each engine keeps its own copy: adblock-rust has no way to share one.
  // Not UTF-8 is not adblock-rust's JSON either.
  const bool has_resources =
      !resources.empty() && base::IsStringUTF8(resources) &&
      engines->lists->use_resources(Str(resources)) &&
      engines->rules->use_resources(Str(resources));
  return base::WrapUnique(new AdBlocker(std::move(engines), has_resources));
}

AdBlocker::Cosmetics::Cosmetics() = default;
AdBlocker::Cosmetics::Cosmetics(Cosmetics&&) = default;
AdBlocker::Cosmetics& AdBlocker::Cosmetics::operator=(Cosmetics&&) = default;
AdBlocker::Cosmetics::~Cosmetics() = default;

AdBlocker::AdBlocker(std::unique_ptr<Engines> engines, bool has_resources)
    : engines_(std::move(engines)), has_resources_(has_resources) {}

AdBlocker::~AdBlocker() = default;

bool AdBlocker::ShouldBlock(const GURL& url,
                            const GURL& page,
                            std::string_view type,
                            std::string_view method,
                            bool block_first_party) const {
  return Decide(url, page, type, method, block_first_party).blocks();
}

AdBlocker::Decision AdBlocker::Decide(const GURL& url,
                                      const GURL& page,
                                      std::string_view type,
                                      std::string_view method,
                                      bool block_first_party) const {
  const bool third_party = IsThirdParty(url, page);
  const auto check = [&](const ad_blocking::FilterEngine& engine) {
    return engine.check(Str(url.spec()), Str(url.host()), Str(page.host()),
                        Str(type), third_party, Str(method));
  };
  const ad_blocking::Verdict mine = check(*engines_->rules);
  const ad_blocking::Verdict listed = check(*engines_->lists);
  const auto blocked = [](const ad_blocking::Verdict& verdict) {
    if (verdict.redirect.empty()) {
      return Decision{.kind = Decision::Kind::kBlock};
    }
    return Decision{.kind = Decision::Kind::kRedirect,
                    .url = GURL(std::string(verdict.redirect))};
  };
  const auto rewritten = [&](const ad_blocking::Verdict& verdict) {
    const GURL to(std::string(verdict.rewritten_url));
    // A redirect would send a POST's body again, and the network has not
    // seen the request to know it may.
    if (verdict.rewritten_url.empty() || !to.is_valid() || to == url ||
        !(method == "GET" || method == "HEAD")) {
      return Decision{};
    }
    return Decision{.kind = Decision::Kind::kRewrite, .url = to};
  };
  if (mine.important) {
    return blocked(mine);
  }
  if (listed.important) {
    return blocked(listed);
  }
  if (mine.exception) {
    return rewritten(mine);
  }
  if (mine.matched) {
    return blocked(mine);
  }
  const bool lists_apply = third_party || block_first_party;
  if (!lists_apply) {
    return rewritten(mine);
  }
  if (listed.matched && !listed.exception) {
    return blocked(listed);
  }
  const Decision own = rewritten(mine);
  return own.kind == Decision::Kind::kRewrite ? own : rewritten(listed);
}

std::string AdBlocker::CspDirectives(const GURL& url,
                                     const GURL& page,
                                     std::string_view type,
                                     std::string_view method) const {
  const bool third_party = IsThirdParty(url, page);
  const auto csp = [&](const ad_blocking::FilterEngine& engine) {
    return std::string(engine.csp_directives(Str(url.spec()), Str(url.host()),
                                             Str(page.host()), Str(type),
                                             third_party, Str(method)));
  };
  const std::string listed = csp(*engines_->lists);
  const std::string mine = csp(*engines_->rules);
  if (listed.empty() || mine.empty()) {
    return listed + mine;
  }
  // Two policies in one header, each enforced, as two headers would be.
  return listed + "," + mine;
}

AdBlocker::Cosmetics AdBlocker::CosmeticsFor(const GURL& page) const {
  Cosmetics cosmetics;
  if (!page.SchemeIsHTTPOrHTTPS()) {
    return cosmetics;
  }
  const ad_blocking::Cosmetics mine = engines_->rules->cosmetics(
      Str(page.spec()));
  const ad_blocking::Cosmetics listed = engines_->lists->cosmetics(
      Str(page.spec()));
  std::set<std::string> hide;
  AddAll(hide, mine.hide_selectors);
  AddAll(hide, listed.hide_selectors);
  std::set<std::string> exceptions;
  AddAll(exceptions, mine.exceptions);
  AddAll(exceptions, listed.exceptions);
  cosmetics.hide_selectors.assign(hide.begin(), hide.end());
  cosmetics.exceptions.assign(exceptions.begin(), exceptions.end());
  cosmetics.generichide = mine.generichide || listed.generichide;
  // The lists' first, so the user's can wrap what they set up.
  cosmetics.scriptlets =
      std::string(listed.injected_script) + std::string(mine.injected_script);
  std::set<std::string> procedural;
  AddAll(procedural, mine.procedural_actions);
  AddAll(procedural, listed.procedural_actions);
  cosmetics.procedural_filters.assign(procedural.begin(), procedural.end());
  return cosmetics;
}

std::vector<std::string> AdBlocker::HiddenClassIdSelectors(
    const std::vector<std::string>& classes,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& exceptions) const {
  const rust::Vec<rust::String> rust_classes = RustStrings(classes);
  const rust::Vec<rust::String> rust_ids = RustStrings(ids);
  const rust::Vec<rust::String> rust_exceptions = RustStrings(exceptions);
  std::set<std::string> selectors;
  AddAll(selectors, engines_->rules->hidden_class_id_selectors(
                        rust_classes, rust_ids, rust_exceptions));
  AddAll(selectors, engines_->lists->hidden_class_id_selectors(
                        rust_classes, rust_ids, rust_exceptions));
  return std::vector<std::string>(selectors.begin(), selectors.end());
}

}  // namespace domicile
