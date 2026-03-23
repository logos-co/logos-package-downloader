{
  description = "Logos Package Downloader - Online package catalog and download library";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-nix, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        dirBundler = nix-bundle-dir.bundlers.${system}.default;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, dirBundler }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;

          lib = import ./nix/lib.nix { inherit pkgs common src; };
          cli = import ./nix/cli.nix { inherit pkgs common src; };

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
        } // {
          # Tests
          tests = import ./nix/tests.nix { inherit pkgs common src; };

          default = combined;
        }
      );

      checks = forAllSystems ({ pkgs, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;
        in {
          tests = import ./nix/tests.nix { inherit pkgs common src; };
        }
      );

      devShells = forAllSystems ({ pkgs, ... }: {
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
          ];

          shellHook = ''
            echo "Logos Package Downloader development environment"
          '';
        };
      });
    };
}
