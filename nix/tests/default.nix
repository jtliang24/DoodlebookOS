{
  self,
  nixpkgs,
  home-manager,
  plasma-manager,
  system,
}:
let
  pkgs = nixpkgs.legacyPackages.${system};
  lib = pkgs.lib;
  package = self.packages.${system}.default;
  home =
    modules:
    (home-manager.lib.homeManagerConfiguration {
      inherit pkgs;
      modules = [
        self.homeManagerModules.default
        {
          home.username = "test";
          home.homeDirectory = "/home/test";
          home.stateVersion = "26.05";
        }
      ]
      ++ modules;
    }).config;
  standalone = home [ { programs.doodlebookos.enable = true; } ];
  integrated = home [
    plasma-manager.homeModules.plasma-manager
    {
      programs.plasma.enable = true;
      programs.doodlebookos = {
        enable = true;
        variant = "light";
      };
    }
  ];
  fallback = home [
    plasma-manager.homeModules.plasma-manager
    {
      programs.plasma.enable = true;
      programs.doodlebookos = {
        enable = true;
        integratePlasmaManager = false;
      };
    }
  ];
  minimal = home [
    {
      programs.doodlebookos = {
        enable = true;
        enableFonts = false;
        enableWallpapers = false;
        enableTopBarColorDaemon = false;
      };
    }
  ];
  disabled = home [ ];
  nixos =
    (nixpkgs.lib.nixosSystem {
      inherit system;
      modules = [
        self.nixosModules.default
        {
          system.stateVersion = "26.05";
          programs.doodlebookos = {
            enable = true;
            enableSddmIntegration = true;
          };
        }
      ];
    }).config;
  valid = c: lib.all (a: a.assertion) c.assertions;
in
{
  inherit package;
  modules =
    assert valid standalone && valid integrated && valid fallback && valid minimal && valid disabled;
    assert lib.hasSuffix " --daemon" (
      builtins.head standalone.systemd.user.services.doodlebookos-topbar.Service.ExecStart
    );
    assert standalone.home.activation ? doodlebookos;
    assert fallback.home.activation ? doodlebookos;
    assert !(integrated.home.activation ? doodlebookos);
    assert integrated.programs.plasma.workspace.theme == "DoodlebookOS_Light";
    assert lib.hasSuffix "LIGHT.jpeg" integrated.programs.plasma.workspace.wallpaper;
    assert !(minimal.systemd.user.services ? doodlebookos-topbar);
    assert !(minimal.systemd.user.services ? doodlebookos-wallpaper);
    assert !minimal.fonts.fontconfig.enable;
    assert !(disabled.home.activation ? doodlebookos);
    assert builtins.elem nixos.programs.doodlebookos.package nixos.environment.systemPackages;
    assert builtins.elem nixos.programs.doodlebookos.package nixos.fonts.packages;
    assert builtins.elem nixos.programs.doodlebookos.package
      nixos.services.displayManager.sddm.extraPackages;
    # Force full activation generation, including plasma-manager's option types.
    assert builtins.isString standalone.home.activationPackage.drvPath;
    assert builtins.isString integrated.home.activationPackage.drvPath;
    assert builtins.isString minimal.home.activationPackage.drvPath;
    pkgs.runCommand "doodlebookos-module-checks" { nativeBuildInputs = [ pkgs.kdePackages.kconfig ]; }
      ''
        # Exercise fallback activation in an isolated config directory, preserving
        # unrelated keys rather than replacing the user's whole KDE config.
        export XDG_CONFIG_HOME="$TMPDIR/kdeconfig"
        mkdir -p "$XDG_CONFIG_HOME"
        kwriteconfig6 --file kdeglobals --group General --key KeepMe untouched
        run() { "$@"; }
        ${standalone.home.activation.doodlebookos.data}
        test "$(kreadconfig6 --file kdeglobals --group General --key KeepMe)" = untouched
        test "$(kreadconfig6 --file kdeglobals --group General --key ColorScheme)" = DoodlebookOS_DARK
        test "$(kreadconfig6 --file kwinrc --group org.kde.kdecoration2 --key library)" = org.doodlebookos.decoration
        touch "$out"
      '';
  assets = pkgs.runCommand "doodlebookos-assets" { nativeBuildInputs = [ pkgs.python3 ]; } ''
    test -f ${package}/lib/qt-6/plugins/styles/doodlebookos6.so
    test -f ${package}/lib/qt-6/plugins/org.kde.kdecoration3/org.doodlebookos.decoration.so
    test -e ${package}/lib/qt-6/plugins/plasma/applets/org.doodlebookos.datetime.so
    test -e ${package}/lib/qt-6/plugins/plasma/applets/org.doodlebookos.statuscluster.so
    test -e ${package}/share/plasma/plasmoids/DoodlebookOS-Dock/metadata.json
    test -e ${package}/share/plasma/plasmoids/org.kde.plasma.folder/metadata.json
    test -f ${package}/share/plasma/look-and-feel/org.doodlebookos.desktop/contents/defaults
    test -e ${package}/share/wallpapers/DoodlebookOS/DoodlebookOS_Wallpaper_DARK.png
    test -e ${package}/share/wallpapers/DoodlebookOS/DoodlebookOS_Wallpaper_LIGHT.jpeg
    test -f ${package}/share/fonts/truetype/GoogleSansFlex/OFL.txt
    ${package}/bin/dostheme status
    # A missing wallpaper exercises the Python wrapper without D-Bus.
    ${package}/bin/doodlebookos-topbar-color "$TMPDIR/missing-wallpaper"
    mkdir -p "$TMPDIR/data/applications"
    cat > "$TMPDIR/data/applications/nix-test.desktop" <<'DESKTOP'
    [Desktop Entry]
    Type=Application
    Name=Doodlebook Nix Test
    Exec=/test/bin/example %U
    DESKTOP
    XDG_DATA_DIRS="$TMPDIR/data" \
      python3 ${package}/share/plasma/plasmoids/org.doodlebookos.appgroup/contents/ui/list_apps.py > apps.json
    python3 -c 'import json; assert any(a["name"] == "Doodlebook Nix Test" for a in json.load(open("apps.json")))'
    touch "$out"
  '';
}
