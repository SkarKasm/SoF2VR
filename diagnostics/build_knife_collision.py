"""Generate blade locators from the already-packed, user-scaled knife.

Reads the pack and writes only a small runtime header. No mesh generation.
"""
import hashlib, json, math, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def generate():
    config = json.loads((ROOT/'resources/knife_collision.json').read_text())
    with zipfile.ZipFile(ROOT/'resources/z_sof2vr_native.pk3') as z:
        metadata = json.loads(z.read('sof2vr-weapon-replacements.json'))
        edits = z.read('sof2vr-manual-hand-edits.json')
    knife = next(w for w in metadata['ready'] if w['id'] == 1)
    if knife['asset'] != config['asset']:
        raise ValueError('Knife collision locators do not describe the packed asset')
    matrix = knife['calibration']['matrix']
    def point(source):
        return [sum(row[k]*source[k] for k in range(3))+row[3] for row in matrix]
    base, tip = point(config['bladeBase']), point(config['bladeTip'])
    length = math.dist(base, tip)
    if not .05*39.37 < length < .6*39.37:
        raise ValueError('Invalid packed blade length')
    radius = float(config['contactRadiusCm'])*.3937
    if not .1 <= radius <= 1.5:
        raise ValueError('Invalid blade contact tolerance')
    text = '/* Packed knife calibration; includes saved weapon scale. Do not hand-edit. */\n'
    text += '#ifndef SOF2_KNIFE_COLLISION_H\n#define SOF2_KNIFE_COLLISION_H\n'
    for name, values in [('vrKnifeBladeBase',base), ('vrKnifeBladeTip',tip)]:
        text += 'static const float '+name+'[3]={'+','.join(f'{v:.8f}f' for v in values)+'};\n'
    text += f'#define VR_KNIFE_CONTACT_RADIUS {radius:.8f}f\n#endif\n'
    path = ROOT/'src/knife_collision.h'
    if not path.exists() or path.read_text()!=text:
        path.write_text(text)
    return dict(asset=knife['asset'],base=base,tip=tip,lengthCm=length/0.3937,
                packedEditsSHA256=hashlib.sha256(edits).hexdigest())

if __name__=='__main__':
    print(json.dumps(generate(),indent=2))
