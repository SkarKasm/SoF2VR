"""Local Mullins torso/arm prototype. No RTCW hand assets are read.

Original Raven UVs/materials are retained. The first prototype uses rigid arm
segments; its manifest explicitly distinguishes this from smooth skinning.
"""
import argparse,hashlib,json,math,struct,zipfile
from pathlib import Path
from build_hand_assets import OUTFITS,make_md3
from build_magazine_assets import multipart
from build_weapon_profiles import parse,value
from native_pose import pose,bolt,vertices,IDENTITY
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT.parent/'Soldier of Fortune 2/base'

def dot(a,b):return sum(x*y for x,y in zip(a,b))
def sub(a,b):return [x-y for x,y in zip(a,b)]
def unit(a):return [v/math.sqrt(dot(a,a)) for v in a]
def game(p):return [-p[1],p[0],p[2]]
def cross(a,b):return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
def mean(ps):return [sum(p[k] for p in ps)/len(ps) for k in range(3)]
def wide_axis(points):
 # A cuff's longest diameter gives its width direction without depending on
 # vertex order, UV seams or the sign returned by an eigenvector solver.
 points=sorted(set(tuple(p) for p in points));a,b=max(((a,b) for i,a in enumerate(points) for b in points[i+1:]),key=lambda pair:math.dist(*pair))
 axis=unit(sub(b,a));largest=max(range(3),key=lambda k:abs(axis[k]))
 return axis if axis[largest]>=0 else [-v for v in axis]
def arm_geometry(mesh,start,end,caps,wrist_roll=False):
 points,normals,uv,tri=mesh;axis=unit(sub(end,start));depth=[0,-1,0];projection=dot(axis,depth);depth=unit([depth[k]-projection*axis[k] for k in range(3)]);lateral=cross(depth,axis);basis=[axis,lateral,depth]
 # Lower arms use the native distal cuff width as their roll reference.
 # Runtime can then match that width to the rendered glove, rather than
 # rotating the sleeve with the torso while the player's wrist turns.
 if wrist_roll:
  width=wide_axis(caps[1]);projection=dot(width,axis);width=unit([width[k]-projection*axis[k] for k in range(3)]);basis=[axis,width,cross(axis,width)]
 length=math.dist(start,end);ps=[list(dot(sub(p,start),a) for a in basis) for p in points];ns=[list(dot(n,a) for a in basis) for n in normals];uv=list(uv);tri=list(tri)
 # Raven's dismemberment seams project past the skeletal joint. Flatten the
 # actual shared seam vertices onto its plane and close them; otherwise the
 # independent rigid pieces expose sharp, open sleeve tips when bent in VR.
 for seam,plane in zip(caps,[0,length]):
  ring=[i for i,p in enumerate(points) if min(math.dist(p,q) for q in seam)<.002]
  if len(ring)<3:raise ValueError('Missing original sleeve seam')
  for i in ring:ps[i][0]=plane
  unique={tuple(round(v,4) for v in ps[i]):i for i in ring};ring=list(unique.values());centre=mean([ps[i] for i in ring]);tex=[sum(uv[i][k] for i in ring)/len(ring) for k in range(2)]
  ring.sort(key=lambda i:math.atan2(ps[i][2]-centre[2],ps[i][1]-centre[1]));ci=len(ps);ps.append(centre);ns.append([-1 if plane==0 else 1,0,0]);uv.append(tex)
  for a,b in zip(ring,ring[1:]+ring[:1]):
   n=cross(sub(ps[a],centre),sub(ps[b],centre));tri.append((ci,b,a) if dot(n,ns[-1])>0 else (ci,a,b))
 return (ps,ns,uv,tri),length

def cuff_frame(data, overlap=0):
 h=struct.unpack_from('<4si64s9i',data);at=h[10];centres=[];ring=[];normal=[0.,0.,0.]
 for _ in range(h[6]):
  s=struct.unpack_from('<4s64s10i',data,at)
  for i in range(s[5]):
   uv=struct.unpack_from('<2f',data,at+s[9]+i*8)
   # The closing fan centre has this exact dedicated leather UV, unlike
   # the original hand islands. Work in the packed/quantized geometry.
   if abs(uv[0]-.13)<1e-6 and abs(uv[1]-.20)<1e-6:
    point=lambda index:[v/64 for v in struct.unpack_from('<3h',data,at+s[10]+index*8)]
    centres.append(point(i))
    for triangle in range(s[6]):
     indices=struct.unpack_from('<3i',data,at+s[7]+triangle*12)
     if i in indices:
      a,b,c=[point(index) for index in indices];n=cross(sub(b,a),sub(c,a))
      normal=[normal[k]-n[k] for k in range(3)]
      ring.extend(point(index) for index in indices if index!=i)
  at+=s[11]
 if not centres:raise ValueError('Glove has no explicit closed wrist centre')
 centre=mean(centres)
 if overlap:centre=[p-overlap*n for p,n in zip(centre,unit(normal))]
 return centre,wide_axis(list({tuple(p) for p in ring}))

def cuff(data):return cuff_frame(data)[0]

def build(existing):
 entries={};report={'stage':'experimental rigid torso and two-bone segmented arms; smooth skinning pending','revision':'native-cuff-roll-20261024','source':'Installed SoF2 skins.pk3; native first-person gloves remain unchanged','forearmRoll':'native sleeve cuff width aligned to actual rendered glove cuff width','outfits':[],'wrists':[]}
 with zipfile.ZipFile(BASE/'skins.pk3') as z,zipfile.ZipFile(BASE/'therest.pk3') as animations:
  for outfit,(skin,model) in OUTFITS.items():
   skinpath=f'models/characters/skins/{skin}.g2skin';skinbytes=z.read(skinpath);blocks=parse(skinbytes.decode('cp1252'))
   shaders={value(v,'name'):value(value(v,'group',[]),'texture1') or value(value(v,'group',[]),'shader1') for k,v in blocks if k=='material'}
   data=z.read(f'models/characters/{model}/{model}.glm');h=struct.unpack_from('<4si64s64s7i',data)
   skeleton=h[3].split(b'\0')[0].decode()+'.gla';evaluated=pose(animations.read(skeleton),25920,IDENTITY);world=evaluated[0]
   mesh={}
   for i in range(h[8]):
    p=164+struct.unpack_from('<i',data,164+4*i)[0];name=data[p:p+64].split(b'\0')[0].decode();v,t=vertices(data,i,world)
    mesh[name]=([x[0] for x in v],[x[1] for x in v],[x[2] for x in v],t)
   eye=mean(mesh['eyeball_l'][0]+mesh['eyeball_r'][0]);eyeGame=game(eye)
   prefs=value(blocks,'prefs',[]);on=set(v for k,v in value(prefs,'surfaces_on',[]));off=set(v for k,v in value(prefs,'surfaces_off',[]))
   groups={};included=[]
   # Only torso/hips: the head would cover the eyes, and static legs are
   # deliberately not presented as locomotion IK. Preserve outfit coats.
   for i in range(h[8]):
    p=164+struct.unpack_from('<i',data,164+4*i)[0];name=data[p:p+64].split(b'\0')[0].decode();material=data[p+68:p+132].split(b'\0')[0].decode();flags=struct.unpack_from('<i',data,p+64)[0]
    if not name.startswith(('torso_','hip_','suitcoat_','trenchcoat_')):continue
    if name in off or (flags&3 and name not in on):continue
    if material not in shaders:raise ValueError(f'Unmapped Mullins material {outfit}: {material}')
    points,normals,uv,tri=mesh[name];points=[game(p) for p in points];normals=[game(n) for n in normals]
    group=groups.setdefault(shaders[material],[[],[],[],[]]);offset=len(group[0]);group[0]+=points;group[1]+=normals;group[2]+=uv;group[3]+=[tuple(v+offset for v in t) for t in tri];included.append(name)
   prefix=f'models/sof2vr_body/{outfit}'
   entries[prefix+'/torso.md3']=multipart(groups,eyeGame)
   arms=[]
   for side in 'lr':
    parts=[];joints=[[r[3] for r in bolt(evaluated,side+name)] for name in ['humerus','radius','hand']]
    for segment in ['upper','lower']:
     index=0 if segment=='upper' else 1;part='uppr' if index==0 else 'lwr';seams=[mesh[f'cap_arm_{part}_{side}_{suffix}_off'][0] for suffix in (['torso','lwr'] if index==0 else ['uppr','hand'])];geometry,length=arm_geometry(mesh['arm_'+part+'_'+side],joints[index],joints[index+1],seams,wrist_roll=index==1);entries[f'{prefix}/{side}_{segment}.md3']=make_md3(*geometry,shaders['arms'],side);parts.append(length)
    arms.append({'side':side,'lengths':parts,'shoulder':sub(game(joints[0]),eyeGame),'joints':joints})
   report['outfits'].append(dict(name=outfit,prefix=prefix,sourceModel=model,sourceSHA256=hashlib.sha256(data).hexdigest(),skin=skinpath,skinSHA256=hashlib.sha256(skinbytes).hexdigest(),surfaces=included,arms=arms,eye=eyeGame,skeleton=skeleton,frame=25920))
 grips=json.loads(existing['sof2vr-grip-poses.json'])
 for row in grips:
  hands=row['hands'];frames=[cuff_frame(existing[hands[s]['model']]) if hands[s] else ([0,0,0],[0,1,0]) for s in ['right','left']];report['wrists'].append({'id':row['id'],'points':[f[0] for f in frames],'widths':[f[1] for f in frames]})
 frames=[[cuff_frame(existing[f'models/sof2vr_native/{s}hand_grip{step:02d}.md3']) for step in range(17)] for s in 'lr']
 report['freeWrists']=[[f[0] for f in hand] for hand in frames];report['freeWristWidths']=[[f[1] for f in hand] for hand in frames]
 floats=lambda ps:'{'+','.join(f'{v:.8f}f' for v in ps)+'}'
 header=['/* Local native Mullins body prototype. Generated by build_body_assets.py. */','#ifndef SOF2_BODY_PROFILES_H','#define SOF2_BODY_PROFILES_H','typedef struct {const char *name,*prefix;float shoulder[2][3],length[2][2];} VRBodyProfile;','static const VRBodyProfile vrBodyProfiles[]={']
 for row in report['outfits']:header.append('{"%s","%s",{%s},{%s}},'%(row['name'],row['prefix'],','.join(floats(a['shoulder']) for a in row['arms']),','.join(floats(a['lengths']) for a in row['arms'])))
 header+=['};','typedef struct {int id;float point[2][3],width[2][3];} VRBodyWrist;','static const VRBodyWrist vrBodyWrists[]={']
 for row in report['wrists']:header.append('{%d,{%s},{%s}},'%(row['id'],','.join(floats(p) for p in row['points']),','.join(floats(p) for p in row['widths'])))
 header+=['};']
 for name,key in [('vrBodyFreeWrist','freeWrists'),('vrBodyFreeWristWidth','freeWristWidths')]:header.append('static const float '+name+'[2][17][3]={'+','.join('{'+','.join(floats(p) for p in side)+'}' for side in report[key])+'};')
 header+=['#endif','']
 (ROOT/'src/body_profiles.h').write_text('\n'.join(header))
 entries['sof2vr-body-prototype.json']=(json.dumps(report,indent=2)+'\n').encode()
 return entries,report

if __name__=='__main__':
 parser=argparse.ArgumentParser();parser.add_argument('--publish',action='store_true');args=parser.parse_args()
 pack=ROOT/'resources/z_sof2vr_native.pk3'
 with zipfile.ZipFile(pack) as z:existing={n:z.read(n) for n in z.namelist()}
 entries,report=build(existing);(ROOT/'build/body-asset-report.json').write_text(json.dumps(report,indent=2)+'\n')
 if args.publish:
  existing.update(entries);temporary=pack.with_suffix('.pk3.tmp')
  with zipfile.ZipFile(temporary,'w',zipfile.ZIP_DEFLATED) as z:
   for n,d in sorted(existing.items()):
    info=zipfile.ZipInfo(n,date_time=(2002,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,d)
  temporary.replace(pack)
 else:
  with zipfile.ZipFile(ROOT/'build/body-prototype.pk3','w',zipfile.ZIP_DEFLATED) as z:
   for n,d in entries.items():z.writestr(n,d)
 print('Prepared',len(entries),'native torso/arm assets; seven outfit profiles, 25 weapon wrist anchors. RTCW meshes were not read.')
