#!/usr/bin/env bash
# Asserts `engine-crates.sh` puts each crate `crates/CRATES` names into the
# checkout's `third_party/rust/chromium_crates_io/vendor`, as `cargo vendor`
# leaves it, and keeps the Chromium tree clean.
#
# - **As vendored:** the `.crate`'s files, its own `Cargo.lock` dropped, and
#   `.cargo-checksum.json` as Chromium's vendored crates have it.
# - **The tree stays clean.** `apply.sh` refuses a dirty tree.
# - **Warm runs do nothing**, so ninja sees the old mtimes.
# - **A crate whose sum is not the manifest's fails the run** and is not laid
#   in.
# - **A crate the manifest drops goes.**
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CRATES_SH="$ROOT/.github/scripts/engine-crates.sh"
[ -x "$CRATES_SH" ] || { echo "no $CRATES_SH" >&2; exit 1; }
for tool in git tar curl sha256sum; do
  command -v "$tool" >/dev/null || { echo "  SKIP: no $tool"; exit 77; }
done

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
gitq() { git -c user.email=t@example.invalid -c user.name=t "$@"; }

# A crates.io stand-in: <name>/<name>-<version>.crate, as static.crates.io
# serves them.
MIRROR="$WORK/mirror"
crate() { # name version
  local dir="$WORK/pack/$1-$2"
  mkdir -p "$dir/src" "$MIRROR/$1"
  printf '[package]\nname = "%s"\nversion = "%s"\n' "$1" "$2" >"$dir/Cargo.toml"
  printf 'pub fn f() {}\n' >"$dir/src/lib.rs"
  printf '# a lock cargo vendor drops\n' >"$dir/Cargo.lock"
  tar -czf "$MIRROR/$1/$1-$2.crate" -C "$WORK/pack" "$1-$2"
  sha256sum "$MIRROR/$1/$1-$2.crate" | cut -d' ' -f1
}
one="$(crate one 1.2.3)"
two="$(crate two 0.4.0)"

FAKE="$WORK/repo"
mkdir -p "$FAKE/.github/scripts" "$FAKE/packages/domicile-engine/crates"
cp "$CRATES_SH" "$FAKE/.github/scripts/"
manifest() { printf '%s\n' "# a comment" "$@" >"$FAKE/packages/domicile-engine/crates/CRATES"; }
manifest "one 1.2.3 one-v1 $one" "two 0.4.0 two-v0_4 $two"

TREE="$WORK/chromium/src"
mkdir -p "$TREE/third_party/rust/chromium_crates_io/vendor"
printf 'tracked\n' >"$TREE/third_party/rust/chromium_crates_io/vendor/.keep"
gitq -C "$TREE" init -q
gitq -C "$TREE" add -A && gitq -C "$TREE" commit -qm pin
VENDOR="$TREE/third_party/rust/chromium_crates_io/vendor"

run() { DOMICILE_CRATES_URL="file://$MIRROR" "$FAKE/.github/scripts/engine-crates.sh" "$TREE" 2>&1; }

out="$(run)"
expect "a first run succeeds" "0" "$?"
expect "lays in the crate's files" "pub fn f() {}" "$(cat "$VENDOR/one-v1/src/lib.rs" 2>/dev/null)"
expect "in the directory the manifest names" "yes" "$([ -f "$VENDOR/two-v0_4/Cargo.toml" ] && echo yes)"
expect "drops the crate's own Cargo.lock" "no" "$([ -e "$VENDOR/one-v1/Cargo.lock" ] && echo yes || echo no)"
expect "writes .cargo-checksum.json as Chromium's crates have it" '{"files":{}}' "$(cat "$VENDOR/one-v1/.cargo-checksum.json" 2>/dev/null)"
expect "leaves the Chromium tree clean" "" "$(git -C "$TREE" status --porcelain)"

touch -d @1000000000 "$VENDOR/one-v1/src/lib.rs"
out="$(run)"
expect "a warm run succeeds" "0" "$?"
expect "and leaves a crate it already laid in alone" "1000000000" "$(stat -c %Y "$VENDOR/one-v1/src/lib.rs")"

manifest "one 1.2.3 one-v1 $one" "two 0.4.0 two-v0_4 0000000000000000000000000000000000000000000000000000000000000000"
rm -rf "$VENDOR/two-v0_4"
out="$(run)"
expect "a sum that is not the manifest's fails the run" "1" "$?"
expect "and names the crate" "yes" "$(printf '%s' "$out" | grep -q "two-0.4.0" && echo yes)"
expect "and lays nothing in for it" "no" "$([ -e "$VENDOR/two-v0_4" ] && echo yes || echo no)"

manifest "one 1.2.3 one-v1 $one"
out="$(run)"
expect "a run after the manifest drops a crate succeeds" "0" "$?"
expect "and removes the crate it dropped" "no" "$([ -e "$VENDOR/two-v0_4" ] && echo yes || echo no)"
expect "but keeps what Chromium vendors itself" "tracked" "$(cat "$VENDOR/.keep")"

[ "$FAILED" -eq 0 ] && echo "all ok" || { echo "$FAILED failed"; exit 1; }
