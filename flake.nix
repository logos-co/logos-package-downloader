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
  };

  outputs = { self, nixpkgs, logos-nix, logos-package, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # Build info baked into the lgpd binary so `--version` reports the release
      # version, this repo's commit, and the locked commits of the flake inputs.
      # `revOf` yields the input's locked rev, a "<sha>-dirty" marker for a dirty
      # checkout, or "dirty" for a path override.
      revOf = input: input.rev or input.dirtyRev or "dirty";
      buildInfo = {
        # VERSION is only present on release branches. On master (pre-release CI
        # builds) there is no VERSION file, so fall back to a "pre-release-{sha7}"
        # string derived from self.rev. Dirty local builds lack self.rev and get
        # an empty string, which the CLI renders as "dev".
        version = if builtins.pathExists ./VERSION
          then nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION)
          else if (self ? rev) then "pre-release-${builtins.substring 0 7 self.rev}" else "";
        commit = revOf self;
        commits = [
          { name = "logos-package"; commit = revOf logos-package; }
          { name = "logos-nix"; commit = revOf logos-nix; }
          { name = "nix-bundle-dir"; commit = revOf nix-bundle-dir; }
          { name = "nix-bundle-appimage"; commit = revOf nix-bundle-appimage; }
        ];
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        dirBundler = nix-bundle-dir.bundlers.${system}.permissive;
        logosPackageLib = logos-package.packages.${system}.lib;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, dirBundler, logosPackageLib }:
        let
          common = import ./nix/default.nix { inherit pkgs logosPackageLib; };
          src = ./.;

          lib = import ./nix/lib.nix { inherit pkgs common src logosPackageLib; };
          cli = import ./nix/cli.nix { inherit pkgs common src logosPackageLib buildInfo; };

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
