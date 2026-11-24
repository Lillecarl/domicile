# Incremental builds that are still Nix derivations.
#
# The build runs outside the sandbox (`__noChroot`, which needs
# `sandbox = relaxed`) in a directory that outlives it, and installs to `$out`
# as any derivation does. The store path depends on the derivation's inputs
# alone, never on what the directory holds.
#
# The directory is made once, by root, writable by every nix build user:
#
# Without the setgid bit, which `claim`'s copy could not keep.
#
#   install -d -m 775 -g nixbld /srv/domicile-engine
#   setfacl -m g:nixbld:rwX /srv/domicile-engine
#   setfacl -d -m g:nixbld:rwX,m:rwx /srv/domicile-engine
{ pkgs, root ? "/srv/domicile-engine" }:
let
  # One build at a time in $1, owned by this build's user.
  #
  # Nix picks a free build user per build, and only a file's owner may set its
  # mtime, which cargo and siso both do. A reflink copy hands the tree over
  # without copying its data.
  claim = ''
    claim() {
      local dir="$1"
      exec 9>>"$dir.lock"
      flock 9
      mkdir -p "$dir"
      if [ "$(stat -c %u "$dir")" != "$(id -u)" ]; then
        # A name of this user's own: a copy a killed build left is writable by
        # its user alone.
        local copy="$dir.claimed.$(id -u)" old="$dir.old.$$"
        rm -rf "$copy"
        # Not the modes: nix's seccomp filter refuses the setgid bit and ACLs,
        # which the new directories inherit anyway.
        cp -RP --preserve=timestamps,links --reflink=always "$dir" "$copy"
        mv "$dir" "$old"
        mv "$copy" "$dir"
        rm -rf "$old"
      fi
    }
  '';

  # $1 into $2, rewriting only files whose bytes changed. Everything in the
  # store has one mtime, so a tree copied with its mtimes hides every edit.
  syncByContent = ''
    sync_by_content() {
      local from="$1" to="$2"
      shift 2
      mkdir -p "$to"
      # Group-writable, or the next build's user cannot remove it in `claim`.
      rsync --recursive --links --perms --chmod=ug+w --checksum --delete "$@" "$from/" "$to/"
    }
  '';
in
{
  inherit claim syncByContent root;

  /**
    The engine's files, built from the pin and the series in the checkout at
    `${root}/chromium/src`, and laid out as a release tarball holds them.

    `${root}/chromium` and `${root}/depot_tools` come from a first
    `gclient sync` by hand. A pin that moves later syncs itself.
  */
  engineFiles =
    let
      # What Chromium's tools/nix/shell.nix gives a build.
      fhs = pkgs.buildFHSEnv {
        name = "domicile-engine-env";
        targetPkgs = p: with p; [
          alsa-lib at-spi2-atk bash cairo ccache coreutils cups curl dbus diffutils
          expat file findutils gawk git glib gnugrep gnused gperf gzip libdrm libgbm
          libx11 libxcb libxcomposite libxdamage libxext libxfixes libxkbcommon
          libxrandr libxtst mesa nspr nss pango patch perl pkg-config procps python3
          systemd unzip util-linux which xz zlib
        ];
        runScript = "bash";
      };
      tree = pkgs.lib.fileset.toSource {
        root = ../.;
        fileset = pkgs.lib.fileset.unions [
          ../.github/scripts
          ../packages/domicile-engine/CHROMIUM_PIN
          ../packages/domicile-engine/node
          ../packages/domicile-engine/patches
          ../packages/domicile-engine/scripts/apply.sh
          ../packages/domicile-engine/src
        ];
      };
    in
    pkgs.runCommand "domicile-engine-files" {
      __noChroot = true;
      nativeBuildInputs = [ pkgs.util-linux pkgs.git ];
      requiredSystemFeatures = [ "big-parallel" ];
    } ''
      ${claim}
      umask 002
      claim ${root}/chromium
      cp -r --no-preserve=mode,ownership ${tree} fork
      chmod -R u+rwX fork
      chmod +x fork/.github/scripts/*.sh fork/packages/domicile-engine/scripts/apply.sh
      export HOME=$TMPDIR/home
      mkdir -p $HOME ${root}/ccache
      # Group-writable, for the next build's user.
      export CCACHE_DIR=${root}/ccache CCACHE_UMASK=002 CCACHE_MAXSIZE=60G
      export CCACHE_BASEDIR=${root}/chromium/src CCACHE_NOHASHDIR=1
      export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt
      export GIT_SSL_CAINFO=$SSL_CERT_FILE CURL_CA_BUNDLE=$SSL_CERT_FILE
      export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'
      ${fhs}/bin/domicile-engine-env ${./engine-build.sh} \
        $PWD/fork ${root}/chromium/src ${root}/depot_tools $PWD/stage
      umask 022
      mkdir $out
      cp -r --no-preserve=ownership stage/. $out/
    '';

  /**
    A `buildRustPackage` derivation, built in `${root}/<name>`.

    `binaries` are the ones to install. The kept target directory also holds
    what other builds left, so they cannot be found by looking.
  */
  incrementalRust = { name, binaries }: package: package.overrideAttrs (old: {
    __noChroot = true;
    # No LTO, unlike the workspace's release profile: rustc refuses it
    # together with incremental state, which is what makes a rebuild fast.
    env = (old.env or { }) // {
      CARGO_PROFILE_RELEASE_LTO = "off";
      CARGO_PROFILE_RELEASE_CODEGEN_UNITS = "16";
      CARGO_INCREMENTAL = "1";
      # The linker wrapper refuses to link in a directory that is neither the
      # store nor this build's own.
      NIX_ENFORCE_PURITY = "0";
    };
    nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.rsync pkgs.util-linux ];
    preBuild = (old.preBuild or "") + ''
      ${claim}
      ${syncByContent}
      umask 002
      claim ${root}/${name}
      # rustc's incremental locks are private to this build's user, and the
      # next one could not copy them. On a failed build too.
      shareTree() { chmod -R g+rwX ${root}/${name}; }
      exitHooks+=(shareTree)
      failureHooks+=(shareTree)
      sync_by_content "$PWD" ${root}/${name} --exclude=/target --exclude=/.cargo
      # The vendored crates from the store, not the copy in this build's own
      # directory: cargo rebuilds a crate whose path changed.
      mkdir -p ${root}/${name}/.cargo
      substitute "$cargoDeps/.cargo/config.toml" ${root}/${name}/.cargo/config.toml \
        --replace-fail '"cargo-vendor-dir"' "\"$cargoDeps\""
      cd ${root}/${name}
      # The seed is the output's hash, so it differs in every derivation, and
      # bindgen's crates rebuild whenever this variable changes.
      export BINDGEN_EXTRA_CLANG_ARGS="$(sed -E 's/-frandom-seed=[^ ]+//g' <<< "''${BINDGEN_EXTRA_CLANG_ARGS-}")"
    '';
    # cargo's own install hook takes every executable in the target directory.
    dontCargoInstall = true;
    installPhase = ''
      runHook preInstall
      # Nix rejects a group-writable output.
      umask 022
      for binary in ${toString binaries}; do
        install -Dm755 "target/${pkgs.stdenv.hostPlatform.rust.rustcTarget}/release/$binary" "$out/bin/$binary"
      done
      runHook postInstall
    '';
  });
}
