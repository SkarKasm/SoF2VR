"""Repair incomplete reload bodies from the exact packed weapon geometry.

Leaves weapons, hands, scales, skeletons and authored poses unchanged. Also
exports magazine bounds for cosmetic drop collision. No game launch.
"""
import hashlib, json, os, struct, zipfile
from pathlib import Path
from native_pose import read, pose, vertices
from build_presentation import idle
from build_magazine_assets import multipart

ROOT=Path(__file__).resolve().parents[1]

def md3_surfaces(data):
    header=struct.unpack_from('<4si64s9i',data);offset=header[10]
    for _ in range(header[6]):
        surface=struct.unpack_from('<4s64s10i',data,offset)
        shader=data[offset+surface[8]:offset+surface[8]+64].split(b'\0')[0].decode()
        yield shader,surface[6]
        offset+=surface[-1]

def repair():
    pack=ROOT/'resources/z_sof2vr_native.pk3';changes={};report=[];bounds=[]
    rows={w['id']:w for w in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons']}
    with zipfile.ZipFile(pack) as z:
        reloads=json.loads(z.read('sof2vr-magazine-parts.json'))['ready']
        replacements={w['id']:w for w in json.loads(z.read('sof2vr-weapon-replacements.json'))['ready']}
        for reload in reloads:
            mag=z.read(reload['prefix']+'_magazine.md3');header=struct.unpack_from('<4si64s9i',mag)
            frame=struct.unpack_from('<6f',mag,header[8])
            half=[max(abs(frame[k]),abs(frame[k+3])) for k in range(3)]
            bounds.append((reload['id'],half))
            replacement=replacements.get(reload['id'])
            if not replacement:continue
            row=rows[reload['id']];data=z.read(reload['model']);h=struct.unpack_from('<4si64s64s7i',data)
            rest=idle(row['animations']['sequences'],Path(row['viewModel']).stem)
            skeleton=row['skeleton']['path'];world=pose(z.read(skeleton) if skeleton in z.namelist() else read(skeleton),rest['startFrame'])[0]
            groups={};expected={}
            for index in range(h[8]):
                at=164+struct.unpack_from('<i',data,164+4*index)[0]
                name=data[at:at+64].split(b'\0')[0].decode()
                part=next((part for part in sorted(replacement['parts'],key=len,reverse=True) if name.startswith('vr_'+part+'_')),None)
                if not part or part in ['magazine','bolt']:continue
                shader=data[at+68:at+132].split(b'\0')[0].decode()
                verts,tris=vertices(data,index,world)
                if not tris:continue
                expected[shader]=expected.get(shader,0)+len(tris)
                group=groups.setdefault(shader,[[],[],[],[]]);offset=len(group[0])
                group[0].extend(v[0] for v in verts);group[1].extend(v[1] for v in verts);group[2].extend(v[2] for v in verts)
                group[3].extend([[v+offset for v in tri] for tri in tris])
            path=reload['prefix']+'_body.md3';existing={}
            for shader,count in md3_surfaces(z.read(path)):existing[shader]=existing.get(shader,0)+count
            if not expected or existing==expected:continue
            # Weld identical position/normal/UV tuples without changing faces,
            # seams or smoothing. Keep legacy MD3 surface counts bounded.
            for shader,group in groups.items():
                unique={};merged=[[],[],[],[]];indices={}
                for i,(p,n,uv) in enumerate(zip(*group[:3])):
                    key=tuple(round(v,6) for values in [p,n,uv] for v in values)
                    if key not in unique:
                        unique[key]=len(merged[0]);merged[0].append(p);merged[1].append(n);merged[2].append(uv)
                    indices[i]=unique[key]
                merged[3]=[[indices[v] for v in tri] for tri in group[3]];groups[shader]=merged
            output=multipart(groups,[0,0,0]);changes[path]=output
            report.append(dict(id=reload['id'],path=path,beforeTriangles=sum(existing.values()),afterTriangles=sum(expected.values()),sha256=hashlib.sha256(output).hexdigest()))
        if changes:
            changes['sof2vr-reload-body-repair.json']=(json.dumps(dict(repairs=report,source='Exact final packed viewmodels; hands and weapon meshes unchanged'),indent=2)+'\n').encode()
            pending=pack.with_suffix('.repair-pending')
            try:
                with zipfile.ZipFile(pending,'w') as out:
                    for entry in z.infolist():out.writestr(entry,changes.pop(entry.filename,z.read(entry)))
                    for name,data in changes.items():
                        entry=zipfile.ZipInfo(name,date_time=(2002,1,1,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED;out.writestr(entry,data)
            except BaseException:
                pending.unlink(missing_ok=True);raise
    if report:os.replace(pending,pack)
    text='/* Packed magazine half bounds, game units; generated, do not edit. */\n#ifndef SOF2_MAGAZINE_BOUNDS_H\n#define SOF2_MAGAZINE_BOUNDS_H\n'
    text+='typedef struct {int id;float half[3];} VRMagazineBounds;\nstatic const VRMagazineBounds vrMagazineBounds[]={\n'
    for id,half in bounds:text+=' {%d,{%s}},\n'%(id,','.join('%.8ff'%v for v in half))
    text+='};\n#endif\n';path=ROOT/'src/magazine_bounds.h'
    if not path.exists() or path.read_text()!=text:path.write_text(text)
    return report

if __name__=='__main__':print(json.dumps(repair(),indent=2))
