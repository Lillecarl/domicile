// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#ifndef COMPONENTS_DOMICILE_RENDERER_COSMETIC_FILTERS_H_
#define COMPONENTS_DOMICILE_RENDERER_COSMETIC_FILTERS_H_

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "components/domicile/mojom/cosmetic_filters.mojom.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_frame_observer_tracker.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"

namespace domicile {

// A frame's cosmetic filters, in the renderer: hides what the lists name by
// its elements. The browser sends what to hide before each navigation
// commits (CosmeticFiltering, in domicile_ad_blocking.cc); this puts it in
// the new document as a user stylesheet, so it applies from the first style
// and the page can't override it. See docs/architecture/AD-BLOCKING.md.
//
// The lists' generic rules name classes and ids (`##.ad-banner`), tens of
// thousands of them, so the browser sends only the site's own and the
// generic ones that are not a bare class or id. This then gathers the
// document's classes and ids once it has parsed, once it has loaded, and
// every few seconds for a while after, as pages add ads late, and asks the
// browser which of the generic rules hide them.
//
// The browser also sends the page's scriptlets (`##+js()`), which run at
// document start in the page's world, before its own scripts, and its
// procedural filters (`:has-text()`, `:upward()`), which Brave's
// content_cosmetic.ts applies, unchanged, from a world of their own as the
// page changes.
//
// Every frame has one; only those the browser sends a Hide to do anything.
class CosmeticFilters final
    : public content::RenderFrameObserver,
      public content::RenderFrameObserverTracker<CosmeticFilters>,
      public mojom::CosmeticFilters {
 public:
  // Gives `frame` one. The frame owns it.
  static void Attach(content::RenderFrame* frame);

  // Runs `frame`'s scriptlets, and starts its procedural filters in the
  // isolated world `world_id`, for the document that just started. From
  // ChromeContentRendererClient::RunScriptsAtDocumentStart.
  static void RunAtDocumentStart(content::RenderFrame* frame,
                                 int32_t world_id);

  CosmeticFilters(const CosmeticFilters&) = delete;
  CosmeticFilters& operator=(const CosmeticFilters&) = delete;
  ~CosmeticFilters() override;

  // content::RenderFrameObserver:
  void ReadyToCommitNavigation(
      blink::WebDocumentLoader* document_loader) override;
  void DidCreateDocumentElement() override;
  void DidCreateScriptContext(v8::Local<v8::Context> context,
                              int32_t world_id) override;
  void DidDispatchDOMContentLoadedEvent() override;
  void DidFinishLoad() override;
  void OnDestruct() override;

  // mojom::CosmeticFilters:
  void Hide(const std::vector<std::string>& selectors,
            mojo::PendingAssociatedRemote<mojom::CosmeticFiltersHost> host,
            const std::string& scriptlets,
            const std::vector<std::string>& procedural_filters) override;

 private:
  explicit CosmeticFilters(content::RenderFrame* frame);

  void Bind(mojo::PendingAssociatedReceiver<mojom::CosmeticFilters> receiver);

  // What a Hide sent for the document about to commit.
  struct Next {
    Next();
    Next(Next&&);
    Next& operator=(Next&&);
    ~Next();

    std::vector<std::string> selectors;
    mojo::PendingAssociatedRemote<mojom::CosmeticFiltersHost> host;
    std::string scriptlets;
    std::vector<std::string> procedural_filters;
  };

  // Inserts `selectors` into the document as a user stylesheet.
  void Insert(const std::vector<std::string>& selectors);
  // Inserts `css` into the document as a user stylesheet: content_cosmetic's
  // `cf_worker.injectStylesheet`.
  void InjectStylesheet(const std::string& css);
  // Whether `url` is the document's own site: content_cosmetic's
  // `cf_worker.isFirstPartyUrl`.
  bool IsFirstPartyUrl(const std::string& url);
  void RunScripts(int32_t world_id);
  // Asks the host about the document's classes and ids it has not asked
  // about yet.
  void Gather();
  void Hidden(int document, const std::vector<std::string>& selectors);

  mojo::AssociatedReceiver<mojom::CosmeticFilters> receiver_{this};
  std::optional<Next> next_;
  // The current document's: its selectors until its element exists, and
  // its host.
  std::vector<std::string> pending_;
  mojo::AssociatedRemote<mojom::CosmeticFiltersHost> host_;
  // The current document's, until document start runs them.
  std::string scriptlets_;
  std::vector<std::string> procedural_filters_;
  // The world the procedural filters run in, once a document has had some:
  // Blink makes its context again for each new document, before document
  // start, and each needs `cf_worker`.
  std::optional<int32_t> world_id_;
  // The classes and ids already asked about, as `.name` and `#name`.
  std::set<std::string> asked_;
  // Counts documents, so an answer for an older one is dropped.
  int document_ = 0;
  // Gathers again every few seconds after the load, a few times.
  base::RepeatingTimer again_;
  int gathers_left_ = 0;
  base::WeakPtrFactory<CosmeticFilters> weak_factory_{this};
};

}  // namespace domicile

#endif  // COMPONENTS_DOMICILE_RENDERER_COSMETIC_FILTERS_H_
