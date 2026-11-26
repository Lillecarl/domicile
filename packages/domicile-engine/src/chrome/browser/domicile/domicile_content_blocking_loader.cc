// Copyright 2026 Connor Prussin
// SPDX-License-Identifier: MIT

#include "chrome/browser/domicile/domicile_content_blocking_loader.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "base/byte_size.h"
#include "base/memory/self_deleting.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/domicile/domicile_content_blocking.h"
#include "chrome/browser/domicile/domicile_electron_page.h"
#include "components/domicile/content_blocking/request_type.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/address_list.h"
#include "net/base/data_url.h"
#include "net/base/host_port_pair.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/proxy_server.h"
#include "net/dns/public/dns_query_type.h"
#include "net/proxy_resolution/proxy_info.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/parsed_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/cpp/simple_host_resolver.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/host_resolver.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/proxy_lookup_client.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/origin.h"

namespace domicile {

namespace {

using content_blocking::Decision;
using content_blocking::Mode;
using network::mojom::RequestDestination;

// Where the loads of one factory come from.
struct Origin {
  // The frame whose loads these are; null for a worker's.
  content::GlobalRenderFrameHostId frame;
  // The worker's, or the frame's, origin when the frame is gone.
  url::Origin initiator;
  bool electron = false;
};

bool IsDocument(const network::ResourceRequest& request) {
  return request.destination == RequestDestination::kDocument ||
         request.destination == RequestDestination::kIframe ||
         request.destination == RequestDestination::kFrame ||
         request.destination == RequestDestination::kFencedframe;
}

// The address of the page the request is for, whose site picks the mode.
GURL PageOf(const Origin& origin, const network::ResourceRequest& request) {
  // kDocument is a frame tree's main document: a frame's is kIframe.
  if (request.destination == RequestDestination::kDocument) {
    return request.url;
  }
  // The frame tree's own main frame, not the outermost one: a browser
  // window's page may sit inside the shell's.
  if (content::RenderFrameHost* frame =
          content::RenderFrameHost::FromID(origin.frame)) {
    return frame->GetMainFrame()->GetLastCommittedURL();
  }
  return origin.initiator.GetURL();
}

content_blocking::Request FilterRequest(
    const network::ResourceRequest& request) {
  const bool own = request.destination == RequestDestination::kDocument ||
                   !request.request_initiator;
  return {
      .url = request.url,
      .source = own ? request.url : request.request_initiator->GetURL(),
      .type = content_blocking::RequestTypeFor(request.destination,
                                               request.keepalive),
      .method = request.method,
  };
}

// One request, from the page's loader to the network's.
class Loader final : public network::mojom::URLLoader,
                     public network::mojom::URLLoaderClient,
                     public network::mojom::ProxyLookupClient {
 public:
  Loader(Mode mode,
         content::GlobalRenderFrameHostId frame,
         mojo::PendingRemote<network::mojom::URLLoaderFactory> network,
         int32_t request_id,
         uint32_t options,
         const network::ResourceRequest& request,
         mojo::PendingReceiver<network::mojom::URLLoader> loader,
         mojo::PendingRemote<network::mojom::URLLoaderClient> client,
         const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      : mode_(mode),
        frame_(frame),
        network_(std::move(network)),
        request_id_(request_id),
        options_(options),
        request_(request),
        traffic_annotation_(traffic_annotation),
        loader_(this, std::move(loader)),
        client_(std::move(client)) {
    loader_.set_disconnect_handler(
        base::BindOnce(&Loader::Done, base::Unretained(this)));
    client_.set_disconnect_handler(
        base::BindOnce(&Loader::Done, base::Unretained(this)));
  }

  void Start() {
    const Decision decision = Decide(request_);
    switch (decision.kind) {
      case Decision::Kind::kBlock:
        return Fail(net::ERR_BLOCKED_BY_CLIENT);
      case Decision::Kind::kRedirect:
        return Stub(decision.url);
      case Decision::Kind::kRewrite:
        return RedirectTo(decision.url);
      case Decision::Kind::kAllow:
        return Uncloak();
    }
  }

  // network::mojom::ProxyLookupClient:
  void OnProxyLookupComplete(
      int32_t net_error,
      const std::optional<net::ProxyInfo>& proxy_info) override {
    proxy_lookup_.reset();
    // Through a proxy, the name this host resolves to here is not the one
    // the request reaches, and a lookup would tell the DNS server what the
    // proxy is meant to hide. Brave skips these too.
    if (net_error != net::OK || !proxy_info || !proxy_info->is_direct()) {
      return StartOnTheNetwork();
    }
    content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_);
    if (frame == nullptr) {
      return StartOnTheNetwork();
    }
    resolver_ = network::SimpleHostResolver::Create(
        frame->GetStoragePartition()->GetNetworkContext());
    auto parameters = network::mojom::ResolveHostParameters::New();
    parameters->include_canonical_name = true;
    parameters->dns_query_type = net::DnsQueryType::A;
    resolver_->ResolveHost(network::mojom::HostResolverHost::NewHostPortPair(
                               net::HostPortPair::FromURL(request_.url)),
                           anonymization_key_, std::move(parameters),
                           base::BindOnce(&Loader::Resolved,
                                          base::Unretained(this)));
  }

  // network::mojom::URLLoader:
  void FollowRedirect(network::HttpRequestHeadersUpdateParams update,
                      const std::optional<GURL>& new_url) override {
    if (rewritten_) {
      // Our own redirect: the network has not seen the request yet.
      request_.url = *std::exchange(rewritten_, std::nullopt);
      for (const std::string& name : update.removed_headers) {
        request_.headers.RemoveHeader(name);
      }
      request_.headers.MergeFrom(update.modified_headers);
      request_.cors_exempt_headers.MergeFrom(
          update.modified_cors_exempt_headers);
      return StartOnTheNetwork();
    }
    if (network_loader_) {
      network_loader_->FollowRedirect(std::move(update), new_url);
    }
  }

  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {
    request_.priority = priority;
    if (network_loader_) {
      network_loader_->SetPriority(priority, intra_priority_value);
    }
  }

  // network::mojom::URLLoaderClient:
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr hints) override {
    client_->OnReceiveEarlyHints(std::move(hints));
  }

  void OnReceiveResponse(
      network::mojom::URLResponseHeadPtr head,
      mojo::ScopedDataPipeConsumerHandle body,
      std::optional<mojo_base::BigBuffer> cached_metadata) override {
    if (IsDocument(request_) && head->headers) {
      if (const content_blocking::Blocker* blocker =
              ContentBlocker::Get().blocker()) {
        const std::string directives =
            blocker->CspDirectives(FilterRequest(request_), mode_);
        if (!directives.empty()) {
          head->headers->AddHeader("Content-Security-Policy", directives);
          // The page's policy is read from the parsed headers.
          head->parsed_headers =
              network::PopulateParsedHeaders(head->headers.get(), request_.url);
        }
      }
    }
    client_->OnReceiveResponse(std::move(head), std::move(body),
                               std::move(cached_metadata));
  }

  void OnReceiveRedirect(const net::RedirectInfo& redirect,
                         network::mojom::URLResponseHeadPtr head) override {
    network::ResourceRequest next = request_;
    next.url = redirect.new_url;
    next.method = redirect.new_method;
    const Decision decision = Decide(next);
    if (decision.kind == Decision::Kind::kBlock ||
        decision.kind == Decision::Kind::kRedirect) {
      return Fail(net::ERR_BLOCKED_BY_CLIENT);
    }
    request_ = std::move(next);
    client_->OnReceiveRedirect(redirect, std::move(head));
  }

  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        OnUploadProgressCallback callback) override {
    client_->OnUploadProgress(current_position, total_size,
                              std::move(callback));
  }

  void OnTransferSizeUpdated(int32_t transfer_size_diff) override {
    client_->OnTransferSizeUpdated(transfer_size_diff);
  }

  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    client_->OnComplete(status);
    Done();
  }

 private:
  ~Loader() override = default;

  Decision Decide(const network::ResourceRequest& request) const {
    const content_blocking::Blocker* blocker = ContentBlocker::Get().blocker();
    // A page itself loads; only what it loads is blocked. A shell draws its
    // own page for a site it blocks outright.
    if (blocker == nullptr || !request.url.SchemeIsHTTPOrHTTPS() ||
        request.destination == RequestDestination::kDocument) {
      return {};
    }
    return blocker->Decide(FilterRequest(request), mode_);
  }

  // CNAME uncloaking: a request nothing blocked is checked again under the
  // canonical name its host resolves to, as Brave checks it, which finds a
  // tracker a site serves from its own subdomain. Only another site's
  // requests, or any in aggressive mode, and only those that go straight to
  // the host.
  void Uncloak() {
    content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_);
    const bool other_site =
        !request_.request_initiator ||
        content_blocking::IsThirdParty(request_.url,
                                       request_.request_initiator->GetURL());
    if (frame == nullptr || IsDocument(request_) ||
        !request_.url.SchemeIsHTTPOrHTTPS() || request_.url.HostIsIPAddress() ||
        (mode_ != Mode::kAggressive && !other_site)) {
      return StartOnTheNetwork();
    }
    anonymization_key_ =
        frame->GetIsolationInfoForSubresources().network_anonymization_key();
    frame->GetStoragePartition()->GetNetworkContext()->LookUpProxyForURL(
        request_.url, anonymization_key_,
        proxy_lookup_.BindNewPipeAndPassRemote());
    proxy_lookup_.set_disconnect_handler(
        base::BindOnce(&Loader::StartOnTheNetwork, base::Unretained(this)));
  }

  void Resolved(int result,
                const net::ResolveErrorInfo&,
                const net::AddressList& addresses,
                const net::HostResolverEndpointResults&) {
    resolver_.reset();
    const std::vector<std::string>& aliases = addresses.dns_aliases();
    if (result == net::OK && !aliases.empty() &&
        aliases.front() != request_.url.host()) {
      GURL::Replacements host;
      host.SetHostStr(aliases.front());
      network::ResourceRequest cloaked = request_;
      cloaked.url = request_.url.ReplaceComponents(host);
      const Decision decision = Decide(cloaked);
      if (decision.kind == Decision::Kind::kBlock ||
          decision.kind == Decision::Kind::kRedirect) {
        return Fail(net::ERR_BLOCKED_BY_CLIENT);
      }
    }
    StartOnTheNetwork();
  }

  void StartOnTheNetwork() {
    network_->CreateLoaderAndStart(
        network_loader_.BindNewPipeAndPassReceiver(), request_id_, options_,
        request_, network_client_.BindNewPipeAndPassRemote(),
        traffic_annotation_);
    network_client_.set_disconnect_handler(base::BindOnce(
        &Loader::Fail, base::Unretained(this), net::ERR_ABORTED));
  }

  // Answers with a `$redirect` resource, as Brave's MakeStubResponse does.
  void Stub(const GURL& data_url) {
    std::string mime_type;
    std::string charset;
    std::string data;
    if (!net::DataURL::Parse(data_url, &mime_type, &charset, &data)) {
      return Fail(net::ERR_BLOCKED_BY_CLIENT);
    }
    Counted();
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(std::max<size_t>(data.size(), 1), producer,
                             consumer) != MOJO_RESULT_OK) {
      return Fail(net::ERR_INSUFFICIENT_RESOURCES);
    }
    auto head = network::mojom::URLResponseHead::New();
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(base::StringPrintf(
            "HTTP/1.1 200 OK\n"
            "Content-Type: %s\n"
            "Access-Control-Allow-Origin: *\n\n",
            mime_type.c_str())));
    head->mime_type = mime_type;
    head->charset = charset;
    head->content_length = data.size();
    client_->OnReceiveResponse(std::move(head), std::move(consumer),
                               std::nullopt);
    producer->WriteAllData(base::as_byte_span(data));
    producer.reset();
    network::URLLoaderCompletionStatus status(net::OK);
    status.decoded_body_length = base::ByteSize(data.size());
    client_->OnComplete(status);
    Done();
  }

  // Redirects to `url` before the network sees the request, as Chrome's
  // webRequest does when an extension redirects.
  void RedirectTo(const GURL& url) {
    constexpr int kStatus = net::HTTP_TEMPORARY_REDIRECT;
    const net::RedirectInfo redirect = net::RedirectInfo::ComputeRedirectInfo(
        request_.method, request_.url, request_.site_for_cookies,
        request_.update_first_party_url_on_redirect
            ? net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT
            : net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL,
        request_.referrer_policy, request_.referrer.spec(),
        request_.request_initiator, kStatus, url,
        /*referrer_policy_header=*/std::nullopt,
        /*insecure_scheme_was_upgraded=*/false, /*copy_fragment=*/false,
        /*is_signed_exchange_fallback_redirect=*/false);
    auto head = network::mojom::URLResponseHead::New();
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(base::StringPrintf(
            "HTTP/1.1 %i Internal Redirect\n"
            "Location: %s\n"
            "Non-Authoritative-Reason: Domicile content blocking\n\n",
            kStatus, url.spec().c_str())));
    head->encoded_data_length = 0;
    rewritten_ = url;
    client_->OnReceiveRedirect(redirect, std::move(head));
  }

  void Fail(int error) {
    if (error == net::ERR_BLOCKED_BY_CLIENT) {
      Counted();
    }
    client_->OnComplete(network::URLLoaderCompletionStatus(error));
    Done();
  }

  // One more blocked for the shell's count; a worker's are not counted.
  void Counted() {
    if (content::RenderFrameHost* frame =
            content::RenderFrameHost::FromID(frame_)) {
      BlockedRequests::Count(*frame);
    }
  }

  void Done() { delete this; }

  const Mode mode_;
  // The frame whose request this is; null for a worker's, which is not
  // uncloaked.
  const content::GlobalRenderFrameHostId frame_;
  net::NetworkAnonymizationKey anonymization_key_;
  mojo::Receiver<network::mojom::ProxyLookupClient> proxy_lookup_{this};
  std::unique_ptr<network::SimpleHostResolver> resolver_;
  mojo::Remote<network::mojom::URLLoaderFactory> network_;
  const int32_t request_id_;
  const uint32_t options_;
  network::ResourceRequest request_;
  const net::MutableNetworkTrafficAnnotationTag traffic_annotation_;
  // The URL RedirectTo sent the page to, until it follows.
  std::optional<GURL> rewritten_;

  mojo::Receiver<network::mojom::URLLoader> loader_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  mojo::Remote<network::mojom::URLLoader> network_loader_;
  mojo::Receiver<network::mojom::URLLoaderClient> network_client_{this};
};

class Factory final : public network::SelfDeletingURLLoaderFactory {
 public:
  Factory(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
          mojo::PendingRemote<network::mojom::URLLoaderFactory> network,
          Origin origin,
          base::SelfDeletingPassKey key)
      : network::SelfDeletingURLLoaderFactory(std::move(receiver), key),
        network_(std::move(network)),
        origin_(std::move(origin)) {}

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    const ContentBlocker& blocker = ContentBlocker::Get();
    const GURL page = PageOf(origin_, request);
    // Only a website's loads: the shell, extensions' pages and Electron
    // apps' own schemes load as they do.
    const Mode mode = page.SchemeIsHTTPOrHTTPS() &&
                              (!origin_.electron || blocker.FiltersElectronApps())
                          ? blocker.ModeFor(page)
                          : Mode::kOff;
    if (mode == Mode::kOff) {
      network_->CreateLoaderAndStart(std::move(loader), request_id, options,
                                     request, std::move(client),
                                     traffic_annotation);
      return;
    }
    mojo::PendingRemote<network::mojom::URLLoaderFactory> network;
    network_->Clone(network.InitWithNewPipeAndPassReceiver());
    (new Loader(mode, origin_.frame, std::move(network), request_id, options,
                request,
                std::move(loader), std::move(client), traffic_annotation))
        ->Start();
  }

 private:
  ~Factory() override = default;

  mojo::Remote<network::mojom::URLLoaderFactory> network_;
  const Origin origin_;
};

// The mode `frame`'s requests are checked in: its page's, for a website's
// frame, unless it is an Electron app's and the config spares those.
Mode ModeOf(content::RenderFrameHost& frame) {
  const ContentBlocker& blocker = ContentBlocker::Get();
  const GURL page = frame.GetMainFrame()->GetLastCommittedURL();
  content::WebContents* contents =
      content::WebContents::FromRenderFrameHost(&frame);
  const bool electron = contents != nullptr &&
                        static_cast<bool>(ElectronPage::SessionOf(*contents));
  if (!page.SchemeIsHTTPOrHTTPS() ||
      (electron && !blocker.FiltersElectronApps())) {
    return Mode::kOff;
  }
  return blocker.ModeFor(page);
}

}  // namespace

bool ChecksWebSockets(content::RenderFrameHost& frame) {
  return ContentBlocker::Get().Enabled() && ModeOf(frame) != Mode::kOff;
}

bool MayOpenWebSocket(content::RenderFrameHost& frame, const GURL& url) {
  const content_blocking::Blocker* blocker = ContentBlocker::Get().blocker();
  const Mode mode = ModeOf(frame);
  if (blocker == nullptr || mode == Mode::kOff) {
    return true;
  }
  const Decision decision = blocker->Decide(
      {.url = url,
       .source = frame.GetLastCommittedOrigin().GetURL(),
       .type = content_blocking::RequestType::kWebSocket},
      mode);
  if (decision.kind == Decision::Kind::kBlock ||
      decision.kind == Decision::Kind::kRedirect) {
    BlockedRequests::Count(frame);
    return false;
  }
  return true;
}

void ProxyContentBlocking(content::RenderFrameHost* frame,
                          const url::Origin& initiator,
                          network::URLLoaderFactoryBuilder& builder) {
  // Pages that start while blocking is off load straight from the network,
  // at no cost; a config that turns it on reaches pages started after.
  if (!ContentBlocker::Get().Enabled()) {
    return;
  }
  Origin origin{.initiator = initiator};
  if (frame != nullptr) {
    origin.frame = frame->GetGlobalId();
    content::WebContents* contents =
        content::WebContents::FromRenderFrameHost(frame);
    origin.electron = contents != nullptr &&
                      static_cast<bool>(ElectronPage::SessionOf(*contents));
  }
  auto [receiver, network] = builder.Append();
  base::MakeSelfDeleting<Factory>(std::move(receiver), std::move(network),
                                  std::move(origin));
}

}  // namespace domicile
