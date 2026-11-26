// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_CONTENT_BLOCKING_ENGINE_H_
#define COMPONENTS_DOMICILE_CONTENT_BLOCKING_ENGINE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "components/domicile/content_blocking/bridge.rs.h"
#include "third_party/rust/cxx/v1/cxx.h"
#include "url/gurl.h"

namespace domicile::content_blocking {

// A request's kind, as filter options name it (`$script`, `$image`, ...).
enum class RequestType {
  kDocument,
  kSubdocument,
  kScript,
  kStylesheet,
  kImage,
  kFont,
  kMedia,
  kObject,
  kXmlHttpRequest,
  kWebSocket,
  kPing,
  kOther,
};

// The name adblock-rust gives `type`.
std::string_view RequestTypeName(RequestType type);

struct Request {
  GURL url;
  // The page the request is for: its top-level document. First and third
  // party, `$domain` and `$csp` are measured from it.
  GURL source;
  RequestType type = RequestType::kOther;
  std::string method = "GET";
};

// Whether `url` is on another site than `source`, private registries such as
// github.io counting as sites.
bool IsThirdParty(const GURL& url, const GURL& source);

struct Verdict {
  bool matched = false;
  bool important = false;
  bool exception = false;
  // A `data:` URL to answer with instead, from `$redirect` or
  // `$redirect-rule`. It does not mean the request is blocked.
  std::optional<GURL> redirect;
  // The URL with the parameters `$removeparam` removes.
  std::optional<GURL> rewritten_url;

  bool ShouldBlock() const { return important || (matched && !exception); }
};

// One adblock-rust engine. Not thread-safe: it lives on one sequence.
class Engine {
 public:
  // The rules of every list in `lists`. A list that is not UTF-8 adds none.
  static std::unique_ptr<Engine> FromLists(
      const std::vector<std::string>& lists);
  // An engine `Serialize` wrote, or null when `data` is not one.
  static std::unique_ptr<Engine> FromSerialized(base::span<const uint8_t> data);

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine();

  // Empty when the engine cannot be written. Resources are not part of it.
  std::vector<uint8_t> Serialize() const;

  // The scriptlets and stub resources `##+js()` and `$redirect` name, as
  // adblock-rust's JSON list. False, and the old resources kept, when `json`
  // is not one.
  bool UseResources(std::string_view json);

  // `previously_matched` skips the blocking rules and looks for exceptions
  // only, for a request another engine already matched. `force_exceptions`
  // reports a matching exception even when no rule blocks.
  Verdict Check(const Request& request,
                bool previously_matched = false,
                bool force_exceptions = true) const;

  // The directives `$csp` adds to a document's response, comma-separated, or
  // empty.
  std::string CspDirectives(const Request& request) const;

  // adblock-rust's `UrlSpecificResources` for a page at `url`, as JSON: its
  // hide selectors, procedural actions, exceptions, scriptlets and
  // `generichide`.
  std::string CosmeticResources(const GURL& url) const;

  // The generic hide selectors that `classes` and `ids` on a page call for,
  // less `exceptions` from `CosmeticResources`.
  std::vector<std::string> HiddenClassIdSelectors(
      const std::vector<std::string>& classes,
      const std::vector<std::string>& ids,
      const std::vector<std::string>& exceptions) const;

 private:
  explicit Engine(::rust::Box<rust::Engine> engine);

  ::rust::Box<rust::Engine> engine_;
};

// A list's `! Expires:` in hours, or nullopt when it names none.
std::optional<int> ListExpiresHours(std::string_view list);

}  // namespace domicile::content_blocking

#endif  // COMPONENTS_DOMICILE_CONTENT_BLOCKING_ENGINE_H_
