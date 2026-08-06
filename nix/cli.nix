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
    # .exe when cross-compiling to Windows.
    exe="bin/lgpd${pkgs.stdenv.hostPlatform.extensions.executable}"
    if [ -f "$exe" ]; then
      cp "$exe" $out/bin/
    else
      echo "Error: $exe not found"
      exit 1
    fi

    # Copy libraries alongside the binary so RPATH can find them
    cp -a ${lib}/lib/* $out/lib/

    # Windows has no RPATH: a DLL is found in the EXECUTABLE's directory, so
    # the runtime DLLs must sit in bin/ next to the .exe, not in lib/.
    # nixpkgs' win-dll-link.sh stages dependency DLLs automatically but only
    # scans $out/bin, so it never saw these -- lgpm.exe exited 53 with no
    # output because libpackage_manager_lib.dll was in lib/ where Windows does
    # not look. (.dll.a import libraries stay in lib/; they are link-time only.)
    shopt -s nullglob
    dlls=($out/lib/*.dll)
    if [ ''${#dlls[@]} -gt 0 ]; then
      cp -a "''${dlls[@]}" $out/bin/
    fi

    runHook postInstall
  '';
}
