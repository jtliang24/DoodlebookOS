{
  description = "DoodlebookOS Material 3 desktop suite for KDE Plasma 6";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # These inputs exercise both supported Home Manager integration paths.
    home-manager = {
      url = "github:nix-community/home-manager";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    plasma-manager = {
      url = "github:nix-community/plasma-manager";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.home-manager.follows = "home-manager";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      home-manager,
      plasma-manager,
    }:
    let
      forSystems = nixpkgs.lib.genAttrs [
        "x86_64-linux"
        "aarch64-linux"
      ];
    in
    {
      packages = forSystems (system: rec {
        doodlebookos = nixpkgs.legacyPackages.${system}.callPackage ./nix/package.nix { };
        default = doodlebookos;
      });
      apps = forSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/dostheme";
          meta.description = "Switch the running Plasma session between DoodlebookOS themes";
        };
      });
      homeManagerModules = rec {
        default = import ./nix/modules/home-manager.nix self;
        doodlebookos = default;
      };
      nixosModules = rec {
        default = import ./nix/modules/nixos.nix self;
        doodlebookos = default;
      };
      overlays.default = final: prev: {
        doodlebookos = final.callPackage ./nix/package.nix { };
      };
      checks = forSystems (
        system:
        import ./nix/tests {
          inherit
            self
            nixpkgs
            home-manager
            plasma-manager
            system
            ;
        }
      );
      formatter = forSystems (system: nixpkgs.legacyPackages.${system}.nixfmt);
    };
}
