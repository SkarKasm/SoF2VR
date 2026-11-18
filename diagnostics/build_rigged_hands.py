"""Retarget approved RTCW hand topology to SoF2's original first-person bind rig.
Original native attachment links and animation calls remain authoritative.
"""
from pathlib import Path
import struct,zipfile,math,json,hashlib
from build_hand_assets import source_mesh,closest,sub,dot,add,bounds
ROOT=Path(__file__).resolve().parents[1]
def build(original,side):
 h=struct.unpack_from('<4si64s64s7i',original);native=[];tri=[];uv=[];weights=[]
 for i in range(3):
  o=h[7]+4+struct.unpack_from('<i',original,h[7]+4+4*i)[0];s=struct.unpack_from('<10i',original,o);refs=struct.unpack_from('<'+'i'*s[7],original,o+s[8]);base=len(native)
  for v in range(s[3]):
   at=o+s[4]+v*32;native.append(struct.unpack_from('<3f',original,at+12));uv.append(struct.unpack_from('<2f',original,o+s[4]+32*s[3]+8*v));bits=struct.unpack_from('<I',original,at+24)[0];count=(bits>>30)+1;total=0;ws={}
   for k in range(count):
    w=1-total if k==count-1 else (original[at+28+k]|(((bits>>(20+2*k))&3)<<8))/1023;total+=w;ws[refs[(bits>>(5*k))&31]]=w
   weights.append(ws)
  tri.extend(tuple(base+j for j in struct.unpack_from('<3i',original,o+s[6]+12*t)) for t in range(s[5]))
 with zipfile.ZipFile(ROOT.parent/'xr-release-0.8.7/Main/z_realrtcw_models_hands_vr.pk3') as z:source=z.read('models/weapons/vrhands/v_offhand.md3')
 points,normals,triangles=source_mesh(source);lo,hi=bounds(points)
 cropped=[p for p in native if p[1]<=2.7];dlo,dhi=bounds(cropped);out=[];outn=[];outuv=[];outw=[]
 scales=[(dhi[1]-dlo[1])/(hi[0]-lo[0]),(dhi[0]-dlo[0])/(hi[1]-lo[1]),(dhi[2]-dlo[2])/(hi[2]-lo[2])]
 centres=[tuple(sum(native[v][k] for v in t)/3 for k in range(3)) for t in tri]
 for p,n in zip(points,normals):
  c=[(p[k]-lo[k])/(hi[k]-lo[k]) for k in range(3)];lateral=c[1] if side=='l' else 1-c[1]
  q=(dlo[0]+lateral*(dhi[0]-dlo[0]),dhi[1]-c[0]*(dhi[1]-dlo[1]),dlo[2]+c[2]*(dhi[2]-dlo[2]));out.append(q)
  normal=(n[1]/scales[1]*(1 if side=='l' else -1),-n[0]/scales[0],n[2]/scales[2]);length=math.sqrt(dot(normal,normal));outn.append(tuple(x/length for x in normal))
  candidates=sorted(range(len(tri)),key=lambda i:dot(sub(centres[i],q),sub(centres[i],q)))[:12];best=None
  for i in candidates:
   t=tri[i];at,bary=closest(q,*(native[v] for v in t));dist=dot(sub(at,q),sub(at,q))
   if best is None or dist<best[0]:best=(dist,t,bary)
  _,t,bary=best;outuv.append(tuple(sum(bary[j]*uv[v][k] for j,v in enumerate(t)) for k in range(2)));ws={}
  for j,v in enumerate(t):
   for bone,w in weights[v].items():ws[bone]=ws.get(bone,0)+bary[j]*w
  chosen=sorted(ws.items(),key=lambda x:-x[1])[:4];total=sum(w for b,w in chosen);outw.append([(b,w/total) for b,w in chosen if w>1e-6])
 # Match Raven's clockwise surface winding against the outward normals.
 score=0
 for a,b,c in triangles:
  ab,ac=sub(out[b],out[a]),sub(out[c],out[a]);cross=(ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]);score+=dot(cross,add(add(outn[a],outn[b]),outn[c]))
 if score>0:triangles=[(a,c,b) for a,b,c in triangles]
 batches=[];mapping={};batch=[]
 for tri_indices in triangles:
  if batch and (len(mapping)+sum(v not in mapping for v in tri_indices)>900 or len(batch)>=1700):batches.append((mapping,batch));mapping={};batch=[]
  batch.append(tuple(mapping.setdefault(v,len(mapping)) for v in tri_indices))
 if batch:batches.append((mapping,batch))
 assert len(batches)<=6
 prefix=bytearray(original[:h[7]]);struct.pack_into('<i',prefix,144,1)
 for i in range(6):
  at=164+struct.unpack_from('<i',prefix,164+4*i)[0];flags=struct.unpack_from('<I',prefix,at+64)[0];struct.pack_into('<I',prefix,at+64,(flags&~2) if i<len(batches) else flags|2)
  prefix[at+68:at+132]=b'models/hands/hands'.ljust(64,b'\0')
 surfaces=[];offset=h[7]+28
 for i in range(6):
  if i<len(batches):
   mapping,indices=batches[i];vertex=bytearray()
   for v in mapping:
    p,n,ws=out[v],outn[v],outw[v];total=sum(w for b,w in ws);ws=[(b,w/total) for b,w in ws];bits=(len(ws)-1)<<30;packed=[0]*4;used=0
    for k,(bone,w) in enumerate(ws):
     bits|=bone<<(5*k)
     if k<len(ws)-1:
      quant=max(0,min(1023-used,round(w*1023)));used+=quant;packed[k]=quant&255;bits|=(quant>>8)<<(20+2*k)
    vertex+=struct.pack('<6fI4B',*n,*p,bits,*packed)
   uvbytes=b''.join(struct.pack('<2f',*outuv[v]) for v in mapping);tribytes=b''.join(struct.pack('<3i',*t) for t in indices);refs=b''.join(struct.pack('<i',j) for j in range(26))
   end=40+len(vertex)+len(uvbytes)+len(tribytes)+len(refs);surf=struct.pack('<10i',0,i,-offset,len(mapping),40,len(indices),40+len(vertex)+len(uvbytes),26,40+len(vertex)+len(uvbytes)+len(tribytes),end)+vertex+uvbytes+tribytes+refs
  else:
   old=h[7]+4+struct.unpack_from('<i',original,h[7]+4+4*i)[0];oldh=struct.unpack_from('<10i',original,old);surf=bytearray(original[old:old+oldh[9]]);struct.pack_into('<i',surf,8,-offset);struct.pack_into('<i',surf,20,0);end=len(surf)
  surfaces.append(surf);offset+=end
 lod=bytearray(struct.pack('<i',offset-h[7]));at=24
 for surf in surfaces:lod+=struct.pack('<i',at);at+=len(surf)
 data=prefix+lod+b''.join(surfaces);struct.pack_into('<i',data,160,len(data))
 (ROOT/'src/native_hand_rig.h').write_text('#ifndef SOF2_NATIVE_HAND_RIG_H\n#define SOF2_NATIVE_HAND_RIG_H\n#define NATIVE_HAND_PARTS %d\n#endif\n'%len(batches))
 return bytes(data),dict(method='approved topology retargeted to native first-person bind coordinates, barycentric native UV/weights; original 26-bone skeleton and attachment hierarchy',vertices=len(out),triangles=len(triangles),parts=len(batches),sourceSHA256=hashlib.sha256(source).hexdigest(),nativeSHA256=hashlib.sha256(original).hexdigest(),nativeBounds=[dlo,dhi])
