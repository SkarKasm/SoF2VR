"""Export current packed weapon surfaces for independent dropped objects.

Reads the owner's already edited meshes; does not regenerate hand poses.
No game, headset or test fixture is launched.
"""
import hashlib,json,os,struct,zipfile
from pathlib import Path
from native_pose import read,pose,vertices
from build_presentation import idle
from build_magazine_assets import multipart

ROOT=Path(__file__).resolve().parents[1]
def build():
    pack=ROOT/'resources/z_sof2vr_native.pk3'
    rows=json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons']
    entries={};report=[]
    with zipfile.ZipFile(pack) as z:
        paths={p.lower():p for p in z.namelist()}
        protected={p:hashlib.sha256(z.read(p)).hexdigest() for p in z.namelist() if 'sof2vr_grips/' in p or p.lower().endswith('.glm')}
        for row in rows:
            if row['id'] not in list(range(1,14))+[15,16]+list(range(18,26))+[35,36]:continue
            key=row['viewModel'];data=z.read(paths[key.lower()]) if key.lower() in paths else read(key)
            rest=idle(row['animations']['sequences'],Path(key).stem)
            world=pose(read(row['skeleton']['path']),rest['startFrame'])[0]
            h=struct.unpack_from('<4si64s64s7i',data);groups={}
            for i in range(h[8]):
                at=164+struct.unpack_from('<i',data,164+4*i)[0]
                name=data[at:at+64].split(b'\0')[0].decode().lower()
                flags=struct.unpack_from('<I',data,at+64)[0]
                shader=data[at+68:at+132].split(b'\0')[0].decode()
                if flags&2 or '_off' in name or name.startswith(('tag_','lhand','rhand')):continue
                verts,tris=vertices(data,i,world)
                if not tris:continue
                shader=shader if shader.startswith('sof2vr') else 'sof2vr/'+shader
                out=groups.setdefault(shader,[[],[],[],[]]);offset=len(out[0])
                for point,normal,uv,*unused in verts:
                    out[0].append(point);out[1].append(normal);out[2].append(uv)
                out[3].extend(tuple(v+offset for v in tri) for tri in tris)
            if not groups:raise ValueError('No visible weapon geometry: '+key)
            path=f"models/sof2vr_dropped/w{row['id']}.md3"
            points=[p for group in groups.values() for p in group[0]]
            lo=[min(p[k] for p in points) for k in range(3)];hi=[max(p[k] for p in points) for k in range(3)]
            centre=[(lo[k]+hi[k])*.5 for k in range(3)];half=[max(.25,(hi[k]-lo[k])*.5) for k in range(3)]
            entries[path]=multipart(groups,[0,0,0]);report.append(dict(id=row['id'],path=path,surfaces=len(groups),centre=centre,half=half))
        pending=pack.with_suffix('.drop-pending')
        try:
            with zipfile.ZipFile(pending,'w',compression=zipfile.ZIP_DEFLATED) as out:
                for info in z.infolist():
                    if not info.filename.startswith('models/sof2vr_dropped/') and info.filename!='sof2vr-dropped-weapons.json':out.writestr(info,z.read(info))
                for name,data in entries.items():out.writestr(name,data)
                out.writestr('sof2vr-dropped-weapons.json',json.dumps(report,separators=(',',':')))
            with zipfile.ZipFile(pending) as check:
                for name,digest in protected.items():
                    if hashlib.sha256(check.read(name)).hexdigest()!=digest:raise ValueError('Authored mesh changed: '+name)
        except BaseException:
            pending.unlink(missing_ok=True);raise
    os.replace(pending,pack)
    header=['/* Generated from the packed owner-edited weapon meshes. */','#ifndef SOF2_DROP_PROFILES_H','#define SOF2_DROP_PROFILES_H',
            'typedef struct {int id;float centre[3],half[3];} VRDropProfile;','static const VRDropProfile dropProfiles[]={']
    vector=lambda v:'{'+','.join(f'{n:.8f}f' for n in v)+'}'
    for row in report:header.append(' {%d,%s,%s},'%(row['id'],vector(row['centre']),vector(row['half'])))
    header+=['};','static const VRDropProfile *DropProfile(int id){for(unsigned i=0;i<sizeof(dropProfiles)/sizeof(dropProfiles[0]);++i)if(dropProfiles[i].id==id)return &dropProfiles[i];return 0;}','#endif','']
    (ROOT/'src/vr_drop_profiles.h').write_text('\n'.join(header))
    (ROOT/'build/dropped-weapons.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Exported',len(report),'standalone dropped weapon meshes; packed guns and hands unchanged.')
if __name__=='__main__':build()
