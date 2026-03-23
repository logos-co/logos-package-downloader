# Builds and runs logos-package-downloader tests
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs meta env;

  buildInputs = common.buildInputs ++ [ pkgs.gtest ];

  configurePhase = ''
    runHook preConfigure

    cmake -S . -B build \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DLGPD_BUILD_TESTS=ON

    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    cmake --build build
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    cd build
    ctest --output-on-failure
    cd ..
    runHook postCheck
  '';

  installPhase = ''
    mkdir -p $out
    echo "Tests passed" > $out/test-results.txt
  '';
}
