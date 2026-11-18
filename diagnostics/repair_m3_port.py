"""Restore the source M3 receiver and its original ejection window.

Keep the action separate and the authored weapon fit intact. Extra cuts and
the temporary interior liner are removed because they show through the shell.
"""
import gzip,hashlib,json,os,struct,zipfile
from pathlib import Path
from native_pose import read,pose,vertices
from build_presentation import idle
from build_weapon_replacements import chunks,apply,inverse,unit

ROOT=Path(__file__).resolve().parents[1]
MARKER='sof2vr-m3-port-repair.json'
def sha(b):return hashlib.sha256(b).hexdigest()

def surface(points,normals,uv,tris,index,bone,world,start):
    inv=inverse(world[bone]);td=b''.join(struct.pack('<3i',*t) for t in tris);vd=bytearray()
    for p,n in zip(points,normals):vd+=struct.pack('<6fI4B',*unit(apply(inv,n,True)),*apply(inv,p),0,0,0,0,0)
    st=b''.join(struct.pack('<2f',*v) for v in uv);vo=40+len(td);br=vo+len(vd)+len(st);end=br+4
    return struct.pack('<10i',0,index,-start,len(points),vo,len(tris),40,1,br,end)+td+vd+st+struct.pack('<i',bone)

def repair():
    pack=ROOT/'resources/z_sof2vr_native.pk3'
    with zipfile.ZipFile(pack) as z:
        path='models/weapons/m3a1/m3a1.glm';data=z.read(path)
        if MARKER in z.namelist():
            report=json.loads(z.read(MARKER))
            if sha(data)!=report['outputSHA256']:raise ValueError('M3 changed after receiver repair')
            if report['version']==2:return report
        row=next(w for w in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons'] if w['id']==8)
        replacement=next(w for w in json.loads(z.read('sof2vr-weapon-replacements.json'))['ready'] if w['id']==8)
        matrix=replacement['calibration']['matrix'];inv=inverse(matrix)
        normals=[[inv[j][i] for j in range(3)]+[0] for i in range(3)]
        source=ROOT/'build/replacement-intake/3-grease-gun/selected-meshes.json.gz'
        with gzip.open(source,'rt') as f:mesh=json.load(f)
        groups={}
        for g in mesh['groups']:
            if g['part']!='body':continue
            out=groups.setdefault(g['material'],[[],[],[],[]])
            for t in g['triangles']:
                base=len(out[0])
                for i in t:
                    v=g['vertices'][i];out[0].append(apply(matrix,v[:3]));out[1].append(unit(apply(normals,v[3:6],True)));out[2].append(v[6:8])
                out[3].append([base,base+1,base+2])
        originals=[c for g in groups.values() for c in chunks(g)]
        h=list(struct.unpack_from('<4si64s64s7i',data));assert h[6]==1
        frame=idle(row['animations']['sequences'],'m3a1')['startFrame']
        world=pose(z.read(row['skeleton']['path']) if row['skeleton']['path'] in z.namelist() else read(row['skeleton']['path']),frame)[0]
        indices=[];liners=[]
        for i in range(h[8]):
            at=164+struct.unpack_from('<i',data,164+4*i)[0]
            name=data[at:at+64].split(b'\0')[0]
            if name.startswith(b'vr_body_'):indices.append(i)
            if name.startswith(b'vr_donor_interior_'):liners.append(i)
        assert len(indices)==len(originals)
        converted=dict(zip(indices,originals))
        sourceTriangles=sum(len(group[3]) for group in originals)
        block=bytearray(4+4*h[8]);lod=h[7]
        for i in range(h[8]):
            offset=len(block);struct.pack_into('<i',block,4+4*i,offset-4)
            p=lod+4+struct.unpack_from('<i',data,lod+4+4*i)[0];s=struct.unpack_from('<10i',data,p)
            if i in converted:out=surface(*converted[i],i,1,world,lod+offset)
            else:
                out=bytearray(data[p:p+s[-1]])
                struct.pack_into('<i',out,8,-(lod+offset))
                if i in liners:struct.pack_into('<i',out,20,0)
            block+=out
        struct.pack_into('<i',block,0,len(block));h[-1]=lod+len(block)
        fixed=struct.pack('<4si64s64s7i',*h)+data[164:lod]+block
        report=dict(version=2,sourceSHA256=sha(source.read_bytes()),beforeSHA256=sha(data),outputSHA256=sha(fixed),
                    sourceTriangles=sourceTriangles,outputBodyTriangles=sourceTriangles,disabledLiners=len(liners),
                    policy='Complete source receiver and original ejection window, without procedural cuts or a duplicate liner')
        changes={path:fixed,MARKER:(json.dumps(report,indent=2)+'\n').encode()}
        if 'sof2vr-visual-finishes.json' in z.namelist():
            finish=json.loads(z.read('sof2vr-visual-finishes.json'));finish['outputs'][path]=sha(fixed)
            changes['sof2vr-visual-finishes.json']=(json.dumps(finish,indent=2)+'\n').encode()
        pending=pack.with_suffix('.port-pending')
        with zipfile.ZipFile(pending,'w') as out:
            for info in z.infolist():out.writestr(info,changes.pop(info.filename,z.read(info)))
            for name,b in changes.items():out.writestr(name,b,compress_type=zipfile.ZIP_DEFLATED)
    os.replace(pending,pack);return report

if __name__=='__main__':print(json.dumps(repair(),indent=2))
