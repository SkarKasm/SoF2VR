"""Display-only authoring orientation; never changes saved/runtime geometry."""
import json
from functools import lru_cache
from pathlib import Path
from manual_hand_pose import ROOT,load_cache
import numpy as np

@lru_cache(maxsize=8)
def throwable_view(id):
    if not 18 <= id <= 25:return None
    cache=load_cache()
    if not cache:raise ValueError('Matching editor baseline required')
    grip=cache['grips'][str(id)];basis=np.asarray(grip['basis'])
    settings={w['id']:w for w in json.loads((ROOT/'resources/weapon_replacements.json').read_text())['weapons']}
    converted={w['id']:w for w in json.loads((ROOT/'build/weapon-replacement-build.json').read_text())['ready']}
    if id in settings and id in converted:
        matrix=np.asarray(converted[id]['calibration']['matrix'])
        up=matrix[:,:3]@np.asarray(settings[id]['up'])
        method='licensed source model up axis'
    else:
        from native_pose import read,pose,bolt
        from build_presentation import idle
        row=next(w for w in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons'] if w['id']==id)
        rest=idle(row['animations']['sequences'],Path(row['viewModel']).stem)
        evaluated=pose(read(row['skeleton']['path']),rest['startFrame'])
        up=np.asarray(bolt(evaluated,'gun'))[:,2]
        method='native grenade body bone +Z axis'
    up=basis.T@up;up/=np.linalg.norm(up)
    return dict(uprightDefault=True,upCanonical=up.tolist(),method=method)
