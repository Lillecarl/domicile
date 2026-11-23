# The nixpkgs whose Chromium recipe builds the engine: nixos-unstable on
# 2026-11-23, Chromium 158.0.8127.36, the nearest release recipe to the pin.
builtins.fetchTarball {
  url = "https://github.com/NixOS/nixpkgs/archive/2786a2a72088ecf32aea5b3ec52099e6a7cff78f.tar.gz";
  sha256 = "sha256-f/VVvxf5RsQgWVhUtefoca5ktVg0DA2bnkMOcmSnozo=";
}
