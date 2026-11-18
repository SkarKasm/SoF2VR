# Flatscreen and VR launcher

Open **SoF2 Launcher.exe** beside **SoF2.exe**, then choose **Play Flatscreen** or **Play VR**. This launcher supports the single-player game, including its mission generator.

- **Flatscreen:** Windows OpenGL, native weapons/hands, original animations and effects. The proxy DLL and every `base/*sof2vr*.pk3` pack are moved outside the game search path before the process starts. No VR proxy or headset helper loads.
- **VR:** connect Quest Link and start SteamVR first, with SteamVR selected as the OpenXR runtime. The existing VR mod and settings load normally. The launcher does not change the runtime registry.

Both modes share the game's saved games. Flatscreen settings are kept separately in `sof2-launcher/flatscreen.cfg`. On first use, keyboard bindings and video resolution are retained; VR comfort, pacing, and gamma overrides are excluded so native defaults apply. Subsequent flatscreen settings are saved independently. The 24 October migration also removes these inherited VR overrides from the previously contaminated flat profile, with a backup. VR settings, including resolution and brightness, are restored after flatscreen exits. Each mode enables its appropriate crosshair at startup.

Keep the launcher open while playing; it minimizes automatically. During flatscreen play it parks the proxy and all matching VR packs under `sof2-launcher/flat-assets/`, using moves without copying game data. `sof2-launcher/flat-assets.ini` records their exact original paths. It restores those files and the VR configuration when the game exits. No mod uninstall or game executable replacement is required.

If Windows or the launcher stops unexpectedly, close any remaining game process and open the launcher again before playing. Its recovery journal restores all parked files and the saved VR configuration, including sessions from the older launcher. Flatscreen settings from an interrupted session may be lost. Recovery stops on unexpected duplicate files or invalid paths. Do not manually delete the `sof2-launcher` folder during a session or before recovery.

Use this launcher for mode switching. Launching SoF2.exe directly keeps the default VR behavior; do not launch another game instance while the launcher is running a session. Installation refuses pending recovery sessions, so complete recovery before updating the mod.

Implementation status: compiled and installed. Game launches, headset checks, and functional tests were not run, as requested by the project owner.
# VR accessibility options

The launcher's **VR Accessibility & Controls** button opens the VR-only options
before launch. In VR, **Main Menu → VR OPTIONS** opens the same settings inside
the game; its **SAVE AND APPLY** button takes effect immediately. Both use the
same INI file. See [the current controls and settings](ACCESSIBILITY_AND_HOLSTERS.md).

