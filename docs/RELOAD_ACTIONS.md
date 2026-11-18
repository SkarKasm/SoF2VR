# Manual reload/action candidate — October 28, 2026 (r2)

October 29 hotfix: classify the normal weapon using its unmodified submitted
flags and apply the world-depth change after its tracked transform. The r2
order cleared the identifying bit too early and skipped normal tracking.

This candidate was compiled without launching the game. Gameplay confirmation
belongs to the project owner. The saved weapon sizes and authored hand/finger
poses remain unchanged.

## Cycling loaded weapons

Bring the off hand to the slide/charging handle, hold the off-hand trigger,
pull back and return or release. The visual hand latches to the moving part.
The support grip remains the normal way to hold the front of a gun.

A complete loaded cycle discards one primary-clip round through the native
inventory setter. Reserve ammunition stays unchanged. A partial stroke and
tracking loss discard nothing. Keep the trigger held to cycle repeatedly;
release it to let go of the action.
Pistols use a shorter slide stroke; the slide locks after the last round.

Cycling an empty weapon after inserting a fresh magazine chambers it through
the existing reload transaction. That first charging stroke does not discard
a round. B remains the pistol slide-release button after an empty reload.
The ejection prop uses the base game's brass model as a temporary visual.

## Magazine seating

A ejects the magazine. Grip a replacement at the stomach pouch and present
its feed end to the well. A wider capture area and angle tolerance acquire a
magnetic guide; reaching the mouth seats it automatically with a short
animation. It no longer requires holding a narrow position deep inside the
grip. Twelve supported detachable-magazine weapons have fixed palm-held
magazine poses and feed-end landmarks. M590 shell loading is separate.

## M3

The charging anchor is moved to the ejection-port side. The misplaced
replacement action is replaced with the original SoF2 M3 bolt geometry and
chamber faces, using original UVs/material references. A bounded opening in
the replacement receiver exposes the temporary donor interior. Both the
normal weapon and the split reload body use these changes. The owner's M3
scale and hand edits are retained.

## Related candidate repairs

- The wheel is painted into the submitted eye images after the game image.
  Captured gameplay HUD pages are suppressed while the wheel is open.
- Native depth-hack flags are removed for VR eye entities so world muzzle
  effects can be occluded by the weapon instead of covering it.
- Additive glare has a separate additive canvas, retaining its blend mode.
- Cinematic solid backdrops use projected bounds rather than monitor pixels.
- Native hand model handles refresh across renderer registrations/reloads.
- Knife hit queries prioritize the current blade and only recent motion;
  holding the trigger no longer generates unrelated flat-screen knife attacks.
  Repeated swing/slash sounds are quiet; native impact sounds are retained.

These are implementation changes, not a claim that the reported spotlight,
cutscene, wheel or reload problems have passed a headset check.
