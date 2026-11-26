// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_CONTENT_BLOCKING_DOMAIN_RESOLVER_H_
#define COMPONENTS_DOMICILE_CONTENT_BLOCKING_DOMAIN_RESOLVER_H_

#include <string>

namespace domicile::content_blocking::rust {

struct DomainPosition;

// Where `host`'s registrable domain starts and ends in it, private registries
// included, for adblock-rust's `$domain` and `||` matching. A host with no
// registrable domain (`localhost`, an address) is its own domain.
DomainPosition resolve_domain_position(const std::string& host);

}  // namespace domicile::content_blocking::rust

#endif  // COMPONENTS_DOMICILE_CONTENT_BLOCKING_DOMAIN_RESOLVER_H_
