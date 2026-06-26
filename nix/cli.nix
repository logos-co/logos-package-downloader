# Builds the logos-package-downloader CLI (lgpd)
{ pkgs, common, src, logosPackageLib, buildInfo }:

let
  lib = import ./lib.nix { inherit pkgs common src logosPackageLib; };
  buildInfoHeader = import ./build-info.nix { inherit pkgs buildInfo; };
in
pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-cli";
  version = common.version;

  inherit src;
  inherit (common) buildInputs cmakeFlags meta env;

  # Add autoPatchelfHook on Linux to fix RPATHs
  nativeBuildInputs = common.nativeBuildInputs ++
    pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.autoPatchelfHook ];

  # Drop the generated build-info header next to cmd/version_info.h so the CLI
  # can bake in version + commit hashes. Quoted-include resolution finds it.
  preConfigure = ''
    cp ${buildInfoHeader} cmd/logos_build_info.h
    chmod +w cmd/logos_build_info.h
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin $out/lib
    if [ -f bin/lgpd ]; then
      cp bin/lgpd $out/bin/
    else
      echo "Error: lgpd executable not found"
      exit 1
    fi

    # Copy libraries alongside the binary so RPATH can find them
    cp -a ${lib}/lib/* $out/lib/

    runHook postInstall
  '';
}
