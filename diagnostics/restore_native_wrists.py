"""Re-emit reviewed hand poses with the original SoF2 wrist geometry."""
import hashlib
import json
import os
import zipfile

from build_body_assets import cuff_frame
from build_magazine_interactions import derive
from build_native_assets import tracked_glove
from close_glove_wrist import CUFF_PLANE
from fit_weapon_hands import authored_hand, hand_mesh
from manual_hand_pose import ROOT, edit_pose, load_cache, saved_edits
from native_pose import read


def build():
    pack = ROOT / 'resources/z_sof2vr_native.pk3'
    cache = load_cache()
    if cache is None:
        raise RuntimeError('The reviewed hand pose cache is required')
    edits = saved_edits()
    rows = {w['id']: w for w in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons']}
    _, hands = derive()
    with zipfile.ZipFile(pack) as source:
        if json.loads(source.read('sof2vr-manual-hand-edits.json')) != edits:
            raise RuntimeError('The packed hand edits differ from the saved edits')
        grips = json.loads(source.read('sof2vr-grip-poses.json'))
        for grip in grips:
            weapon = grip['id']
            baseline = cache['grips'][str(weapon)]
            for label, side in (('right', 'r'), ('left', 'l')):
                hand = grip['hands'][label]
                if hand is None:
                    continue
                evaluated = cache['poses'].get(str(weapon), {}).get(side)
                if evaluated is None:
                    evaluated = authored_hand(rows[weapon], baseline['hands'][label], side)
                edit = edits['weapons'].get(str(weapon), {}).get('hands', {}).get(label)
                if edit is not None:
                    evaluated = edit_pose(evaluated, side, baseline, edit)
                hands[hand['model']] = hand_mesh(evaluated, side)
        for side in 'lr':
            stock = read(f'models/weapons/{side}hand/{side}hand.glm')
            hands[f'models/sof2vr_native/{side}hand.md3'] = tracked_glove(stock, side)[0]
            for step in range(17):
                hands[f'models/sof2vr_native/{side}hand_grip{step:02d}.md3'] = tracked_glove(stock, side, step/16)[0]

        expected = {n for n in source.namelist() if n.endswith('.md3') and
                    ('/sof2vr_grips/' in n or '/sof2vr_native/' in n or
                     ('/sof2vr_reload/' in n and n.endswith(('_lhand.md3', '_rhand.md3'))))}
        if set(hands) != expected:
            raise RuntimeError(f'Unexpected hand mesh inventory: {sorted(set(hands) ^ expected)}')
        frames = {name: cuff_frame(mesh, overlap=.22) for name, mesh in sorted(hands.items())}
        report = dict(plane=CUFF_PLANE, meshes={n: hashlib.sha256(b).hexdigest() for n,b in hands.items()})
        changes = {**hands, 'sof2vr-native-wrists.json': (json.dumps(report, indent=2)+'\n').encode()}
        finish_path = 'sof2vr-visual-finishes.json'
        finish = json.loads(source.read(finish_path))
        finish['wrists'] = []
        for name in hands:
            finish['outputs'].pop(name, None)
        changes[finish_path] = (json.dumps(finish, indent=2)+'\n').encode()
        if any(name not in source.namelist() or source.read(name) != data for name,data in changes.items()):
            pending = pack.with_suffix('.native-wrists-pending')
            if pending.exists():
                raise RuntimeError('Interrupted native wrist build exists')
            try:
                with zipfile.ZipFile(pending, 'x', zipfile.ZIP_DEFLATED, compresslevel=2) as out:
                    for info in source.infolist():
                        out.writestr(info, changes.pop(info.filename) if info.filename in changes else source.read(info))
                    for name,data in changes.items():
                        out.writestr(name, data)
                source.close()
                os.replace(pending, pack)
            finally:
                pending.unlink(missing_ok=True)

    def vector(values):
        return '{'+','.join(f'{v:.8f}f' for v in values)+'}'
    lines = ['/* Native first-person cuff centers and sleeve overlap anchors. */',
             '#ifndef SOF2_NATIVE_WRISTS_H', '#define SOF2_NATIVE_WRISTS_H',
             'typedef struct {const char *model;float point[3],width[3];} VRNativeWrist;',
             'static const VRNativeWrist vrNativeWrists[]={']
    lines += [f'{{"{name}",{vector(point)},{vector(width)}}},' for name,(point,width) in frames.items()]
    (ROOT/'src/native_wrists.h').write_text('\n'.join(lines+['};','#endif','']))
    print(f'Restored the native cuff on {len(hands)} hand meshes.', flush=True)


if __name__ == '__main__':
    build()
