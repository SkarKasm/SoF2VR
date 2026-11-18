"""Fit native third-person Mullins wrist sections to reviewed first-person cuffs.

Separate attachments preserve every byte of the owner's hands/fingers. Donor
UVs and outfit arm materials are retained. Original game archives are read only.
"""
import collections,hashlib,json,math,os,struct,sys,zipfile
from pathlib import Path
sys.path.insert(0,'E:/SoF2-asset-tools/python-packages')
import numpy as np
from native_pose import pose,vertices,bolt,IDENTITY
from build_hand_assets import OUTFITS,make_md3
from build_body_assets import wide_axis
from build_weapon_profiles import parse,value
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT.parent/'Soldier of Fortune 2/base'
LENGTH=1.18 # 3 cm of actual wrist, not a replacement forearm.
def unit(v):return np.asarray(v)/np.linalg.norm(v)
def cuff(data):
    h=struct.unpack_from('<4si64s9i',data);at=h[10]
    for _ in range(h[6]):
        s=struct.unpack_from('<4s64s10i',data,at);shader=data[at+s[8]:at+s[8]+64].split(b'\0')[0].decode()
        if shader=='sof2vr/wrist_leather':
            p=np.array([np.array(struct.unpack_from('<3h',data,at+s[10]+8*i))/64 for i in range(s[5])])
            ts=[struct.unpack_from('<3i',data,at+s[7]+12*i) for i in range(s[6])]
            # The seal's first vertex is the outer closing fan centre.
            c=p[0];fan=[t for t in ts if 0 in t];ring=np.array([p[i] for i in sorted({i for t in fan for i in t if i})])
            normal=np.zeros(3)
            for t in fan:normal-=np.cross(p[t[1]]-c,p[t[2]]-c)
            n=unit(normal);w=unit(wide_axis(ring));w=unit(w-n*np.dot(w,n));depth=np.cross(-n,w)
            return c,n,w,depth,ring
        at+=s[-1]
    raise ValueError('Missing closed leather cuff')
def donor(z,a,outfit,side):
    skin,model=OUTFITS[outfit];path=f'models/characters/{model}/{model}.glm';data=z.read(path);h=struct.unpack_from('<4si64s64s7i',data)
    ev=pose(a.read(h[3].split(b'\0')[0].decode()+'.gla'),25920,IDENTITY);mesh={}
    for i in range(h[8]):
        at=164+struct.unpack_from('<i',data,164+4*i)[0];name=data[at:at+64].split(b'\0')[0].decode()
        if name in ['hand_'+side,'cap_hand_'+side+'_off']:mesh[name]=vertices(data,i,ev[0])
    hand=np.array([r[3] for r in bolt(ev,side+'hand')]);elbow=np.array([r[3] for r in bolt(ev,side+'radius')]);axis=unit(hand-elbow)
    seam=np.array([v[0] for v in mesh['cap_hand_'+side+'_off'][0]]);width=unit(wide_axis(seam));width=unit(width-axis*np.dot(width,axis));depth=np.cross(axis,width)
    vs,ts=mesh['hand_'+side];out=[]
    for t in ts:
        poly=[]
        for i in t:
            p,n,uv=vs[i][:3];p=np.array(p);x=max(0,float(np.dot(p-hand,axis)))
            if min(np.linalg.norm(p-q) for q in seam)<.002:x=0
            poly.append([np.array([x,np.dot(p-hand,width),np.dot(p-hand,depth)]),np.array(uv)])
        clipped=[]
        for a0,b in zip(poly,poly[1:]+poly[:1]):
            inside=a0[0][0]<=LENGTH;other=b[0][0]<=LENGTH
            if inside:clipped.append(a0)
            if inside!=other:
                t0=(LENGTH-a0[0][0])/(b[0][0]-a0[0][0]);clipped.append([a0[j]+(b[j]-a0[j])*t0 for j in range(2)])
        for j in range(1,len(clipped)-1):out.append([clipped[0],clipped[j],clipped[j+1]])
    if not out:raise ValueError('No donor wrist for '+outfit+side)
    blocks=parse(z.read(f'models/characters/skins/{skin}.g2skin').decode('cp1252'))
    material=next(value(value(v,'group',[]),'texture1') or value(value(v,'group',[]),'shader1') for k,v in blocks if k=='material' and value(v,'name')=='arms')
    return out,material,path
def fit(mesh,frame):
    c,n,w,d,ring=frame;p=[];uv=[];ts=[]
    # Native source seam centre can be offset from the hand bone.
    source=np.array([v[0] for t in mesh for v in t if v[0][0]<.01]);centre=source[:,1:].mean(axis=0)
    radius=np.max(np.abs(source[:,1:]-centre),axis=0)
    target=np.array([[np.dot(r-c,w),np.dot(r-c,d)] for r in ring]);scale=np.max(np.abs(target),axis=0)/np.maximum(radius,.1)
    polar=sorted((math.atan2(q[1],q[0]),float(np.linalg.norm(q))) for q in target)
    def target_radius(angle):
        rows=[(polar[-1][0]-2*math.pi,polar[-1][1]),*polar,(polar[0][0]+2*math.pi,polar[0][1])]
        for (a,r),(b,s) in zip(rows,rows[1:]):
            if a<=angle<=b:return r+(s-r)*(angle-a)/max(b-a,1e-8)
        return polar[0][1]
    for tri in mesh:
        start=len(p)
        for v,tex in tri:
            x=float(v[0]);cross=(v[1:]-centre)*scale;length=np.linalg.norm(cross);t=x/LENGTH
            if length>.001:
                # Finish inside the closed glove seal; preserve the donor's
                # natural proximal section rather than making a flat cap.
                targetR=target_radius(math.atan2(cross[1],cross[0]))*.97
                cross*=1+(targetR/length-1)*t*t*(3-2*t)
            point=c+n*(LENGTH-x-.08)+w*cross[0]+d*cross[1]
            p.append(point.tolist());uv.append(tex.tolist())
        if np.linalg.norm(np.cross(np.array(p[-2])-p[-3],np.array(p[-1])-p[-3]))>1e-6:ts.append([start,start+1,start+2])
    normals=np.zeros((len(p),3))
    for tri in ts:
        a,b,cc=[np.array(p[i]) for i in tri];normal=-np.cross(b-a,cc-a)
        for i in tri:normals[i]+=normal
    normals=[unit(n0).tolist() if np.linalg.norm(n0)>.0001 else n.tolist() for n0 in normals]
    return p,normals,uv,ts
def build():
    pack=ROOT/'resources/z_sof2vr_native.pk3';entries={};rows=[];sources={};protected={}
    with zipfile.ZipFile(pack) as current,zipfile.ZipFile(BASE/'skins.pk3') as z,zipfile.ZipFile(BASE/'therest.pk3') as a:
        for outfit in OUTFITS:
            for side in 'lr':sources[outfit,side]=donor(z,a,outfit,side)
        hands=[p for p in current.namelist() if p.endswith('.md3') and ('/sof2vr_grips/' in p or '/sof2vr_native/' in p or '/sof2vr_reload/' in p and 'hand' in p)]
        armRims=[]
        bodies={r['name']:r for r in json.loads(current.read('sof2vr-body-prototype.json'))['outfits']}
        for outfit in OUTFITS:
            pair=[]
            for side in 'lr':
                d=current.read(f'models/sof2vr_body/{outfit}/{side}_lower.md3');h=struct.unpack_from('<4si64s9i',d);at=h[10];s=struct.unpack_from('<4s64s10i',d,at)
                ps=[tuple(v/64 for v in struct.unpack_from('<3h',d,at+s[10]+8*i)) for i in range(s[5])];uvs=[struct.unpack_from('<2f',d,at+s[9]+8*i) for i in range(s[5])];ts=[struct.unpack_from('<3i',d,at+s[7]+12*i) for i in range(s[6])];end=round(bodies[outfit]['arms']['lr'.index(side)]['lengths'][1]*64)/64;edges=collections.Counter();edgeUV={}
                for tri in ts:
                    if all(abs(ps[i][0]-end)<.01 for i in tri):continue
                    for a0,b in zip(tri,tri[1:]+tri[:1]):
                        if abs(ps[a0][0]-end)<.01 and abs(ps[b][0]-end)<.01:
                            key=tuple(sorted([ps[a0],ps[b]]));edges[key]+=1;edgeUV[key]={ps[a0]:uvs[a0],ps[b]:uvs[b]}
                points=sorted({v for e,n0 in edges.items() if n0==1 for v in e})
                if not points:
                    # Some coats have a separate closed cuff island instead
                    # of an open arm boundary. Match its exterior rim.
                    from scipy.spatial import ConvexHull
                    candidates=sorted({p for p in ps if abs(p[0]-end)<.01});hull=ConvexHull(np.array(candidates)[:,1:]);points=[candidates[i] for i in hull.vertices]
                centre=np.mean(points,axis=0);points.sort(key=lambda p:math.atan2(p[2]-centre[2],p[1]-centre[1]));tex=[uvs[ps.index(p)] for p in points]
                beginUV=[];endUV=[]
                for p,q in zip(points,points[1:]+points[:1]):
                    edge=edgeUV.get(tuple(sorted([p,q])),{});beginUV.append(edge.get(p,uvs[ps.index(p)]));endUV.append(edge.get(q,uvs[ps.index(q)]))
                pair.append(dict(points=points,uv=beginUV,endUV=endUV,centre=centre.tolist(),shader=sources[outfit,side][1]))
                # The join replaces the flat dismemberment cap. Retaining it
                # exposes a fan of triangles across the newly bent wrist.
                keep=[t for t in ts if not all(abs(ps[i][0]-end)<.01 for i in t)]
                if len(keep)!=len(ts):
                    fixed=bytearray(d);struct.pack_into('<i',fixed,at+84,len(keep))
                    for j,t in enumerate(keep):struct.pack_into('<3i',fixed,at+s[7]+12*j,*t)
                    entries[f'models/sof2vr_body/{outfit}/{side}_lower.md3']=bytes(fixed)
            armRims.append(pair)
        for path in hands:
            data=current.read(path)
            try:frame=cuff(data)
            except ValueError:continue
            protected[path]=hashlib.sha256(data).hexdigest();key=hashlib.sha1(path.encode()).hexdigest()[:12]
            name=Path(path).stem;side='l' if name.startswith('lhand') or name.endswith(('_l','_lhand')) else 'r'
            geometry=fit(sources['jungle',side][0],frame)
            c,n,w,_,_=frame
            ring=frame[-1];ring=sorted(ring.tolist(),key=lambda p:math.atan2(np.dot(np.array(p)-c,frame[3]),np.dot(np.array(p)-c,w)))
            outfitUV=[]
            for outfit in OUTFITS:
                source=sources[outfit,side][0];verts=[v for t in source for v in t];coords=np.array([v[0] for v in verts]);crossCentre=coords[:,1:].mean(axis=0);radius=np.max(np.abs(coords[:,1:]-crossCentre),axis=0)
                rowUV=[]
                for point in ring:
                    angle=math.atan2(np.dot(np.array(point)-c,frame[3]),np.dot(np.array(point)-c,w));wanted=np.array([LENGTH,crossCentre[0]+math.cos(angle)*radius[0],crossCentre[1]+math.sin(angle)*radius[1]])
                    rowUV.append(verts[int(np.argmin(np.linalg.norm(coords-wanted,axis=1)))][1].tolist())
                outfitUV.append(rowUV)
            rows.append(dict(hand=path,key=key,point=(c+n*(LENGTH-.08)).tolist(),width=w.tolist(),triangles=len(geometry[-1]),ring=ring,uv=outfitUV))
        report=dict(version=2,lengthCm=LENGTH*2.54,donors=sorted({v[2] for v in sources.values()}),outfits=list(OUTFITS),hands=rows,unchangedHands=protected,policy='Dynamic boundary-matched wrists using native Mullins forearm seams/materials. Original first-person fingers and placements stay unchanged; no independent wrist stubs.')
        report['armRims']=armRims
        entries['sof2vr-donor-wrists.json']=(json.dumps(report,indent=2)+'\n').encode()
        shader=current.read('shaders/weapons.shader').decode('cp1252');start='// BEGIN NATIVE WRIST JOINS';end='// END NATIVE WRIST JOINS'
        if start in shader:shader=shader[:shader.index(start)]+shader[shader.index(end)+len(end):]
        shader+='\n'+start+'\n'
        for i,pair in enumerate(armRims):shader+='sof2vr/wrist_join_%d\n{\n cull disable\n {\n map %s\n rgbGen vertex\n }\n}\n'%(i,pair[0]['shader'])
        shader+=end+'\n';entries['shaders/weapons.shader']=shader.encode('cp1252')
        finish=json.loads(current.read('sof2vr-visual-finishes.json'));finish['outputs']['shaders/weapons.shader']=hashlib.sha256(entries['shaders/weapons.shader']).hexdigest();entries['sof2vr-visual-finishes.json']=(json.dumps(finish,indent=2)+'\n').encode()
        pending=pack.with_suffix('.wrists-pending')
        with zipfile.ZipFile(pending,'w') as out:
            for info in current.infolist():
                if info.filename.startswith('models/sof2vr_wrists/'):continue # Superseded rigid test attachments.
                out.writestr(info,entries.pop(info.filename,current.read(info)))
            for name,data in entries.items():out.writestr(name,data,compress_type=zipfile.ZIP_DEFLATED)
    os.replace(pending,pack)
    f=lambda v:'{'+','.join(f'{x:.8f}f' for x in v)+'}'
    header=['/* Native third-person wrist attachments and exact boundary rings. */','#ifndef SOF2_DONOR_WRISTS_H','#define SOF2_DONOR_WRISTS_H','#define VR_WRIST_RING_MAX 64','typedef struct {const char *hand,*key;float point[3],width[3];int count;float ring[64][3],uv[7][64][2];} VRDonorWrist;','static const VRDonorWrist vrDonorWrists[]={']
    header += ['{"%s","%s",%s,%s,%d,{%s},{%s}},'%(r['hand'],r['key'],f(r['point']),f(r['width']),len(r['ring']),','.join(f(p) for p in r['ring']),','.join('{'+','.join(f(uv) for uv in row)+'}' for row in r['uv'])) for r in rows]
    header+=['};','typedef struct {int count;float point[64][3],uv[64][2],endUV[64][2],centre[3];} VRArmRim;','static const VRArmRim vrArmRims[7][2]={']
    for pair in armRims:header.append('{'+','.join('{%d,{%s},{%s},{%s},%s}'%(len(r['points']),','.join(f(p) for p in r['points']),','.join(f(uv) for uv in r['uv']),','.join(f(uv) for uv in r['endUV']),f(r['centre'])) for r in pair)+'},')
    (ROOT/'src/donor_wrists.h').write_text('\n'.join(header+['};','#endif','']))
    print('Prepared',len(rows),'continuous wrist boundaries for',len(OUTFITS),'native outfits; all hand meshes unchanged.')
if __name__=='__main__':build()
