#!/usr/bin/env bash
# Tests how `guard-ad-blocking.sh` reads the engine's and the page server's
# logs and which results it passes.
#
# Runs the guard's own reading and verdict blocks, not copies. Key cases:
#
#   - default blocking another site's listed script, strict the page's own too,
#     and off loading everything passes.
#   - A blocked script the server served anyway fails: the block was not the
#     blocker's.
#   - off blocking anything fails: the control proves nothing then.
#   - off hiding anything, or dropping a parameter, fails for the same reason.
#   - Each profile but off reading the canvas, audio and processor count a
#     little off, the same way twice, passes; off's must be the truth.
#   - Each wrong letter names the part of the blocker it points at.
#   - The lists' other rules: a `$redirect` stand-in the server never served,
#     a removed parameter, a refused image, a scriptlet and a procedural
#     filter in default and strict, and none of them in off, passes.
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
GUARD="$ROOT/packages/domicile-engine/scripts/guard-ad-blocking.sh"
[ -f "$GUARD" ] || {
  echo "no guard at $GUARD" >&2
  exit 1
}

READING="$(awk '/^READ="\$\(awk/,/"\$HTTP_LOG" 2>\/dev\/null\)"$/' "$GUARD")"
BLOCK="$(awk '/^FAILURE=""$/,/^esac$/' "$GUARD")"
EXTRAS_READING="$(awk '/^READ_EXTRAS="\$\(awk/,/"\$HTTP_LOG" 2>\/dev\/null\)"$/' "$GUARD")"
EXTRAS_BLOCK="$(awk '/^EXTRA_FAILURE=""$/,/^esac$/' "$GUARD")"
[ -n "$READING" ] && [ -n "$BLOCK" ] && [ -n "$EXTRAS_READING" ] && [ -n "$EXTRAS_BLOCK" ] || {
  echo "no reading or verdict block in $GUARD — its markers moved. Fix this test with it." >&2
  exit 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FAILED=0
expect() {
  local what="$1" want="$2" got="$3"
  if [ "$got" = "$want" ]; then
    printf '  ok    %s\n' "$what"
  else
    printf '  FAIL  %s\n    wanted: %s\n    got:    %s\n' "$what" "$want" "$got"
    FAILED=$((FAILED + 1))
  fi
}

# Writes the engine's log, one console line per argument, and the server's,
# one `served` line per `served=AS:WHAT` argument.
log() { # lines...
  : >"$WORK/engine.log"
  : >"$WORK/http.log"
  for line in "$@"; do
    case "$line" in
    served=*)
      line="${line#served=}"
      printf 'GUARD served as=%s what=%s\n' "${line%%:*}" "${line#*:}" >>"$WORK/http.log"
      printf '127.0.0.1 - "GET /x.js?as=%s HTTP/1.1" 200 -\n' "${line%%:*}" >>"$WORK/http.log"
      ;;
    *)
      printf '[1:1:INFO:CONSOLE(7)] "GUARD %s", source: http://127.0.0.1:9/page (7)\n' "$line" >>"$WORK/engine.log"
      ;;
    esac
  done
}

reads() { # lines...
  log "$@"
  (
    ENGINE_LOG="$WORK/engine.log"
    HTTP_LOG="$WORK/http.log"
    eval "$READING"
    echo "$READ"
  )
}

verdict() { # $1 MEASURED
  (
    MEASURED="$1"
    eval "$BLOCK"
    if [ -n "$PASSED" ]; then
      echo "pass"
    elif [ -n "$FAILURE" ]; then
      echo "fail"
    else
      echo "neither"
    fi
  )
}

says() { # $1 MEASURED, $2 what the sentence must contain
  case "$(
    MEASURED="$1"
    eval "$BLOCK"
    echo "$FAILURE"
  )" in
  *"$2"*) echo yes ;;
  *) echo no ;;
  esac
}

ads() { # $1 as, then third first mine allowed ok
  echo "ads as=$1 third=$2 first=$3 mine=$4 allowed=$5 ok=$6"
}

hidden() { # $1 as, then site generic complex late kept plain
  echo "hidden as=$1 site=$2 generic=$3 complex=$4 late=$5 kept=$6 plain=$7"
}

landed() { # $1 as, then keep fbclid utm_source
  echo "landed as=$1 keep=$2 fbclid=$3 utm_source=$4"
}

device() { # $1 as, then cores canvas canvas_again image audio
  echo "device as=$1 cores=$2 canvas=$3 canvas_again=$4 image=$5 audio=$6"
}

echo "reading the logs"
expect "the three profiles as configured" "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" \
  "$(reads shell-loaded \
    "$(ads default blocked loaded blocked loaded loaded)" \
    "$(ads strict blocked blocked blocked loaded loaded)" \
    "$(ads off loaded loaded loaded loaded loaded)" \
    "$(hidden default hidden hidden hidden hidden shown shown)" \
    "$(hidden strict hidden hidden hidden hidden shown shown)" \
    "$(hidden off shown shown shown shown shown shown)" \
    "$(landed default kept dropped kept)" \
    "$(landed strict kept dropped dropped)" \
    "$(landed off kept kept kept)" \
    "$(device default 4 c1 c1 i1 a1)" \
    "$(device strict 3 c2 c2 i2 a2)" \
    "$(device off 8 c0 c0 i0 a0)" \
    served=default:first served=default:allowed served=default:ok \
    served=off:third served=off:mine)"
expect "a page that never reported" "1 ????? ????? ????? 0 ?????? ?????? ?????? ??? ??? ??? ????? ????? ?" "$(reads shell-loaded)"
expect "an empty log reads nothing" "0 ????? ????? ????? 0 ?????? ?????? ?????? ??? ??? ??? ????? ????? ?" "$(reads)"
expect "a blocked script the server served is a leak" "1 BLBLL ????? ????? 1 ?????? ?????? ?????? ??? ??? ??? ????? ????? ?" \
  "$(reads shell-loaded "$(ads default blocked loaded blocked loaded loaded)" \
    served=default:third)"
expect "a page that read the device's true details" "1 ????? ????? ????? 0 ?????? ?????? ?????? ??? ??? ??? TTTKS CTCXV S" \
  "$(reads shell-loaded "$(device default 8 c0 c0 i0 a0)" \
    "$(device strict 9 c2 c3 i0 a2)" "$(device off 8 c0 c0 i0 a0)")"
expect "one processor stays one" "1 ????? ????? ????? 0 ?????? ?????? ?????? ??? ??? ??? CCCKS ????? S" \
  "$(reads shell-loaded "$(device default 1 c1 c1 i1 a1)" "$(device off 1 c0 c0 i0 a0)")"
expect "nothing to compare without off" "1 ????? ????? ????? 0 ?????? ?????? ?????? ??? ??? ??? ????? ????? ?" \
  "$(reads shell-loaded "$(device default 4 c1 c1 i1 a1)")"
expect "a script served for another profile is not" "1 BLBLL ????? ????? 0 ?????? ?????? ?????? ??? ??? ??? ????? ????? ?" \
  "$(reads shell-loaded "$(ads default blocked loaded blocked loaded loaded)" \
    served=off:third)"

# MEASURED is "<shell> <default> <strict> <off> <leaked> <default> <strict>
# <off> <default> <strict> <off> <default> <strict> <off>", each profile as third first mine allowed
# ok, then as site generic complex late kept plain, then as keep fbclid
# utm_source, then "<default> <strict>" as canvas image audio cores again and
# "<off>" as again.
echo
echo "the verdict"
expect "the three profiles as configured is the pass" "pass" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "a shell that never ran is a failure" "fail" \
  "$(verdict "0 ????? ????? ????? 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "a page that never reported is a failure" "fail" \
  "$(verdict "1 BLBLL ????? LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "a served blocked script is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 1 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and says it failed after the network" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 1 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "after the network")"
expect "off blocking anything is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL BLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and says the claim then proves nothing" "yes" \
  "$(says "1 BLBLL BBBLL LLBLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "prove nothing")"
expect "default loading another site's listed script is a failure" "fail" \
  "$(verdict "1 LLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names the proxy" "yes" \
  "$(says "1 LLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "ProxyAdBlocking")"
expect "default blocking the page's own is a failure" "fail" \
  "$(verdict "1 BBBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names standard's third-party check" "yes" \
  "$(says "1 BBBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "third_party")"
expect "the user's rule not blocking is a failure" "fail" \
  "$(verdict "1 BLLLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names block_rules" "yes" \
  "$(says "1 BLLLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "block_rules")"
expect "the exception blocked is a failure" "fail" \
  "$(verdict "1 BLBLL BBBBL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names exceptions" "yes" \
  "$(says "1 BLBLL BBBBL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "exception")"
expect "an unlisted script blocked is a failure" "fail" \
  "$(verdict "1 BLBLB BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "strict loading the page's own is a failure" "fail" \
  "$(verdict "1 BLBLL BLBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names the strict mode" "yes" \
  "$(says "1 BLBLL BLBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "AdBlockingModeOf")"
expect "strict loading what default blocked is a failure" "fail" \
  "$(verdict "1 BLBLL LLLLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and says the mode is not strict's" "yes" \
  "$(says "1 BLBLL LLLLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "not the one used")"

expect "off hiding anything is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSHSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and says the claim then proves nothing" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS HSSSSS KDK KDD KKK CCCKS CCCKS S" "proves nothing")"
expect "default hiding nothing is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 SSSSSS SSSSSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names the renderer's hook" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 SSSSSS SSSSSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "patch 0292")"
expect "the site's own rule not hiding is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 SHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names CosmeticsFor" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 SHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "CosmeticsFor")"
expect "a generic class not hiding is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HSHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names the gathering" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HSHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "Gather")"
expect "a generic rule that is not a class not hiding is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHSHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names the generic selectors" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHSHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "generic selectors")"
expect "a late class not hiding is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHSSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names the timer" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHSSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "timer")"
expect "the site's exception hidden is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHHS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and names the exceptions" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHHS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "exceptions")"
expect "an unnamed element hidden is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSH HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "strict hiding other than default is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS SSSSSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"
expect "and says they are the same" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS SSSSSS SSSSSS KDK KDD KKK CCCKS CCCKS S" "same")"

expect "off losing a parameter is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KDK CCCKS CCCKS S")"
expect "and says the drops then prove nothing" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKD CCCKS CCCKS S" "prove nothing")"
expect "a link losing a parameter that is no tracking is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS DDK KDD KKK CCCKS CCCKS S")"
expect "and names WithoutTracking" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK DDD KKK CCCKS CCCKS S" "WithoutTracking drops")"
expect "default keeping the click id is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KKK KDD KKK CCCKS CCCKS S")"
expect "and names the throttle" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KKK KDD KKK CCCKS CCCKS S" "MaybeCreateAndAdd")"
expect "default dropping the campaign parameter is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDD KDD KKK CCCKS CCCKS S")"
expect "strict keeping the campaign parameter is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDK KKK CCCKS CCCKS S")"
expect "and names strict's mode" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDK KKK CCCKS CCCKS S" "strict profile's mode")"
expect "strict hiding other than default, with every link right, is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHSHSS SSSSSS KDK KDD KKK CCCKS CCCKS S")"

expect "default reading every true detail is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK TTTKS CCCKS S")"
expect "and names patch 0293" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK TTTKS CCCKS S" "patch 0293")"
expect "strict reading every true detail is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS TTTKS S")"
expect "and names DeviceDetailsHiding" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS TTTKS S" "DeviceDetailsHiding")"
expect "a true canvas export is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK TCCKS CCCKS S")"
expect "and names ToDataURLInternal" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK TCCKS CCCKS S" "ToDataURLInternal")"
expect "true pixels is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CTCKS CCCKS S")"
expect "and names DomicileFarbleImageData" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CTCKS CCCKS S" "DomicileFarbleImageData")"
expect "true audio is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCTKS S")"
expect "and names DomicileFarbleAudio" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCTKS S" "DomicileFarbleAudio")"
expect "a processor count above the true one is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCXS CCCKS S")"
expect "and names FarbledHardwareConcurrency" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCXS CCCKS S" "FarbledHardwareConcurrency")"
expect "a canvas farbled two ways is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKV S")"
expect "and names FarblePixels" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKV S" "FarblePixels")"
expect "off's canvas varying by itself is a failure" "fail" \
  "$(verdict "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS V")"
expect "and names varies on this machine" "yes" \
  "$(says "1 BLBLL BBBLL LLLLL 0 HHHHSS HHHHSS SSSSSS KDK KDD KKK CCCKS CCCKS V" "varies on this machine")"

extras_reads() { # lines...
  log "$@"
  (
    ENGINE_LOG="$WORK/engine.log"
    HTTP_LOG="$WORK/http.log"
    eval "$EXTRAS_READING"
    echo "$READ_EXTRAS"
  )
}

extras_says() { # $1 EXTRAS, $2 what the sentence must contain, or "" for a pass
  (
    EXTRAS="$1"
    eval "$EXTRAS_BLOCK"
    case "$EXTRA_FAILURE" in
    "") [ -z "$2" ] && echo yes || echo no ;;
    *"$2"*) [ -n "$2" ] && echo yes || echo no ;;
    *) echo no ;;
    esac
  )
}

extras() { # $1 as, then stub rp csp js proc
  echo "extras as=$1 stub=$2 rp=$3 csp=$4 js=$5 proc=$6"
}

echo
echo "the lists' other rules"
expect "the three profiles as configured" "SDARH SDARH LKNNS" \
  "$(extras_reads \
    "$(extras default loaded dropped applied ran hidden)" \
    "$(extras strict loaded dropped applied ran hidden)" \
    "$(extras off loaded kept none none shown)" \
    served=off:stub)"
expect "a stub the server served is the server's script" "LDARH ????? ?????" \
  "$(extras_reads "$(extras default loaded dropped applied ran hidden)" \
    served=default:stub)"
expect "a page that never reported" "????? ????? ?????" "$(extras_reads)"
expect "the three profiles as configured is the pass" "yes" \
  "$(extras_says "SDARH SDARH LKNNS" "")"
expect "a missing report is a failure" "yes" \
  "$(extras_says "SDARH ????? LKNNS" "never said")"
expect "off changed is a failure that proves nothing" "yes" \
  "$(extras_says "SDARH SDARH LKNRS" "prove nothing")"
expect "a blocked stub names the resources" "yes" \
  "$(extras_says "BDARH SDARH LKNNS" "no resources")"
expect "a kept parameter names the rewrite" "yes" \
  "$(extras_says "SKARH SDARH LKNNS" "kRewrite")"
expect "a refused-nothing image names the policy" "yes" \
  "$(extras_says "SDNRH SDARH LKNNS" "PopulateParsedHeaders")"
expect "a scriptlet that never ran names patch 0296" "yes" \
  "$(extras_says "SDANH SDARH LKNNS" "0296")"
expect "a procedural filter that hid nothing names its world" "yes" \
  "$(extras_says "SDARS SDARH LKNNS" "PROCEDURAL_FILTERS")"
expect "strict unlike default is a failure" "yes" \
  "$(extras_says "SDARH SDANH LKNNS" "strict did other")"

echo
if [ "$FAILED" -eq 0 ]; then
  echo "the ad-blocking guard reads its logs and blames the right end"
  exit 0
fi
echo "$FAILED case(s) wrong" >&2
exit 1
