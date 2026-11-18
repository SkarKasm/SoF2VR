"""Complete local M203 geometry by reflecting native triangles across X=0.
Keep authored skinning, UVs, materials, surface IDs and every LOD.
"""
from pathlib import Path
import struct, zipfile, hashlib

BASE=Path(__file__).resolve().parents[2]/'Soldier of Fortune 2/base'

def build():
    name='models/weapons/m203/m203.glm'
    for archive in ['update103.pk3','update102.pk3','update101.PK3','models.pk3']:
        with zipfile.ZipFile(BASE/archive) as z:
            if name in z.namelist():original=z.read(name);break
    h=struct.unpack_from('<4si64s64s7i',original)
    assert h[0]==b'2LGM' and h[1]==6 and h[8]==12
    output=bytearray(original[:h[7]]);lod=h[7];stats=[]
    for level in range(h[6]):
        surfaces=[]
        for i in range(h[8]):
            at=lod+4+struct.unpack_from('<i',original,lod+4+4*i)[0]
            sh=struct.unpack_from('<10i',original,at)
            vertices=[bytearray(original[at+sh[4]+32*v:at+sh[4]+32*(v+1)]) for v in range(sh[3])]
            uv=[original[at+sh[4]+32*sh[3]+8*v:at+sh[4]+32*sh[3]+8*(v+1)] for v in range(sh[3])]
            tri=[struct.unpack_from('<3i',original,at+sh[6]+12*t) for t in range(sh[5])]
            refs=list(struct.unpack_from('<'+'i'*sh[7],original,at+sh[8]))
            surfaces.append([sh,vertices,uv,tri,refs])
        added=0
        for i in range(0,h[8],2):
            base,back=surfaces[i],surfaces[i+1]
            refs=list(dict.fromkeys(base[4]+back[4]));assert len(refs)<=32
            vertices=[];uv=[];tri=[]
            for part in [base,back]:
                offset=len(vertices)
                for src in part[1]:
                    v=bytearray(src);bits=struct.unpack_from('<I',v,24)[0];num=(bits>>30)+1;encoded=bits
                    for j in range(num):
                        slot=(bits>>(5*j))&31;replacement=refs.index(part[4][slot])
                        encoded=(encoded&~(31<<(5*j)))|(replacement<<(5*j))
                    struct.pack_into('<I',v,24,encoded);vertices.append(v)
                uv.extend(part[2]);tri.extend(tuple(v+offset for v in t) for t in part[3])
            def key(t):
                return tuple(sorted(tuple(round(c,4) for c in struct.unpack_from('<3f',vertices[v],12)) for v in t))
            seen={key(t) for t in tri};count=len(vertices)
            for v in vertices[:count]:
                reflected=bytearray(v)
                struct.pack_into('<f',reflected,0,-struct.unpack_from('<f',v,0)[0])
                struct.pack_into('<f',reflected,12,-struct.unpack_from('<f',v,12)[0])
                vertices.append(reflected)
            uv+=uv[:count]
            for t in tri[:]:
                reflected=(count+t[0],count+t[2],count+t[1]);k=key(reflected)
                if k not in seen:tri.append(reflected);seen.add(k);added+=1
            assert len(vertices)<=1024 and len(tri)<=2048
            surfaces[i]=[base[0],vertices,uv,tri,refs]
            # Complementary faces are now in the same moving/visible part.
            surfaces[i+1]=[back[0],back[1],back[2],[],back[4]]
        start=len(output);block=bytearray(4+4*h[8])
        for i,(sh,vertices,uv,tri,refs) in enumerate(surfaces):
            offset=len(block);struct.pack_into('<i',block,4+4*i,offset-4)
            tris=b''.join(struct.pack('<3i',*t) for t in tri)
            vbytes=b''.join(vertices);tex=b''.join(uv);bones=struct.pack('<'+'i'*len(refs),*refs)
            ot=40;ov=ot+len(tris);ob=ov+len(vbytes)+len(tex);end=ob+len(bones)
            block+=struct.pack('<10i',sh[0],i,-(start+offset),len(vertices),ov,len(tri),ot,len(refs),ob,end)+tris+vbytes+tex+bones
        struct.pack_into('<i',block,0,len(block));output+=block
        stats.append(dict(lod=level,addedTriangles=added));lod+=struct.unpack_from('<i',original,lod)[0]
    struct.pack_into('<i',output,160,len(output))
    return {name:bytes(output)},dict(path=name,sourceArchive=archive,sourceSHA256=hashlib.sha256(original).hexdigest(),method='merge authored sides, reflect missing triangles across native X=0; reversed winding and normals, original UVs and bone weights',lods=stats)
