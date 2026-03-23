# Builds the logos-package-downloader CLI (lgpd)
{ pkgs, common, src }:

let
  lib = import ./lib.nix { inherit pkgs common src; };
in
pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-cli";
  version = common.version;

  inherit src;
  inherit (common) buildInputs cmakeFlags meta env;

  # Add autoPatchelfHook on Linux to fix RPATHs
  nativeBuildInputs = common.nativeBuildInputs ++
    pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.autoPatchelfHook ];

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
