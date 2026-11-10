{
  lib,
  stdenv,
  cmake,
  ninja,
  makeWrapper,
  python3,
  glib,
  systemd,
  kdePackages,
}:
let
  python = python3.withPackages (ps: [ ps.pillow ]);
in
stdenv.mkDerivation {
  pname = "doodlebookos";
  version = "0-unstable-2026-11-10";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../themes
      ../extras
      ../widgets
    ];
  };

  nativeBuildInputs = [
    cmake
    ninja
    makeWrapper
    python3
    kdePackages.extra-cmake-modules
    kdePackages.wrapQtAppsHook
  ];
  buildInputs = with kdePackages; [
    qtbase
    qtdeclarative
    qtsvg
    kcoreaddons
    kconfig
    kconfigwidgets
    kguiaddons
    ki18n
    kiconthemes
    kwindowsystem
    kcolorscheme
    kdecoration
  ];
  dontWrapQtApps = true;
  # Two independent CMake projects share an installation prefix.
  configurePhase = ''
    runHook preConfigure
    for component in app-style window-decoration; do
      cmake -S "themes/$component/DoodlebookOS" -B "build-$component" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$out" \
        -DKDE_INSTALL_LIBDIR=lib \
        -DKDE_INSTALL_QTPLUGINDIR="lib/qt-6/plugins" \
        -DKDE_INSTALL_PLUGINDIR="lib/qt-6/plugins" \
        -DBUILD_TESTING=OFF
    done
    runHook postConfigure
  '';
  buildPhase = ''
    runHook preBuild
    for component in app-style window-decoration; do
      cmake --build "build-$component" --parallel "$NIX_BUILD_CORES"
    done
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    for component in app-style window-decoration; do
      cmake --install "build-$component"
    done
    mkdir -p "$out/share/plasma/desktoptheme" "$out/share/plasma/look-and-feel" \
      "$out/share/plasma/plasmoids" "$out/share/color-schemes" "$out/share/icons" \
      "$out/share/wallpapers/DoodlebookOS" "$out/share/fonts/truetype/GoogleSansFlex" \
      "$out/bin" "$out/libexec"
    cp -r themes/plasma-theme/* "$out/share/plasma/desktoptheme/"
    cp -r themes/look-and-feel/DoodlebookOS "$out/share/plasma/look-and-feel/org.doodlebookos.desktop"
    cp themes/color-schemes/*.colors "$out/share/color-schemes/"
    cp -r extras/icons/DoodlebookOS extras/cursors/DoodlebookOS-Cursors "$out/share/icons/"
    cp -r extras/wallpapers/. "$out/share/wallpapers/DoodlebookOS/"
    cp extras/fonts/GoogleSansFlex/*.ttf extras/fonts/GoogleSansFlex/OFL.txt \
      "$out/share/fonts/truetype/GoogleSansFlex/"
    for widget in widgets/*/metadata.json; do
      id=$(${python3.interpreter} -c 'import json,sys; print(json.load(open(sys.argv[1]))["KPlugin"]["Id"])' "$widget")
      cp -r "$(dirname "$widget")" "$out/share/plasma/plasmoids/$id"
    done
    while read -r alias target; do
      ln -s "$target" "$out/share/plasma/plasmoids/$alias"
    done <<'ALIASES'
    DoodlebookOS-Dock org.doodlebookos.dock
    DoodlebookOS-StatusCluster org.doodlebookos.statuscluster
    DoodlebookOS-DateTime org.doodlebookos.datetime
    DoodlebookOS-PanelSpacer org.doodlebookos.panelspacer
    DoodlebookOS-Desktop org.kde.desktopcontainment
    org.kde.plasma.folder org.kde.desktopcontainment
    DoodlebookOS-Folder org.doodlebookos.appgroup
    org.doodlebookos.folder org.doodlebookos.appgroup
    DoodlebookOS-Clock org.doodlebookos.clock
    local.pixelclock org.doodlebookos.clock
    DoodlebookOS-Weather org.doodlebookos.weather
    local.pixelweather org.doodlebookos.weather
    ALIASES
    # The QML reuses native implementations from plasma-workspace.
    applets="$out/lib/qt-6/plugins/plasma/applets"
    mkdir -p "$applets"
    ln -s ${kdePackages.plasma-workspace}/lib/qt-6/plugins/plasma/applets/org.kde.plasma.notifications.so \
      "$applets/org.doodlebookos.statuscluster.so"
    ln -s ${kdePackages.plasma-workspace}/lib/qt-6/plugins/plasma/applets/org.kde.plasma.digitalclock.so \
      "$applets/org.doodlebookos.datetime.so"
    install -m755 extras/scripts/doodlebookos-topbar-color "$out/libexec/"
    makeWrapper ${python.interpreter} "$out/bin/doodlebookos-topbar-color" \
      --add-flags "$out/libexec/doodlebookos-topbar-color" \
      --prefix PATH : ${
        lib.makeBinPath [
          kdePackages.kconfig
          glib
        ]
      }
    install -m755 extras/scripts/dostheme "$out/bin/dostheme"
    wrapProgram "$out/bin/dostheme" \
      --set-default DOODLEBOOKOS_WALLPAPER_DIR "$out/share/wallpapers/DoodlebookOS" \
      --prefix PATH : "$out/bin:${
        lib.makeBinPath [
          kdePackages.kconfig
          kdePackages.plasma-workspace
          systemd
        ]
      }"
    # AppGroup invokes Python explicitly; give it a store interpreter on NixOS.
    substituteInPlace "$out/share/plasma/plasmoids/org.doodlebookos.appgroup/contents/ui/main.qml" \
      --replace-fail 'appScanner.connectSource("python3 ' 'appScanner.connectSource("${python3.interpreter} '
    runHook postInstall
  '';
  meta = {
    description = "Material 3 desktop suite for KDE Plasma 6";
    homepage = "https://github.com/PanOptikuum/DoodlebookOS";
    license = lib.licenses.gpl3Plus;
    platforms = lib.platforms.linux;
    mainProgram = "dostheme";
  };
}
