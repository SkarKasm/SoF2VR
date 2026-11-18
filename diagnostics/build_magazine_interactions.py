"""Derive packed-magazine landmarks and native reload-only grip hands.

Authored weapon/finger poses and weapon/magazine meshes are never modified.
"""
import json, struct, sys, os
from pathlib import Path
import zipfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,os.environ.get('SOF2_ASSET_PYTHON_LIB','E:/SoF2-asset-tools/python-packages'))
import numpy as np

def points(data):
    h=struct.unpack_from('<4si64s9i',data);at=h[10];out=[]
    for _ in range(h[6]):
        s=struct.unpack_from('<4s64s10i',data,at)
        out.extend([[v/64 for v in struct.unpack_from('<3h',data,at+s[10]+8*i)] for i in range(s[5])]);at+=s[-1]
    return np.unique(np.round(out,6),axis=0)

def authored_hand(side,row):
    from native_pose import read,pose,bolt,vertices,ROOT as hand_root
    from build_reload_assets import compose
    from build_weapon_profiles import parse,value
    from build_weapon_grips import hand_mesh,palm_points
    name=f'm1911a1{side}idle.xsi'
    frame=next(s['startFrame'] for s in row['animations']['sequences'] if s['name']==name)
    wp=pose(read(row['skeleton']['path']),frame)
    buffer=pose(read('skeletons/weapons/buffer/pistol/pistol.gla'),0,bolt(wp,'gun'))
    frames=parse(read(f'skeletons/weapons/{side}hand/{side}hand.frames').decode('cp1252'))
    hf=next(int(value(b,'startframe')) for k,b in frames if k.replace('\\','/').split('/')[-1].lower()==side+name)
    e=pose(read(f'skeletons/weapons/{side}hand/{side}hand.gla'),hf,bolt(buffer,side+'hand_m1911'))
    wrist=bolt(e,side+'_metacarple');index=e[2].index(side+'_metacarple')
    inverse=[[wrist[j][i] for j in range(3)]+[-sum(wrist[j][i]*wrist[j][3] for j in range(3))] for i in range(3)]
    canonical=np.asarray(compose(hand_root,compose(e[1][index],inverse)))
    data=read(f'models/weapons/{side}hand/{side}hand.glm')
    palm=np.asarray(palm_points(data,[hand_root for _ in e[2]]));canonical[:,3]-=palm
    closed=[compose(canonical.tolist(),m) for m in e[0]]
    mesh=hand_mesh(data,(closed,e[1],e[2]),side)
    original=read(row['viewModel']);mag=[];body=[]
    for i,surface in enumerate(row['surfaces']):
        if '_off' in surface['name']:continue
        vs,_=vertices(original,i,wp[0]);target=mag if surface['name'].startswith('clip') else body
        target.extend(v[0] for v in vs)
    p=np.asarray(mag)@canonical[:,:3].T+canonical[:,3]
    _,v=np.linalg.eigh(np.cov(p.T));up=v[:,-1]
    towards=np.asarray(body).mean(0)@canonical[:,:3].T+canonical[:,3]-p.mean(0)
    if np.dot(up,towards)<0:up=-up
    front=v[:,-2];forward=canonical[:,:3]@np.asarray([1.,0,0])
    if np.dot(front,forward)<0:front=-front
    thin=np.cross(up,front);thin/=np.linalg.norm(thin);front=np.cross(thin,up)
    return mesh,np.asarray([up,front,thin]),(p.min(0)+p.max(0))/2

def derive():
    out=[];assets={}
    rows={w['id']:w for w in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons']}
    with zipfile.ZipFile(ROOT/'resources/z_sof2vr_native.pk3') as z:
        reloads={r['id']:r for r in json.loads(z.read('sof2vr-magazine-parts.json'))['ready']}
        configs={r['id']:r for r in json.loads((ROOT/'resources/magazine_interactions.json').read_text())['weapons']}
        # Every detachable magazine uses its actual packed feed end and a
        # stable native palm grip. Weapon/hand calibration is untouched.
        for id in sorted(reloads):
            config=configs.get(id,dict(id=id,gripHeightFraction=.2,mouthInsetCm=.6,radialToleranceCm=4.5,seatFraction=.65))
            r=reloads[config['id']];p=points(z.read(r['prefix']+'_magazine.md3'))
            # PCA gives the actual tilted tube, not the weapon model's Z axis.
            values,vectors=np.linalg.eigh(np.cov(p.T));up=vectors[:,-1]
            if np.dot(up,np.asarray(r['bolt'])-r['socket'])<0:up=-up
            front=vectors[:,-2]
            if front[0]<0:front=-front
            thin=np.cross(up,front);thin/=np.linalg.norm(thin);front=np.cross(thin,up)
            frame=np.asarray([up,front,thin]);q=p@frame.T
            lo,hi=q[:,0].min(),q[:,0].max();length=hi-lo
            cap=q[q[:,0]>hi-.05*length]
            transverse=(cap[:,1:].min(0)+cap[:,1:].max(0))/2
            feed=np.asarray([hi,*transverse])@frame
            mouth=np.asarray([lo+config['mouthInsetCm']*.3937,*transverse])@frame
            grip=np.asarray([lo+length*config['gripHeightFraction'],*transverse])@frame
            bases=[];origins=[]
            models=[]
            for side in range(2):
                hand,target,centre=authored_hand('l' if side==0 else 'r',rows[3])
                path=r['prefix']+('_lhand.md3' if side==0 else '_rhand.md3');assets[path]=hand;models.append(path)
                if side==1:target[2]*=-1 # Match the mirrored left-handed weapon assembly.
                basis=frame.T@target;origin=centre-grip@basis
                bases.append(basis.tolist());origins.append(origin.tolist())
            out.append(dict(id=config['id'],feed=feed.tolist(),mouth=mouth.tolist(),up=up.tolist(),front=front.tolist(),
                holdOrigin=origins,holdBasis=bases,handModels=models,
                radial=config['radialToleranceCm']*.3937,seatDepth=float(np.dot(feed-mouth,up))*config['seatFraction']))
    return out,assets

def profiles():return derive()[0]

def generate():
    data,assets=derive()
    def floats(v):
        return '{'+','.join(floats(x) if isinstance(x,list) else f'{x:.8f}f' for x in v)+'}'
    text='/* Packed magazine landmarks and fixed native-palm grips; generated. */\n#ifndef SOF2_MAGAZINE_INTERACTIONS_H\n#define SOF2_MAGAZINE_INTERACTIONS_H\n'
    text+='typedef struct {int id;float feed[3],mouth[3],up[3],front[3],holdOrigin[2][3],holdBasis[2][3][3];float radial,seatDepth;const char *handModels[2];} VRMagazineInteraction;\n'
    text+='static const VRMagazineInteraction vrMagazineInteractions[]={\n'
    for r in data:text+=' {%d,%s,%s,%s,%s,%s,%s,%.8ff,%.8ff,{"%s","%s"}},\n'%(r['id'],*(floats(r[k]) for k in ['feed','mouth','up','front','holdOrigin','holdBasis']),r['radial'],r['seatDepth'],*r['handModels'])
    text+='};\nstatic inline const VRMagazineInteraction *MagazineInteraction(int id){for(unsigned i=0;i<sizeof(vrMagazineInteractions)/sizeof(vrMagazineInteractions[0]);++i)if(vrMagazineInteractions[i].id==id)return &vrMagazineInteractions[i];return 0;}\n#endif\n'
    path=ROOT/'src/magazine_interactions.h'
    if not path.exists() or path.read_text()!=text:path.write_text(text)
    pack=ROOT/'resources/z_sof2vr_native.pk3';pending=pack.with_suffix('.magazine-pending')
    replaced=False
    with zipfile.ZipFile(pack) as z:
        changes={n:b for n,b in assets.items() if n not in z.namelist() or z.read(n)!=b}
        if changes:
            try:
                with zipfile.ZipFile(pending,'w') as target:
                    for entry in z.infolist():target.writestr(entry,changes.pop(entry.filename,z.read(entry)))
                    for name,body in changes.items():
                        entry=zipfile.ZipInfo(name,date_time=(2002,1,1,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED;target.writestr(entry,body)
            except BaseException:
                pending.unlink(missing_ok=True);raise
            replaced=True
    if replaced:os.replace(pending,pack)
    return data

if __name__=='__main__':print(json.dumps(generate(),indent=2))
