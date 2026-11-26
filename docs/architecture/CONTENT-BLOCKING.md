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
  canonical name. The name comes from `NetworkContext::ResolveHost` with
  `include_canonical_name`, in the request's profile and with its
  `NetworkAnonymizationKey`. It is skipped when a proxy is set, as Brave skips
  it.
- **Not checked:** the shell and other `domicile:` pages, extensions'
  requests, non-HTTP schemes, top-level navigations, a site in `off` mode, and
  Electron apps unless `electron_apps`.

### Element hiding and scriptlets

A renderer agent applies the cosmetic rules in each http and https frame.
It follows Brave's `CosmeticFiltersJsRenderFrameObserver`, without Brave's
content settings plumbing:

- **The browser decides the frame's mode.** At `ReadyToCommitNavigation` the
  agent asks `domicile.mojom.ContentFilter.FrameResources(url)`, a sync call.
  The browser answers from the frame's top-level origin and `sites`, so the
  renderer needs no settings. The call blocks the renderer until the engine
  sequence answers; a guard measures that time.
- **Scriptlets run first.** At `RunScriptsAtDocumentStart` the agent runs the
  frame's scriptlets in the main world. It appends a `<script>` element from a
  domicile isolated world and removes it, as Brave does. A page CSP that
  forbids inline scripts stops them there too.
- **Specific hiding:** one `insertRule` per selector into a constructed sheet
  the isolated world adopts. A selector that holds a `{` breaks out of no rule:
  `insertRule` refuses more than one rule. This replaces adblock-rust's
  `css-validation` feature and the Servo crates it needs (`cssparser`,
  `selectors`).
- **Generic hiding:** Brave's `content_cosmetic.ts` and
  `procedural_filters.ts` (MPL-2.0, headers kept) run in the isolated world.
  They watch class and id names and ask `HiddenClassIdSelectors`, and they
  apply procedural filters (`:has-text`, `:upward` and the rest). The bundle
  is built with bun and embedded in the engine binary.
- **Not hidden:** `domicile:` pages, extension pages, and frames of a site in
  `off` mode.

### The shell

- **`contentBlocking.site(url)`** gives a page's mode and whether the config
  locks it. **`contentBlocking.setSite(host, mode)`** sets it, kept in the
  shell profile's prefs below `sites`, as pinned sites are.
- **`window.blocked`** counts the requests the engine blocked for a browser
  window's current page, for a shell's badge. A shell that shows nothing loses
  nothing.

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

## Order of work

1. Vendor the crates, and build the bridge with a unit test.
2. The `ContentBlocker` service: config, lists, cache, serialized engines.
3. Network blocking, stubs and `removeparam`, and the guard's network half.
4. Element hiding and scriptlets, and the guard's cosmetic half.
5. `$csp` and CNAME uncloaking.
6. The shell API and the blocked count.

## Not in this design

- WebSockets. Brave proxies them through a content patch
  (`CreateWebSocketWithFrameId`). They are left open until a list needs them.
- Blocking top-level navigations. Brave shows an interstitial page; a shell
  would draw its own.
- Brave's element picker.
- Fingerprinting reduction. Another design, tested against online
  fingerprinting checks.
