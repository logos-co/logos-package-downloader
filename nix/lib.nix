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

    # Windows adds a third spelling -- libpackage_downloader_lib.dll plus its
    # .dll.a import library, which consumers must link against. Every pattern
    # carries a wildcard so nullglob can drop the ones that do not match; a
    # literal path with no wildcard would survive and break the copy.
    shopt -s nullglob
    libs=(lib/libpackage_downloader_lib*.dylib lib/libpackage_downloader_lib*.so \
          lib/libpackage_downloader_lib*.dll lib/libpackage_downloader_lib*.dll.a)
    if [ ''${#libs[@]} -eq 0 ]; then
      echo "Error: No library file found" >&2
      ls -la lib >&2 || true
      exit 1
    fi
    cp "''${libs[@]}" $out/lib/

    # Bundle liblgx alongside so the downloader lib can resolve it at
    # runtime via @loader_path / $ORIGIN (the verify-against-index step
    # links lgx). Mirrors what logos-package-manager's lib.nix does.
    # liblgx.dll lives in bin/, not lib/: CMake installs Windows RUNTIME
    # artifacts there. Missing it used to be a WARNING, which on Windows would
    # have produced a package that links but cannot start.
    lgxlibs=(${logosPackageLib}/lib/liblgx*.dylib ${logosPackageLib}/lib/liblgx*.so \
             ${logosPackageLib}/bin/liblgx*.dll ${logosPackageLib}/lib/liblgx*.dll.a)
    if [ ''${#lgxlibs[@]} -eq 0 ]; then
      echo "Error: no liblgx runtime found under ${logosPackageLib}" >&2
      ls -la ${logosPackageLib}/lib ${logosPackageLib}/bin >&2 || true
      exit 1
    fi
    cp "''${lgxlibs[@]}" $out/lib/

    cp ${src}/src/package_downloader_lib.h $out/include/
    cp ${src}/src/lgpd.h $out/include/

    runHook postInstall
  '';
}
