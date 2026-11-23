/**
  The engine built from source with nixpkgs' Chromium recipe (experiment).

  nixpkgs' recipe (nixpkgs.nix) at the pin's DEPS (chromium-info.json, from
  lock.py), with `src/` laid in, the series applied, Electron's Node placed
  and patched as `.github/scripts/engine-node.sh` does, and domicile's gn
  args after nixpkgs' own. In gn a later scalar assignment replaces an earlier
  one, so `is_official_build = false` and the component build win.

  # Outputs

  `engine`
  : the unwrapped engine tree

  `configured`
  : stops after `gn gen` and keeps args.gn and the ninja files, to read
*/
{
  pkgs ? import (import ./nixpkgs.nix) {
    config = { };
    overlays = [ ];
  },
}:
let
  inherit (pkgs) lib;
  series = ../.;
  info = lib.importJSON ./chromium-info.json;
  nodeLock = lib.importJSON ./node-info.json;
  # nixpkgs' recipe from nixpkgs.nix, vendored: a version-keyed table in it
  # (crubit) needs a 160 entry, and no override reaches a `let` binding.
  chromium = pkgs.callPackage ./chromium-recipe { upstream-info = info; };

  ordered = dir: lib.filter (p: p != "") (lib.splitString "\n" (builtins.readFile "${dir}/.patches"));
  applySeries =
    name: path:
    lib.concatMapStrings (p: ''
      patch -p1 --no-backup-if-mismatch -d ${path} < ${series}/node/patches/${name}/${p}
    '') (ordered "${series}/node/patches/${name}");

  domicilePatches = map (p: "${series}/patches/${p}") (
    lib.sort lib.lessThan (lib.attrNames (builtins.readDir "${series}/patches"))
  );

  nodeSrc = pkgs.fetchFromGitHub {
    owner = "nodejs";
    repo = "node";
    inherit (nodeLock) rev hash;
  };

  domicileArgs = ''
    is_official_build = false
    is_debug = false
    symbol_level = 0
    is_component_build = true
    use_ozone = true
    ozone_auto_platforms = false
    ozone_platform_wayland = true
    ozone_platform_headless = true
    ozone_platform_drm = true
    use_libinput = true
    import("//build/args/domicile_codecs.gn")
    import("//build/args/domicile_trim.gn")
    import("//build/args/domicile_node.gn")
  '';

  engine = chromium.browser.overrideAttrs (old: {
    pname = "domicile-engine";
    # `use_libinput`, for the DRM platform's touchpads.
    buildInputs = old.buildInputs ++ [ pkgs.libinput ];
    # The series is cut against the bare pin, so it goes on before nixpkgs'.
    patches = domicilePatches ++ old.patches;
    prePatch = (old.prePatch or "") + ''
      cp -r ${series}/src/. .
      chmod -R u+w .
    '';
    # A Linux checkout has OpenXR, and at 160 the VR browser tests need
    # //device/vr:openxr_data on Linux whatever enable_openxr says.
    postPatch = (old.postPatch or "") + ''
      substituteInPlace build/config/gclient_args.gni \
        --replace-fail 'checkout_openxr = false' 'checkout_openxr = true'
      ${applySeries "v8" "v8"}
      ${applySeries "boringssl" "third_party/boringssl/src"}
      cp -r ${nodeSrc} third_party/electron_node
      chmod -R u+w third_party/electron_node
      ${applySeries "node" "third_party/electron_node"}
    '';
    # nixpkgs' own configure phase, with domicile's args after nixpkgs' in the
    # one `--args` string; the shell joins the adjacent quoted words.
    configurePhase =
      let
        anchor = " out/Release | tee gn-gen-outputs.txt";
      in
      assert lib.assertMsg (lib.hasInfix anchor old.configurePhase)
        "nixpkgs' configurePhase no longer runs `gn gen ...${anchor}`";
      lib.replaceStrings
        [ anchor ]
        [
          "$'\\n'${lib.escapeShellArg domicileArgs}${anchor}"
        ]
        old.configurePhase;
  });

  configured = engine.overrideAttrs {
    pname = "domicile-engine-configured";
    outputs = [ "out" ];
    buildPhase = ''
      mkdir -p $out
      cp out/Release/args.gn out/Release/toolchain.ninja gn-gen-outputs.txt $out/
      (cd out/Release && find obj/base -name '*.ninja' -exec cp --parents {} $out/ \;)
    '';
    installPhase = ":";
    dontFixup = true;
    postFixup = "";
    doCheck = false;
    doInstallCheck = false;
  };
in
{
  inherit engine configured;
}
