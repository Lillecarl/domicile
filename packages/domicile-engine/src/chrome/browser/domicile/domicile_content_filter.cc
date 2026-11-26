// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "chrome/browser/domicile/domicile_content_filter.h"

#include <memory>
#include <utility>

#include "chrome/browser/domicile/domicile_content_blocking.h"
#include "chrome/browser/domicile/domicile_electron_page.h"
#include "components/domicile/content_blocking/content_filter.mojom.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "url/gurl.h"

namespace domicile {

namespace {

using content_blocking::Mode;

class ContentFilterHost final : public mojom::ContentFilter {
 public:
  explicit ContentFilterHost(content::GlobalRenderFrameHostId frame)
      : frame_(frame) {}

  // mojom::ContentFilter:
  void RulesFor(const GURL& document, RulesForCallback callback) override {
    const content_blocking::Blocker* blocker = ContentBlocker::Get().blocker();
    std::optional<content_blocking::CosmeticRules> rules =
        blocker ? blocker->Cosmetic(document, ModeFor(document))
                : std::nullopt;
    if (!rules) {
      std::move(callback).Run(nullptr);
      return;
    }
    std::move(callback).Run(mojom::CosmeticRules::New(
        std::move(rules->hide_selectors), std::move(rules->injected_script),
        rules->generichide, std::move(rules->exceptions)));
  }

  void GenericSelectors(const std::vector<std::string>& classes,
                        const std::vector<std::string>& ids,
                        const std::vector<std::string>& exceptions,
                        GenericSelectorsCallback callback) override {
    const content_blocking::Blocker* blocker = ContentBlocker::Get().blocker();
    std::move(callback).Run(
        blocker ? blocker->GenericSelectors(classes, ids, exceptions)
                : std::vector<std::string>());
  }

 private:
  // The mode of the page the frame's `document` belongs to. A main frame's
  // document is the page; a frame in it follows the page it is in.
  Mode ModeFor(const GURL& document) const {
    content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_);
    if (frame == nullptr) {
      return Mode::kOff;
    }
    // The frame tree's own main frame, not the outermost one: a browser
    // window's page may sit inside the shell's.
    const GURL page = frame->GetParent() == nullptr
                          ? document
                          : frame->GetMainFrame()->GetLastCommittedURL();
    content::WebContents* contents =
        content::WebContents::FromRenderFrameHost(frame);
    const bool electron =
        contents != nullptr &&
        static_cast<bool>(ElectronPage::SessionOf(*contents));
    const ContentBlocker& blocker = ContentBlocker::Get();
    if (!page.SchemeIsHTTPOrHTTPS() ||
        (electron && !blocker.FiltersElectronApps())) {
      return Mode::kOff;
    }
    return blocker.ModeFor(page);
  }

  const content::GlobalRenderFrameHostId frame_;
};

}  // namespace

void BindContentFilter(content::RenderFrameHost* frame,
                       mojo::PendingReceiver<mojom::ContentFilter> receiver) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<ContentFilterHost>(frame->GetGlobalId()),
      std::move(receiver));
}

}  // namespace domicile
