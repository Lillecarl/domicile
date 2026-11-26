// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/content_filter_agent.h"

#include <utility>

#include "base/strings/string_split.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "url/gurl.h"

namespace domicile {

namespace {

// Past this many elements a document's classes are not all asked about: the
// generic rules then cover the first ones.
constexpr size_t kMostElements = 20000;

// Whether `selector` stays one selector in `selector{...}`. A list's rule
// with a brace in it would otherwise end the rule and start its own; a valid
// selector needs one only inside an attribute's quotes, and such a rule is
// left out.
bool IsPlainSelector(std::string_view selector) {
  return !selector.empty() && selector.find_first_of("{}") == std::string::npos &&
         selector.find("/*") == std::string::npos;
}

}  // namespace

// static
void ContentFilterAgent::Attach(content::RenderFrame* frame) {
  new ContentFilterAgent(frame);
}

// static
void ContentFilterAgent::RunAtDocumentStart(content::RenderFrame* frame) {
  if (ContentFilterAgent* agent = Get(frame)) {
    agent->AtDocumentStart();
  }
}

ContentFilterAgent::ContentFilterAgent(content::RenderFrame* frame)
    : content::RenderFrameObserver(frame),
      content::RenderFrameObserverTracker<ContentFilterAgent>(frame) {}

ContentFilterAgent::~ContentFilterAgent() = default;

mojom::ContentFilter& ContentFilterAgent::Filter() {
  if (!filter_.is_bound() || !filter_.is_connected()) {
    filter_.reset();
    render_frame()->GetBrowserInterfaceBroker().GetInterface(
        filter_.BindNewPipeAndPassReceiver());
  }
  return *filter_;
}

void ContentFilterAgent::DidCommitProvisionalLoad(ui::PageTransition) {
  active_ = false;
  generichide_ = false;
  exceptions_.clear();
  seen_classes_.clear();
  seen_ids_.clear();
  weak_factory_.InvalidateWeakPtrs();
}

void ContentFilterAgent::AtDocumentStart() {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  const GURL document = frame->GetDocument().Url();
  if (!document.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  mojom::CosmeticRulesPtr rules;
  if (!Filter().RulesFor(document, &rules) || !rules) {
    return;
  }
  active_ = true;
  generichide_ = rules->generichide;
  exceptions_ = std::move(rules->exceptions);
  if (!rules->injected_script.empty()) {
    // In the main world, as the page's own: scriptlets wrap the page's
    // globals. From here a page's CSP does not stop them.
    frame->ExecuteScript(blink::WebScriptSource(
        blink::WebString::FromUtf8(rules->injected_script)));
  }
  Hide(rules->hide_selectors);
}

void ContentFilterAgent::DidDispatchDOMContentLoadedEvent() {
  AskForGenericSelectors();
}

void ContentFilterAgent::DidFinishLoad() {
  AskForGenericSelectors();
}

void ContentFilterAgent::AskForGenericSelectors() {
  if (!active_ || generichide_) {
    return;
  }
  std::vector<std::string> classes;
  std::vector<std::string> ids;
  const std::vector<blink::WebElement> elements =
      render_frame()->GetWebFrame()->GetDocument().QuerySelectorAll(
          blink::WebString::FromASCII("[class],[id]"));
  for (size_t i = 0; i < elements.size() && i < kMostElements; ++i) {
    const blink::WebElement& element = elements[i];
    for (const std::string& name :
         base::SplitString(element.GetAttribute(blink::WebString::FromASCII("class")).Utf8(),
                           base::kWhitespaceASCII, base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      if (seen_classes_.insert(name).second) {
        classes.push_back(name);
      }
    }
    std::string id = element.GetAttribute(blink::WebString::FromASCII("id")).Utf8();
    if (!id.empty() && seen_ids_.insert(id).second) {
      ids.push_back(std::move(id));
    }
  }
  if (classes.empty() && ids.empty()) {
    return;
  }
  Filter().GenericSelectors(
      std::move(classes), std::move(ids), exceptions_,
      base::BindOnce(
          [](base::WeakPtr<ContentFilterAgent> agent,
             const std::vector<std::string>& selectors) {
            if (agent) {
              agent->Hide(selectors);
            }
          },
          weak_factory_.GetWeakPtr()));
}

void ContentFilterAgent::Hide(const std::vector<std::string>& selectors) {
  std::string css;
  for (const std::string& selector : selectors) {
    if (IsPlainSelector(selector)) {
      css += selector + "{display:none!important}\n";
    }
  }
  if (css.empty()) {
    return;
  }
  // A user stylesheet, which a page's own `!important` cannot outrank.
  render_frame()->GetWebFrame()->GetDocument().InsertStyleSheet(
      blink::WebString::FromUtf8(css), nullptr, blink::WebCssOrigin::kUser);
}

void ContentFilterAgent::OnDestruct() {
  delete this;
}

}  // namespace domicile
