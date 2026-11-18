"""Approved RTCW geometry with UVs transferred onto original SoF2 outfit arms.
User reported author permission on 19 October 2026. No RTCW textures are copied.
Generated derivatives containing SoF2 material references remain local.
"""
from pathlib import Path
import struct,zipfile,math,hashlib,json,re
ROOT=Path(__file__).resolve().parents[1]
OUTFITS={'urban':('mullins_urban','average_sleeves'),'jungle':('mullins_jungle','average_sleeves'),'snow':('mullins_snow','snow'),'suit':('mullins_suit','suit_long_coat'),'noncombat':('mullins_non_combat','suit_long_coat'),'young':('mullins_young','suit_long_coat'),'chem':('chem_mullins','chem_suit')}
def sub(a,b):return tuple(x-y for x,y in zip(a,b))
def add(a,b):return tuple(x+y for x,y in zip(a,b))
def mul(a,n):return tuple(x*n for x in a)
def dot(a,b):return sum(x*y for x,y in zip(a,b))
def unit(a):return mul(a,1/math.sqrt(dot(a,a)))
def bounds(points):return [tuple(f(p[k] for p in points) for k in range(3)) for f in (min,max)]
def glm_surface(data,name):
 h=struct.unpack_from('<4si64s64s7i',data)
 for i in range(h[8]):
  o=164+struct.unpack_from('<i',data,164+4*i)[0]
  if data[o:o+64].split(b'\0')[0].decode()!=name:continue
  o=h[7]+4+struct.unpack_from('<i',data,h[7]+4+i*4)[0];s=struct.unpack_from('<10i',data,o)
  points=[struct.unpack_from('<3f',data,o+s[4]+v*32+12) for v in range(s[3])]
  uv=[struct.unpack_from('<2f',data,o+s[4]+s[3]*32+v*8) for v in range(s[3])]
  tri=[struct.unpack_from('<3i',data,o+s[6]+v*12) for v in range(s[5])]
  return points,uv,tri
 raise ValueError('Missing body hand surface '+name)
def closest(p,a,b,c):
 # Closest point and barycentric weights (including triangle edges).
 ab,ac,ap=sub(b,a),sub(c,a),sub(p,a);d1,d2=dot(ab,ap),dot(ac,ap)
 if d1<=0 and d2<=0:return a,(1,0,0)
 bp=sub(p,b);d3,d4=dot(ab,bp),dot(ac,bp)
 if d3>=0 and d4<=d3:return b,(0,1,0)
 vc=d1*d4-d3*d2
 if vc<=0 and d1>=0 and d3<=0:
  v=d1/(d1-d3);return add(a,mul(ab,v)),(1-v,v,0)
 cp=sub(p,c);d5,d6=dot(ab,cp),dot(ac,cp)
 if d6>=0 and d5<=d6:return c,(0,0,1)
 vb=d5*d2-d1*d6
 if vb<=0 and d2>=0 and d6<=0:
  w=d2/(d2-d6);return add(a,mul(ac,w)),(1-w,0,w)
 va=d3*d6-d5*d4
 if va<=0 and d4-d3>=0 and d5-d6>=0:
  w=(d4-d3)/((d4-d3)+(d5-d6));return add(b,mul(sub(c,b),w)),(0,1-w,w)
 total=va+vb+vc
 if abs(total)<1e-12:return a,(1,0,0)
 v,w=vb/total,vc/total;return add(a,add(mul(ab,v),mul(ac,w))),(1-v-w,v,w)
def source_mesh(data):
 h=struct.unpack_from('<4si64s9i',data);o=h[10];s=struct.unpack_from('<4s64s10i',data,o)
 points=[];normals=[]
 for i in range(s[5]):
  x,y,z,n=struct.unpack_from('<3hH',data,o+s[10]+i*8);points.append((x/64,-z/64,y/64))
  lat=(n>>8)*2*math.pi/255;lon=(n&255)*2*math.pi/255
  normals.append((math.cos(lat)*math.sin(lon),-math.cos(lon),math.sin(lat)*math.sin(lon)))
 triangles=[struct.unpack_from('<3i',data,o+s[7]+i*12) for i in range(s[6])]
 return points,normals,triangles

def make_md3(points,normals,uv,triangles,shader,side):
 batches=[];mapping={};batch=[]
 for tri in triangles:
  if batch and (len(mapping)+sum(v not in mapping for v in tri)>900 or len(batch)>=1700):batches.append((mapping,batch));mapping={};batch=[]
  batch.append(tuple(mapping.setdefault(v,len(mapping)) for v in tri))
 if batch:batches.append((mapping,batch))
 surfaces=[]
 for j,(mapping,tri) in enumerate(batches):
  td=b''.join(struct.pack('<3i',*t) for t in tri);sd=struct.pack('<64si',shader.encode(),0);st=b''.join(struct.pack('<2f',*uv[v]) for v in mapping);xyz=bytearray()
  for v in mapping:
   n=unit(normals[v]);lat=round(math.atan2(n[1],n[0])*255/(2*math.pi))&255;lon=round(math.acos(max(-1,min(1,n[2])))*255/(2*math.pi))&255
   xyz+=struct.pack('<3hH',*(round(c*64) for c in points[v]),(lat<<8)|lon)
  offs=[108,108+len(td),108+len(td)+len(sd),108+len(td)+len(sd)+len(st)];end=offs[-1]+len(xyz)
  surfaces.append(struct.pack('<4s64s10i',b'IDP3',f'approved_{side}_{j}'.encode(),0,1,1,len(mapping),len(tri),*offs,end)+td+sd+st+xyz)
 lo,hi=bounds(points);radius=max(math.sqrt(dot(p,p)) for p in points);frame=struct.pack('<10f16s',*lo,*hi,0,0,0,radius,b'sof2_uv_transfer');body=b''.join(surfaces)
 return struct.pack('<4si64s9i',b'IDP3',15,f'sof2_approved_{side}'.encode(),0,1,0,len(surfaces),0,108,164,164,164+len(body))+frame+body

def arm_mesh(body,side,segment):
 points,uv,triangles=glm_surface(body,'arm_'+('uppr' if segment=='upper' else 'lwr')+'_'+side)
 proximal='cap_arm_uppr_'+side+'_torso_off' if segment=='upper' else 'cap_arm_lwr_'+side+'_uppr_off'
 distal='cap_arm_uppr_'+side+'_lwr_off' if segment=='upper' else 'cap_arm_lwr_'+side+'_hand_off'
 mean=lambda ps:tuple(sum(p[k] for p in ps)/len(ps) for k in range(3))
 proxPoints=glm_surface(body,proximal)[0];distPoints=glm_surface(body,distal)[0]
 start=mean(proxPoints);end=mean(distPoints);bone=sub(end,start);length=math.sqrt(dot(bone,bone));axis=unit(bone);left=unit((axis[2],0,-axis[0]));depth=(0,1,0)
 # Original body seams interleave with adjacent skeletal surfaces. Flatten
 # their cap rings onto the IK joint planes before splitting rigid segments;
 # retaining the authored staggered seam produces projecting spikes.
 def joint_x(p):
  if min(math.dist(p,q) for q in proxPoints)<.001:return 0.
  if min(math.dist(p,q) for q in distPoints)<.001:return length
  return max(0.,min(length,dot(sub(p,start),axis)))
 canonical=[(joint_x(p),dot(sub(p,start),left),dot(sub(p,start),depth)) for p in points]
 normals=[(0.,0.,0.) for p in points]
 for a,b,c in triangles:
  ab,ac=sub(canonical[b],canonical[a]),sub(canonical[c],canonical[a]);normal=(ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0])
  for v in (a,b,c):normals[v]=add(normals[v],normal)
 # Raven model triangles use the opposite winding convention to RTCW MD3.
 normals=[mul(unit(n),-1) if dot(n,n)>1e-10 else (0,0,-1) for n in normals]
 assert 3<length<30
 return (canonical,normals,uv,triangles),length

def approved_hands():
 source=ROOT.parent/'xr-release-0.8.7/Main/z_realrtcw_models_hands_vr.pk3';entries={};metadata={'permission':'User explicitly reported author permission, 2026-10-19','sourceArchive':str(source),'textures':'Original SoF2 outfit arms; no RTCW raster media','poses':{},'outfits':{},'method':'closest-surface barycentric UV transfer in a wrist-aligned body-hand frame; triangle winding normalized per pose to Raven convention'}
 with zipfile.ZipFile(source) as z:
  meshes={}
  for pose,n in [('relaxed','v_offhand.md3'),('grab','v_offhand_grab.md3')]:
   path='models/weapons/vrhands/'+n;d=z.read(path);meshes[pose]=source_mesh(d);metadata['poses'][pose]={'path':path,'sha256':hashlib.sha256(d).hexdigest()}
 srcLo,srcHi=bounds(meshes['relaxed'][0]);modelCache={};armCache={};lengths=[]
 with zipfile.ZipFile(ROOT.parent/'Soldier of Fortune 2/base/skins.pk3') as z:
  for outfit,(skin,model) in OUTFITS.items():
   skinPath=f'models/characters/skins/{skin}.g2skin';skinData=z.read(skinPath);skinText=skinData.decode('cp1252');shader=re.search(r'name\s*"arms".*?(?:texture1|shader1)\s*"([^"]+)"',skinText,re.S)[1]
   bodyPath=f'models/characters/{model}/{model}.glm';body=z.read(bodyPath)
   metadata['outfits'][outfit]={'skin':skinPath,'skinSHA256':hashlib.sha256(skinData).hexdigest(),'bodyModel':bodyPath,'bodySHA256':hashlib.sha256(body).hexdigest(),'material':shader}
   outfitLengths=[];metadata['outfits'][outfit]['armLengths']={}
   for side in 'lr':
    sideLengths=[]
    for segment in ('upper','lower'):
     armKey=(model,side,segment)
     if armKey not in armCache:armCache[armKey]=arm_mesh(body,side,segment)
     geometry,length=armCache[armKey];sideLengths.append(length)
     entries[f'models/sof2vr_licensed/{outfit}/{side}_{segment}.md3']=make_md3(*geometry,shader,side)
    outfitLengths.append(sideLengths);metadata['outfits'][outfit]['armLengths'][side]=sideLengths
    cacheKey=(model,side)
    if cacheKey not in modelCache:
     target,tuv,tri=glm_surface(body,'hand_'+side);arm,auv,atri=glm_surface(body,'arm_lwr_'+side)
     mean=lambda ps:tuple(sum(p[k] for p in ps)/len(ps) for k in range(3))
     forward=unit(sub(mean(target),mean(arm)));lateral=unit((forward[2],0,-forward[0]));depth=(0,1,0)
     orient=lambda p:(dot(p,forward),dot(p,lateral),dot(p,depth))
     projected=[orient(p) for p in target];tlo,thi=bounds(projected)
     allPoints=projected+[orient(p) for p in arm];allUV=tuv+auv;allTri=tri+[tuple(v+len(target) for v in t) for t in atri]
     centers=[mul(add(add(allPoints[a],allPoints[b]),allPoints[c]),1/3) for a,b,c in allTri]
     results={}
     for pose,(points,normals,triangles) in meshes.items():
      outPoints=[];outNormals=[];uv=[]
      # Same calibration for both poses; pose changes do not rescale the hand.
      for point,normal in zip(points,normals):
       canonical=tuple((point[k]-srcLo[k])/(srcHi[k]-srcLo[k]) for k in range(3))
       desired=tuple(tlo[k]+canonical[k]*(thi[k]-tlo[k]) for k in range(3))
       candidates=sorted(range(len(allTri)),key=lambda i:dot(sub(centers[i],desired),sub(centers[i],desired)))[:8]
       best=None
       for i in candidates:
        a,b,c=allTri[i];q,weights=closest(desired,allPoints[a],allPoints[b],allPoints[c]);distance=dot(sub(q,desired),sub(q,desired))
        if best is None or distance<best[0]:best=(distance,tuple(sum(weights[j]*allUV[v][k] for j,v in enumerate((a,b,c))) for k in range(2)))
       uv.append(best[1]);outPoints.append((point[0],point[2],-point[1]))
       outNormals.append((normal[0],normal[2],-normal[1]))
      if side=='r':outPoints=[(x,-y,z) for x,y,z in outPoints];outNormals=[(x,-y,z) for x,y,z in outNormals];triangles=[(a,c,b) for a,b,c in triangles]
      # The source relaxed/grab files use opposite winding conventions.
      # Normalize each pose against its outward normals for Raven's renderer.
      score=0
      for a,b,c in triangles:
       ab,ac=sub(outPoints[b],outPoints[a]),sub(outPoints[c],outPoints[a])
       cross=(ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0])
       score+=dot(cross,add(add(outNormals[a],outNormals[b]),outNormals[c]))
      if score>0:triangles=[(a,c,b) for a,b,c in triangles]
      results[pose]=(outPoints,outNormals,uv,triangles)
     modelCache[cacheKey]=results
    for pose,args in modelCache[cacheKey].items():entries[f'models/sof2vr_licensed/{outfit}/{side}_{pose}.md3']=make_md3(*args,shader,side)
   lengths.append(outfitLengths)
 header=['/* Original SoF2 arm segment lengths, game units; generated. */','#ifndef SOF2_HAND_PROFILES_H','#define SOF2_HAND_PROFILES_H','static const float vrArmLengths[7][2][2]={']
 for pairs in lengths:header.append(' {{%.7ff,%.7ff},{%.7ff,%.7ff}},'%tuple(pairs[0]+pairs[1]))
 header+=['};','#endif',''];(ROOT/'src/hand_profiles.h').write_text('\n'.join(header),encoding='utf8')
 return entries,metadata
if __name__=='__main__':
 entries,metadata=approved_hands();print('Prepared',len(entries),'outfit/pose models with SoF2 UV/material transfer.')
