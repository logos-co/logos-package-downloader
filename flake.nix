{
  description = "Logos Package Downloader - Online package catalog and download library";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    # logos-package supplies the lgx C library — used post-download to
    # verify a fetched .lgx against what the catalog advertised.
    logos-package.url = "github:logos-co/logos-package";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
    # Pinned to the non-Qt-bundle fix (logos-co/nix-bundle-macos-app#4) until
    # it lands on the default branch — lgpd is a plain-C++ CLI, so its .app has
    # no Resources/qt tree and trips the unguarded find in the published mkMacOSApp.
    nix-bundle-macos-app = {
      url = "github:logos-co/nix-bundle-macos-app/463d7608c7539a18eb8d6a467609b85e88d781bb";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.nix-bundle-dir.follows = "nix-bundle-dir";
    };
  };

  outputs = { self, nixpkgs, logos-nix, logos-package, nix-bundle-dir, nix-bundle-appimage, nix-bundle-macos-app }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        dirBundler = nix-bundle-dir.bundlers.${system}.permissive;
        logosPackageLib = logos-package.packages.${system}.lib;
        macosAppBundler = nix-bundle-macos-app.lib.${system}.mkMacOSApp;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, dirBundler, logosPackageLib, macosAppBundler }:
        let
          common = import ./nix/default.nix { inherit pkgs logosPackageLib; };
          src = ./.;

          lib = import ./nix/lib.nix { inherit pkgs common src logosPackageLib; };
          cli = import ./nix/cli.nix { inherit pkgs common src logosPackageLib; };

          combined = pkgs.symlinkJoin {
            name = "logos-package-downloader";
            paths = [ lib cli ];
          };
        in
        {
          logos-package-downloader-lib = lib;
          logos-package-downloader-cli = cli;
          lib = lib;
          cli = cli;

          cli-bundle-dir = dirBundler cli;
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          cli-appimage = nix-bundle-appimage.lib.${system}.mkAppImage {
            drv = cli;
            name = "lgpd";
            bundle = dirBundler cli;
            desktopFile = ./assets/lgpd.desktop;
            icon = ./assets/lgpd.png;
          };
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isDarwin {
          # macOS .app bundle (ad-hoc signed, notarization-ready structure).
          # Released as a tar.gz; see .github/workflows/release.yml.
          cli-macos-app = macosAppBundler {
            drv = cli;
            name = "lgpd";
            bundle = dirBundler cli;
            icon = ./assets/lgpd.png;
            infoPlist = ./assets/macos/Info.plist.in;
            entitlements = ./assets/macos/lgpd.entitlements;
          };
        } // {
          # Tests
          tests = import ./nix/tests.nix { inherit pkgs common src logosPackageLib; };

          default = combined;
        }
      );

      checks = forAllSystems ({ pkgs, logosPackageLib, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs logosPackageLib; };
          src = ./.;
        in {
          tests = import ./nix/tests.nix { inherit pkgs common src logosPackageLib; };
        }
      );

      devShells = forAllSystems ({ pkgs, logosPackageLib, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.nlohmann_json
            pkgs.curl
            pkgs.zstd
            logosPackageLib
          ];

          shellHook = ''
            echo "Logos Package Downloader development environment"
          '';
        };
      });
    };
}
