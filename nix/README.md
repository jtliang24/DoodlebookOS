# Nix and Home Manager

This flake packages the application style, window decoration, eight widgets,
desktop themes, look-and-feel package, color schemes, icons, cursors, fonts,
wallpapers, and the `dostheme` and top-bar helpers. It supports `x86_64-linux` and
`aarch64-linux` with KDE Plasma 6. The GUI installer is unnecessary on NixOS.

## Build and check

```sh
nix build .#default
nix flake check
./result/bin/dostheme status
```

For an untracked working tree, use `path:.#default` and `path:.` instead. Git flakes
only include tracked files. The module checks evaluate real Home Manager
configurations with and without plasma-manager and a NixOS configuration. Asset
checks verify plugin aliases and helper execution. Builds need no running desktop.
Actual rendering and session behavior require a Plasma session. ARM outputs need
an ARM builder or emulation to build on x86.

## Home Manager

Add your fork as an input in your configuration later:

```nix
inputs.doodlebookos = {
  url = "github:<your-username>/DoodlebookOS";
  inputs.nixpkgs.follows = "nixpkgs"; # Or your configuration's "unstable" input.
};
```

Import the module in your Home Manager configuration:

```nix
imports = [
  inputs.doodlebookos.homeManagerModules.default
  # Optional, if you already use plasma-manager:
  # inputs.plasma-manager.homeModules.plasma-manager
];

programs.doodlebookos = {
  enable = true;
  variant = "dark";
  enableTopBarColorDaemon = true;
};
```

When `programs.plasma.enable` is true and plasma-manager is imported, DoodlebookOS
sets its workspace options and font keys through plasma-manager. Otherwise it
updates individual KDE config keys at activation and applies the wallpaper through
a graphical-session user service. `integratePlasmaManager = false` explicitly
selects this fallback even when plasma-manager is enabled; avoid managing the same
theme keys in plasma-manager in that case. Log out and back in after first activation
so Plasma and Qt applications pick up the profile and plugin paths.

| Home Manager option | Default | Behavior |
| --- | --- | --- |
| `enable` | `false` | Install the suite and configure KDE appearance |
| `package` | Built with the consumer's `pkgs` | Override the suite derivation |
| `variant` | `"dark"` | `"dark"` or `"light"` |
| `enableWallpapers` | `true` | Apply the variant's wallpaper |
| `enableFonts` | `true` | Enable fontconfig integration and set KDE typography |
| `enableTopBarColorDaemon` | `true` | Monitor wallpaper changes during graphical sessions |
| `integratePlasmaManager` | `true` | Use plasma-manager when imported and enabled |

The package always contains both wallpapers and the font assets; the switches
control their configuration. The wallpapers are also linked into the historical
user data location for upstream widget compatibility. `dostheme dark`,
`dostheme light`, and `dostheme toggle` change the running desktop. A later Home
Manager activation reapplies the declared variant.

The module selects appearance settings and installs widgets. Existing panels and
desktop widgets are preserved. To use the supplied panel layout, explicitly apply
the DoodlebookOS global theme's desktop layout in Plasma System Settings. This is
a separate user action because it replaces the current layout. The upstream
installer also patches Plasma's private clipboard UI and changes shortcuts and
effects; this flake leaves those upstream system files and preferences alone.

## Optional NixOS module

The NixOS module exposes assets and Qt plugins system-wide, registers fonts, and
can make the application style available to an existing SDDM installation:

```nix
imports = [ inputs.doodlebookos.nixosModules.default ];
programs.doodlebookos = {
  enable = true;
  enableFonts = true;
  enableSddmIntegration = false;
};
```

It does not enable Plasma or SDDM, select an SDDM theme (none is bundled), or set
user appearance. Use Home Manager for per-user settings. Both modules build the
package with the consuming configuration's `pkgs` to match its Qt/KDE versions;
when overriding `package`, keep those versions aligned.

The flake also exports `packages.<system>.doodlebookos`, `apps.<system>.default`
(the theme switcher), and `overlays.default`. The overlay builds against the
overlaid package set. Module aliases are available as
`homeManagerModules.doodlebookos` and `nixosModules.doodlebookos`.

Disabling the module removes its managed files and services. Settings written by
the fallback activation remain until another theme or configuration changes them.
No host configuration is modified by building or checking this repository.
