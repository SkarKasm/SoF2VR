> Current controls (1 November 2026): hold grip at a holster to open its category wheel. The physical right stick selects, right stick click changes pages, and weapon-hand trigger equips while grip remains held. Release drops the weapon. The knife uses its fixed shoulder slot. See [accessibility and current holster controls](ACCESSIBILITY_AND_HOLSTERS.md). The detailed descriptions below document earlier wheel revisions and are superseded where controls differ.

# Paged weapon wheel

Adapted from the local Brutally RealRTCW VR implementation for SoF2 SP.

## Controls

- Grip away from holsters opens the general wheel. Briefly grip/release a body slot to draw; hold 350 ms to select from that category. Gripping never reloads or ejects a magazine.
- Move that hand from its opening position toward a card. About 10 cm reaches the edge; the center deadzone is 3 cm. Returning to center retains the selection.
- Move the dominant stick left/right to change pages. Release it to center before the next page change. Pages wrap and the current page is remembered.
- Release grip or press the dominant trigger to confirm. A face button, stick click or menu button cancels.
- Six owned weapons fit on each page. The final page contains **Quick Save** and **Quick Load**. Quick Load is disabled when no local save exists.
- Tracking/focus loss, pause or a menu cancels. Release grip before reopening after cancellation. Return the stick to center and release trigger before gameplay inputs resume.

Left-handed mode moves the wheel to the left controller. The normal movement/turn stick roles are unchanged outside the wheel. Gameplay input is suppressed while selecting; the world continues running.

The wheel is a flat transparent OpenXR quad over stereo gameplay, 1.1 m in front of the view. Main/pause menus remain their existing flat 2D panels. Native weapon names and clip/reserve counts are displayed; no RealRTCW artwork or hand assets are copied.

## Native SoF2 adaptation

The verified SP client inventory map supplies owned IDs, names, ammunition and availability. Duplicate display names share a card; native equipment-toggle categories are excluded. Inventory data refreshes every 250 ms.

Confirmation validates ownership/availability again. It sets the same pending primary-selection fields used by the original inventory UI and calls the original holster/draw transition (cgame RVA 0x61e50), followed by that UI's native `newwpn%d` command interface. Ammunition, ownership and weapon state objects remain managed by the game. Binary fingerprints gate these hooks.

The October 23 regression fix also waits for the native post-equip timer and submits immediately before weapon update. The selector can return an ID while that timer still blocks the holster; an ID alone is not accepted as a completed switch.

Save/load calls the actual engine `quicksave` and `quickload` commands. Quick Save uses the game's rotating quick-save slots; Quick Load loads the native most recent save, which may include an autosave. No confirmation dialog is added, matching the requested utility page behavior.

Bridge protocol **5** contains a bounded wheel snapshot under its own nonblocking lock. Both processes must use the matching protocol. Pixel offset is 800 bytes and the shared mapping size is 52,429,600 bytes.

## Behavior source and license

Local source: `dist/Brutally-RealRTCW-VR-main-check/code/client/cl_vr.c`, `cgame/cg_draw.c`, `cgame/cg_weapons.c` and `cgame/cg_consolecmds.c`, relative to the general workspace. The six-slot paging, utility page, hold/hand-motion selection, 0.35 smoothing, 12-degree hysteresis, grip release/trigger confirmation and page rearming were adapted into `src/weapon_wheel.c`.

The original source notices are retained in the new module; GPL-3.0-or-later text and original RTCW additional terms are in `licenses/RealRTCW-VR-GPL-3.0.txt`. No media rights are inferred from the code license. RTCW-specific binocular logic, slow motion and its weapon artwork are not implemented here.

## Validation and limits

`tests/wheel_unit.c` exercises inventories of 0 through 64 entries, paging, disabled Quick Load, confirmation/cancellation, invalid input and reload taps. `tests/check-wheel.py` validates real native weapon transitions, actual save/load, page changes, cancellation, no accidental firing and the separate flat XR quad in isolated game fixtures. `tests/run-slice.ps1` includes right- and left-handed wheel runs alongside the gameplay regression suite.

Current slots now use original SoF2 weapon/equipment icons with ammunition counts; the centre shows the hovered name. Quick Save/Quick Load keep their utility page. Gun-hand grip taps eject magazines on supported physical-reload profiles, with native reload fallback on others. See TEASER_WEAPONS_REVISION.md. Current changes have compilation evidence only; the older runtime evidence below does not validate them.

## Installed build evidence

Protocol 5 is current. Earlier protocol-4 runtime reports are historical; no runtime tests were run for the icon/teaser checkpoint, at the user's instruction. See [current status](PLAYABLE_SLICE.md) and [installation record](../build/feature-installation.json).
