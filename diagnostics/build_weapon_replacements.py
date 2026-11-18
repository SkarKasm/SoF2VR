"""Retarget licensed source weapons to native Ghoul2 skeletons and reload parts.

Original surface indices and skeletons remain intact. Added surfaces are rigidly
weighted to the native gun, magazine and action bones in the authored idle pose.
Source albedo/UVs are retained; this legacy renderer does not implement PBR.
"""
from pathlib import Path
import gzip,json,struct,math,io,re,os
from native_pose import read,pose,vertices
from build_presentation import idle
from build_magazine_assets import multipart
ROOT=Path(__file__).resolve().parents[1]
STAGE=ROOT/'build/replacement-intake'

def dot(a,b):return sum(x*y for x,y in zip(a,b))
def sub(a,b):return [x-y for x,y in zip(a,b)]
def cross(a,b):return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
def unit(a):
 n=math.sqrt(dot(a,a))
 if n<1e-14:raise ValueError('Degenerate direction')
 return [v/n for v in a]
def frame(forward,up):
 forward=unit(forward);left=unit(cross(up,forward));return [forward,left,unit(cross(forward,left))]
def centre(points):return [(min(p[k] for p in points)+max(p[k] for p in points))*.5 for k in range(3)]

def inverse(m):
 # Native evaluated transforms are rotations, with minor quaternion rounding.
 a,b,c=[r[:3] for r in m];rows=[cross(b,c),cross(c,a),cross(a,b)];det=dot(a,rows[0])
 r=[[rows[j][i]/det for j in range(3)] for i in range(3)]
 return [row+[-sum(row[j]*m[j][3] for j in range(3))] for row in r]
def apply(m,p,normal=False):return [dot(row[:3],p)+(0 if normal else row[3]) for row in m]

def fitted(w,row,data,world,grips,muzzles):
 source=frame(w['forward'],w['up'])
 if 'bore' in w:
  points=[v[0] for i,s in enumerate(row['surfaces']) if not s['name'].startswith('blur') for v in vertices(data,i,world)[0]]
  muzzle=muzzles.get(w['id'])
  if muzzle:
   # Native viewmodels can be authored sideways around the bore. Their palm
   # correction removes that roll at runtime. Fit the source's upright axis
   # through the same correction rather than assuming native Z is upright.
   # basis rows express each native model axis in controller coordinates.
   upright=[axis[2] for axis in grips[w['id']]['basis']]
   target=frame(muzzle['direction'],upright);dst=muzzle['anchor']
  else:
   palm=grips[w['id']]['anchor'];far=max(points,key=lambda p:dot(sub(p,palm),sub(p,palm)));target=frame(sub(far,palm),[0,0,1]);dst=far
  with gzip.open(STAGE/w['asset']/'selected-meshes.json.gz','rt') as f:mesh=json.load(f)
  longitudinal=[dot(v[:3],source[0]) for g in mesh['groups'] for v in g['vertices']]
  scale=w['targetLengthMetres']*39.37007874/(max(longitudinal)-min(longitudinal))
  if w.get('scalePolicy')=='native-weapon-length':
   excluded=w.get('nativeLengthExcludedSurfaces',[])
   native=[v[0] for i,s in enumerate(row['surfaces']) if not any(s['name'].startswith(prefix) for prefix in excluded) for v in vertices(data,i,world)[0]]
   length=max(dot(p,target[0]) for p in native)-min(dot(p,target[0]) for p in native)
   scale=length/(max(longitudinal)-min(longitudinal))
  scale*=w.get('gameScaleFactor',1.)
  scales=[scale]*3
  # Place the source palm at the authored SoF2 palm, preserving proportions.
  # The muzzle follows the new mesh, rather than stretching a replacement to
  # a flatscreen weapon's exaggerated barrel position.
  src=w['palm'];dst=grips[w['id']]['anchor']
 else:
  points=[v[0] for i in range(len(row['surfaces'])) for v in vertices(data,i,world)[0]];dst=centre(points)
  with gzip.open(STAGE/w['asset']/'selected-meshes.json.gz','rt') as f:mesh=json.load(f)
  sp=[v[:3] for g in mesh['groups'] for v in g['vertices']];src=centre(sp);target=frame([1,0,0],[0,0,1])
  scale=max(max(p[k] for p in points)-min(p[k] for p in points) for k in range(3))/max(max(p[k] for p in sp)-min(p[k] for p in sp) for k in range(3));scales=[scale]*3
 matrix=[[sum(target[k][i]*scales[k]*source[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
 transform=[r+[dst[i]-dot(r,src)] for i,r in enumerate(matrix)]
 inv=inverse(transform);normal=[[inv[j][i] for j in range(3)]+[0] for i in range(3)]
 report=dict(sourceOrigin=src,targetOrigin=dst,scales=scales,matrix=transform)
 if 'palm' in w:
  report['palmErrorGameUnits']=math.sqrt(sum(v*v for v in sub(apply(transform,w['palm']),grips[w['id']]['anchor'])))
 return transform,normal,report

def chunks(group):
 result=[];mapping={};points=[];normals=[];uv=[];tris=[]
 for tri in group[3]:
  keys=[tuple(round(v,6) for v in (*group[0][i],*group[1][i],*group[2][i])) for i in tri]
  if tris and (len(mapping)+sum(k not in mapping for k in keys)>900 or len(tris)>=1700):
   result.append((points,normals,uv,tris));mapping={};points=[];normals=[];uv=[];tris=[]
  out=[]
  for i,key in zip(tri,keys):
   if key not in mapping:mapping[key]=len(points);points.append(group[0][i]);normals.append(group[1][i]);uv.append(group[2][i])
   out.append(mapping[key])
  tris.append(out)
 if tris:result.append((points,normals,uv,tris))
 return result

def glm(original,surfaces,world):
 h=list(struct.unpack_from('<4si64s64s7i',original));oldCount=h[8];count=oldCount+len(surfaces)
 if count>64:raise ValueError(f'Replacement exceeds conservative Ghoul2 surface budget: {count}')
 hierarchy=[]
 for i in range(oldCount):
  p=164+struct.unpack_from('<i',original,164+i*4)[0];children=struct.unpack_from('<i',original,p+140)[0]
  record=bytearray(original[p:p+144+4*children])
  if i==0:
   struct.pack_into('<i',record,140,children+len(surfaces));record+=struct.pack('<'+'i'*len(surfaces),*range(oldCount,count))
  hierarchy.append(record)
 for i,(part,shader,bone,geometry) in enumerate(surfaces):
  hierarchy.append(struct.pack('<64sI64s3i',f'vr_{part}_{i}'.encode(),0,shader.encode(),0,0,0))
 offsets=[];at=4*count
 for record in hierarchy:offsets.append(at);at+=len(record)
 tree=struct.pack('<'+'i'*count,*offsets)+b''.join(hierarchy);lodStart=164+len(tree)
 body=[];offsets=[];at=4+count*4
 for i in range(oldCount):
  p=h[7]+4+struct.unpack_from('<i',original,h[7]+4+i*4)[0];sh=struct.unpack_from('<10i',original,p);record=bytearray(original[p:p+sh[9]])
  struct.pack_into('<i',record,8,-(lodStart+at));struct.pack_into('<i',record,20,0)
  offsets.append(at-4);body.append(record);at+=len(record)
 for n,(part,shader,bone,(points,normals,uv,tris)) in enumerate(surfaces):
  inv=inverse(world[bone]);td=b''.join(struct.pack('<3i',*t) for t in tris);vd=bytearray();st=b''.join(struct.pack('<2f',*v) for v in uv)
  for point,normal in zip(points,normals):
   xyz=apply(inv,point);normal=unit(apply(inv,normal,True));vd+=struct.pack('<6fI4B',*normal,*xyz,0,0,0,0,0)
  vertexOffset=40+len(td);refsOffset=vertexOffset+len(vd)+len(st);end=refsOffset+4
  header=struct.pack('<10i',0,oldCount+n,-(lodStart+at),len(points),vertexOffset,len(tris),40,1,refsOffset,end)
  record=header+td+vd+st+struct.pack('<i',bone);offsets.append(at-4);body.append(record);at+=len(record)
 lod=struct.pack('<i',at)+struct.pack('<'+'i'*count,*offsets)+b''.join(body)
 h[6]=1;h[7]=lodStart;h[8]=count;h[9]=164+4*count;h[10]=lodStart+len(lod)
 return struct.pack('<4si64s64s7i',*h)+tree+lod

def material_image(folder,material):
 if material['file']:
  return (folder/material['prepared']).read_bytes(),material['size'],'.png'
 else:
  def srgb(v):return round(255*(12.92*v if v<=.0031308 else 1.055*v**(1/2.4)-.055))
  colour=bytes(srgb(max(0,min(1,c))) for c in reversed(material['colour'][:3]))
  return struct.pack('<BBBHHBHHHHBB',0,0,2,0,0,0,0,0,16,16,24,0)+colour*256,[16,16],'.tga'

def translated_hand(data,delta):
 """Translate an original posed SoF2 hand to a replacement support grip."""
 data=bytearray(data);h=struct.unpack_from('<4si64s9i',data);at=h[10];points=[]
 if h[4]!=1:raise ValueError('Expected single authored grip pose')
 for _ in range(h[6]):
  sh=struct.unpack_from('<4s64s10i',data,at)
  for i in range(sh[5]):
   p=at+sh[10]+i*8;xyz=struct.unpack_from('<3h',data,p);moved=[round(x+64*d) for x,d in zip(xyz,delta)]
   if any(abs(v)>32767 for v in moved):raise ValueError('Hand translation exceeds MD3 range')
   struct.pack_into('<3h',data,p,*moved);points.append([v/64 for v in moved])
  at+=sh[11]
 low=[min(p[k] for p in points) for k in range(3)];high=[max(p[k] for p in points) for k in range(3)]
 struct.pack_into('<10f',data,h[8],*low,*high,0,0,0,max(math.sqrt(dot(p,p)) for p in points))
 return bytes(data)

def relocate_m3_flash(entries,row,rest,target):
 """Move only the rifle buffer's M3 flash tag to the replacement bore.

 The rifle buffer is shared, so its other weapon/hand tags remain untouched.
 """
 from build_weapon_profiles import parse,value
 from native_pose import bolt
 definition=next(b for k,b in parse(read('inview/SOF2.inview').decode('cp1252')) if k=='weapon' and value(b,'name').lower()==row['name'].lower())
 buffer=value(value(definition,'weaponmodel',[]),'buffer',[])
 bd=read(value(buffer,'model'));path=struct.unpack_from('<4si64s64s7i',bd)[3].split(b'\0')[0].decode()+'.gla'
 g=bytearray(entries.get(path,read(path)))
 wp=pose(read(row['skeleton']['path']),rest['startFrame'])
 bp=pose(g,0,bolt(wp,value(buffer,'bolttobone')));tag=value(buffer,'muzzle');index=bp[2].index(tag)
 at=100+struct.unpack_from('<i',g,100+4*index)[0]
 local=apply(inverse(bp[0][index]),target)
 base=[list(struct.unpack_from('<4f',g,at+72+i*16)) for i in range(3)]
 for i in range(3):base[i][3]=local[i]
 inv=inverse(base)
 for i in range(3):
  struct.pack_into('<4f',g,at+72+i*16,*base[i]);struct.pack_into('<4f',g,at+120+i*16,*inv[i])
 entries[path]=bytes(g)
 return dict(skeleton=path,tag=tag,target=target,method='replacement bore, shared buffer leaf tag only')

def build(entries):
 config=json.loads((ROOT/'resources/weapon_replacements.json').read_text())['weapons']
 if not (STAGE/config[0]['asset']/'selected-meshes.json.gz').exists():
  (ROOT/'src/replacement_models.h').write_text('#ifndef SOF2_REPLACEMENT_MODELS_H\n#define SOF2_REPLACEMENT_MODELS_H\nstatic int VRReplacementWeapon(int id){(void)id;return 0;}\n#endif\n')
  return {'ready':[],'status':'Source exports not prepared; original weapon assets retained'}
 rows={w['id']:w for w in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons']}
 grips={w['id']:w for w in json.loads((ROOT/'build/weapon-grip-calibration.json').read_text())};muzzles={w['id']:w for w in json.loads((ROOT/'build/weapon-muzzle-calibration.json').read_text())}
 from fit_weapon_hands import HandFitter
 handFitter=HandFitter(rows,grips)
 report=[];shaders=[];reloads=json.loads(entries['sof2vr-magazine-parts.json'])
 for w in config:
  authoring=os.environ.get('SOF2_VR_AUTHORING_WEAPONS','')
  if authoring and w['id'] not in [int(v) for v in authoring.split(',')]:continue
  row=rows[w['id']];folder=STAGE/w['asset']
  with gzip.open(folder/'selected-meshes.json.gz','rt') as source:model=json.load(source)
  if w.get('deriveForegrip'):
   # Reference XM29 grip: reuse this licensed source's textured pistol-grip
   # surfaces, rotated upright under its fore-end. No unrelated artwork.
   sourceGrip=w['gripCentre'];destination=w['supportCentre'];angle=math.atan2(w['gripAxis'][1],w['gripAxis'][2]);c=math.cos(angle);s=math.sin(angle)
   for group in list(model['groups']):
    if group['material']!='ar':continue
    selected=[t for t in group['triangles'] if all(group['vertices'][i][1]<-.025 and group['vertices'][i][2]<-.025 for i in t)]
    mapping={};verts=[];tris=[]
    for tri in selected:
     output=[]
     for i in tri:
      if i not in mapping:
       v=group['vertices'][i];p=sub(v[:3],sourceGrip);n=v[3:6]
       moved=[p[0],c*p[1]-s*p[2],s*p[1]+c*p[2]];normal=[n[0],c*n[1]-s*n[2],s*n[1]+c*n[2]]
       mapping[i]=len(verts);verts.append([moved[k]+destination[k] for k in range(3)]+normal+v[6:8])
      output.append(mapping[i])
     tris.append(output)
    if not tris:raise ValueError('Missing licensed OICW grip donor geometry')
    model['groups'].append(dict(part='body',material='ar',vertices=verts,triangles=tris))
  for group in model['groups']:
   offset=w.get('partOffsets',{}).get(group['part'])
   if offset:
    for v in group['vertices']:v[:3]=[v[k]+offset[k] for k in range(3)]
  rest=idle(row['animations']['sequences'],Path(row['viewModel']).stem);world=pose(read(row['skeleton']['path']),rest['startFrame'])[0];original=entries.get(row['viewModel'],read(row['viewModel']))
  transform,normalTransform,calibration=fitted(w,row,read(row['viewModel']),world,grips,muzzles);groups={};materials={};materialsReport=[]
  if 'gripCentre' in w:transform=handFitter.fit(w,model,transform,calibration,grips[w['id']],entries)
  for index,(name,mat) in enumerate(model['materials'].items()):
   shader=f"sof2vr_ext/w{w['id']}/m{index}";image,size,extension=material_image(folder,mat);path=shader+extension;entries[path]=image;materials[name]=shader
   shaders.append(shader+'\n{\n cull disable\n {\n map '+path+'\n rgbGen lightingDiffuse\n }\n}\n')
   materialsReport.append(dict(material=name,shader=shader,albedo=mat['file'],size=size,constantColour=mat['file'] is None))
  for group in model['groups']:
   key=(group['part'],materials[group['material']]);out=groups.setdefault(key,[[],[],[],[]])
   for tri in group['triangles']:
    vs=[group['vertices'][i] for i in tri];face=cross(sub(vs[1][:3],vs[0][:3]),sub(vs[2][:3],vs[0][:3]))
    if dot(face,face)<1e-20:continue
    base=len(out[0])
    for v in vs:
     normal=v[3:6] if dot(v[3:6],v[3:6])>1e-12 else unit(face)
     out[0].append(apply(transform,v[:3]));out[1].append(unit(apply(normalTransform,normal,True)));out[2].append(v[6:8])
    out[3].append([base,base+1,base+2])
  groups={key:g for key,g in groups.items() if g[3]}
  names={b['name'].lower():b['index'] for b in row['bones']};surfaces=[]
  for (part,shader),group in groups.items():
   bone=names.get({'magazine':'clip1','launcher_magazine':'clip2','bolt':'bolt1','pump':'bolt1','pin':'clip1'}.get(part,'gun'),names.get('gun',names.get('handle',1)))
   for geometry in chunks(group):surfaces.append((part,shader,bone,geometry))
  entries[row['viewModel']]=glm(original,surfaces,world)
  parts={}
  for (part,shader),group in groups.items():parts.setdefault(part,{})[shader]=group
  sockets={}
  for part in ['magazine','bolt','pump']:
   if part in parts:sockets[part]=centre([p for g in parts[part].values() for p in g[0]])
  reload=next((r for r in reloads['ready'] if r['id']==w['id']),None)
  if reload:
   for part in ['body','magazine','bolt']:
    selected={shader:[list(values) for values in group] for shader,group in parts.get(part,{}).items()}
    if part=='body':
     for other,subgroups in parts.items():
      if other not in ['body','magazine','bolt']:
       for shader,group in subgroups.items():
        out=selected.setdefault(shader,[[],[],[],[]]);offset=len(out[0])
        for index in range(3):out[index].extend(group[index])
        out[3].extend([[v+offset for v in triangle] for triangle in group[3]])
    if not selected:continue # Keep the native small action when absent in source.
    anchor=sockets.get('magazine',[0,0,0]) if part=='magazine' else [0,0,0]
    entries[reload['prefix']+'_'+part+'.md3']=multipart(selected,anchor)
   if 'magazine' in sockets:reload['socket']=sockets['magazine']
   if 'bolt' in sockets:reload['bolt']=sockets['bolt']
   reload['replacementAsset']=w['asset']
   if w['id']==9:
    legacy=json.loads(entries['sof2vr-reload-parts.json']);legacy.update(replacementAsset=w['asset'],socket=reload['socket'],boltSocket=reload['bolt'],gripSocket=grips[w['id']]['anchor'],muzzleSocket=apply(transform,w['bore']))
    entries['sof2vr-reload-parts.json']=(json.dumps(legacy,indent=2)+'\n').encode()
    path=ROOT/'src/reload_parts.h';text=path.read_text()
    for name,point in [('m4MagazineSocket',reload['socket']),('m4BoltSocket',reload['bolt']),('m4GripSocket',grips[w['id']]['anchor']),('m4MuzzleSocket',apply(transform,w['bore']))]:
     text=re.sub(r'(static const float '+name+r'\[3\]=)\{[^}]+\}',lambda m:m[1]+'{'+','.join('%.7ff'%v for v in point)+'}',text)
    path.write_text(text)
  if w['id']==6:
   for part in ['body','pump']:entries[f'models/sof2vr_loading/m590_{part}.md3']=multipart(parts[part],[0,0,0])
   # Loading port follows the replacement receiver; pump uses its actual centre.
   port=apply(transform,[0,-.65,.24]);pump=sockets['pump']
   (ROOT/'src/loading_parts.h').write_text('/* Replacement M590 receiver and pump anchors. */\n#ifndef SOF2_LOADING_PARTS_H\n#define SOF2_LOADING_PARTS_H\n'+''.join('static const float '+name+'[3]={'+','.join('%.8ff'%x for x in point)+'};\n' for name,point in [('m590LoadPort',port),('m590PumpSocket',pump)])+'#define M590_PUMP_TRAVEL 3.5433071f\n#endif\n')
   entries['sof2vr-shotgun-loading.json']=(json.dumps(dict(weapon=6,asset=w['asset'],port=port,pump=pump,pumpTravelCentimetres=9,status='source-built; physical evaluation pending'),indent=2)+'\n').encode()
  if 'supportPalm' in w and 'gripCentre' not in w and grips[w['id']]['hands']['left']:
   hand=grips[w['id']]['hands']['left'];support=apply(transform,w['supportPalm']);delta=sub(support,hand['anchor'])
   entries[hand['model']]=translated_hand(entries[hand['model']],delta);hand['anchor']=support;hand['replacementAsset']=w['asset'];calibration['supportPalm']=support;calibration['supportTranslation']=delta
  report.append(dict(id=w['id'],name=row['name'],asset=w['asset'],model=row['viewModel'],triangles=sum(len(g[3]) for g in groups.values()),addedSurfaces=len(surfaces),parts=list(parts),sockets=sockets,calibration=calibration,materials=materialsReport,geometryCorrections=w.get('geometryCorrections',[]),actionFallback='native action geometry' if reload and 'bolt' not in parts else None))
  if w['id'] in muzzles:
   muzzles[w['id']]['anchor']=apply(transform,w['bore']);muzzles[w['id']]['replacementAsset']=w['asset'];muzzles[w['id']]['directionMethod']='Replacement source bore transformed with the same proportion-preserving transform as rendered geometry'
  if w['id']==8:report[-1]['muzzleFlashAttachment']=relocate_m3_flash(entries,row,rest,apply(transform,w['bore']))
  print('RETARGETED',w['id'],row['name'],report[-1]['triangles'],'triangles;',len(surfaces),'added surfaces',flush=True)
 entries['shaders/weapons.shader']+=('\n'+''.join(shaders)).encode('ascii')
 handFitter.remaining(grips,entries)
 from manual_hand_pose import prepare_baseline,apply_overrides
 prepare_baseline(handFitter,grips,entries,rows)
 apply_overrides(handFitter,grips,entries,rows,report,muzzles,reloads)
 # The replacement M4 already contains its M203. Keep the original bolton's
 # skeleton/attachment metadata but remove its duplicate visible geometry.
 if any(w['id']==9 for w in config):
  path='models/weapons/m203/m203.glm';d=bytearray(entries.get(path,read(path)));h=struct.unpack_from('<4si64s64s7i',d);lod=h[7]
  for _ in range(h[6]):
   end=struct.unpack_from('<i',d,lod)[0]
   for i in range(h[8]):
    at=lod+4+struct.unpack_from('<i',d,lod+4+i*4)[0];struct.pack_into('<i',d,at+20,0)
   lod+=end
  entries[path]=bytes(d)
 muzzleHeader=['/* Shared rendered replacement/native bore origins and directions. */','#ifndef SOF2_WEAPON_MUZZLES_H','#define SOF2_WEAPON_MUZZLES_H','typedef struct {int id;float point[3],direction[3];} VRWeaponMuzzle;','static const VRWeaponMuzzle vrWeaponMuzzles[]={']
 for r in muzzles.values():muzzleHeader.append(' {%d,{%s},{%s}},'%(r['id'],','.join('%.8ff'%v for v in r['anchor']),','.join('%.8ff'%v for v in r['direction'])))
 muzzleHeader+=['};','#endif',''];(ROOT/'src/weapon_muzzles.h').write_text('\n'.join(muzzleHeader));(ROOT/'build/weapon-muzzle-calibration.json').write_text(json.dumps(list(muzzles.values()),indent=2)+'\n')
 header=['/* Generated native/replacement detachable part anchors. */','#ifndef SOF2_MAGAZINE_PARTS_H','#define SOF2_MAGAZINE_PARTS_H','typedef struct {int id;const char *model,*prefix;float socket[3],bolt[3];int count,surfaces[16];} VRReloadParts;','static const VRReloadParts vrReloadParts[]={']
 for r in reloads['ready']:header.append(' {%d,"%s","%s",{%s},{%s},%d,{%s}},'%(r['id'],r['model'],r['prefix'],','.join('%.7ff'%v for v in r['socket']),','.join('%.7ff'%v for v in r['bolt']),len(r['magSurfaces']),','.join(map(str,r['magSurfaces']))))
 header+=['};','static inline const VRReloadParts *ReloadParts(int id){for(unsigned i=0;i<sizeof(vrReloadParts)/sizeof(vrReloadParts[0]);++i)if(vrReloadParts[i].id==id)return &vrReloadParts[i];return 0;}','#endif',''];(ROOT/'src/magazine_parts.h').write_text('\n'.join(header))
 entries['sof2vr-magazine-parts.json']=(json.dumps(reloads,indent=2)+'\n').encode()
 gripHeaderPath=ROOT/'src/weapon_grips.h';footer='/* Physical left'+gripHeaderPath.read_text().split('/* Physical left',1)[1]
 floats=lambda values:','.join('%.8ff'%v for v in values)
 gripHeader=['/* Original SoF2 hand poses with replacement support locations. */','#ifndef SOF2_WEAPON_GRIPS_H','#define SOF2_WEAPON_GRIPS_H','typedef struct {int id;float point[3],basis[3][3],support[3];const char *rightModel,*leftModel;int supportAuthored;} VRWeaponGrip;','static const VRWeaponGrip vrWeaponGrips[]={']
 for r in grips.values():
  left=r['hands']['left'];gripHeader.append(' {%d,{%s},{%s},{%s},"%s","%s",%d},'%(r['id'],floats(r['anchor']),','.join('{'+floats(axis)+'}' for axis in r['basis']),floats(left['anchor'] if left else r['anchor']),r['hands']['right']['model'],left['model'] if left else '',r['supportAuthored']))
 gripHeader+=['};','static inline const VRWeaponGrip *WeaponGrip(int id){for(unsigned i=0;i<sizeof(vrWeaponGrips)/sizeof(vrWeaponGrips[0]);++i)if(vrWeaponGrips[i].id==id)return &vrWeaponGrips[i];return 0;}',footer]
 gripHeaderPath.write_text('\n'.join(gripHeader));(ROOT/'build/weapon-grip-calibration.json').write_text(json.dumps(list(grips.values()),indent=2)+'\n');entries['sof2vr-grip-poses.json']=(json.dumps(list(grips.values()),indent=2)+'\n').encode()
 metadata=dict(ready=report,scope='First-person viewmodels and corresponding VR reload pieces; native world/dropped models retained',evaluation='Built from source; no game/headset tests performed',renderer='Original albedo/UVs with legacy diffuse lighting, not a PBR renderer')
 if os.environ.get('SOF2_VR_AUTHORING_WEAPONS'):
  metadata['authoringPreviewOnly']=True;entries['sof2vr-authoring-preview.txt']=b'Offline partial geometry preview. Do not install.\n'
 from build_weapon_effects import build as effect_locators
 metadata['effectLocators']=effect_locators(entries,report,config,rows)
 (ROOT/'build/weapon-replacement-build.json').write_text(json.dumps(metadata,indent=2)+'\n')
 entries['sof2vr-weapon-replacements.json']=(json.dumps(metadata,indent=2)+'\n').encode()
 from prepare_weapon_attribution import prepare
 prepare()
 entries['licenses/WEAPON_ASSET_CREDITS.md']=(ROOT/'docs/WEAPON_ASSET_CREDITS.md').read_bytes()
 entries['licenses/weapon_asset_sources.json']=(ROOT/'resources/weapon_asset_sources.json').read_bytes()
 for license in ['CC-BY-4.0.txt','CC-BY-NC-4.0.txt']:entries['licenses/'+license]=(ROOT/'licenses'/license).read_bytes()
 (ROOT/'src/replacement_models.h').write_text('/* Source-built replacement viewmodels. */\n#ifndef SOF2_REPLACEMENT_MODELS_H\n#define SOF2_REPLACEMENT_MODELS_H\nstatic int VRReplacementWeapon(int id){switch(id){'+''.join('case %d:'%w['id'] for w in config)+'return 1;default:return 0;}}\n#endif\n')
 return metadata
