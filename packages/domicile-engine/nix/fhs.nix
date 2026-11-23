/**
  The FHS environment Siso runs in, so a step Chromium keeps local can run
  Chromium's prebuilt tools: they load /lib64/ld-linux-x86-64.so.2, libz,
  libgcc_s and libstdc++, as on the Ubuntu image the remote steps run in.

  # Outputs

  `env`
  : the buildFHSEnv wrapper; `env/bin/chromium-fhs -c CMD` runs CMD inside

  `probe`
  : runs Chromium's clang bare (must fail) and inside `env` (must work),
  in the Nix sandbox
*/
{
  pkgs ? import (import ./nixpkgs.nix) {
    config = { };
    overlays = [ ];
  },
}:
let
  inherit (pkgs) lib;
  env = pkgs.buildFHSEnv {
    name = "chromium-fhs";
    targetPkgs = p: [
      p.zlib
      p.stdenv.cc.cc.lib
    ];
    runScript = "bash";
  };
  info = lib.importJSON ./chromium-info.json;
  clang = lib.findFirst (
    o: lib.hasInfix "/clang-llvmorg" o.url
  ) null info.gcs."src/third_party/llvm-build/Release+Asserts";
  clangTar = pkgs.fetchurl { inherit (clang) url sha256; };
  probe = pkgs.runCommand "chromium-fhs-probe" { } ''
    mkdir llvm && tar -xf ${clangTar} -C llvm
    if llvm/bin/clang++ --version; then
      echo "Chromium's clang ran bare in the sandbox; the probe proves nothing" >&2
      exit 1
    fi
    ${env}/bin/chromium-fhs -c "llvm/bin/clang++ --version" | tee $out
  '';
in
{
  inherit env probe;
}
