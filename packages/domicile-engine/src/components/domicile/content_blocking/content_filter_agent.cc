// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "components/domicile/content_blocking/content_filter_agent.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "components/domicile/content_blocking/content_filter_bundle.h"
#include "content/public/renderer/render_frame.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-object.h"

namespace domicile {

namespace {

// Brave's kCosmeticFilteringInitScript: the flags content_cosmetic.ts reads.
constexpr char kFlagsScript[] = R"((() => {
  window.content_cosmetic = window.content_cosmetic || {};
  const CC = window.content_cosmetic;
  CC.hide1pContent = %s;
  CC.generichide = %s;
})();)";

// Brave's kProceduralActionsScript: a filter that is one selector with a
// style becomes a stylesheet rule, and content_cosmetic.ts applies the rest.
constexpr char kProceduralScript[] = R"((() => {
  const CC = window.content_cosmetic;
  let stylesheet = '';
  CC.proceduralActionFilters = %s.map((f) => JSON.parse(f)).filter((f) => {
    if (f.selector.length === 1 && f.selector[0].type === 'css-selector' &&
        f.action && f.action.type === 'style') {
      stylesheet += f.selector[0].arg + '{' + f.action.arg + '}\n';
      return false;
    }
    return true;
  });
  CC.hasProceduralActions = CC.proceduralActionFilters.length > 0;
  if (stylesheet !== '') {
    cf_worker.injectStylesheet(stylesheet);
  }
})();)";

// Brave's kHideSelectorsInjectScript: the lists' selectors go into the sheet
// content_cosmetic.ts takes a selector back from, in standard mode, when it
// covers the site's own content.
constexpr char kHideScript[] = R"((() => {
  const CC = window.content_cosmetic;
  let next = CC.cosmeticStyleSheet.cssRules.length;
  for (const selector of %s) {
    if (CC.hide1pContent || !CC.allSelectorsToRules.has(selector)) {
      try {
        CC.cosmeticStyleSheet.insertRule(
            selector + '{display:none !important;}', next);
        if (!CC.hide1pContent) {
          CC.allSelectorsToRules.set(selector, next);
          CC.firstRunQueue.add(selector);
        }
        next++;
      } catch (e) {}
    }
  }
  if (!document.adoptedStyleSheets.includes(CC.cosmeticStyleSheet)) {
    document.adoptedStyleSheets =
        [CC.cosmeticStyleSheet, ...document.adoptedStyleSheets];
  }
})();)";

constexpr char kStartScript[] =
    "window.content_cosmetic.tryScheduleQueuePump();";

std::string Json(const std::vector<std::string>& strings) {
  base::ListValue list;
  for (const std::string& string : strings) {
    list.Append(string);
  }
  return base::WriteJson(list).value_or("[]");
}

// Whether `selector` stays one selector in `selector{...}` in a user
// stylesheet. A list's rule with a brace in it would otherwise end the rule
// and start its own; a valid selector needs one only inside an attribute's
// quotes, and such a rule is left out. The adopted sheet's insertRule
// refuses a second rule by itself.
bool IsPlainSelector(std::string_view selector) {
  return !selector.empty() &&
         selector.find_first_of("{}") == std::string::npos &&
         selector.find("/*") == std::string::npos;
}

template <typename Sig>
void Bind(v8::Isolate* isolate,
          v8::Local<v8::Context> context,
          v8::Local<v8::Object> object,
          std::string_view name,
          base::RepeatingCallback<Sig> callback) {
  object
      ->Set(context, gin::StringToSymbol(isolate, name),
            gin::CreateFunctionTemplate(isolate, std::move(callback))
                ->GetFunction(context)
                .ToLocalChecked())
      .Check();
}

}  // namespace

// static
void ContentFilterAgent::Attach(content::RenderFrame* frame,
                                int32_t world_id) {
  new ContentFilterAgent(frame, world_id);
}

// static
void ContentFilterAgent::RunAtDocumentStart(content::RenderFrame* frame) {
  if (ContentFilterAgent* agent = Get(frame)) {
    agent->AtDocumentStart();
  }
}

ContentFilterAgent::ContentFilterAgent(content::RenderFrame* frame,
                                       int32_t world_id)
    : content::RenderFrameObserver(frame),
      content::RenderFrameObserverTracker<ContentFilterAgent>(frame),
      world_id_(world_id) {}

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
  hide_first_party_ = false;
  document_ = GURL();
  exceptions_.clear();
  weak_factory_.InvalidateWeakPtrs();
}

void ContentFilterAgent::DidCreateScriptContext(v8::Local<v8::Context> context,
                                                int32_t world_id) {
  // Not only while active: a new document recreates the context of every
  // world the frame used before, ahead of document start.
  if (world_id != world_id_) {
    return;
  }
  v8::Isolate* isolate =
      render_frame()->GetWebFrame()->GetAgentGroupScheduler()->Isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Object> worker = v8::Object::New(isolate);
  Bind(isolate, context, worker, "hiddenClassIdSelectors",
       base::BindRepeating(&ContentFilterAgent::HiddenClassIdSelectors,
                           base::Unretained(this)));
  Bind(isolate, context, worker, "isFirstPartyUrl",
       base::BindRepeating(&ContentFilterAgent::IsFirstPartyUrl,
                           base::Unretained(this)));
  Bind(isolate, context, worker, "injectStylesheet",
       base::BindRepeating(&ContentFilterAgent::InjectStylesheet,
                           base::Unretained(this)));
  context->Global()
      ->Set(context, gin::StringToSymbol(isolate, "cf_worker"), worker)
      .Check();
}

void ContentFilterAgent::RunInWorld(const std::string& script) {
  render_frame()->GetWebFrame()->ExecuteScriptInIsolatedWorld(
      world_id_, blink::WebScriptSource(blink::WebString::FromUtf8(script)),
      blink::BackForwardCacheAware::kAllow);
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
  document_ = document;
  hide_first_party_ = rules->hide_first_party;
  exceptions_ = std::move(rules->exceptions);
  if (!rules->injected_script.empty()) {
    // In the main world, as the page's own: scriptlets wrap the page's
    // globals. From here, unlike Brave's <script> element, a page's CSP does
    // not stop them.
    frame->ExecuteScript(blink::WebScriptSource(
        blink::WebString::FromUtf8(rules->injected_script)));
  }
  // The first script in the world makes its context, which gets cf_worker.
  RunInWorld(base::StringPrintf(kFlagsScript,
                                hide_first_party_ ? "true" : "false",
                                rules->generichide ? "true" : "false"));
  RunInWorld(base::StringPrintf(kProceduralScript,
                                Json(rules->procedural_actions).c_str()));
  RunInWorld(kContentFilterBundle);
  Hide(rules->force_hide_selectors, /*force=*/true);
  Hide(rules->hide_selectors, /*force=*/hide_first_party_);
  RunInWorld(kStartScript);
}

void ContentFilterAgent::Hide(const std::vector<std::string>& selectors,
                              bool force) {
  if (selectors.empty()) {
    return;
  }
  if (!force) {
    RunInWorld(base::StringPrintf(kHideScript, Json(selectors).c_str()));
    return;
  }
  std::string css;
  for (const std::string& selector : selectors) {
    if (IsPlainSelector(selector)) {
      css += selector + "{display:none!important}\n";
    }
  }
  InjectStylesheet(css);
}

void ContentFilterAgent::HiddenClassIdSelectors(const std::string& json) {
  std::optional<base::DictValue> asked =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!active_ || !asked) {
    return;
  }
  auto strings = [&](std::string_view key) {
    std::vector<std::string> values;
    if (const base::ListValue* list = asked->FindList(key)) {
      for (const base::Value& value : *list) {
        if (value.is_string()) {
          values.push_back(value.GetString());
        }
      }
    }
    return values;
  };
  Filter().GenericSelectors(
      strings("classes"), strings("ids"), exceptions_,
      base::BindOnce(
          [](base::WeakPtr<ContentFilterAgent> agent,
             mojom::GenericHidePtr hide) {
            if (agent && hide) {
              agent->Hide(hide->force_hide_selectors, /*force=*/true);
              agent->Hide(hide->hide_selectors,
                          /*force=*/agent->hide_first_party_);
            }
          },
          weak_factory_.GetWeakPtr()));
}

bool ContentFilterAgent::IsFirstPartyUrl(const std::string& url) {
  return net::registry_controlled_domains::SameDomainOrHost(
      GURL(url), document_,
      net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

void ContentFilterAgent::InjectStylesheet(const std::string& css) {
  if (!active_ || css.empty()) {
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
