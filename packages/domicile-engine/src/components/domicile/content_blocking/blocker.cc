// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/blocker.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/values.h"

namespace domicile::content_blocking {

namespace {

// Brave rewrites only these, since a rewritten POST would lose its body.
bool MayRewrite(std::string_view method) {
  return base::EqualsCaseInsensitiveASCII(method, "GET") ||
         base::EqualsCaseInsensitiveASCII(method, "HEAD") ||
         base::EqualsCaseInsensitiveASCII(method, "OPTIONS");
}

// adblock-rust's `UrlSpecificResources` JSON, or an empty dict.
base::DictValue Resources(std::string_view json) {
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  return parsed ? std::move(*parsed) : base::DictValue();
}

std::vector<std::string> Strings(const base::DictValue& resources,
                                 std::string_view key) {
  std::vector<std::string> strings;
  if (const base::ListValue* list = resources.FindList(key)) {
    for (const base::Value& value : *list) {
      if (value.is_string()) {
        strings.push_back(value.GetString());
      }
    }
  }
  return strings;
}

// Whether a procedural or action filter is a single CSS selector, which
// standard mode keeps (Brave's StripProceduralFilters).
bool IsPlainSelectorFilter(std::string_view json) {
  std::optional<base::DictValue> filter =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  const base::ListValue* selector = filter ? filter->FindList("selector")
                                           : nullptr;
  if (selector == nullptr || selector->size() != 1 ||
      !selector->front().is_dict()) {
    return false;
  }
  const std::string* type = selector->front().GetDict().FindString("type");
  return type != nullptr && *type == "css-selector";
}

void AddScript(const base::DictValue& resources, std::string& into) {
  const std::string* script = resources.FindString("injected_script");
  if (script == nullptr || script->empty()) {
    return;
  }
  if (!into.empty()) {
    into += "\n";
  }
  into += *script;
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

std::optional<CosmeticRules> Blocker::Cosmetic(const GURL& document,
                                               Mode mode) const {
  if (mode == Mode::kOff) {
    return std::nullopt;
  }
  const bool aggressive = mode == Mode::kAggressive;
  const base::DictValue listed = Resources(lists_->CosmeticResources(document));
  const base::DictValue own = Resources(filters_->CosmeticResources(document));
  CosmeticRules rules{.hide_first_party = aggressive};
  for (std::string& selector : Strings(listed, "hide_selectors")) {
    // Brave keeps the lists' `:has()` out of standard mode.
    if (aggressive || selector.find(":has(") == std::string::npos) {
      rules.hide_selectors.push_back(std::move(selector));
    }
  }
  rules.force_hide_selectors = Strings(own, "hide_selectors");
  for (std::string& filter : Strings(listed, "procedural_actions")) {
    if (aggressive || IsPlainSelectorFilter(filter)) {
      rules.procedural_actions.push_back(std::move(filter));
    }
  }
  for (std::string& filter : Strings(own, "procedural_actions")) {
    rules.procedural_actions.push_back(std::move(filter));
  }
  AddScript(listed, rules.injected_script);
  AddScript(own, rules.injected_script);
  rules.generichide = listed.FindBool("generichide").value_or(false) ||
                      own.FindBool("generichide").value_or(false);
  rules.exceptions = Strings(listed, "exceptions");
  for (std::string& exception : Strings(own, "exceptions")) {
    rules.exceptions.push_back(std::move(exception));
  }
  return rules;
}

GenericHide Blocker::GenericSelectors(
    const std::vector<std::string>& classes,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& exceptions) const {
  return {
      .hide_selectors =
          lists_->HiddenClassIdSelectors(classes, ids, exceptions),
      .force_hide_selectors =
          filters_->HiddenClassIdSelectors(classes, ids, exceptions),
  };
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
