// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/engine.h"

#include <utility>

#include "base/check.h"
#include "base/containers/span_rust.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_view_rust.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace domicile::content_blocking {

namespace {

// adblock-rust keeps one resolver for the process, and refuses a second.
void EnsureDomainResolver() {
  static const bool installed = rust::install_domain_resolver();
  CHECK(installed);
}

::rust::Str Str(std::string_view text) {
  return base::StringViewToRustStrUTF8(text);
}

std::optional<GURL> UrlOrNullopt(const ::rust::String& text) {
  if (text.empty()) {
    return std::nullopt;
  }
  GURL url{std::string(text)};
  if (!url.is_valid()) {
    return std::nullopt;
  }
  return url;
}

}  // namespace

std::string_view RequestTypeName(RequestType type) {
  switch (type) {
    case RequestType::kDocument:
      return "document";
    case RequestType::kSubdocument:
      return "sub_frame";
    case RequestType::kScript:
      return "script";
    case RequestType::kStylesheet:
      return "stylesheet";
    case RequestType::kImage:
      return "image";
    case RequestType::kFont:
      return "font";
    case RequestType::kMedia:
      return "media";
    case RequestType::kObject:
      return "object";
    case RequestType::kXmlHttpRequest:
      return "xmlhttprequest";
    case RequestType::kWebSocket:
      return "websocket";
    case RequestType::kPing:
      return "ping";
    case RequestType::kOther:
      return "other";
  }
}

bool IsThirdParty(const GURL& url, const GURL& source) {
  return !net::registry_controlled_domains::SameDomainOrHost(
      url, source,
      net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

// static
std::unique_ptr<Engine> Engine::FromLists(
    const std::vector<std::string>& lists) {
  EnsureDomainResolver();
  ::rust::Box<rust::FilterSet> set = rust::new_filter_set();
  for (const std::string& list : lists) {
    set->add_list(Str(list));
  }
  return base::WrapUnique(
      new Engine(rust::engine_from_filter_set(std::move(set))));
}

// static
std::unique_ptr<Engine> Engine::FromSerialized(
    base::span<const uint8_t> data) {
  EnsureDomainResolver();
  ::rust::Box<rust::Engine> engine = rust::new_engine();
  if (!engine->deserialize(base::SpanToRustSlice(data))) {
    return nullptr;
  }
  return base::WrapUnique(new Engine(std::move(engine)));
}

Engine::Engine(::rust::Box<rust::Engine> engine) : engine_(std::move(engine)) {}

Engine::~Engine() = default;

std::vector<uint8_t> Engine::Serialize() const {
  ::rust::Vec<uint8_t> data = engine_->serialize();
  return std::vector<uint8_t>(data.begin(), data.end());
}

bool Engine::UseResources(std::string_view json) {
  return engine_->use_resources(Str(json));
}

Verdict Engine::Check(const Request& request,
                      bool previously_matched,
                      bool force_exceptions) const {
  const rust::Verdict verdict = engine_->check(
      Str(request.url.spec()), Str(request.url.host()),
      Str(request.source.host()), Str(RequestTypeName(request.type)),
      IsThirdParty(request.url, request.source), Str(request.method),
      previously_matched, force_exceptions);
  return {
      .matched = verdict.matched,
      .important = verdict.important,
      .exception = verdict.exception,
      .redirect = UrlOrNullopt(verdict.redirect),
      .rewritten_url = UrlOrNullopt(verdict.rewritten_url),
  };
}

std::string Engine::CspDirectives(const Request& request) const {
  return std::string(engine_->csp_directives(
      Str(request.url.spec()), Str(request.url.host()),
      Str(request.source.host()), Str(RequestTypeName(request.type)),
      IsThirdParty(request.url, request.source), Str(request.method)));
}

std::string Engine::CosmeticResources(const GURL& url) const {
  return std::string(engine_->cosmetic_resources(Str(url.spec())));
}

std::vector<std::string> Engine::HiddenClassIdSelectors(
    const std::vector<std::string>& classes,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& exceptions) const {
  ::rust::Vec<::rust::String> selectors =
      engine_->hidden_class_id_selectors(classes, ids, exceptions);
  std::vector<std::string> result;
  result.reserve(selectors.size());
  for (const ::rust::String& selector : selectors) {
    result.emplace_back(selector);
  }
  return result;
}

std::optional<int> ListExpiresHours(std::string_view list) {
  const uint32_t hours = rust::list_expires_hours(Str(list));
  if (hours == 0) {
    return std::nullopt;
  }
  return static_cast<int>(hours);
}

}  // namespace domicile::content_blocking
