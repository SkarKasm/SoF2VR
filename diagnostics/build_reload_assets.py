"""Original SoF2 M4 parts evaluated at its authored idle pose.
Ghoul2's root rotates (-y,x,z); compressed animation matrices already include
bind compensation. Weight decoding follows Raven's published mdx_format.h.
No third party artwork is included. Originals stay untouched.
"""
from pathlib import Path
import struct,zipfile,math,json,hashlib
from build_hand_assets import make_md3
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT.parent/'Soldier of Fortune 2/base'
def compose(a,b):
 return [[sum(a[i][k]*b[k][j] for k in range(3))+(a[i][3] if j==3 else 0) for j in range(4)] for i in range(3)]
def transform(m,p,normal=False):return tuple(sum(m[i][k]*p[k] for k in range(3))+(0 if normal else m[i][3]) for i in range(3))
def generate():
 row=next(w for w in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons'] if w['id']==9)
 with zipfile.ZipFile(BASE/row['archive']) as z:d=z.read(row['viewModel'])
 with zipfile.ZipFile(BASE/row['skeleton']['archive']) as z:g=z.read(row['skeleton']['path'])
 frame=next(a['startFrame'] for a in row['animations']['sequences'] if a['name']=='m4idle.xsi')
 _,nf,of,nb,op,os,oe=struct.unpack_from('<f6i',g,72);world=[]
 root=[[0,-1,0,0],[1,0,0,0],[0,0,1,0]]
 for b in range(nb):
  p=100+struct.unpack_from('<i',g,100+4*b)[0];parent=struct.unpack_from('<i',g,p+68)[0]
  p=of+(frame*nb+b)*3;j=int.from_bytes(g[p:p+3],'little');q=struct.unpack_from('<7H',g,op+j*14)
  w,x,y,z=[v/16383.-2 for v in q[:4]]
  m=[[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),q[4]/64-512],
     [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),q[5]/64-512],
     [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y),q[6]/64-512]]
  world.append(compose(root if parent<0 else world[parent],m))
 anchors={}
 h=struct.unpack_from('<4si64s64s7i',d);parts={k:[[],[],[],[]] for k in ['body','magazine','bolt']}
 # Live original M4 surface override list captured in the running SP game.
 # VR includes complementary hidden sides and the full stock.
 hidden=set()
 for i,surface in enumerate(row['surfaces']):
  if i in hidden and surface['name']!='cliph':continue
  p=h[7]+4+struct.unpack_from('<i',d,h[7]+4+4*i)[0];s=struct.unpack_from('<10i',d,p);refs=struct.unpack_from('<'+'i'*s[7],d,p+s[8]);verts=[]
  for v in range(s[3]):
   at=p+s[4]+v*32;n=struct.unpack_from('<3f',d,at);xyz=struct.unpack_from('<3f',d,at+12);bits=struct.unpack_from('<I',d,at+24)[0];num=(bits>>30)+1;total=0;point=[0.]*3;normal=[0.]*3;weights={}
   for j in range(num):
    bone=refs[(bits>>(5*j))&31];weight=1-total if j==num-1 else (d[at+28+j]|((bits>>(12+2*j))&0x300))/1023.;total+=weight;weights[bone]=weights.get(bone,0)+weight
    for k,c in enumerate(transform(world[bone],xyz)):point[k]+=c*weight
    for k,c in enumerate(transform(world[bone],n,True)):normal[k]+=c*weight
   verts.append((point,normal,struct.unpack_from('<2f',d,p+s[4]+32*s[3]+8*v),weights))
  if surface['name'] in ['handle','gunmuzzle']:anchors[surface['name']]=[(min(v[0][k] for v in verts)+max(v[0][k] for v in verts))/2 for k in range(3)]
  for t in range(s[5]):
   tri=struct.unpack_from('<3i',d,p+s[6]+12*t);part='magazine' if 'clip' in surface['name'] else 'bolt' if sum(verts[v][3].get(4,0) for v in tri)>1.5 else 'body';out=parts[part];start=len(out[0])
   for v in tri:
    xyz,n,uv,weights=verts[v];out[0].append(xyz);out[1].append(n);out[2].append(uv)
   out[3].append((start,start+1,start+2))
 mag=parts['magazine'][0];lo=[min(p[k] for p in mag) for k in range(3)];hi=[max(p[k] for p in mag) for k in range(3)];socket=[(a+b)/2 for a,b in zip(lo,hi)]
 # Socket is the centre of the original seated magazine, in posed model space.
 entries={};stats={}
 for name,(p,n,uv,tri) in parts.items():
  if not p:raise ValueError('Missing M4 part '+name)
  anchor=socket if name=='magazine' else [0,0,0]
  p=[tuple(v[k]-anchor[k] for k in range(3)) for v in p]
  entries[f'models/sof2vr_reload/m4_{name}.md3']=make_md3(p,n,uv,tri,'models/weapons/m4/m4',name)
  stats[name]={'triangles':len(tri),'vertices':len(p)}
 bolt=parts['bolt'][0];boltSocket=[(min(p[k] for p in bolt)+max(p[k] for p in bolt))/2 for k in range(3)]
 header='/* Generated original M4 idle-pose socket, game units. */\n#ifndef SOF2_RELOAD_PARTS_H\n#define SOF2_RELOAD_PARTS_H\nstatic const float m4MagazineSocket[3]={'+','.join(f'{v:.7f}f' for v in socket)+'};\nstatic const float m4BoltSocket[3]={'+','.join(f'{v:.7f}f' for v in boltSocket)+'};\nstatic const float m4GripSocket[3]={'+','.join(f'{v:.7f}f' for v in anchors['handle'])+'};\nstatic const float m4MuzzleSocket[3]={'+','.join(f'{v:.7f}f' for v in anchors['gunmuzzle'])+'};\n#endif\n'
 (ROOT/'src/reload_parts.h').write_text(header)
 metadata={'weapon':9,'pose':frame,'source':row['viewModel'],'sha256':hashlib.sha256(d).hexdigest(),'skeletonSHA256':hashlib.sha256(g).hexdigest(),'socket':socket,'boltSocket':boltSocket,'gripSocket':anchors['handle'],'muzzleSocket':anchors['gunmuzzle'],'parts':stats,'nativeHiddenSurfaces':sorted(hidden),'transform':'Raven compressed weighted skinning; Ghoul2 root (-y,x,z), no extra inverse-bind multiplication','reference':'https://github.com/JACoders/OpenJK/blob/master/code/rd-common/mdx_format.h'}
 entries['sof2vr-reload-parts.json']=(json.dumps(metadata,indent=2)+'\n').encode();return entries,metadata
if __name__=='__main__':
 entries,metadata=generate();path=ROOT/'resources/z_sof2vr_native.pk3'
 with zipfile.ZipFile(path) as z:existing={n:z.read(n) for n in z.namelist() if not n.startswith('models/sof2vr_reload/') and n!='sof2vr-reload-parts.json'}
 existing.update(entries)
 with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED) as z:
  for name,data in sorted(existing.items()):
   info=zipfile.ZipInfo(name,(2002,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,data)
 print(json.dumps(metadata,indent=2))
