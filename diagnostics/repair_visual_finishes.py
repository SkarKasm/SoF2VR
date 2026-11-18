"""Bounded repairs to the reviewed pack; keep owner-authored poses and scales.

Suppress an idle-only MM1 reload prop, correct launcher/donor materials and
cover existing wrist cuts with short two-sided leather cuffs. No fitting or
source model regeneration, game launch, or writes to original archives.
"""
import hashlib,io,json,math,os,re,struct,zipfile
from pathlib import Path
from build_hand_assets import make_md3
from native_pose import read,pose,vertices
from build_presentation import idle

ROOT=Path(__file__).resolve().parents[1]
MARKER='sof2vr-visual-finishes.json'
CUFF_SHADER='sof2vr/wrist_leather'
M3_SHADER='sof2vr/m3_action_matte'

def sha(data):return hashlib.sha256(data).hexdigest()
def unit(v):
    n=math.sqrt(sum(x*x for x in v));return [x/n for x in v]

def close_packed_cuff(data):
    """Keep every old vertex byte; replace cap faces with a 4 mm leather seal.

    Only the old cap fans are removed from triangle lists. A dedicated
    two-sided material keeps the cuff visible through reflected hand poses.
    """
    d=bytearray(data);h=list(struct.unpack_from('<4si64s9i',d));at=h[10]
    points=[];normals=[];uv=[];triangles=[];caps=0
    for _ in range(h[6]):
        s=struct.unpack_from('<4s64s10i',d,at)
        tex=[struct.unpack_from('<2f',d,at+s[9]+8*i) for i in range(s[5])]
        xyz=[[v/64 for v in struct.unpack_from('<3h',d,at+s[10]+8*i)] for i in range(s[5])]
        centres={i for i,t in enumerate(tex) if abs(t[0]-.13)<1e-6 and abs(t[1]-.20)<1e-6}
        ts=[struct.unpack_from('<3i',d,at+s[7]+12*i) for i in range(s[6])]
        keep=[t for t in ts if not centres.intersection(t)]
        for c in centres:
            fan=[t for t in ts if c in t]
            if len(fan)<3:continue
            # Preserve the ordered boundary from the original fan, without
            # re-sorting a posed contour or changing any finger geometry.
            boundary=[];normal=[0.,0.,0.]
            for tri in fan:
                n=tri.index(c);a,b=tri[(n+1)%3],tri[(n+2)%3];boundary.append((a,b))
                p=[xyz[a][i]-xyz[c][i] for i in range(3)];q=[xyz[b][i]-xyz[c][i] for i in range(3)]
                cross=[p[1]*q[2]-p[2]*q[1],p[2]*q[0]-p[0]*q[2],p[0]*q[1]-p[1]*q[0]]
                normal=[normal[i]-cross[i] for i in range(3)]
            normal=unit(normal);centre=[xyz[c][i]+normal[i]*.16 for i in range(3)]
            first=len(points);points.append(centre);normals.append(normal);uv.append([.13,.20])
            ring={}
            for a,b in boundary:
                for v in [a,b]:
                    if v in ring:continue
                    ring[v]=(len(points),len(points)+1)
                    for offset in [0,.16]:
                        points.append([xyz[v][i]+normal[i]*offset for i in range(3)])
                        normals.append(normal);uv.append([.13+(tex[v][0]-.13)*.5,.20+(tex[v][1]-.20)*.5])
                a0,a1=ring[a];b0,b1=ring[b]
                triangles.extend([(first,a1,b1),(a0,b0,b1),(a0,b1,a1)])
            caps+=1
        struct.pack_into('<i',d,at+84,len(keep))
        for i,t in enumerate(keep):struct.pack_into('<3i',d,at+s[7]+12*i,*t)
        at+=s[-1]
    if not caps:return data,0
    extra=make_md3(points,normals,uv,triangles,CUFF_SHADER,'cuff')
    eh=struct.unpack_from('<4si64s9i',extra)
    d+=extra[eh[10]:];h[6]+=eh[6];h[-1]=len(d)
    struct.pack_into('<4si64s9i',d,0,*h)
    # Extend frame bounds for the small cuff, keeping the frame/tag layout.
    old=struct.unpack_from('<10f',d,h[8])
    lo=[min(old[k],min(p[k] for p in points)) for k in range(3)]
    hi=[max(old[k+3],max(p[k] for p in points)) for k in range(3)]
    struct.pack_into('<10f',d,h[8],*lo,*hi,*old[6:9],max(old[9],max(math.sqrt(sum(v*v for v in p)) for p in points)))
    return bytes(d),caps

def repair():
    pack=ROOT/'resources/z_sof2vr_native.pk3';changes={}
    with zipfile.ZipFile(pack) as z:
        if MARKER in z.namelist():
            previous=json.loads(z.read(MARKER))
            changed=[p for p,digest in previous['outputs'].items() if sha(z.read(p))!=digest]
            if not changed:return previous
            # A runtime-only build can refresh magazine-holding hands. Re-cap
            # these derivatives without repeating the M3 inset/material edits.
            known={w['model'] for w in previous['wrists']}
            if any(p not in known for p in changed):raise ValueError('Visual repair target changed; rebuild the baseline asset pack before reapplying')
            for path in changed:
                output,caps=close_packed_cuff(z.read(path));assert caps
                changes[path]=output;previous['outputs'][path]=sha(output)
            changes[MARKER]=(json.dumps(previous,indent=2)+'\n').encode()
            pending=pack.with_suffix('.visual-pending')
            with zipfile.ZipFile(pending,'w') as out:
                for info in z.infolist():out.writestr(info,changes.get(info.filename,z.read(info)))
            z.close();os.replace(pending,pack);return previous
        protected={p:sha(z.read(p)) for p in ['sof2vr-manual-hand-edits.json','sof2vr-grip-poses.json']}
        report=dict(version=1,policy='Preserve saved authoring, anchors, all existing hand vertices, weapon scales and original archives',wrists=[],mm1=[],m3=[])
        # This is the loose cartridge used by the flat reload animation, not
        # the launcher body/drum or its loaded ammunition bookkeeping.
        path='models/weapons/mm1/mm1.glm';d=bytearray(z.read(path));h=struct.unpack_from('<4si64s64s7i',d);lod=h[7]
        for level in range(h[6]):
            for i in range(h[8]):
                at=164+struct.unpack_from('<i',d,164+4*i)[0]
                name=d[at:at+64].split(b'\0')[0].decode()
                if name!='_40mmgrenade':continue
                p=lod+4+struct.unpack_from('<i',d,lod+4+4*i)[0];count=struct.unpack_from('<i',d,p+20)[0]
                struct.pack_into('<i',d,p+20,0);report['mm1'].append(dict(surface=name,lod=level,removedTriangles=count))
            lod+=struct.unpack_from('<i',d,lod)[0]
        assert report['mm1'];changes[path]=bytes(d)
        # M3 temporary native donor was proud of the replacement receiver and
        # retained its old additive specular stage. Inset only donor vertices.
        rep=next(w for w in json.loads(z.read('sof2vr-weapon-replacements.json'))['ready'] if w['id']==8)
        direction=unit([rep['calibration']['matrix'][i][1] for i in range(3)])
        delta=[-v*.45 for v in direction]
        row=next(w for w in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons'] if w['id']==8)
        path=row['viewModel'];d=bytearray(z.read(path));h=struct.unpack_from('<4si64s64s7i',d)
        frame=idle(row['animations']['sequences'],'m3a1')['startFrame']
        world=pose(z.read(row['skeleton']['path']) if row['skeleton']['path'] in z.namelist() else read(row['skeleton']['path']),frame)[0]
        for i in range(h[8]):
            at=164+struct.unpack_from('<i',d,164+4*i)[0];name=d[at:at+64].split(b'\0')[0].decode()
            if name not in ['vr_bolt_37','vr_donor_interior_38']:continue
            p=h[7]+4+struct.unpack_from('<i',d,h[7]+4+4*i)[0];s=struct.unpack_from('<10i',d,p)
            assert s[7]==1
            bone=struct.unpack_from('<i',d,p+s[8])[0]
            local=[sum(world[bone][j][k]*delta[j] for j in range(3)) for k in range(3)]
            for v in range(s[3]):
                pos=p+s[4]+32*v+12;xyz=struct.unpack_from('<3f',d,pos)
                struct.pack_into('<3f',d,pos,*[xyz[k]+local[k] for k in range(3)])
            d[at+68:at+132]=M3_SHADER.encode().ljust(64,b'\0');report['m3'].append(dict(surface=name,insetGameUnits=.45,vertices=s[3]))
        assert len(report['m3'])==2;changes[path]=bytes(d)
        # Keep the separately animated charging part at the identical inset.
        from build_magazine_assets import multipart
        bolt=vertices(bytes(d),37,world);group=[[v[0] for v in bolt[0]],[v[1] for v in bolt[0]],[v[2] for v in bolt[0]],bolt[1]]
        reload=next(w for w in json.loads(z.read('sof2vr-magazine-parts.json'))['ready'] if w['id']==8)
        changes[reload['prefix']+'_bolt.md3']=multipart({M3_SHADER:group},[0,0,0])
        shader=z.read('shaders/weapons.shader').decode('cp1252')
        # Metallic source textures lack the environment lighting provided by
        # Sketchfab. Keep diffuse shading, with a modest textured ambient pass.
        pattern=r'(sof2vr_ext/w2/m\d+)\s*\{\s*cull disable\s*\{\s*map ([^\s]+)\s*rgbGen lightingDiffuse\s*\}\s*\}'
        def metal(match):
            material,texture=match.groups()
            ambient=.42
            return (f'{material}\n{{\n cull disable\n {{\n map {texture}\n rgbGen lightingDiffuse\n }}\n'
                    f' {{\n map {texture}\n blendFunc GL_ONE GL_ONE\n rgbGen const ( {ambient} {ambient} {ambient} )\n }}\n}}')
        shader,count=re.subn(pattern,metal,shader)
        assert count==1
        from PIL import Image
        socom='sof2vr_ext/w2/m0.png'
        with Image.open(io.BytesIO(z.read(socom))) as image:
            # Lift the clipped black end of the source albedo while keeping
            # the etched details, UVs and bright pixels intact.
            curve=[round(v+17*max(0,1-v/100)**2) for v in range(256)]
            output=io.BytesIO();image.convert('RGB').point(curve*3).save(output,format='PNG')
        changes[socom]=output.getvalue()
        shader+='\n'+CUFF_SHADER+'\n{\n cull disable\n {\n map models/hands/hands\n rgbGen lightingDiffuse\n }\n}\n'
        shader+='\n'+M3_SHADER+'\n{\n cull disable\n {\n map models/weapons/m3a1/m3a1\n rgbGen lightingDiffuse\n }\n {\n map $whiteimage\n blendFunc GL_DST_COLOR GL_ZERO\n rgbGen const ( 0.40 0.40 0.40 )\n }\n}\n'
        changes['shaders/weapons.shader']=shader.encode('cp1252')
        report['metalFinish']=dict(weapons=[2],method='Textured ambient contribution and shadow detail lift; original UVs',ambient=[.42])
        report['outputs']={name:sha(data) for name,data in changes.items()}
        changes[MARKER]=(json.dumps(report,indent=2)+'\n').encode()
        pending=pack.with_suffix('.visual-pending')
        try:
            with zipfile.ZipFile(pending,'w') as out:
                for info in z.infolist():out.writestr(info,changes.pop(info.filename,z.read(info)))
                for name,data in changes.items():out.writestr(name,data,compress_type=zipfile.ZIP_DEFLATED)
            with zipfile.ZipFile(pending) as out:
                assert all(sha(out.read(p))==digest for p,digest in protected.items())
        except BaseException:pending.unlink(missing_ok=True);raise
    os.replace(pending,pack)
    return report

if __name__=='__main__':
    result=repair();print('Visual repairs:',len(result['wrists']),'capped hand models; MM1 prop, M3 donor and M203 material.')
