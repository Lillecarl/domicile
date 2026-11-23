/**
  The engine built on Buildbarn's Chromium-image pool (experiment).

  Nix does the setup and the install. The setup lays out the tree that
  `gclient sync` gives: the pin's git deps, CIPD and GCS packages
  (chromium-info.json, from lock.py), the series, `src/` and Electron's Node.
  `gn gen` then takes scripts/build.sh's args plus `use_remoteexec`.

  Siso runs with Chromium's own config. Our backend.star names the pool's
  platform and turns on `googlechrome`, `default-remote` and `remote-link`,
  so every action that has a rule, and every action that has none, runs
  remotely in the image Chromium's prebuilt tools are made for. They cannot
  run in the Nix sandbox: they load `/lib64/ld-linux-x86-64.so.2`.

  The derivation requires the `reapi` system feature, so it lands on a
  builder that binds a Buildbarn frontend at `socket`.

  # Outputs

  `tree`
  : the engine as the release tarball holds it (engine-release-package.sh),
  plus the component build's libraries, unpatched

  `configured`
  : stops after `gn gen`, with args.gn and the ninja files, to read

  `base`
  : `//base` only, with Siso's metrics as the output: the first test
*/
{
  pkgs ? import (import ./nixpkgs.nix) {
    config = { };
    overlays = [ ];
  },
  socket ? "/run/reapi.sock",
  instance ? "",
  remoteJobs ? 256,
}:
let
  inherit (pkgs) lib;
  series = ../.;
  info = lib.importJSON ./chromium-info.json;
  nodeLock = lib.importJSON ./node-info.json;

  # Only for its fetchers: `chromiumDeps` is one fixed-output derivation per
  # git dep, with src recompressed.
  recipe = pkgs.callPackage ./chromium-recipe { upstream-info = info; };
  inherit (recipe.browser.passthru) chromiumDeps;

  gn = (pkgs.gn.override info.deps.gn).overrideAttrs (old: {
    # gen.py takes LD from the environment as its link driver, and stdenv's
    # LD is a bare `ld`, which refuses `-Wl,--gc-sections`.
    preConfigure = (old.preConfigure or "") + ''
      export LD="$CXX"
    '';
  });

  storeName = s: lib.replaceStrings [ "/" "+" "@" ":" ] [ "-" "-" "-" "-" ] s;

  cipd = lib.mapAttrs (
    _:
    map (
      p:
      pkgs.fetchurl {
        name = "cipd-${storeName p.package}.zip";
        inherit (p) url sha256;
      }
    )
  ) info.cipd;

  gcs = lib.mapAttrs (
    _:
    map (
      o:
      o
      // {
        src = pkgs.fetchurl {
          name = storeName (baseNameOf o.file);
          inherit (o) url sha256;
        };
      }
    )
  ) info.gcs;

  # A path in the lock starts with `src/`; the tree's root is `src`.
  inTree = path: lib.removePrefix "src/" path;

  unpackGit = lib.concatStrings (
    lib.mapAttrsToList (
      path: dep:
      (
        if dep.unpack or false then
          ''
            mkdir -p ${path}
            tar -xf ${dep} -C ${path} --use-compress-program="zstd -T$NIX_BUILD_CORES"
          ''
        else
          ''
            mkdir -p ${dirOf path}
            cp -r ${dep}/. ${path}
          ''
      )
      # A store copy is read-only, and the next dep may go inside it.
      + ''
        chmod -R u+w ${path}
      ''
    ) chromiumDeps
  );

  # As gclient: a CIPD package is a zip with its metadata in `.cipdpkg`.
  unpackCipd = lib.concatStrings (
    lib.mapAttrsToList (
      path: zips:
      lib.concatMapStrings (zip: ''
        mkdir -p ${path}
        unzip -q -o ${zip} -d ${path} -x '.cipdpkg/*'
      '') zips
    ) cipd
  );

  # As gclient's GcsDependency: a tarball is extracted into the path, anything
  # else is copied there under its output name and made executable.
  unpackGcs = lib.concatStrings (
    lib.mapAttrsToList (
      path: objects:
      lib.concatMapStrings (o: ''
        mkdir -p ${path}
        if tar -tf ${o.src} > /dev/null 2>&1; then
          tar -xf ${o.src} -C ${path}
        else
          install -m 0755 ${o.src} ${path}/${baseNameOf o.file}
        fi
      '') objects
    ) gcs
  );

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

  # The pool's platform, exactly: Buildbarn matches every property.
  backendStar = pkgs.writeText "backend.star" ''
    load("@builtin//struct.star", "module")

    def __platform_properties(ctx):
        props = {
            "OSFamily": "Linux",
            "container-image": "docker://gcr.io/chops-public-images-prod/rbe/siso-chromium/linux@sha256:62ad06f095562b2123928d956b68f778d3279b62f6698806afec2e40eaf67dd7",
        }
        return {"default": props, "large": props}

    def __configs(ctx):
        return ["googlechrome", "default-remote", "remote-link"]

    backend = module(
        "backend",
        platform_properties = __platform_properties,
        configs = __configs,
    )
  '';

  # Chromium's main.star, renamed, with one rule first: Node's and
  # Electron's GN actions (third_party/electron_node, electron) run locally.
  # Their GN leaves inputs undeclared (.pdl includes, mksnapshot's component
  # libraries) and passes absolute paths, so they fail on a worker. Their
  # compiles match no action rule and stay remote.
  mainStar = pkgs.writeText "main.star" ''
    load("@builtin//encoding.star", "json")
    load("@builtin//struct.star", "module")
    load("./chromium_main.star", chromium_init = "init")

    def init(ctx):
        chromium = chromium_init(ctx)
        step_config = json.decode(chromium.step_config)
        step_config["rules"].insert(0, {
            "name": "domicile/electron_node",
            "action": "__(third_party_electron_node|electron)_.*",
            "remote": False,
        })
        return module(
            "config",
            step_config = json.encode(step_config),
            filegroups = chromium.filegroups,
            handlers = chromium.handlers,
        )
  '';

  # scripts/build.sh's args, with remote execution.
  gnArgs = ''
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
    use_remoteexec = true
    # gclient's default comes from a .gclient this tree does not have.
    use_siso = true
  '';

  # What gclient's hooks write. gclient_args.gni is nixpkgs' with OpenXR:
  # at 160 the VR browser tests need //device/vr:openxr_data on Linux.
  hooks = ''
    cp ${./chromium-recipe/files/gclient_args.gni} build/config/gclient_args.gni
    chmod u+w build/config/gclient_args.gni
    substituteInPlace build/config/gclient_args.gni \
      --replace-fail 'checkout_openxr = false' 'checkout_openxr = true'
    cat >> build/config/gclient_args.gni <<EOF
    checkout_mutter = false
    checkout_glic_e2e_tests = false
    checkout_clusterfuzz_data = false
    EOF

    echo 'LASTCHANGE=${info.DEPS."src".rev}-refs/tags/${info.version}@{#0}' > build/util/LASTCHANGE
    echo "$SOURCE_DATE_EPOCH" > build/util/LASTCHANGE.committime
    lastchange() {
      printf '/* Generated by lastchange.py, do not edit.*/\n#ifndef %s_\n#define %s_\n#define %s "%s"\n#endif  // %s_\n' \
        "$1" "$1" "$2" "$3" "$1" > "$4"
    }
    lastchange GPU_CONFIG_GPU_LISTS_VERSION_H GPU_LISTS_VERSION ${info.DEPS."src".rev} gpu/config/gpu_lists_version.h
    lastchange SKIA_EXT_SKIA_COMMIT_HASH_H SKIA_COMMIT_HASH ${
      info.DEPS."src/third_party/skia".rev
    } skia/ext/skia_commit_hash.h
    lastchange GPU_WEBGPU_DAWN_COMMIT_HASH_H DAWN_COMMIT_HASH ${
      info.DEPS."src/third_party/dawn".rev
    } gpu/webgpu/dawn_commit_hash.h
    echo -n '${info.DEPS."src/third_party/dawn".rev}' > gpu/webgpu/DAWN_VERSION

    install -m 0644 ${backendStar} build/config/siso/backend_config/backend.star
    mv build/config/siso/main.star build/config/siso/chromium_main.star
    install -m 0644 ${mainStar} build/config/siso/main.star
  '';

  siso = "third_party/siso/cipd/siso";
  # gn gen and Siso run in it: gn's exec_script and the steps Chromium keeps
  # local run Chromium's own host Python and clang, which load
  # /lib64/ld-linux-x86-64.so.2 (fhs.nix). Nix's Python will not do: a
  # remote step writes .pyc archives (grit.pyz) for Python 3.11, and a local
  # step must read them.
  fhs = (import ./fhs.nix { inherit pkgs; }).env;

  # The tree `gclient sync` and `gclient runhooks` give, then `gn gen`.
  setup = ''
    mkdir build && cd build
    ${unpackGit}
    ${unpackCipd}
    ${unpackGcs}
    cd src

    cp -r ${series}/src/. .
    chmod -R u+w .
    for p in ${toString domicilePatches}; do
      patch -p1 --no-backup-if-mismatch < "$p"
    done
    ${applySeries "v8" "v8"}
    ${applySeries "boringssl" "third_party/boringssl/src"}
    cp -r ${nodeSrc} third_party/electron_node
    chmod -R u+w third_party/electron_node
    ${applySeries "node" "third_party/electron_node"}
    ${hooks}

    mkdir -p out/Release
    cp ${pkgs.writeText "args.gn" gnArgs} out/Release/args.gn
    chmod u+w out/Release/args.gn
    ${fhs}/bin/chromium-fhs -c '${lib.getExe gn} gen out/Release'
  '';

  derivation =
    {
      pname,
      buildPhase,
      installPhase,
      requiredSystemFeatures ? [ ],
    }:
    pkgs.stdenvNoCC.mkDerivation {
      inherit
        pname
        installPhase
        requiredSystemFeatures
        ;
      inherit (info) version;
      buildPhase = ''
        runHook preBuild
        ${setup}
        ${buildPhase}
        runHook postBuild
      '';

      nativeBuildInputs = [
        # Node's generate_config_gypi.py runs `gn args` from PATH.
        gn
        pkgs.pkg-config
        pkgs.unzip
        pkgs.zstd
        pkgs.jq
      ];

      dontUnpack = true;
      dontConfigure = true;
      dontFixup = true;
    };

  build =
    {
      pname,
      targets,
      installPhase,
    }:
    derivation {
      inherit pname installPhase;
      requiredSystemFeatures = [ "reapi" ];
      buildPhase = ''
        # In an FHS environment: steps Chromium's config keeps local run its
        # prebuilt tools, which need /lib64's loader (fhs.nix).
        export HOME=$TMPDIR RBE_service_no_security=true
        ${fhs}/bin/chromium-fhs -c '${siso} ninja -C out/Release \
          -reapi_address unix://${socket} -reapi_instance "${instance}" \
          -strict_remote -k 0 -local_jobs "$NIX_BUILD_CORES" -remote_jobs ${toString remoteJobs} \
          ${toString targets}' || {
          # The build directory goes with a failed build; keep Siso's own log.
          grep -E '^[EW]' out/Release/siso.INFO | tail -100 >&2 || true
          exit 1
        }

        jq -s '{
            remote: map(select(.is_remote and (.cached | not))) | length,
            cached: map(select(.cached)) | length,
            local: map(select((.is_remote or .cached) | not)) | map(.rule // .action // "") | group_by(.) | map({(.[0]): length}) | add
          }' out/Release/siso_metrics.json > out/Release/siso_summary.json
        echo "siso: $(< out/Release/siso_summary.json)" >&2
      '';
    };

  configured = derivation {
    pname = "domicile-engine-configured";
    buildPhase = ":";
    installPhase = ''
      mkdir -p $out
      cp out/Release/args.gn out/Release/build.ninja out/Release/toolchain.ninja $out/
      (cd out/Release && find obj/base -name '*.ninja' -exec cp --parents {} $out/ \;)
    '';
  };

  # engine-release-package.sh's lists, and the component build's libraries.
  required = [
    "chrome"
    "icudtl.dat"
    "resources.pak"
    "chrome_100_percent.pak"
    "libdomicile_engine.so"
  ];
  optional = [
    "chrome_200_percent.pak"
    "v8_context_snapshot.bin"
    "snapshot_blob.bin"
    "chrome_crashpad_handler"
    "vk_swiftshader_icd.json"
  ];

  tree = build {
    pname = "domicile-engine-tree";
    targets = [
      "chrome"
      "domicile_engine"
    ];
    installPhase = ''
      mkdir -p $out/locales $out/dictionaries $out/nix-support
      cd out/Release
      cp -a ${toString required} $out/
      for f in ${toString optional}; do
        if [ -e "$f" ]; then cp -a "$f" $out/; fi
      done
      shopt -s nullglob
      cp -a *.so *.so.* $out/
      cp -a locales/*.pak $out/locales/
      cp -a dictionaries/*.bdic $out/dictionaries/
      cp siso_summary.json args.gn $out/nix-support/
    '';
  };

  base = build {
    pname = "domicile-engine-base";
    targets = [ "base" ];
    installPhase = ''
      mkdir -p $out
      cp out/Release/siso_metrics.json out/Release/siso_summary.json out/Release/args.gn $out/
    '';
  };
in
{
  inherit
    tree
    base
    configured
    gn
    cipd
    gcs
    ;
}
