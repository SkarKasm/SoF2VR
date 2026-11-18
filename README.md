# Soldier of Fortune II VR

An experimental PC VR mod for **Soldier of Fortune II: Double Helix**, by **SkarKasm**, creator of **Brutally RealRTCW VR**.

The mod targets the matching GOG single-player 1.03 game and uses an x86 OpenGL proxy with a separate x64 OpenXR helper. The current tested headset setup uses Quest Link with SteamVR as the active OpenXR runtime.

## Download and install

Download the latest ZIP from [Releases](https://github.com/SkarKasm/SoF2VR/releases), extract it, and follow [START-HERE](docs/START-HERE.txt). Run Install.cmd and select your game folder, then open **SoF2 Launcher.exe** and choose **Play VR**. Initialize a fresh game installation in flatscreen once before installing. When updating an earlier showcase package, use its Uninstall.cmd first.

## Features

- Head tracking and stereo rendering with headset projection.
- Tracked weapons, two-handed support, and grip-driven hand poses.
- Manual magazines, pistol slide actions, rifle charging, and M590 pumping.
- Weapon category wheels, body slots, visible holstered weapons, and a stomach ammunition pouch.
- Knife interaction and pistol-plus-knife handling.
- Stationary menu panel, compact health and armor numbers, weapon-side ammo, and VR accessibility options.
- A launcher for choosing flatscreen or VR.

See [controls](docs/CONTROLS.txt), [known issues](docs/KNOWN-ISSUES.txt), and [verification notes](docs/VISUAL-CHECKS.txt). This is a development preview. Physical headset comfort and performance need further feedback.

## Build and contribute

Use the included generated headers and the asset pack from the release when changing runtime code. [Build instructions](docs/BUILD-SOURCE.md) describe Build-Runtime.ps1 and the llvm-mingw toolchain. Please include your game edition, headset, map, weapon, and reproduction steps when reporting an issue.

## Support development

If you would like to support my VR modding work, [buy me a coffee](https://buymeacoffee.com/mrskarkasm).

## Credits and licenses

Code retains the license notices in its files, including GPL-3.0-or-later adaptations and RTCW additional terms. Asset licenses are separate and include CC BY, CC BY-NC, and original game-derived material. Read [CREDITS.md](CREDITS.md) and [licenses](licenses) before redistributing. The MP5 and M60 meshes carry noncommercial restrictions. Authors retain their respective rights.
