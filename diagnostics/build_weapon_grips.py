"""Explicit original hand poses and full palm transforms for every view model."""
import json,struct,math
from pathlib import Path
from native_pose import read,pose,bolt,vertices,ROOT as HAND_ROOT
from build_reload_assets import compose
from build_presentation import idle
from build_weapon_profiles import parse,value
from build_hand_assets import make_md3
ROOT=Path(__file__).resolve().parents[1]

def unit(v):
 n=math.sqrt(sum(x*x for x in v));assert n>.001
 return [x/n for x in v]

def rotation(m):
 x=unit([m[i][0] for i in range(3)]);raw=[m[i][1] for i in range(3)];dot=sum(a*b for a,b in zip(x,raw))
 y=unit([raw[i]-dot*x[i] for i in range(3)]);z=[x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]]
 return [[x[i],y[i],z[i]] for i in range(3)]

def palm_points(data,world):
 from close_glove_wrist import WRIST_PLANE
 points=[]
 for i in range(3):
  vs,_=vertices(data,i,world)
  points.extend(v[0] for v in vs if v[3].get(3,0)>.5 and v[4][1]<=WRIST_PLANE)
 assert points
 return [(min(p[k] for p in points)+max(p[k] for p in points))*.5 for k in range(3)]

def hand_mesh(data,evaluated,side):
 points=[];normals=[];uv=[];triangles=[];binds=[]
 for i in range(3):
  vs,tris=vertices(data,i,evaluated[0]);base=len(points)
  for point,normal,tex,weights,bind in vs:points.append(point);normals.append(normal);uv.append(tex);binds.append(bind)
  triangles.extend(tuple(base+v for v in tri) for tri in tris)
 from close_glove_wrist import close_wrist
 points,normals,uv,triangles=close_wrist(points,normals,uv,triangles,binds)
 return make_md3(points,normals,uv,triangles,'models/hands/hands',side)

def generate(with_assets=False):
 inview=parse(read('inview/SOF2.inview').decode('cp1252'));weapons={value(b,'name').lower():b for k,b in inview if k=='weapon'}
 rows=json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons'];out=[];entries={};handSources={};free=[]
 for side in ['r','l']:
  path=f'skeletons/weapons/{side}hand/{side}hand';data=read(f'models/weapons/{side}hand/{side}hand.glm');g=read(path+'.gla')
  frames={k.replace('\\','/').split('/')[-1].lower():int(value(b,'startframe','0')) for k,b in parse(read(path+'.frames').decode('cp1252')) if isinstance(b,list)}
  reference=pose(g,0);index=reference[2].index(side+'_metacarple');canonical=rotation(compose(HAND_ROOT,reference[1][index]))
  free.append(dict(side=side,palm=palm_points(data,[HAND_ROOT for _ in reference[2]])))
  handSources[side]=(data,g,frames,canonical)
 for row in rows:
  definition=weapons.get(row['name'].lower());rest=idle(row['animations']['sequences'],Path(row['viewModel']).stem)
  if not definition or not rest:raise ValueError('Missing authored weapon idle: '+row['name'])
  model=value(definition,'weaponmodel',[]);buffer=value(model,'buffer',[]);attachments=value(model,'hands',[])
  bufferModel=value(buffer,'model');parent=value(buffer,'bolttobone')
  if not bufferModel or not parent:raise ValueError('Missing native attachment buffer: '+row['name'])
  weaponPose=pose(read(row['skeleton']['path']),rest['startFrame'])
  bufferData=read(bufferModel);skeleton=struct.unpack_from('<4si64s64s7i',bufferData)[3].split(b'\0')[0].decode()+'.gla'
  bufferPose=pose(read(skeleton),0,bolt(weaponPose,parent));record={}
  for side,label in [('r','right'),('l','left')]:
   data,g,frames,canonical=handSources[side]
   attachment=value(value(attachments,label,[]),'bolttobone');explicit=bool(attachment)
   if not attachment:attachment=side+'hand_'+Path(row['viewModel']).stem
   handPose=None
   for k,group in definition:
    if k=='anim' and isinstance(group,list) and value(group,'name')=='idle':
     for key,info in group:
      if key=='info' and isinstance(info,list) and value(info,'type')=='hands' and value(info,'name')==label and value(info,'anim'):handPose=value(info,'anim')+'.xsi'
   frameOverride=None
   if not handPose and side=='r' and row['id']==8:
    # The M3 retains the LAST rm3a1ready pose in native idle. It has no
    # rm3a1idle sequence; borrowing rm4idle changes its grip alignment.
    handPose='rm3a1ready.xsi'
    ready=next(b for k,b in parse(read('skeletons/weapons/rhand/rhand.frames').decode('cp1252')) if k.replace('\\','/').split('/')[-1].lower()==handPose)
    frameOverride=int(value(ready,'startframe'))+int(value(ready,'duration'))-1
   if not handPose:
    matches=[side+rest['name'],side+Path(row['viewModel']).stem+'idle.xsi']
    handPose=next((name for name in matches if name.lower() in frames),side+'m4idle.xsi')
   if handPose.lower() not in frames:raise ValueError('Missing native hand pose '+handPose)
   if attachment.lower() not in [n.lower() for n in bufferPose[2]]:
    if side=='r':raise ValueError('Missing dominant hand bolt for '+row['name'])
    record[label]=None;continue
   from native_hand_size import evaluated_size
   evaluated=evaluated_size(pose(g,frames[handPose.lower()] if frameOverride is None else frameOverride,bolt(bufferPose,attachment)),side);anchor=palm_points(data,evaluated[0]);palm=rotation(bolt(evaluated,side+'_metacarple'))
   # Canonical palm * authored palm inverse supplies a full rotation.
   correction=[[sum(canonical[i][k]*palm[j][k] for k in range(3)) for j in range(3)] for i in range(3)]
   basis=[[correction[i][j] for i in range(3)] for j in range(3)]
   if not all(math.isfinite(v) and abs(v)<128 for v in anchor):raise ValueError('Implausible palm '+row['name'])
   path=f"models/sof2vr_grips/w{row['id']}_{side}.md3"
   if with_assets:entries[path]=hand_mesh(data,evaluated,side)
   record[label]=dict(anchor=anchor,basis=basis,handPose=handPose,attachment=attachment,explicitAttachment=explicit,frame=frames[handPose.lower()] if frameOverride is None else frameOverride,model=path)
  right=record['right'];left=record['left']
  out.append(dict(id=row['id'],name=row['name'],weaponPose=rest['name'],anchor=right['anchor'],basis=right['basis'],hands=record,supportAuthored=bool(left and left['explicitAttachment']),method='explicit native pose and full controller palm frame; no M4 exception'))
 header=['/* Full native palm transforms and explicit posed hand meshes. */','#ifndef SOF2_WEAPON_GRIPS_H','#define SOF2_WEAPON_GRIPS_H','typedef struct {int id;float point[3],basis[3][3],support[3];const char *rightModel,*leftModel;int supportAuthored;} VRWeaponGrip;','static const VRWeaponGrip vrWeaponGrips[]={']
 # Use a native CLOSED weapon grip as the angular reference. The bind hand
 # is open/flat and would roll the entire gun about 77 degrees. The M4's
 # original closed palm defines neutral aim orientation, not its old anchor.
 neutral=next(row for row in out if row['id']==9)
 references={side:[axis[:] for axis in neutral['hands'][side]['basis']] for side in ['right','left']}
 for row in out:
  for side in ['right','left']:
   hand=row['hands'][side]
   if not hand:continue
   old=hand['basis'];ref=references[side]
   hand['basis']=[[sum(ref[i][k]*old[j][k] for k in range(3)) for i in range(3)] for j in range(3)]
  row['basis']=row['hands']['right']['basis'];row['angularReference']='authored closed M4 palm; controller aim frame'
 # Remove the flatscreen model's bore cant from the tracked AIM frame. A
 # shared reticle/shot ray alone does not fix a gun that points ten degrees
 # sideways relative to the controller. Regenerate ORIGINAL muzzle data so
 # repeated builds never calibrate from already-retargeted replacement data.
 from build_weapon_muzzles import generate as native_muzzles
 muzzles={m['id']:m for m in native_muzzles()}
 for row in out:
  muzzle=muzzles.get(row['id'])
  if not muzzle:continue
  forward=unit(muzzle['direction']);up=[axis[2] for axis in row['basis']]
  cross=lambda a,b:[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
  left=unit(cross(up,forward));up=unit(cross(forward,left))
  row['basis']=[[forward[k],left[k],up[k]] for k in range(3)]
  row['hands']['right']['basis']=row['basis']
  row['angularReference']='native authored bore aligned to tracked controller aim; closed native palm position retained'
 floats=lambda v:','.join('%.8ff'%x for x in v)
 for r in out:
  left=r['hands']['left'];header.append(' {%d,{%s},{%s},{%s},"%s","%s",%d},'%(r['id'],floats(r['anchor']),','.join('{'+floats(axis)+'}' for axis in r['basis']),floats(left['anchor'] if left else r['anchor']),r['hands']['right']['model'],left['model'] if left else '',r['supportAuthored']))
 header+=['};','static inline const VRWeaponGrip *WeaponGrip(int id){for(unsigned i=0;i<sizeof(vrWeaponGrips)/sizeof(vrWeaponGrips[0]);++i)if(vrWeaponGrips[i].id==id)return &vrWeaponGrips[i];return 0;}','/* Physical left, right free-hand bind palm centres. */','static const float vrFreeHandPalm[2][3]={{'+floats(free[1]['palm'])+'},{'+floats(free[0]['palm'])+'}};','#endif','']
 (ROOT/'src/weapon_grips.h').write_text('\n'.join(header));(ROOT/'build/weapon-grip-calibration.json').write_text(json.dumps(out,indent=2)+'\n')
 entries['sof2vr-grip-poses.json']=(json.dumps(out,indent=2)+'\n').encode()
 return (entries,out) if with_assets else out

if __name__=='__main__':print('Generated',len(generate()),'full native palm transforms; M4 recalibrated.')
