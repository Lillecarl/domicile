# What engine-release-package.sh puts in a release tarball.
#
#   engine-stage.sh <chromium/src> <stage>
set -euo pipefail

src="$1" stage="$2"
mkdir -p "$stage/locales" "$stage/dictionaries"
cd "$src/out/Local"
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
