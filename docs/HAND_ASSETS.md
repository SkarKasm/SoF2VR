> Superseded on 20 October 2026: the current build uses original SoF2 first-person hand meshes and textures, trimmed at the wrist. Free hands have 17 pressure poses; attached hands retain native weapon grips. RealRTCW mesh retargeting below is historical. See [current revision](TEASER_WEAPONS_REVISION.md).

# Hand asset permission and adaptation

On 19 October 2026 the user explicitly reported permission from the RealRTCW hand authors to use their meshes in SoF2. This supersedes the earlier permission-pending restriction for this project. No additional confirmation is required to develop/install the requested adaptation.

Use the meshes from `xr-release-0.8.7/Main/z_realrtcw_models_hands_vr.pk3`. Retarget appearance to SoF2 rather than restoring the former RTCW glove texture pack. Preserve source attribution and hashes. Keep generated SoF2 game-content derivatives local.

The historical `z_sof2vr_assets.pk3` has the old fixed RTCW appearance and remains retired. Quarantine/backup markers describe the historical permission state; they do not override the user's new authorization. Do not automatically install the old pack.

The adapted native pack uses the approved RealRTCW relaxed/gripping meshes with UVs transferred onto seven original SoF2 outfit material sets. Campaign hints and native skin observations select the outfit, with an explicit override. Native stock glove-only GLMs/MD3s remain a fallback. Original archives are untouched.

Inferred IK arms use original SoF2 sleeve surfaces. They pass structural and rendering checks but their rigid joints remain visibly unfinished, so `Hands/Arms=0` is the default. `Arms=1` opts into the prototype. Full-body IK and articulated fingers remain pending.

The converter normalizes winding separately for relaxed and gripping poses because their source conventions differ. Next: articulated fingers, refined sleeve joints, continuous outfit validation and optional full-body IK. See FEATURE_PORT_PLAN.md for the implementation sequence and required validation.


## 20 October native attachment revision

Held hands now retarget the approved topology onto the original first-person glove rig, native UVs and bone weights. Detached hands retain the outfit adaptation and original RealRTCW axes. See [native hand rig](NATIVE_HAND_RIG.md) for scope, provenance and remaining cuff/UV polish.
