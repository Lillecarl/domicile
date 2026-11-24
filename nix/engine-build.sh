# Runs in the engine's FHS environment, in a directory it owns.
#
#   engine-build.sh <fork's tree> <chromium/src> <depot_tools> <stage>
set -euo pipefail

fork="$1" src="$2" tools="$3" stage="$4"
export PATH="$tools:$PATH" DEPOT_TOOLS_UPDATE=0 DEPOT_TOOLS_METRICS=0
chromium="$(dirname "$src")"
pin="$(grep -v '^#' "$fork/packages/domicile-engine/CHROMIUM_PIN" | tr -d '[:space:]')"
cd "$fork"

# The scripts give unchanged files their old mtimes back, so a rebuild compiles
# what the series changed and no more.
.github/scripts/engine-reset.sh "$src"
if [ "$(cat "$chromium/.domicile-synced-pin" 2>/dev/null || true)" != "$pin" ]; then
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
autoninja -C out/Local chrome domicile_engine third_party/electron_node:node

# What engine-release-package.sh puts in a release tarball.
mkdir -p "$stage/locales" "$stage/dictionaries"
cd out/Local
cp chrome icudtl.dat resources.pak chrome_100_percent.pak libdomicile_engine.so "$stage/"
for file in chrome_200_percent.pak v8_context_snapshot.bin snapshot_blob.bin \
  chrome_crashpad_handler libEGL.so libGLESv2.so libvulkan.so.1 \
  libvk_swiftshader.so vk_swiftshader_icd.json; do
  if [ -e "$file" ]; then cp "$file" "$stage/"; fi
done
cp locales/*.pak "$stage/locales/"
cp dictionaries/*.bdic "$stage/dictionaries/"
for file in "$stage"/*; do
  if [ -f "$file" ] && [ "$(od -An -tx1 -N4 "$file")" = ' 7f 45 4c 46' ]; then
    "$src/third_party/llvm-build/Release+Asserts/bin/llvm-objcopy" --strip-all "$file"
  fi
done
echo "engine staged"
