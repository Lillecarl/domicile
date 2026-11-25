# Runs in the engine's FHS environment, in a directory it owns.
#
#   engine-build.sh <fork's tree> <chromium/src> <depot_tools> <target>...
set -euo pipefail

fork="$1" src="$2" tools="$3" targets=("${@:4}")
export PATH="$tools:$PATH" DEPOT_TOOLS_UPDATE=0 DEPOT_TOOLS_METRICS=0
chromium="$(dirname "$src")"
pin="$(grep -v '^#' "$fork/packages/domicile-engine/CHROMIUM_PIN" | tr -d '[:space:]')"
cd "$fork"

# Git knows a file by its inode, ctime and owner too, all new in a tree that
# `claim` copied: the reset would rewrite every file, and rebuild everything.
# A killed build's lock would stop the refresh; this one holds the tree.
rm -f "$src/.git/index.lock"
git -C "$src" update-index -q --refresh || true

# A rebuild of everything takes hours, so nothing starts one unasked. To ask,
# `touch` this; the build that uses it removes it.
allowed="$(dirname "$chromium")/allow-full-build"
refuse_unless_allowed() { # $1 how many, $2 of what, $3 the most an edit explains
  if [ "$1" -gt "$3" ]; then
    if [ ! -e "$allowed" ]; then
      echo "error: $1 $2, so this would rebuild most of Chromium; touch $allowed to let it" >&2
      exit 1
    fi
    echo "$1 $2; $allowed lets it"
    rebuilt=1
  fi
}
rebuilt=0
synced="$(cat "$chromium/.domicile-synced-pin" 2>/dev/null || true)"
if [ "$synced" = "$pin" ]; then
  # Before the reset, which would give each of them a new mtime.
  refuse_unless_allowed "$(git -C "$src" diff-files --name-only --ignore-submodules | wc -l)" \
    "files differ from git's index" 5000
else
  refuse_unless_allowed 1 "Chromium pin moved" 0
fi
# The scripts give unchanged files their old mtimes back, so a rebuild compiles
# what the series changed and no more.
.github/scripts/engine-reset.sh "$src"
if [ "$synced" != "$pin" ]; then
  # The pin moved: gclient brings every dependency to it.
  (cd "$chromium" && gclient sync --revision "src@$pin" --no-history --shallow \
    --reset --delete_unversioned_trees)
  # The sysroots unpack files private to this build's user, and the next one
  # could not copy them.
  chmod -R g+rwX "$chromium"
  printf '%s\n' "$pin" >"$chromium/.domicile-synced-pin"
fi
.github/scripts/engine-apply.sh "$src"

cd "$src"
# Siso, rustc and jinja2 write files private to this build's user, and the next
# one could not copy them. On a failed build too. Modes leave mtimes alone.
trap 'chmod -R g+rwX "$src/out"' EXIT
# Neither of CI's builds. Like the official one it has no DCHECKs, but no
# ThinLTO or PGO: their link needs more memory than a desktop has, and a
# rebuild would repeat all of it.
gn gen out/Local --args='
  is_debug = false
  is_component_build = false
  is_official_build = false
  dcheck_always_on = false
  symbol_level = 0
  blink_symbol_level = 0
  use_ozone = true
  ozone_auto_platforms = false
  ozone_platform_wayland = true
  ozone_platform_headless = true
  ozone_platform_drm = true
  use_libinput = true
  concurrent_links = 2
  cc_wrapper = "ccache"
  import("//build/args/domicile_codecs.gn")
  import("//build/args/domicile_trim.gn")
  import("//build/args/domicile_node.gn")
'
# The plan, with nothing compiled. One that cannot be read stops the build.
plan="$(autoninja -n -C out/Local "${targets[@]}" 2>&1 | tr '\r' '\n')"
steps="$(grep -oE 'Build Succeeded: [0-9]+ steps' <<<"$plan" | grep -oE '[0-9]+' | tail -1)" ||
  { echo "error: cannot tell how much this would build:" >&2; tail -20 <<<"$plan" >&2; exit 1; }
echo "to build: $steps steps"
refuse_unless_allowed "$steps" "steps to build" 5000
if [ "$rebuilt" = 1 ]; then rm -f "$allowed"; fi
autoninja -C out/Local "${targets[@]}"
