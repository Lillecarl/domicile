// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/request_type.h"

#include "base/notreached.h"

namespace domicile::content_blocking {

RequestType RequestTypeFor(network::mojom::RequestDestination destination,
                           bool keepalive) {
  using network::mojom::RequestDestination;
  switch (destination) {
    case RequestDestination::kDocument:
      return RequestType::kDocument;
    case RequestDestination::kFrame:
    case RequestDestination::kIframe:
    case RequestDestination::kFencedframe:
      return RequestType::kSubdocument;
    case RequestDestination::kScript:
    case RequestDestination::kWorker:
    case RequestDestination::kSharedWorker:
    case RequestDestination::kServiceWorker:
    case RequestDestination::kAudioWorklet:
    case RequestDestination::kPaintWorklet:
    case RequestDestination::kSpeculationRules:
      return RequestType::kScript;
    case RequestDestination::kStyle:
    case RequestDestination::kXslt:
      return RequestType::kStylesheet;
    case RequestDestination::kImage:
      return RequestType::kImage;
    case RequestDestination::kFont:
      return RequestType::kFont;
    case RequestDestination::kAudio:
    case RequestDestination::kVideo:
    case RequestDestination::kTrack:
      return RequestType::kMedia;
    case RequestDestination::kObject:
    case RequestDestination::kEmbed:
      return RequestType::kObject;
    case RequestDestination::kReport:
      return RequestType::kPing;
    case RequestDestination::kEmpty:
    case RequestDestination::kJson:
    case RequestDestination::kText:
      return keepalive ? RequestType::kPing : RequestType::kXmlHttpRequest;
    case RequestDestination::kManifest:
    case RequestDestination::kWebBundle:
    case RequestDestination::kWebIdentity:
    case RequestDestination::kCompressionDictionary:
    case RequestDestination::kEmailVerification:
      return RequestType::kOther;
  }
  NOTREACHED();
}

}  // namespace domicile::content_blocking
