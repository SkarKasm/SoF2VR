"""Deterministic manual hand authoring, shared by the editor and VR exporter.

Edits are relative to a cached authored baseline, never to the last edited mesh.
Native glove skin weights, UVs and bone lengths are preserved.
"""
import copy, gzip, hashlib, json, math, os, re, struct, sys
from pathlib import Path
from functools import lru_cache
ROOT = Path(__file__).resolve().parents[1]
LIB = Path(os.environ.get('SOF2_ASSET_PYTHON_LIB', 'E:/SoF2-asset-tools/python-packages'))
if LIB.is_dir(): sys.path.insert(0, str(LIB))
import numpy as np
from scipy.spatial.transform import Rotation
from native_pose import read, vertices, pose
STATE = ROOT / 'build/hand-editor'
OVERRIDES = ROOT / 'resources/hand_pose_overrides.json'
DIGITS = ['thumb', 'index', 'middle', 'ring', 'little']
WEAPON_SCALES = {}

def native_effect_point(id, point):
    if id not in WEAPON_SCALES:return point
    anchor,scale=WEAPON_SCALES[id]
    return (anchor+scale*(np.asarray(point)-anchor)).tolist()

def fingerprint():
    h = hashlib.sha256(b'manual-editor-baseline-v3')
    for name in ['diagnostics/fit_weapon_hands.py', 'diagnostics/fit_hand_contact.py',
                 'diagnostics/hand_contact_surface.py', 'diagnostics/native_pose.py',
                 'diagnostics/native_hand_size.py', 'diagnostics/build_weapon_grips.py',
                 'diagnostics/build_weapon_replacements.py', 'resources/weapon_replacements.json',
                 'resources/weapon_parts.json']:
        h.update((ROOT / name).read_bytes())
    return h.hexdigest()

def atomic_json(path, data, compressed=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + '.pending')
    if compressed:
        with gzip.open(pending, 'wt', encoding='utf8') as f: json.dump(data, f, separators=(',', ':'))
    else: pending.write_text(json.dumps(data, indent=2) + '\n', encoding='utf8')
    os.replace(pending, path)

def load_cache():
    path = STATE / 'poses.json.gz'
    if not path.exists(): return None
    with gzip.open(path, 'rt', encoding='utf8') as f: data = json.load(f)
    return data if data.get('fingerprint') == fingerprint() else None

def defaults():
    return dict(translationCm=[0., 0., 0.], rotationDeg=[0., 0., 0.], scale=1.,
                fingers={d: [0., 0., 0., 0., 0.] for d in DIGITS})

def validate(data):
    if not isinstance(data, dict) or data.get('version') != 1 or not isinstance(data.get('weapons'), dict):
        raise ValueError('Expected version 1 hand pose document')
    result = dict(version=1, weapons={})
    def number(v, lo, hi):
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or not lo <= v <= hi:
            raise ValueError(f'Expected finite number between {lo} and {hi}')
        return float(v)
    def vector(v, n, lo, hi):
        if not isinstance(v, list) or len(v) != n: raise ValueError(f'Expected {n} values')
        return [number(x, lo, hi) for x in v]
    for key, weapon in data['weapons'].items():
        if not key.isdigit() or not 1 <= int(key) <= 64 or not isinstance(weapon, dict): raise ValueError('Invalid weapon')
        out = dict(weaponScale=number(weapon.get('weaponScale', 1), .25, 3), hands={})
        for side, edit in weapon.get('hands', {}).items():
            if side not in ['right', 'left'] or not isinstance(edit, dict): raise ValueError('Invalid hand')
            hand = defaults()
            hand['translationCm'] = vector(edit.get('translationCm', hand['translationCm']), 3, -100, 100)
            hand['rotationDeg'] = vector(edit.get('rotationDeg', hand['rotationDeg']), 3, -180, 180)
            hand['scale'] = number(edit.get('scale', 1), .4, 2)
            for digit, angles in edit.get('fingers', {}).items():
                if digit not in DIGITS: raise ValueError('Invalid finger')
                hand['fingers'][digit] = vector(angles, 5, -150, 150)
            if 'fingerPose' in edit:
                template=edit['fingerPose'];prefix='r' if side=='right' else 'l'
                expected={f'{prefix}_d{n}_j{j}' for n in range(1,6) for j in range(1,4)}|{f'{prefix}_mc5'}
                if not isinstance(template,dict) or set(template)!=expected:raise ValueError('Incomplete native finger pose template')
                hand['fingerPose']={}
                for bone,matrix in template.items():
                    if not isinstance(matrix,list) or len(matrix)!=3:raise ValueError('Expected a 3 by 4 finger transform')
                    hand['fingerPose'][bone]=[vector(row,4,-1024,1024) for row in matrix]
                    rotation=np.asarray(hand['fingerPose'][bone])[:,:3]
                    if not np.allclose(rotation.T@rotation,np.eye(3),atol=.015) or np.linalg.det(rotation)<.98:
                        raise ValueError('Finger pose must preserve native bone scale')
                source=edit.get('fingerPoseSource',{})
                if source:
                    if not isinstance(source,dict) or not isinstance(source.get('weaponId'),int) or not isinstance(source.get('name'),str) or len(source['name'])>80:
                        raise ValueError('Invalid finger pose reference')
                    hand['fingerPoseSource']=dict(weaponId=source['weaponId'],name=source['name'][:80],savedSHA256=str(source.get('savedSHA256',''))[:64])
            out['hands'][side] = hand
        result['weapons'][key] = out
    return result

def saved_edits():
    return validate(json.loads(OVERRIDES.read_text(encoding='utf8'))) if OVERRIDES.exists() else dict(version=1, weapons={})

@lru_cache(None)
def parents(side):
    g = read(f'skeletons/weapons/{side}hand/{side}hand.gla')
    n = struct.unpack_from('<i', g, 84)[0]
    return [struct.unpack_from('<i', g, 100 + struct.unpack_from('<i', g, 100 + 4*i)[0] + 68)[0] for i in range(n)]

def normal(v, fallback):
    length = np.linalg.norm(v)
    return np.asarray(v) / length if length > 1e-7 else np.asarray(fallback, dtype=float)

def edit_pose(e, side, grip, edit):
    """Rotate true native joints and descendants; then apply the hand root edit."""
    world, bases, names = np.array(e[0], dtype=float), np.array(e[1]), e[2]
    root_world=world.copy()
    table = parents(side)
    if edit.get('fingerPose'):
        # Copy the donor's resulting joint-local transforms, rather than its
        # slider deltas. Parent/palm placement stays specific to this weapon.
        local=[]
        for i,parent in enumerate(table):
            matrix=np.eye(4);matrix[:3]=world[i]
            if parent>=0:
                p=np.eye(4);p[:3]=world[parent];matrix=np.linalg.solve(p,matrix)
            if names[i] in edit['fingerPose']:matrix[:3]=edit['fingerPose'][names[i]]
            local.append(matrix)
        resolved={}
        def resolve(i):
            if i not in resolved:
                parent=table[i];resolved[i]=(resolve(parent)@local[i]) if parent>=0 else local[i]
            return resolved[i]
        world=np.asarray([resolve(i)[:3] for i in range(len(names))])
    original = world.copy()
    def joint(w, b): return w[b, :, :3] @ bases[b, :, 3] + w[b, :, 3]
    def descendants(b):
        out = []
        for i in range(len(names)):
            p = i
            while p >= 0:
                if p == b: out.append(i); break
                p = table[p]
        return out
    for n, digit in enumerate(DIGITS, 1):
        chain = [names.index(f'{side}_d{n}_j{j}') for j in range(1, 4)]
        points = [joint(original, b) for b in chain]
        proximal = normal(points[1]-points[0], [1, 0, 0])
        hinge = normal(np.cross(points[1]-points[0], points[2]-points[1]), original[chain[0], :, 2])
        # Store axes in each bone's baseline frame, so upstream edits carry them.
        axes = [np.linalg.solve(original[b, :, :3], hinge) for b in chain]
        splay = np.linalg.solve(original[chain[0], :, :3], normal(np.cross(hinge, proximal), [0, 1, 0]))
        twist = np.linalg.solve(original[chain[0], :, :3], proximal)
        angles = edit.get('fingers', {}).get(digit, [0]*5)
        operations = [(chain[0], splay, angles[3]), (chain[0], twist, angles[4])]
        operations += [(b, axes[j], angles[j]) for j, b in enumerate(chain)]
        for b, axis, angle in operations:
            if not angle: continue
            pivot = joint(world, b); axis = normal(world[b, :, :3] @ axis, [0, 0, 1])
            r = Rotation.from_rotvec(axis * math.radians(angle)).as_matrix()
            ids = descendants(b)
            world[ids, :, :3] = np.einsum('ij,bjk->bik', r, world[ids, :, :3])
            world[ids, :, 3] = (world[ids, :, 3]-pivot) @ r.T + pivot
    from build_weapon_grips import palm_points
    palm = np.asarray(palm_points(read(f'models/weapons/{side}hand/{side}hand.glm'), root_world))
    basis = np.asarray(grip['basis']); scale = edit.get('scale', 1)
    rotation = basis @ Rotation.from_euler('xyz', edit.get('rotationDeg', [0]*3), degrees=True).as_matrix() @ basis.T
    translation = basis @ (np.asarray(edit.get('translationCm', [0]*3))/2.54)
    world[:, :, :3] = np.einsum('ij,bjk->bik', rotation*scale, world[:, :, :3])
    world[:, :, 3] = (world[:, :, 3]-palm) @ rotation.T * scale + palm + translation
    return world.tolist(), e[1], names

def md3_geometry(data):
    h = struct.unpack_from('<4si64s9i', data); at = h[10]; points=[]; triangles=[]
    for _ in range(h[6]):
        s=struct.unpack_from('<4s64s10i', data, at); base=len(points)
        points += [[v/64 for v in struct.unpack_from('<3h', data, at+s[10]+i*8)] for i in range(s[5])]
        triangles += [[base+v for v in struct.unpack_from('<3i', data, at+s[7]+i*12)] for i in range(s[6])]
        at += s[-1]
    return np.asarray(points), triangles

def canonical(points, grip):
    return ((np.asarray(points)-grip['anchor']) @ np.asarray(grip['basis'])*2.54).tolist()

def hand_preview(e, side, grip, edit):
    from fit_weapon_hands import hand_mesh
    from build_weapon_grips import palm_points
    evaluated=edit_pose(e, side, grip, edit)
    points, triangles=md3_geometry(hand_mesh(evaluated, side))
    w=np.asarray(evaluated[0]); b=np.asarray(evaluated[1])
    joints=[(w[i,:,:3]@b[i,:,3]+w[i,:,3]).tolist() for i in range(len(w))]
    return dict(points=canonical(points,grip), triangles=triangles, joints=canonical(joints,grip),
                names=e[2], parents=parents(side), palm=canonical([palm_points(read(f'models/weapons/{side}hand/{side}hand.glm'),evaluated[0])],grip)[0])

def prepare_baseline(fitter, grips, entries, rows):
    """One small reusable geometry/pose cache, no copied game installations."""
    if os.environ.get('SOF2_VR_AUTHORING_WEAPONS'): return
    from build_presentation import idle
    catalog=[]
    for id, grip in grips.items():
        row=rows[id]; rest=idle(row['animations']['sequences'],Path(row['viewModel']).stem)
        data=entries.get(row['viewModel'])
        if data is None: data=read(row['viewModel'])
        world=pose(read(row['skeleton']['path']),rest['startFrame'])[0]
        points=[]; triangles=[]
        for i in range(struct.unpack_from('<4si64s64s7i',data)[8]):
            vs,ts=vertices(data,i,world)
            if not ts: continue
            base=len(points);points.extend(v[0] for v in vs);triangles.extend([[base+v for v in t] for t in ts])
        hands={}
        for label, side in [('right','r'),('left','l')]:
            e=fitter.poses.get(str(id),{}).get(side)
            if e:
                hands[label]=hand_preview(e,side,grip,defaults())
        geometry=dict(id=id,name=row['name'],grip=grip,weapon=dict(points=canonical(points,grip),triangles=triangles),hands=hands)
        atomic_json(STATE/f'w{id:02d}.json.gz',geometry,True)
        catalog.append(dict(id=id,name=row['name'],hands=list(hands)))
    atomic_json(STATE/'poses.json.gz',dict(fingerprint=fingerprint(),fits=fitter.fits,poses=fitter.poses,grips=grips),True)
    atomic_json(STATE/'catalog.json',dict(fingerprint=fingerprint(),weapons=catalog))

def scale_md3(data, scale, anchor):
    # Same single-frame format for every detachable part. Bounds are recomputed.
    d=bytearray(data);h=struct.unpack_from('<4si64s9i',d);at=h[10];points=[]
    if h[4]!=1: raise ValueError('Expected static detachable mesh')
    for _ in range(h[6]):
        s=struct.unpack_from('<4s64s10i',d,at)
        for i in range(s[5]):
            p=at+s[10]+i*8;old=struct.unpack_from('<3h',d,p)
            xyz=[round((anchor[k]+scale*(old[k]/64-anchor[k]))*64) for k in range(3)]
            if any(abs(v)>32767 for v in xyz): raise ValueError('Scaled model exceeds MD3 coordinate range')
            struct.pack_into('<3h',d,p,*xyz);points.append([v/64 for v in xyz])
        at+=s[-1]
    lo=np.min(points,axis=0);hi=np.max(points,axis=0)
    struct.pack_into('<10f',d,h[8],*lo,*hi,0,0,0,float(np.linalg.norm(points,axis=1).max()))
    return bytes(d)

def scale_glm(data, world, scale, anchor):
    """Scale posed geometry about its grip, solving the LBS bind displacement."""
    d=bytearray(data);h=struct.unpack_from('<4si64s64s7i',d);lod=h[7];world=np.asarray(world);anchor=np.asarray(anchor)
    for _ in range(h[6]):
        end=struct.unpack_from('<i',d,lod)[0]
        for i in range(h[8]):
            p=lod+4+struct.unpack_from('<i',d,lod+4+4*i)[0];s=struct.unpack_from('<10i',d,p)
            refs=struct.unpack_from('<'+'i'*s[7],d,p+s[8])
            for v in range(s[3]):
                at=p+s[4]+32*v;xyz=np.asarray(struct.unpack_from('<3f',d,at+12));bits=struct.unpack_from('<I',d,at+24)[0]
                count=(bits>>30)+1;total=0.;matrix=np.zeros((3,4))
                for j in range(count):
                    weight=1-total if j==count-1 else (d[at+28+j]|(((bits>>(20+2*j))&3)<<8))/1023.
                    total+=weight;matrix+=weight*world[refs[(bits>>(5*j))&31]]
                new=scale*xyz+np.linalg.solve(matrix[:,:3],(1-scale)*(anchor-matrix[:,3]))
                struct.pack_into('<3f',d,at+12,*new)
        lod+=end
    return bytes(d)

def apply_overrides(fitter, grips, entries, rows, report, muzzles, reloads):
    from fit_weapon_hands import hand_mesh
    from build_weapon_grips import palm_points
    from build_presentation import idle
    from build_weapon_replacements import relocate_m3_flash
    import re
    edits=saved_edits()['weapons'];reports={w['id']:w for w in report}
    for id,grip in grips.items():
        edit=edits.get(str(id))
        if not edit:continue
        anchor=np.asarray(grip['anchor']).copy();scale=edit['weaponScale'];row=rows[id]
        point=lambda p:(anchor+scale*(np.asarray(p)-anchor)).tolist()
        if scale!=1:
            WEAPON_SCALES[id]=(anchor,scale)
            rest=idle(row['animations']['sequences'],Path(row['viewModel']).stem)
            world=pose(read(row['skeleton']['path']),rest['startFrame'])[0]
            data=entries.get(row['viewModel'])
            if data is None:data=read(row['viewModel'])
            entries[row['viewModel']]=scale_glm(data,world,scale,anchor)
            reload=next((r for r in reloads['ready'] if r['id']==id),None)
            if reload:
                for part in ['body','magazine','bolt']:
                    path=reload['prefix']+'_'+part+'.md3'
                    if path in entries:entries[path]=scale_md3(entries[path],scale,[0,0,0] if part=='magazine' else anchor)
                for key in ['socket','bolt']:reload[key]=point(reload[key])
            if id in muzzles:muzzles[id]['anchor']=point(muzzles[id]['anchor'])
            if id in reports:
                r=reports[id];m=np.asarray(r['calibration']['matrix']);m[:,:3]*=scale;m[:,3]=point(m[:,3])
                r['calibration']['matrix']=m.tolist();r['calibration']['scales']=[s*scale for s in r['calibration']['scales']]
                r['sockets']={k:point(v) for k,v in r['sockets'].items()}
            if id==9:
                legacy=json.loads(entries['sof2vr-reload-parts.json'])
                for key in ['socket','boltSocket','muzzleSocket']:legacy[key]=point(legacy[key])
                entries['sof2vr-reload-parts.json']=json.dumps(legacy,indent=2).encode()
                path=ROOT/'src/reload_parts.h';text=path.read_text()
                for name,key in [('m4MagazineSocket','socket'),('m4BoltSocket','boltSocket'),('m4MuzzleSocket','muzzleSocket')]:
                    text=re.sub(r'(static const float '+name+r'\[3\]=)\{[^}]+\}',lambda m:m[1]+'{'+','.join('%.7ff'%v for v in legacy[key])+'}',text)
                path.write_text(text)
            if id==6:
                loading=json.loads(entries['sof2vr-shotgun-loading.json'])
                for part in ['body','pump']:
                    path=f'models/sof2vr_loading/m590_{part}.md3';entries[path]=scale_md3(entries[path],scale,anchor)
                for key in ['port','pump']:loading[key]=point(loading[key])
                loading['pumpTravelCentimetres']*=scale
                entries['sof2vr-shotgun-loading.json']=json.dumps(loading,indent=2).encode()
                (ROOT/'src/loading_parts.h').write_text('#ifndef SOF2_LOADING_PARTS_H\n#define SOF2_LOADING_PARTS_H\n'+''.join('static const float '+name+'[3]={'+','.join('%.8ff'%v for v in loading[key])+'};\n' for name,key in [('m590LoadPort','port'),('m590PumpSocket','pump')])+f'#define M590_PUMP_TRAVEL {3.5433071*scale:.8f}f\n#endif\n')
            if id==8:relocate_m3_flash(entries,row,rest,muzzles[id]['anchor'])
        for label,handedit in edit['hands'].items():
            side='r' if label=='right' else 'l';base=fitter.poses.get(str(id),{}).get(side)
            if not base:raise ValueError(f'No editable {label} hand for {id}')
            evaluated=edit_pose(base,side,grip,handedit)
            hand=grip['hands'][label];entries[hand['model']]=hand_mesh(evaluated,side)
            hand['anchor']=palm_points(read(f'models/weapons/{side}hand/{side}hand.glm'),evaluated[0])
            hand['manualEditor']=handedit
            if side=='r':grip['anchor']=hand['anchor']
        if id==9:
            path=ROOT/'src/reload_parts.h';text=path.read_text();text=re.sub(r'(static const float m4GripSocket\[3\]=)\{[^}]+\}',lambda m:m[1]+'{'+','.join('%.7ff'%v for v in grip['anchor'])+'}',text);path.write_text(text)
            legacy=json.loads(entries['sof2vr-reload-parts.json']);legacy['gripSocket']=grip['anchor'];entries['sof2vr-reload-parts.json']=json.dumps(legacy,indent=2).encode()
    entries['sof2vr-manual-hand-edits.json']=json.dumps(saved_edits(),indent=2).encode()
