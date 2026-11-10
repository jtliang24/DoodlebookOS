self:
{
  config,
  options,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.doodlebookos;
  hasPlasmaManager = options ? programs.plasma;
  usePlasmaManager = cfg.integratePlasmaManager && hasPlasmaManager && config.programs.plasma.enable;
  isDark = cfg.variant == "dark";
  desktopTheme = "DoodlebookOS_${if isDark then "Dark" else "Light"}";
  colorScheme = "DoodlebookOS_${if isDark then "DARK" else "LIGHT"}";
  wallpaper = "${cfg.package}/share/wallpapers/DoodlebookOS/DoodlebookOS_Wallpaper_${
    if isDark then "DARK" else "LIGHT"
  }.jpeg";
  font = size: "Google Sans Flex,${toString size},-1,5,400,0,0,0,0,0";
  settings = {
    kdeglobals = {
      KDE = {
        widgetStyle = "DoodlebookOS";
        LookAndFeelPackage = "org.doodlebookos.desktop";
      };
      General = {
        ColorScheme = colorScheme;
      }
      // lib.optionalAttrs cfg.enableFonts {
        font = font 10;
        menuFont = font 10;
        smallestReadableFont = font 8;
        toolBarFont = font 9;
      };
      Icons.Theme = "DoodlebookOS";
    }
    // lib.optionalAttrs cfg.enableFonts { WM.activeFont = font 10; };
    kwinrc."org.kde.kdecoration2" = {
      library = "org.doodlebookos.decoration";
      theme = "DoodlebookOS";
    };
    plasmarc.Theme.name = desktopTheme;
    kcminputrc.Mouse.cursorTheme = "DoodlebookOS-Cursors";
  };
  writeSettings = lib.concatStringsSep "\n" (
    lib.concatLists (
      lib.mapAttrsToList (
        file: groups:
        lib.concatLists (
          lib.mapAttrsToList (
            group: entries:
            lib.mapAttrsToList (
              key: value:
              "run ${pkgs.kdePackages.kconfig}/bin/kwriteconfig6 --file ${lib.escapeShellArg file} --group ${lib.escapeShellArg group} --key ${lib.escapeShellArg key} ${lib.escapeShellArg value}"
            ) entries
          ) groups
        )
      ) settings
    )
  );
in
{
  options.programs.doodlebookos = {
    enable = lib.mkEnableOption "DoodlebookOS KDE Plasma 6 suite";
    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ../package.nix { };
      defaultText = lib.literalExpression "pkgs.callPackage doodlebookos/nix/package.nix { }";
      description = "DoodlebookOS package, built with the consuming configuration's KDE packages.";
    };
    variant = lib.mkOption {
      type = lib.types.enum [
        "dark"
        "light"
      ];
      default = "dark";
      description = "DoodlebookOS color variant.";
    };
    enableWallpapers = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Apply the wallpaper matching the selected variant.";
    };
    enableFonts = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Expose the bundled fonts through fontconfig and configure KDE typography.";
    };
    enableTopBarColorDaemon = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Run the adaptive top-bar contrast daemon during graphical sessions.";
    };
    integratePlasmaManager = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Use plasma-manager when its module is imported and programs.plasma.enable is true. Otherwise update KDE keys during activation.";
    };
  };

  config = lib.mkIf cfg.enable (
    lib.mkMerge [
      {
        assertions = [
          {
            assertion = pkgs.stdenv.hostPlatform.isLinux;
            message = "DoodlebookOS requires Linux and KDE Plasma 6.";
          }
        ];
        home.packages = [ cfg.package ];
        fonts.fontconfig.enable = lib.mkIf cfg.enableFonts true;
        # Existing widgets also call this historical path directly.
        home.file.".local/bin/doodlebookos-topbar-color".source =
          "${cfg.package}/bin/doodlebookos-topbar-color";
        xdg.dataFile."wallpapers/DoodlebookOS".source = "${cfg.package}/share/wallpapers/DoodlebookOS";
        home.sessionSearchVariables.QT_PLUGIN_PATH = [ "${cfg.package}/lib/qt-6/plugins" ];

        systemd.user.services.doodlebookos-topbar = lib.mkIf cfg.enableTopBarColorDaemon {
          Unit = {
            Description = "DoodlebookOS adaptive top-bar contrast";
            PartOf = [ "graphical-session.target" ];
            After = [ "plasma-plasmashell.service" ];
          };
          Service = {
            ExecStart = "${cfg.package}/bin/doodlebookos-topbar-color --daemon";
            Restart = "on-failure";
            RestartSec = "3s";
          };
          Install.WantedBy = [ "graphical-session.target" ];
        };
      }
      (lib.mkIf (!usePlasmaManager) {
        # Update individual keys; do not replace or symlink whole KDE config files.
        home.activation.doodlebookos = lib.hm.dag.entryAfter [ "writeBoundary" ] writeSettings;
        systemd.user.services.doodlebookos-wallpaper = lib.mkIf cfg.enableWallpapers {
          Unit = {
            Description = "Apply the DoodlebookOS wallpaper";
            PartOf = [ "graphical-session.target" ];
            After = [ "plasma-plasmashell.service" ];
          };
          Service = {
            Type = "oneshot";
            ExecStart = "${pkgs.kdePackages.plasma-workspace}/bin/plasma-apply-wallpaperimage ${wallpaper}";
          };
          Install.WantedBy = [ "graphical-session.target" ];
        };
      })
      (lib.optionalAttrs hasPlasmaManager {
        programs.plasma = lib.mkIf usePlasmaManager {
          workspace = {
            theme = desktopTheme;
            inherit colorScheme;
            iconTheme = "DoodlebookOS";
            cursor.theme = "DoodlebookOS-Cursors";
            widgetStyle = "DoodlebookOS";
            windowDecorations = {
              library = "org.doodlebookos.decoration";
              theme = "DoodlebookOS";
            };
            wallpaper = lib.mkIf cfg.enableWallpapers wallpaper;
          };
          # Record the global theme without applying its unrelated effects/shortcuts.
          # workspace owns appearance keys; typography is configured separately.
          configFile.kdeglobals = {
            KDE.LookAndFeelPackage = "org.doodlebookos.desktop";
          }
          // lib.optionalAttrs cfg.enableFonts {
            General = lib.filterAttrs (key: _: key != "ColorScheme") settings.kdeglobals.General;
            WM = settings.kdeglobals.WM;
          };
        };
      })
    ]
  );
}
