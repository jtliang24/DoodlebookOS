self:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.doodlebookos;
in
{
  options.programs.doodlebookos = {
    enable = lib.mkEnableOption "system-wide DoodlebookOS assets and Qt plugins";
    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ../package.nix { };
      defaultText = lib.literalExpression "pkgs.callPackage doodlebookos/nix/package.nix { }";
      description = "DoodlebookOS package built against this system's KDE packages.";
    };
    enableFonts = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Register the bundled Google Sans Flex fonts system-wide.";
    };
    enableSddmIntegration = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Expose the DoodlebookOS style to an already enabled SDDM. The suite does not include an SDDM theme.";
    };
  };
  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];
    environment.pathsToLink = [ "/lib/qt-6/plugins" ];
    fonts.packages = lib.mkIf cfg.enableFonts [ cfg.package ];
    services.displayManager.sddm.extraPackages = lib.mkIf cfg.enableSddmIntegration [ cfg.package ];
  };
}
