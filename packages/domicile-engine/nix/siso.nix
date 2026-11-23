/**
  The engine with its compiles on Buildbarn (experiment).

  `lib.buildbarn.chromium` from nixidae's pynixd: the derivation requires the
  `reapi` system feature, so a builder that binds a Buildbarn frontend takes
  it, and every C, C++ and assembler compile runs remotely.

  # Outputs

  `engine`
  : the whole engine

  `base`
  : only `//base`, with Siso's metrics as the output: the first test
*/
{
  engine ? (import ./engine.nix { }).engine,
  nixidae ? builtins.fetchTarball {
    url = "https://github.com/nixidae/nixidae/archive/25a1e2c9d1dad946f30b2a899821be19ef824589.tar.gz";
    sha256 = "sha256-W7iEraZX6vmXcXcdGlXLfJnpHpDCeKunLgFFLJeKZNM=";
  },
}:
let
  inherit ((import "${nixidae}/pynixd" { }).lib) buildbarn;
  remote = buildbarn.chromium { chromium = engine; };
in
{
  engine = remote;
  base = remote.overrideAttrs {
    pname = "domicile-engine-base";
    outputs = [ "out" ];
    buildPhase = ''
      runHook preBuild
      ninja -C out/Release base
      runHook postBuild
    '';
    installPhase = ''
      mkdir -p $out
      cp out/Release/siso_metrics.json out/Release/siso_compile_summary.json out/Release/siso_output $out/
    '';
    dontFixup = true;
    postFixup = "";
    doCheck = false;
    doInstallCheck = false;
  };
}
