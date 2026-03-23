# Builds the logos-package-downloader library
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-lib";
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs buildInputs cmakeFlags meta env;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/lib $out/include

    if [ -f lib/libpackage_downloader_lib.dylib ]; then
      cp lib/libpackage_downloader_lib.dylib $out/lib/
    elif [ -f lib/libpackage_downloader_lib.so ]; then
      cp lib/libpackage_downloader_lib.so $out/lib/
    else
      echo "Error: No library file found"
      exit 1
    fi

    cp ${src}/src/package_downloader_lib.h $out/include/
    cp ${src}/src/lgpd.h $out/include/

    runHook postInstall
  '';
}
