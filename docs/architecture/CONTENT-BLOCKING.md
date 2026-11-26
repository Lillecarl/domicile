# Content blocking

The engine blocks ads and trackers itself, with Brave's filter engine
([adblock-rust](https://github.com/brave/adblock-rust)), in every profile and
optionally in Electron apps. An extension still works next to it.

## Problem

Domicile loads Manifest V3 extensions only, and MV3 blockers (uBlock Origin
Lite, Ghostery) cannot do some things:

- Many uBlock Origin filters do not convert to `declarativeNetRequest` rules:
  `redirect-rule=`, `strict1p`/`strict3p`, regex `removeparam=`, exceptions to
  modifier filters, and many regex filters. The
  [uBO Lite FAQ](https://github.com/uBlockOrigin/uBOL-home/wiki/Frequently-asked-questions-(FAQ))
  lists them. Sites with anti-adblock walls are where users see the gap.
- No MV3 blocker can uncloak a first-party CNAME tracker.
- Rules update only when the extension updates.
- Electron apps have no extensions, and private profiles have them off by
  default.

Brave filters in the browser, below the extension APIs, and has none of these
limits. Its engine is MPL-2.0 and built to embed: brave-core reaches it through
an 836-line cxx bridge and a few hooks.

## Design

### Config

```json
{
  "content_blocking": {
    "lists": [
      "https://easylist.to/easylist/easylist.txt",
      "https://easylist.to/easylist/easyprivacy.txt",
      "https://ublockorigin.github.io/uAssets/filters/filters.txt",
      "/etc/domicile/my-list.txt"
    ],
    "resources": "https://raw.githubusercontent.com/brave/adblock-resources/master/dist/resources.json",
    "filters": ["||ads.example.com^", "example.com##.banner"],
    "mode": "standard",
    "sites": { "bank.example": "off", "news.example": "aggressive" },
    "electron_apps": false
  }
}
```

- **No `content_blocking`:** the engine blocks nothing, as today.
- **`lists`:** filter lists in Adblock Plus and uBlock Origin syntax, by URL or
  absolute path. Unset: EasyList, EasyPrivacy, and uBlock Origin's filters,
  privacy, unbreak and badware lists. `[]`: only `filters`.
- **`resources`:** the scriptlets and stub resources that `##+js()` and
  `$redirect` name, as adblock-rust's JSON. Unset: Brave's
  `adblock-resources`. The engine downloads it, so no GPL scriptlet ships in
  the MIT engine.
- **`filters`:** the user's own rules. They apply in every mode, as Brave's
  custom filters do.
- **`mode`:** `standard` blocks third-party requests and hides elements
  generically, but lets a site's own first-party content through unless a rule
  is `$important`. `aggressive` blocks first-party matches too. Brave's modes,
  with the same names.
- **`sites`:** a mode per site (`off`, `standard`, `aggressive`), by host or
  `*.host`. A site is the top-level page's host, so an iframe follows the page
  it is in. The most specific entry wins.
- **`electron_apps`:** filters Electron apps' pages too. Off by default,
  because apps break in ways a user cannot fix from a menu.

### Engine

- **One `domicile::ContentBlocker`** in the browser process, for every
  profile. The engines live on one sequenced task runner (`MayBlock`,
  `USER_BLOCKING`, `SKIP_ON_SHUTDOWN`), as Brave's do.
- **Two engines:** one for `lists`, one for `filters`. Standard mode drops the
  first one's first-party matches and keeps the second's, as Brave's
  `AdBlockEngineWrapper` does.
- **Lists are downloaded and cached** under
  `<user-data-dir>/domicile-content-blocking/`, keyed by URL. The engine reads
  `! Expires:` (`read_list_metadata`) and fetches again after it, between one
  hour and 14 days, by default after one day. It fetches through the system
  network context, so no profile's cookies go with it. A path is read again
  when the config changes.
- **Startup reads a serialized engine.** After each build the engine writes
  `serialize()` beside the lists. The next start `deserialize()`s it while the
  lists are unchanged, and builds from the cached texts otherwise.
- **Domains come from Chromium.** adblock-rust's embedded resolver needs the
  `addr` crate. Instead, `resolve_domain_position` answers from
  `net::registry_controlled_domains`, as Brave's resolver does.

### Network requests

`domicile::ProxyContentBlocking` joins `WillCreateURLLoaderFactory` (patch
0282), after `ProxyElectronNetworkLoads`, so an Electron app's
`protocol.handle` still answers first. Its loader asks the engine before the
request starts and on every redirect the server sends. Engines are built on
a worker and asked on the UI thread, where the loaders run, so a request
waits for no thread hop.

- **Inputs:** the URL, the request's initiator, the request's type (from
  `network::mojom::RequestDestination`), and whether it is third-party to the
  initiator (`SameDomainOrHost` with private registries), as Brave measures
  it. The mode comes from the top-level page's host.
- **Factories made while blocking is off** load straight from the network.
  A config that turns blocking on reaches the pages that load after it.
- **Block:** `ERR_BLOCKED_BY_CLIENT`.
- **`$redirect`:** a stub response from the resource's `data:` URL, as
  Brave's `MakeStubResponse` makes it.
- **`$removeparam`:** an internal redirect to the cleaned URL. Only for GET,
  HEAD and OPTIONS.
- **`$csp`:** the directives join the `Content-Security-Policy` header of a
  document's response.
- **CNAME uncloaking:** a request nothing blocked is checked again under its
  host's canonical name, which finds a tracker a site serves from its own
  subdomain. The name comes from `NetworkContext::ResolveHost` with
  `include_canonical_name`, in the frame's profile and with its
  `NetworkAnonymizationKey`, so the real request then hits the DNS cache. It
  runs for another site's requests, or for any in aggressive mode, as Brave's
  does. It is skipped when `LookUpProxyForURL` says the request goes through
  a proxy, so PAC scripts and system proxies count, and for a worker's
  requests. No guard covers it: it needs a DNS server that answers a CNAME.
- **WebSockets** (patch 0284): while blocking is on, `WillInterceptWebSocket`
  intercepts a website's frame's WebSockets, and `CreateWebSocket` asks the
  engine with type `websocket` before it makes one. One it refuses is never
  made; the rest go on through webRequest's proxy, as without blocking.
- **Not checked:** the shell and other `domicile:` pages, extensions'
  requests, non-HTTP schemes, top-level navigations, a site in `off` mode, and
  Electron apps unless `electron_apps`.

### Element hiding and scriptlets

`domicile::ContentFilterAgent`, which every frame gets (patch 0283), applies
the cosmetic rules in each http and https document, in the order Brave's
`CosmeticFiltersJSHandler::ApplyRules` applies them, without Brave's content
settings plumbing:

- **The browser decides the frame's mode.** When the document is about to
  run its first script, the agent asks
  `domicile.mojom.ContentFilter.RulesFor(url)`, a sync call. The browser
  answers on the UI thread from the page the frame is in, so the renderer
  holds no settings. The shell's, extensions' and (unless `electron_apps`)
  Electron apps' frames get no rules.
- **The rules come as Brave splits them:** the lists' selectors, the user's
  (always hidden), procedural and action filters, scriptlets, exceptions,
  `generichide`, and whether the site's own content is hidden too
  (aggressive). Standard mode drops the lists' `:has()` selectors and their
  procedural filters but keeps their one-selector action filters, as Brave's
  `AdBlockEngineWrapper` does.
- **Scriptlets run first.** The agent runs them with
  `WebLocalFrame::ExecuteScript`, in the page's main world, after an Electron
  app's preload and before the page's scripts. Unlike Brave's `<script>`
  element, a page's CSP does not stop them.
- **Brave's `content_cosmetic.ts` and `procedural_filters.ts`** (MPL-2.0,
  unchanged, in `components/domicile/content_blocking/resources`) run in the
  agent's isolated world, `ISOLATED_WORLD_ID_DOMICILE_CONTENT_FILTER`. They
  watch the document's class and id names as it changes and ask for generic
  rules, apply procedural filters (`:has-text()`, `:upward()` and the rest)
  and actions (`:style()`, `:remove()`), and in standard mode show a
  selector's elements again when they hold the site's own content. The
  engine's build has no bun, so `bundle.sh` writes
  `content_filter_bundle.js`, which is checked in and compiled into the
  renderer as a string.
- **`cf_worker`** is the agent's: `hiddenClassIdSelectors` (answered by
  `GenericSelectors` over mojo), `isFirstPartyUrl` and `injectStylesheet`, as
  Brave names them. The element picker's functions are left out.
- **Hiding:** the user's selectors, and all of them in aggressive mode, go
  into a user-origin stylesheet, which no `!important` of the page outranks.
  A selector holding `{`, `}` or `/*` is left out of it, so a list cannot end
  the rule and start its own; this replaces adblock-rust's `css-validation`
  and the Servo crates it needs. The lists' selectors in standard mode go
  into the adopted sheet content_cosmetic.ts may take a selector back from,
  whose `insertRule` refuses a second rule by itself.

### The shell

- **`browser.blockingFor(url)`** gives a page's mode.
  **`browser.setSiteBlocking(site, blocking)`** sets it, kept in the shell
  profile's prefs (`domicile.content_blocking.sites`) below the config's
  `sites`, as pinned sites are kept below `browser.sites`. A site the config
  names is refused.
- **`blockedRequests`** on each browser window counts the requests the
  engine blocked for its current page, stub answers included, for a shell's
  badge. It starts again at each page the tab commits, and the window list
  is sent again at most every quarter second while it moves. A worker's
  requests are not counted.

### Building

- **adblock 0.13.6 is vendored into `third_party/rust/chromium_crates_io`**,
  Chromium's own way. Its features are `full-regex-handling` and
  `single-thread`, with no default features.
- **It adds 12 crates Chromium 160 lacks:** adblock, arrayvec 0.7,
  flatbuffers 25, idna 1, idna_adapter 1, percent-encoding 2, rustc-hash 2,
  seahash 4 and thiserror 1, plus thiserror-impl 1, semver 1 and rustc_version
  0.4, which only build. Patch 0281 changes `Cargo.toml`, `Cargo.lock` and
  `gnrt_config.toml`. The vendored sources and gnrt's `BUILD.gn` files are new
  files in `src/`, so they never conflict on a rebase.
- **Two Chromium crates change:** `regex` ships (Chromium keeps it for tests),
  and `bitflags` gains its `serde` feature.
- **flatbuffers and seahash publish no license file.** Their `LICENSE` sits
  beside their `BUILD.gn`, as Brave keeps it.
- **Regenerating:** gnrt runs on a copy of `third_party/rust` and
  `tools/crates`, with nixpkgs' cargo, since Chromium's links Ubuntu's
  libcurl. `gn format` then formats its `BUILD.gn` files, and
  `zune_jpeg/v0_5/README.chromium` keeps its hand-written notes.
- **The bridge is first-party Rust:** a `rust_static_library` with
  `cxx_bindings` in `components/domicile/content_blocking`, depending on
  `//third_party/rust/adblock/v0_13:lib`. It follows Brave's `lib.rs`, and drops
  the iOS conversion and the deprecated calls.

### Extensions next to it

An extension keeps working. Its rules and the engine's both apply, and the
first one to block wins. With Ghostery, turn off its ad and tracker blocking
and keep Never-Consent: `blockAds` and `blockTrackers` off, `blockAnnoyances`
on.

## Testing

- **Unit tests** (`domicile_unittests`) build an engine from inline rules.
  They check the request type mapping, first and third party in each mode,
  `$important`, `sites` over `mode`, `filters` in standard mode,
  `$removeparam`'s methods and types, stub responses, and list expiry.
- **`guard-content-blocking.sh`** serves a page, a list and resources from a
  local server. The page loads another site's listed script, its own listed
  script, a script the user's rule blocks, a `$redirect` script, a tracked
  pixel, a WebSocket and a frame its `$csp` forbids; it holds elements a
  site rule and generic rules hide, and a scriptlet marks it before its first
  script. `AGGRESSIVE=1` sets the site to aggressive, and its own script and
  element go too. The control, `NEGATIVE=1`, sets the site to `off`, and every
  request must arrive as the page made it, with nothing hidden or run.
- **Online checks** in a headless engine with the default lists, against the
  same engine with blocking off. adblock-tester.com scores 90 of 100 in
  standard mode, 96 in aggressive mode, and 58 off. Its three misses in
  standard mode are banner images the site serves itself, which standard
  mode lets through as Brave does. On speedtest.net, 50 requests from 18
  hosts become 35 from 10: nine hosts go (Amazon's ad system, Google
  Analytics, ID5, Lotame and others) and Blockthrough's ad block detector
  comes. The page's text is the same.

  Two traps a test page can fall into:
  - adblock-rust, like uBlock Origin, applies a `$removeparam` with no type
    to documents, frames and fetches only, not to an image.
  - Brave's first-party test (`isSubTreeFirstParty` in `content_cosmetic.ts`)
    walks an element's next siblings too. An ad next to an element with a
    first-party image is shown again in standard mode.

## Not in this design

- Blocking top-level navigations. Brave shows an interstitial page; a shell
  would draw its own.
- Brave's element picker.
- A worker's WebSockets: Chrome proxies only a frame's
  (`CreateWebSocket` has no frame for a worker's).
- Fingerprinting reduction. Another design, tested against online
  fingerprinting checks.
