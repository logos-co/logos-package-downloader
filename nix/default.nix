# Common build configuration shared across all packages
{ pkgs, logosPackageLib }:

{
  pname = "logos-package-downloader";
  # VERSION is only present on release branches; dev branches use a placeholder.
  version = if builtins.pathExists ../VERSION
    then pkgs.lib.removeSuffix "\n" (builtins.readFile ../VERSION)
    else "1.0.0-dev";

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
  ];

  buildInputs = [
    pkgs.nlohmann_json
    pkgs.curl
    pkgs.zstd
    # lgx C library — the downloader loads a fetched .lgx and compares
    # its manifest + signer against the catalog entry post-download.
    logosPackageLib
  ];

  cmakeFlags = [
    "-GNinja"
    "-DLGX_ROOT=${logosPackageLib}"
  ];

  env = {
    LGX_ROOT = "${logosPackageLib}";
  };

  meta = with pkgs.lib; {
    description = "Logos Package Downloader - Online package catalog and download library";
    platforms = platforms.unix;
  };
}
