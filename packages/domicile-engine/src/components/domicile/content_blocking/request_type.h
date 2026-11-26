// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_CONTENT_BLOCKING_REQUEST_TYPE_H_
#define COMPONENTS_DOMICILE_CONTENT_BLOCKING_REQUEST_TYPE_H_

#include "components/domicile/content_blocking/engine.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"

namespace domicile::content_blocking {

// The filter type of a request to `destination`. A `keepalive` request with
// no destination is a beacon or a ping; one without is a fetch or an XHR, as
// Brave counts them.
RequestType RequestTypeFor(network::mojom::RequestDestination destination,
                           bool keepalive);

}  // namespace domicile::content_blocking

#endif  // COMPONENTS_DOMICILE_CONTENT_BLOCKING_REQUEST_TYPE_H_
