"""Derive local bore-tip anchors from original authored idle view meshes."""
from pathlib import Path
import json,zipfile,struct,math
from build_reload_assets import compose,transform
from build_presentation import idle
from native_pose import read,pose,bolt
from build_weapon_profiles import parse,value
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT.parent/'Soldier of Fortune 2/base'
def generate():
 definitions={value(b,'name').lower():b for k,b in parse(read('inview/SOF2.inview').decode('cp1252')) if k=='weapon'}
 rows=json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons'];profiles={w['id']:w for w in json.loads((ROOT/'resources/weapon_profiles.json').read_text())['weapons']};out=[]
 for row in rows:
  if profiles[row['id']]['category'] not in [2,3,4,5,6,7]:continue
  rest=idle(row['animations']['sequences'],Path(row['viewModel']).stem)
  if not rest:continue
  with zipfile.ZipFile(BASE/row['archive']) as z:d=z.read(row['viewModel'])
  with zipfile.ZipFile(BASE/row['skeleton']['archive']) as z:g=z.read(row['skeleton']['path'])
  _,nf,of,nb,op,os,oe=struct.unpack_from('<f6i',g,72);world=[];root=[[0,-1,0,0],[1,0,0,0],[0,0,1,0]]
  for b in range(nb):
   p=100+struct.unpack_from('<i',g,100+4*b)[0];parent=struct.unpack_from('<i',g,p+68)[0];p=of+(rest['startFrame']*nb+b)*3;j=int.from_bytes(g[p:p+3],'little');q=struct.unpack_from('<7H',g,op+j*14);w,x,y,z=[v/16383-2 for v in q[:4]]
   m=[[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),q[4]/64-512],[2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),q[5]/64-512],[2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y),q[6]/64-512]];world.append(compose(root if parent<0 else world[parent],m))
  h=struct.unpack_from('<4si64s64s7i',d);groups=[]
  for i,surface in enumerate(row['surfaces']):
   name=surface['name'];p=h[7]+4+struct.unpack_from('<i',d,h[7]+4+4*i)[0];s=struct.unpack_from('<10i',d,p);refs=struct.unpack_from('<'+'i'*s[7],d,p+s[8]);points=[]
   for v in range(s[3]):
    at=p+s[4]+32*v;xyz=struct.unpack_from('<3f',d,at+12);bits=struct.unpack_from('<I',d,at+24)[0];num=(bits>>30)+1;total=0;point=[0.]*3
    for j in range(num):
     bone=refs[(bits>>(5*j))&31];weight=1-total if j==num-1 else (d[at+28+j]|(((bits>>(20+2*j))&3)<<8))/1023;total+=weight;q=transform(world[bone],xyz)
     for k in range(3):point[k]+=q[k]*weight
    points.append(point)
   if points:groups.append((name,points))
  chosen=[(n,p) for n,p in groups if 'muzzle' in n.lower() and 'grenade' not in n.lower()]
  if not chosen:chosen=[(n,p) for n,p in groups if 'barrel' in n.lower() and '_off' not in n.lower()]
  if not chosen:chosen=[(n,p) for n,p in groups if not any(s in n.lower() for s in ['silencer','laser','utl','_off'])]
  points=[p for n,ps in chosen for p in ps]
  # Native muzzle attachment orientation is explicit authoring data. A PCA
  # through the entire asymmetric barrel/receiver (sights, lugs and complements)
  # estimates the shape's longest axis, which is not necessarily its bore.
  definition=definitions[row['name'].lower()];model=value(definition,'weaponmodel',[]);buffer=value(model,'buffer',[])
  wp=pose(g,rest['startFrame']);bufferData=read(value(buffer,'model'))
  skeleton=struct.unpack_from('<4si64s64s7i',bufferData)[3].split(b'\0')[0].decode()+'.gla'
  bp=pose(read(skeleton),0,bolt(wp,value(buffer,'bolttobone')))
  muzzle=value(buffer,'muzzle','');native=bolt(bp,muzzle) if muzzle.lower() in [n.lower() for n in bp[2]] else bolt(wp,value(buffer,'bolttobone'))
  direction=[-native[i][1] for i in range(3)];norm=math.sqrt(sum(v*v for v in direction));direction=[v/norm for v in direction]
  if direction[0]<0:direction=[-v for v in direction]
  method='native authored muzzle bolt forward (-Y); gun-bolt fallback if muzzle tag missing'
  side=[-direction[1],direction[0],0.];n=math.sqrt(sum(v*v for v in side));side=[v/n for v in side]
  up=[direction[1]*side[2]-direction[2]*side[1],direction[2]*side[0]-direction[0]*side[2],direction[0]*side[1]-direction[1]*side[0]]
  def dot(a,b):return sum(x*y for x,y in zip(a,b))
  # Use the front muzzle geometry for lateral centring. Do not centre on
  # asymmetric receiver furniture or on a global barrel bounding box.
  front=max(dot(p,direction) for p in points)
  crossSection=[p for p in points if front-dot(p,direction)<.5] or points
  lateral=(min(dot(p,side) for p in crossSection)+max(dot(p,side) for p in crossSection))*.5
  vertical=(min(dot(p,up) for p in crossSection)+max(dot(p,up) for p in crossSection))*.5
  front=max(dot(p,direction) for p in points)
  anchor=[direction[k]*front+side[k]*lateral+up[k]*vertical for k in range(3)]
  assert all(math.isfinite(v) and abs(v)<256 for v in anchor)
  out.append(dict(id=row['id'],name=row['name'],anchor=anchor,direction=direction,directionMethod=method,surfaces=[n for n,p in chosen],idle=rest['name']))
 header=['/* Generated original authored bore-tip anchors, in weapon model coordinates. */','#ifndef SOF2_WEAPON_MUZZLES_H','#define SOF2_WEAPON_MUZZLES_H','typedef struct {int id;float point[3],direction[3];} VRWeaponMuzzle;','static const VRWeaponMuzzle vrWeaponMuzzles[]={']
 for row in out:header.append(' {%d,{%s},{%s}},'%(row['id'],','.join('%.8ff'%v for v in row['anchor']),','.join('%.8ff'%v for v in row['direction'])))
 header+=['};','#endif',''];(ROOT/'src/weapon_muzzles.h').write_text('\n'.join(header));(ROOT/'build/weapon-muzzle-calibration.json').write_text(json.dumps(out,indent=2)+'\n');return out
if __name__=='__main__':print('Generated',len(generate()),'original bore-tip anchors.')
