# Builds and runs logos-package-downloader tests
{ pkgs, common, src, logosPackageLib }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs meta env;

  buildInputs = common.buildInputs ++ [ pkgs.gtest ];

  configurePhase = ''
    runHook preConfigure

    # -DLGX_ROOT is required by CMakeLists.txt (it locates liblgx for the
    # post-download verification path). lib.nix / cli.nix get it from
    # common.cmakeFlags via the default configurePhase; this derivation
    # overrides configurePhase, so it must pass the flag explicitly.
    # common.env sets an LGX_ROOT env var too, but CMake reads the
    # *variable* (-D…), not the environment, so the env var alone isn't
    # enough — which is what broke CI.
    cmake -S . -B build \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DLGX_ROOT=${logosPackageLib} \
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
