// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/blocker.h"

#include <utility>

#include "base/strings/string_util.h"

namespace domicile::content_blocking {

namespace {

// Brave rewrites only these, since a rewritten POST would lose its body.
bool MayRewrite(std::string_view method) {
  return base::EqualsCaseInsensitiveASCII(method, "GET") ||
         base::EqualsCaseInsensitiveASCII(method, "HEAD") ||
         base::EqualsCaseInsensitiveASCII(method, "OPTIONS");
}

}  // namespace

Blocker::Blocker(std::unique_ptr<Engine> lists, std::unique_ptr<Engine> filters)
    : lists_(std::move(lists)), filters_(std::move(filters)) {}

Blocker::~Blocker() = default;

Decision Blocker::Decide(const Request& request, Mode mode) const {
  if (mode == Mode::kOff) {
    return {};
  }
  Verdict listed = lists_->Check(request);
  // `$important` blocks a site's own content in standard mode too.
  if (listed.important) {
    return listed.redirect
               ? Decision{Decision::Kind::kRedirect, *listed.redirect}
               : Decision{Decision::Kind::kBlock};
  }
  if (mode != Mode::kAggressive &&
      !IsThirdParty(request.url, request.source)) {
    // The site's own content: the lists' match goes, their exception stays
    // for the user's rules to see.
    listed = {.exception = listed.exception};
  }

  Request own = request;
  if (listed.rewritten_url) {
    own.url = *listed.rewritten_url;
  }
  const Verdict mine =
      filters_->Check(own, listed.matched || listed.exception,
                      /*force_exceptions=*/!listed.exception);

  const Verdict merged{
      .matched = listed.matched || mine.matched,
      .important = listed.important || mine.important,
      .exception = listed.exception || mine.exception,
      .redirect = mine.redirect ? mine.redirect : listed.redirect,
      .rewritten_url =
          mine.rewritten_url ? mine.rewritten_url : listed.rewritten_url,
  };
  if (merged.ShouldBlock()) {
    return merged.redirect
               ? Decision{Decision::Kind::kRedirect, *merged.redirect}
               : Decision{Decision::Kind::kBlock};
  }
  if (merged.rewritten_url && *merged.rewritten_url != request.url &&
      MayRewrite(request.method)) {
    return {Decision::Kind::kRewrite, *merged.rewritten_url};
  }
  return {};
}

std::string Blocker::CspDirectives(const Request& request, Mode mode) const {
  if (mode == Mode::kOff) {
    return std::string();
  }
  std::string listed = lists_->CspDirectives(request);
  const std::string mine = filters_->CspDirectives(request);
  if (listed.empty()) {
    return mine;
  }
  if (!mine.empty()) {
    listed += "," + mine;
  }
  return listed;
}

}  // namespace domicile::content_blocking
