// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_CONTENT_BLOCKING_CONTENT_FILTER_AGENT_H_
#define COMPONENTS_DOMICILE_CONTENT_BLOCKING_CONTENT_FILTER_AGENT_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "components/domicile/content_blocking/content_filter.mojom.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_frame_observer_tracker.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "url/gurl.h"
#include "v8/include/v8-forward.h"

namespace content {
class RenderFrame;
}

namespace domicile {

// Hides what the config's lists hide in one frame, and runs their scriptlets,
// in Brave's order (CosmeticFiltersJSHandler::ApplyRules). See "Element
// hiding and scriptlets" in docs/architecture/CONTENT-BLOCKING.md.
//
// When a document is about to run its first script, the agent asks the
// browser for its rules (sync), then:
// - runs the scriptlets in the page's main world, before its own scripts;
// - in its isolated world, sets Brave's flags and procedural filters, hides
//   the user's selectors with a user stylesheet and the lists' with the sheet
//   standard mode may take a selector back from, and runs Brave's
//   content_cosmetic.ts, which watches the document's class and id names and
//   asks for generic rules through `cf_worker`.
class ContentFilterAgent final
    : public content::RenderFrameObserver,
      public content::RenderFrameObserverTracker<ContentFilterAgent> {
 public:
  // Gives `frame` an agent, which it owns and which runs its scripts in
  // `world_id`.
  static void Attach(content::RenderFrame* frame, int32_t world_id);

  // `frame`'s document is about to run its first script.
  static void RunAtDocumentStart(content::RenderFrame* frame);

  ContentFilterAgent(const ContentFilterAgent&) = delete;
  ContentFilterAgent& operator=(const ContentFilterAgent&) = delete;

 private:
  ContentFilterAgent(content::RenderFrame* frame, int32_t world_id);
  ~ContentFilterAgent() override;

  // content::RenderFrameObserver:
  void DidCommitProvisionalLoad(ui::PageTransition transition) override;
  void DidCreateScriptContext(v8::Local<v8::Context> context,
                              int32_t world_id) override;
  void OnDestruct() override;

  void AtDocumentStart();
  void RunInWorld(const std::string& script);
  // Hides `selectors`: forced with a user stylesheet, or through the sheet
  // standard mode may take a selector back from.
  void Hide(const std::vector<std::string>& selectors, bool force);
  mojom::ContentFilter& Filter();

  // `cf_worker`, as content_cosmetic.ts calls it.
  void HiddenClassIdSelectors(const std::string& json);
  bool IsFirstPartyUrl(const std::string& url);
  void InjectStylesheet(const std::string& css);

  const int32_t world_id_;
  mojo::Remote<mojom::ContentFilter> filter_;
  // This document's, while it has rules.
  GURL document_;
  bool active_ = false;
  bool hide_first_party_ = false;
  std::vector<std::string> exceptions_;
  base::WeakPtrFactory<ContentFilterAgent> weak_factory_{this};
};

}  // namespace domicile

#endif  // COMPONENTS_DOMICILE_CONTENT_BLOCKING_CONTENT_FILTER_AGENT_H_
