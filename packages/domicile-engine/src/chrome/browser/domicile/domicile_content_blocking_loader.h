// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_BLOCKING_LOADER_H_
#define CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_BLOCKING_LOADER_H_

namespace content {
class RenderFrameHost;
}

namespace network {
class URLLoaderFactoryBuilder;
}

namespace url {
class Origin;
}

namespace domicile {

// Puts ContentBlocker between `frame`'s loads and the network: a request
// it blocks fails with ERR_BLOCKED_BY_CLIENT, a `$redirect` one is answered
// with its stub, `$removeparam` redirects to the cleaned URL, a redirect the
// server sends is checked as a request, and a document gets its `$csp`. For
// a worker, `frame` is null and `initiator` stands in for the page. See
// "Network requests" in docs/architecture/CONTENT-BLOCKING.md.
void ProxyContentBlocking(content::RenderFrameHost* frame,
                          const url::Origin& initiator,
                          network::URLLoaderFactoryBuilder& builder);

}  // namespace domicile

#endif  // CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_BLOCKING_LOADER_H_
