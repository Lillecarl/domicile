// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/domain_resolver.h"

#include <string>

#include "components/domicile/content_blocking/bridge.rs.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace domicile::content_blocking::rust {

DomainPosition resolve_domain_position(const std::string& host) {
  const std::string domain =
      net::registry_controlled_domains::GetDomainAndRegistry(
          host, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  const size_t start = domain.empty() ? std::string::npos : host.rfind(domain);
  if (start == std::string::npos) {
    return {0, static_cast<uint32_t>(host.size())};
  }
  return {static_cast<uint32_t>(start),
          static_cast<uint32_t>(start + domain.size())};
}

}  // namespace domicile::content_blocking::rust
