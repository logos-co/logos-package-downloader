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
  ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isWindows [
    # WINDOWS_EXPORT_ALL_SYMBOLS builds its .def file by running objdump over
    # the object files. CMake looks up CMAKE_OBJDUMP to do that, nixpkgs does
    # not set it for a cross build (it sets AR/RANLIB/STRIP but not OBJDUMP),
    # and when it is missing CMake skips def-file generation SILENTLY -- the
    # property appears to be honoured and exports nothing. Point it at the
    # target objdump explicitly.
    "-DCMAKE_OBJDUMP=${pkgs.stdenv.cc.bintools.bintools}/bin/${pkgs.stdenv.cc.targetPrefix}objdump"
  ];

  env = {
    LGX_ROOT = "${logosPackageLib}";
  };

  meta = with pkgs.lib; {
    description = "Logos Package Downloader - Online package catalog and download library";
    platforms = platforms.unix ++ platforms.windows;
  };
}
