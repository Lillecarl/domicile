#!/usr/bin/env bash
# Checks that the engine's own content blocking blocks a page's requests as
# the config says: a list's rule blocks another site's script, the user's own
# rule blocks the site's own, standard mode lets the site's own listed script
# through, a $redirect rule answers with its resource, and $removeparam drops
# a parameter. The list and resources are fetched from the same server.
#
#   nix develop .#full --command \
#     ./packages/domicile-engine/scripts/guard-content-blocking.sh /build/chromium/src
#
# Headless and software-composited, like guard-webview-spellcheck.sh. A
# compositor stand-in sends `content_blocking` at the handshake. A request the
# engine blocked never reaches the server, so the server's log is the reading.
#
# Assertions, in order:
# - the shell page ran, and the engine fetched the list and the resources
# - the test page loaded, and nothing blocked localhost's /ok.js
# - localhost's /ad.js (the list's) and the site's /mine.js (the user's) were
#   never asked for
# - the site's /own-ad.js was asked for: standard mode spares a site's own
# - localhost's /stub.js was never asked for, and the stub ran in its place
# - the pixel was asked for without utm_source
# - the WebSocket to localhost's /ws-ad was never opened
# - localhost's /frame.html was never asked for: the list's $csp gave the
#   site's page frame-src 'none'
# - the site's .ad-banner and the generic .generic-ad are hidden; the generic
#   .generic-own, which holds only the site's own image, is shown again
#   (standard mode); the story is shown; and the guard-mark scriptlet ran
#   before the page's first script
#
# Aggressive (AGGRESSIVE=1): the config sets the site to aggressive, so its
# own /own-ad.js is never asked for and .generic-own stays hidden.
#
# Control (NEGATIVE=1): the config turns the site off. /ad.js, /mine.js,
# /stub.js and the WebSocket must be asked for, and the pixel with its
# utm_source, and nothing is hidden or run.
set -u

SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=packages/domicile-engine/scripts/lib-annotate.sh
. "$SCRIPTS/lib-annotate.sh"
# shellcheck source=packages/domicile-engine/scripts/lib-ports.sh
. "$SCRIPTS/lib-ports.sh"
# shellcheck source=packages/domicile-engine/scripts/lib-profile.sh
. "$SCRIPTS/lib-profile.sh"
# shellcheck source=packages/domicile-engine/scripts/lib-broker.sh
. "$SCRIPTS/lib-broker.sh"

CHROMIUM="${1:-}"
if [ -z "$CHROMIUM" ]; then
  annotate "guard-content-blocking: no path to chromium/src was given"
  exit 1
fi

NEGATIVE="${NEGATIVE:-0}"
AGGRESSIVE="${AGGRESSIVE:-0}"
OUT="${OUT:-out/Domicile}"
FOR_SECONDS="${FOR_SECONDS:-90}"

# run-checks.sh runs the aggressive guard beside this one, so each keeps its
# own broker, socket, profile and logs.
WHICH=""
[ "$NEGATIVE" = "1" ] && WHICH="-negative"
[ "$AGGRESSIVE" = "1" ] && WHICH="-aggressive"
BROKER="${BROKER:-/tmp/domicile-content-blocking$WHICH-broker}"
CONTROL="${CONTROL:-/tmp/domicile-content-blocking$WHICH-control}"
PROFILE="${PROFILE:-/tmp/domicile-content-blocking$WHICH-profile}"
ENGINE_LOG="${ENGINE_LOG:-/tmp/domicile-content-blocking$WHICH-engine.log}"
HTTP_LOG="${HTTP_LOG:-/tmp/domicile-content-blocking$WHICH-http.log}"
SOCKET_LOG="${SOCKET_LOG:-/tmp/domicile-content-blocking$WHICH-socket.log}"

STARTED=()
cleanup() {
  if [ ${#STARTED[@]} -gt 0 ]; then
    kill "${STARTED[@]}" 2>/dev/null
  fi
  remove_profile "$PROFILE"
  rm -f "$CONTROL"
}
trap cleanup EXIT

[ -x "$CHROMIUM/$OUT/chrome" ] || {
  annotate "guard-content-blocking: no engine at $CHROMIUM/$OUT/chrome; build it with ./packages/domicile-engine/scripts/build.sh"
  exit 1
}
command -v python3 >/dev/null || {
  skip "guard-content-blocking: no python3, and the site and the compositor stand-in are both python"
  exit 77
}

fresh_broker
rm -rf "$PROFILE"; mkdir -p "$PROFILE"

wait_for_line() { # $1 tries, $2 pattern, $3 file
  for _ in $(seq 1 "$1"); do
    grep -qF "$2" "$3" 2>/dev/null && return 0
    sleep 0.25
  done
  return 1
}

# 1. The site, its list and its resources.
rm -f "$HTTP_LOG"
python3 "$SCRIPTS/guard-content-blocking-server.py" --port 0 >"$HTTP_LOG" 2>&1 &
STARTED+=($!)
wait_for_line 240 "serving" "$HTTP_LOG" || {
  annotate_from "guard-content-blocking: its site never came up" "$HTTP_LOG"
  exit 1
}
PORT="$(served_port "$HTTP_LOG")" || {
  annotate_from "guard-content-blocking: its site never said which port it took" "$HTTP_LOG"
  exit 1
}
SITE="http://127.0.0.1:$PORT"

# 2. The compositor, with the desk's content_blocking.
SITES='{}'
[ "$NEGATIVE" = "1" ] && SITES='{"127.0.0.1": "off"}'
[ "$AGGRESSIVE" = "1" ] && SITES='{"127.0.0.1": "aggressive"}'
BLOCKING="{\"enabled\": true, \"lists\": [\"$SITE/list.txt\"],
  \"resources\": \"$SITE/resources.json\", \"filters\": [\"/mine.js\$script\"],
  \"mode\": \"standard\", \"sites\": $SITES, \"electron_apps\": false}"
rm -f "$SOCKET_LOG"
python3 "$SCRIPTS/guard-content-blocking-compositor.py" \
  --socket "$CONTROL" --content-blocking "$BLOCKING" >"$SOCKET_LOG" 2>&1 &
STARTED+=($!)
wait_for_line 240 "listening on" "$SOCKET_LOG" || {
  annotate_from "guard-content-blocking: the compositor stand-in never listened on $CONTROL" "$SOCKET_LOG"
  exit 1
}

# 3. The engine.
rm -f "$ENGINE_LOG"
"$CHROMIUM/$OUT/chrome" \
  --ozone-platform=headless \
  --disable-gpu \
  --app="domicile://shell/?src=$SITE/page" \
  --domicile-shell-root="$SCRIPTS" \
  --domicile-shell-module="guard-content-blocking.js" \
  --domicile-control-socket="$CONTROL" \
  --no-sandbox --password-store=basic --no-first-run --disable-component-update \
  --user-data-dir="$PROFILE" \
  --enable-logging=stderr --log-level=0 \
  --domicile-broker-socket="$BROKER" >"$ENGINE_LOG" 2>&1 &
STARTED+=($!)

TRIES=$((FOR_SECONDS * 4))
wait_for_line "$TRIES" "GUARD shell-loaded" "$ENGINE_LOG" || {
  annotate_from "guard-content-blocking: the shell page never ran" "$ENGINE_LOG"
  tail -40 "$ENGINE_LOG" >&2
  exit 1
}
wait_for_line "$TRIES" "GUARD page-loaded" "$ENGINE_LOG" ||
  echo "the test page never finished loading" >&2
wait_for_line 60 "GUARD cosmetic" "$ENGINE_LOG" ||
  echo "the test page never said what it shows" >&2
# The control blocks nothing, so its shell never counts a request to report on.
[ "$NEGATIVE" = "1" ] || wait_for_line 60 "GUARD shell " "$ENGINE_LOG" ||
  echo "the shell never said what its blocking calls answer" >&2

asked() { # $1 the path and query, $2 the host
  grep -qE "^asked $1 host=$2(:|$)" "$HTTP_LOG" && echo 1 || echo 0
}

SAW_SENT=$(grep -qF "sent the content blocking" "$SOCKET_LOG" && echo 1 || echo 0)
SAW_LIST=$(asked "/list.txt" "127.0.0.1")
SAW_RESOURCES=$(asked "/resources.json" "127.0.0.1")
SAW_PAGE=$(grep -qF "GUARD page-loaded" "$ENGINE_LOG" && echo 1 || echo 0)
SAW_STUB_RAN=$(grep -qF "GUARD page-loaded stub=true" "$ENGINE_LOG" && echo 1 || echo 0)
OK=$(asked "/ok.js" "localhost")
AD=$(asked "/ad.js" "localhost")
OWN=$(asked "/own-ad.js" "127.0.0.1")
MINE=$(asked "/mine.js" "127.0.0.1")
STUB=$(asked "/stub.js" "localhost")
CLEAN=$(asked "/pixel.gif\\?id=1" "localhost")
TRACKED=$(asked "/pixel.gif\\?utm_source=guard&id=1" "localhost")
SOCKET=$(asked "/ws-ad" "localhost")
FRAME=$(asked "/frame.html" "localhost")
saw() { grep -qF "$1" "$ENGINE_LOG" && echo 1 || echo 0; }
OWN_SHOWN=block
[ "$AGGRESSIVE" = "1" ] && OWN_SHOWN=none
HIDDEN=$(saw "GUARD cosmetic banner=none generic=none own=$OWN_SHOWN story=block marked-first=true")
MODE=standard
[ "$AGGRESSIVE" = "1" ] && MODE=aggressive
SHELL_ASKED=$(saw "GUARD shell counted=true mode=$MODE set=aggressive unset=standard refused=OperationError")
SHOWN=$(saw "GUARD cosmetic banner=block generic=block own=block story=block marked-first=false")

echo
echo "sent=$SAW_SENT list=$SAW_LIST resources=$SAW_RESOURCES page=$SAW_PAGE"
echo "asked for: ok=$OK ad=$AD own-ad=$OWN mine=$MINE stub=$STUB" \
  "clean-pixel=$CLEAN tracked-pixel=$TRACKED socket=$SOCKET frame=$FRAME;" \
  "the stub ran=$SAW_STUB_RAN"
echo "cosmetic: hidden=$HIDDEN shown=$SHOWN; the shell's calls=$SHELL_ASKED"
grep -F -e "GUARD cosmetic" -e "GUARD shell " "$ENGINE_LOG" || true
echo "what the site was asked for:"
cat "$HTTP_LOG"
echo

FAILURE=""
PASSED=""
if [ "$SAW_SENT" != "1" ]; then
  FAILURE="the compositor stand-in never sent content_blocking: the control \
channel never connected"
elif [ "$SAW_LIST" != "1" ] || [ "$SAW_RESOURCES" != "1" ]; then
  FAILURE="the engine never fetched the list or the resources: \
ContentBlocker::SetSettings never ran (the sink), or its fetch failed"
elif [ "$SAW_PAGE" != "1" ] || [ "$OK" != "1" ]; then
  FAILURE="the test page never loaded, or not even /ok.js reached the site, \
which nothing blocks"
elif [ "$NEGATIVE" = "1" ]; then
  if [ "$AD" = "1" ] && [ "$MINE" = "1" ] && [ "$STUB" = "1" ] &&
    [ "$TRACKED" = "1" ] && [ "$SHOWN" = "1" ] && [ "$SOCKET" = "1" ] &&
    [ "$FRAME" = "1" ]; then
    PASSED="the control is sharp: with the site off, every request reached \
the site as the page made it, nothing was hidden and no scriptlet ran"
  else
    FAILURE="the site is off, and the engine still blocked or rewrote a \
request: ModeFor or the loader's mode"
  fi
elif [ "$AD" = "1" ]; then
  FAILURE="another site's /ad.js, which the list blocks, reached the server: \
the loader proxy (patch 0282) never asked, or the Blocker let it through"
elif [ "$MINE" = "1" ]; then
  FAILURE="the site's own /mine.js, which the user's rule blocks, reached the \
server: the filters engine"
elif [ "$AGGRESSIVE" != "1" ] && [ "$OWN" != "1" ]; then
  FAILURE="the site's own /own-ad.js never reached the server, though \
standard mode lets a site's own listed content through"
elif [ "$AGGRESSIVE" = "1" ] && [ "$OWN" = "1" ]; then
  FAILURE="the site's own /own-ad.js reached the server, though aggressive \
mode blocks a site's own listed content: ModeFor's sites or the Blocker"
elif [ "$STUB" = "1" ] || [ "$SAW_STUB_RAN" != "1" ]; then
  FAILURE="/stub.js was not answered with its resource: the redirect rule, \
the resources, or the loader's stub response"
elif [ "$CLEAN" != "1" ]; then
  FAILURE="the pixel never arrived without utm_source: \$removeparam's \
internal redirect"
elif [ "$SOCKET" = "1" ]; then
  FAILURE="the WebSocket to localhost's /ws-ad, which the list blocks, \
reached the site: patch 0284's WillInterceptWebSocket or MayOpenWebSocket"
elif [ "$FRAME" = "1" ]; then
  FAILURE="localhost's /frame.html was asked for, though the list's \$csp \
gave the page frame-src 'none': the loader's Content-Security-Policy header"
elif [ "$HIDDEN" != "1" ]; then
  FAILURE="the page did not hide the site's .ad-banner and the generic \
.generic-ad, show .generic-own and the story, or run the guard-mark \
scriptlet before its first script: ContentFilterAgent (patch 0283), \
content_filter_bundle.js or domicile_content_filter.cc"
elif [ "$SHELL_ASKED" != "1" ]; then
  FAILURE="the shell's blocking calls did not answer as the config says: \
blockedRequests, contentBlockingFor or setContentBlockingSite \
(domicile_host.cc, domicile_browser_windows.cc, ContentBlocker::SetSite)"
else
  OWN_SAID="standard mode spared the site's own listed script and showed its \
own generic element again"
  [ "$AGGRESSIVE" = "1" ] && OWN_SAID="aggressive mode blocked the site's own \
listed script and kept its own generic element hidden"
  PASSED="the list blocked another site's script, the user's rule the site's \
own, $OWN_SAID, a \$redirect rule answered with its resource, \$removeparam \
dropped utm_source, \$csp kept out a frame, the site's and a generic rule hid \
their elements, a scriptlet ran before the page, and the shell's calls \
answered as the config says"
fi

if [ -n "$PASSED" ]; then
  echo "PASS: $PASSED"
  exit 0
fi

annotate "guard-content-blocking: $FAILURE"
echo "the engine's last words:" >&2
tail -40 "$ENGINE_LOG" >&2
echo "what the stand-in said:" >&2
tail -10 "$SOCKET_LOG" >&2
exit 1
