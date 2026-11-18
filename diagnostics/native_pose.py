"""Original Raven GLM/GLA pose evaluation for local asset preparation."""
import struct, zipfile
from pathlib import Path
from build_reload_assets import compose, transform
BASE=Path(__file__).resolve().parents[2]/'Soldier of Fortune 2/base'
IDENTITY=[[1,0,0,0],[0,1,0,0],[0,0,1,0]]
ROOT=[[0,-1,0,0],[1,0,0,0],[0,0,1,0]]

def read(path):
 for archive in ['update103.pk3','update102.pk3','update101.PK3','models.pk3','therest.pk3']:
  with zipfile.ZipFile(BASE/archive) as z:
   match=next((name for name in z.namelist() if name.lower()==path.replace('\\','/').lower()),None)
   if match:return z.read(match)
 raise ValueError('Missing original asset '+path)

def pose(g,frame,root=ROOT):
 _,nf,of,nb,op,os,oe=struct.unpack_from('<f6i',g,72)
 assert 0<=frame<nf
 world=[None]*nb;bases=[];names=[];parents=[];local=[]
 for b in range(nb):
  p=100+struct.unpack_from('<i',g,100+4*b)[0]
  names.append(g[p:p+64].split(b'\0')[0].decode());parent=struct.unpack_from('<i',g,p+68)[0]
  bases.append([list(struct.unpack_from('<4f',g,p+72+i*16)) for i in range(3)])
  p=of+(frame*nb+b)*3;j=int.from_bytes(g[p:p+3],'little');q=struct.unpack_from('<7H',g,op+j*14)
  w,x,y,z=[v/16383-2 for v in q[:4]]
  m=[[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),q[4]/64-512],
     [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),q[5]/64-512],
     [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y),q[6]/64-512]]
  parents.append(parent);local.append(m)
 # Character facial bones may precede their parent in Raven's bone table.
 # Weapons happened to be topologically ordered. Resolve by dependency so
 # both layouts retain the exact same transform multiplication order.
 visiting=set()
 def resolve(index):
  if world[index] is not None:return world[index]
  if index in visiting:raise ValueError('Cyclic native skeleton')
  visiting.add(index);parent=parents[index]
  if parent>=nb or parent < -1:raise ValueError('Invalid native parent')
  world[index]=compose(root if parent<0 else resolve(parent),local[index])
  visiting.remove(index);return world[index]
 for index in range(nb):resolve(index)
 return world,bases,names

def bolt(evaluated,name):
 world,bases,names=evaluated;i=[n.lower() for n in names].index(name.lower())
 return compose(world[i],bases[i])

def vertices(d,index,world):
 h=struct.unpack_from('<4si64s64s7i',d);p=h[7]+4+struct.unpack_from('<i',d,h[7]+4+4*index)[0]
 s=struct.unpack_from('<10i',d,p);refs=struct.unpack_from('<'+'i'*s[7],d,p+s[8]);out=[]
 for v in range(s[3]):
  at=p+s[4]+32*v;n=struct.unpack_from('<3f',d,at);xyz=struct.unpack_from('<3f',d,at+12)
  bits=struct.unpack_from('<I',d,at+24)[0];num=(bits>>30)+1;total=0;point=[0.]*3;normal=[0.]*3;weights={}
  for j in range(num):
   bone=refs[(bits>>(5*j))&31];weight=1-total if j==num-1 else (d[at+28+j]|(((bits>>(20+2*j))&3)<<8))/1023.;total+=weight
   weights[bone]=weight
   for k,c in enumerate(transform(world[bone],xyz)):point[k]+=c*weight
   for k,c in enumerate(transform(world[bone],n,True)):normal[k]+=c*weight
  out.append((point,normal,struct.unpack_from('<2f',d,p+s[4]+32*s[3]+8*v),weights,xyz))
 triangles=[struct.unpack_from('<3i',d,p+s[6]+12*t) for t in range(s[5])]
 return out,triangles
