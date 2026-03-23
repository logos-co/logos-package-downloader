# Common build configuration shared across all packages
{ pkgs }:

{
  pname = "logos-package-downloader";
  version = "1.0.0";

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
  ];

  buildInputs = [
    pkgs.nlohmann_json
    pkgs.curl
    pkgs.zstd
  ];

  cmakeFlags = [
    "-GNinja"
  ];

  env = {};

  meta = with pkgs.lib; {
    description = "Logos Package Downloader - Online package catalog and download library";
    platforms = platforms.unix;
  };
}
