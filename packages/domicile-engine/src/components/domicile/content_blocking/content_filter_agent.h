// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_CONTENT_BLOCKING_CONTENT_FILTER_AGENT_H_
#define COMPONENTS_DOMICILE_CONTENT_BLOCKING_CONTENT_FILTER_AGENT_H_

#include <set>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "components/domicile/content_blocking/content_filter.mojom.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_frame_observer_tracker.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace content {
class RenderFrame;
}

namespace domicile {

// Hides what the config's lists hide in one frame, and runs their scriptlets.
// See "Element hiding and scriptlets" in docs/architecture/CONTENT-BLOCKING.md.
//
// - At document start: asks the browser for the document's rules (sync),
//   runs the scriptlets in the page's main world before its own scripts, and
//   hides the site's selectors with a user stylesheet.
// - At DOMContentLoaded and again at load: asks for the generic selectors the
//   document's classes and ids call for, and hides those too.
//
// Not done: procedural filters (`:has-text()` and the rest) and rules for
// elements added after load.
class ContentFilterAgent final
    : public content::RenderFrameObserver,
      public content::RenderFrameObserverTracker<ContentFilterAgent> {
 public:
  // Gives `frame` an agent, which it owns.
  static void Attach(content::RenderFrame* frame);

  // `frame`'s document is about to run its first script.
  static void RunAtDocumentStart(content::RenderFrame* frame);

  ContentFilterAgent(const ContentFilterAgent&) = delete;
  ContentFilterAgent& operator=(const ContentFilterAgent&) = delete;

 private:
  explicit ContentFilterAgent(content::RenderFrame* frame);
  ~ContentFilterAgent() override;

  // content::RenderFrameObserver:
  void DidCommitProvisionalLoad(ui::PageTransition transition) override;
  void DidDispatchDOMContentLoadedEvent() override;
  void DidFinishLoad() override;
  void OnDestruct() override;

  void AtDocumentStart();
  void AskForGenericSelectors();
  void Hide(const std::vector<std::string>& selectors);
  mojom::ContentFilter& Filter();

  mojo::Remote<mojom::ContentFilter> filter_;
  // This document's rules, while it has any.
  bool active_ = false;
  bool generichide_ = false;
  std::vector<std::string> exceptions_;
  // The classes and ids already asked about.
  std::set<std::string> seen_classes_;
  std::set<std::string> seen_ids_;
  base::WeakPtrFactory<ContentFilterAgent> weak_factory_{this};
};

}  // namespace domicile

#endif  // COMPONENTS_DOMICILE_CONTENT_BLOCKING_CONTENT_FILTER_AGENT_H_
