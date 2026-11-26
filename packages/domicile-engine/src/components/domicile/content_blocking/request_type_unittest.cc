// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/request_type.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace domicile::content_blocking {
namespace {

using network::mojom::RequestDestination;

TEST(ContentBlockingRequestTypeTest, DestinationsHaveTheirFilterTypes) {
  EXPECT_EQ(RequestTypeFor(RequestDestination::kIframe, false),
            RequestType::kSubdocument);
  EXPECT_EQ(RequestTypeFor(RequestDestination::kWorker, false),
            RequestType::kScript);
  EXPECT_EQ(RequestTypeFor(RequestDestination::kStyle, false),
            RequestType::kStylesheet);
  EXPECT_EQ(RequestTypeFor(RequestDestination::kVideo, false),
            RequestType::kMedia);
  EXPECT_EQ(RequestTypeFor(RequestDestination::kEmbed, false),
            RequestType::kObject);
}

TEST(ContentBlockingRequestTypeTest, AFetchIsAnXhrAndABeaconAPing) {
  EXPECT_EQ(RequestTypeFor(RequestDestination::kEmpty, false),
            RequestType::kXmlHttpRequest);
  EXPECT_EQ(RequestTypeFor(RequestDestination::kEmpty, true),
            RequestType::kPing);
}

}  // namespace
}  // namespace domicile::content_blocking
