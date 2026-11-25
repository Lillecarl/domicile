// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_FILTER_H_
#define CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_FILTER_H_

#include "components/domicile/content_blocking/content_filter.mojom-forward.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace content {
class RenderFrameHost;
}

namespace domicile {

// Answers `frame`'s ContentFilterAgent from ContentBlocker: the cosmetic
// rules for its documents, in the mode of the page the frame is in. A frame
// of the shell, of an extension or (unless configured) of an Electron app
// gets none.
void BindContentFilter(content::RenderFrameHost* frame,
                       mojo::PendingReceiver<mojom::ContentFilter> receiver);

}  // namespace domicile

#endif  // CHROME_BROWSER_DOMICILE_DOMICILE_CONTENT_FILTER_H_
