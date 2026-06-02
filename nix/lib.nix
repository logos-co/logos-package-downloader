# Builds the logos-package-downloader library
{ pkgs, common, src, logosPackageLib }:

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

    # Bundle liblgx alongside so the downloader lib can resolve it at
    # runtime via @loader_path / $ORIGIN (the verify-against-index step
    # links lgx). Mirrors what logos-package-manager's lib.nix does.
    if [ -f ${logosPackageLib}/lib/liblgx.dylib ]; then
      cp ${logosPackageLib}/lib/liblgx.dylib $out/lib/
    elif [ -f ${logosPackageLib}/lib/liblgx.so ]; then
      cp ${logosPackageLib}/lib/liblgx.so $out/lib/
    else
      echo "Warning: liblgx library not found in ${logosPackageLib}/lib/"
    fi

    cp ${src}/src/package_downloader_lib.h $out/include/
    cp ${src}/src/lgpd.h $out/include/

    runHook postInstall
  '';
}
