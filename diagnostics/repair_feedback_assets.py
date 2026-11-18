"""Bounded M3 donor-action and quiet knife-swing repair; never launches a game.

Only the M3 receiver/action, its reload meshes and knife swing sounds change.
Owner-authored hand meshes, weapon transforms and scale overrides are retained.
"""
import hashlib,io,json,os,struct,wave,zipfile
from pathlib import Path
from native_pose import read,pose,vertices
from build_presentation import idle
from build_magazine_assets import multipart
from build_weapon_replacements import apply,inverse,unit,cross,sub,centre

ROOT=Path(__file__).resolve().parents[1]

def append_surfaces(data,surfaces,world):
    """Append rigid donor surfaces, preserving every existing surface index."""
    h=list(struct.unpack_from('<4si64s64s7i',data));old=h[8];count=old+len(surfaces)
    if count>64:raise ValueError('M3 donor exceeds Ghoul2 surface budget')
    hierarchy=[]
    for i in range(old):
        at=164+struct.unpack_from('<i',data,164+4*i)[0];children=struct.unpack_from('<i',data,at+140)[0]
        row=bytearray(data[at:at+144+4*children])
        if i==0:struct.pack_into('<i',row,140,children+len(surfaces));row+=struct.pack('<'+'i'*len(surfaces),*range(old,count))
        hierarchy.append(row)
    for n,(part,shader,bone,g) in enumerate(surfaces):hierarchy.append(struct.pack('<64sI64s3i',f'vr_{part}_{old+n}'.encode(),0,shader.encode(),0,0,0))
    offsets=[];at=4*count
    for row in hierarchy:offsets.append(at);at+=len(row)
    tree=struct.pack('<'+'i'*count,*offsets)+b''.join(hierarchy);start=164+len(tree)
    body=[];offsets=[];at=4+4*count
    for i in range(old):
        p=h[7]+4+struct.unpack_from('<i',data,h[7]+4+4*i)[0];s=struct.unpack_from('<10i',data,p);row=bytearray(data[p:p+s[9]])
        struct.pack_into('<i',row,8,-(start+at));offsets.append(at-4);body.append(row);at+=len(row)
    for n,(part,shader,bone,(points,normals,uv,tris)) in enumerate(surfaces):
        inv=inverse(world[bone]);td=b''.join(struct.pack('<3i',*t) for t in tris);vd=bytearray()
        for p,normal in zip(points,normals):vd+=struct.pack('<6fI4B',*unit(apply(inv,normal,True)),*apply(inv,p),0,0,0,0,0)
        st=b''.join(struct.pack('<2f',*v) for v in uv);vo=40+len(td);refs=vo+len(vd)+len(st);end=refs+4
        row=struct.pack('<10i',0,old+n,-(start+at),len(points),vo,len(tris),40,1,refs,end)+td+vd+st+struct.pack('<i',bone)
        offsets.append(at-4);body.append(row);at+=len(row)
    lod=struct.pack('<i',at)+struct.pack('<'+'i'*count,*offsets)+b''.join(body)
    h[6]=1;h[7]=start;h[8]=count;h[9]=164+4*count;h[10]=start+len(lod)
    return struct.pack('<4si64s64s7i',*h)+tree+lod

def repair_m3(z,changes):
    marker='sof2vr-m3-action-repair.json'
    if marker in z.namelist():return json.loads(z.read(marker))
    reloads=json.loads(z.read('sof2vr-magazine-parts.json'));r=next(x for x in reloads['ready'] if x['id']==8)
    replacements=json.loads(z.read('sof2vr-weapon-replacements.json'));rep=next(x for x in replacements['ready'] if x['id']==8)
    row=next(x for x in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons'] if x['id']==8)
    frame=idle(row['animations']['sequences'],'m3a1')['startFrame'];native=read(row['viewModel'])
    world=pose(z.read(row['skeleton']['path']) if row['skeleton']['path'] in z.namelist() else read(row['skeleton']['path']),frame)[0]
    matrix=rep['calibration']['matrix'];forward=unit([-matrix[i][0] for i in range(3)]);up=unit([matrix[i][2] for i in range(3)]);side=unit(cross(up,forward));basis=[forward,side,up]
    port=apply(matrix,[-.22,.14,2.16]);outward=unit([matrix[i][1] for i in range(3)])
    boltVerts,boltTris=vertices(native,0,pose(read(row['skeleton']['path']),frame)[0])
    boltTris=[t for t in boltTris if sum(boltVerts[i][3].get(4,0) for i in t)>1.5]
    donorCentre=centre([boltVerts[i][0] for t in boltTris for i in t])
    def transplant(vs,tris,target,scale):
        g=[[],[],[],[]]
        for t in tris:
            first=len(g[0])
            for i in t:
                p,n,uv=vs[i][:3];d=sub(p,donorCentre)
                g[0].append([target[j]+scale*sum(basis[k][j]*d[k] for k in range(3)) for j in range(3)])
                g[1].append(unit([sum(basis[k][j]*n[k] for k in range(3)) for j in range(3)]));g[2].append(uv)
            g[3].append([first,first+1,first+2])
        return g
    shader='models/weapons/m3a1/m3a1'
    inner=[port[i]-outward[i]*.45 for i in range(3)]
    bolt=transplant(boltVerts,boltTris,inner,.82)
    # Reuse the native chamber's textured inner face as a temporary cavity
    # backing. It remains inside the replacement receiver, not on its cover.
    vs,ts=vertices(native,2,pose(read(row['skeleton']['path']),frame)[0]);linerCentre=centre([v[0] for v in vs])
    shifted=[([v[0][k]-linerCentre[k]+donorCentre[k] for k in range(3)],*v[1:]) for v in vs]
    liner=transplant(shifted,ts,[port[i]-outward[i]*.8 for i in range(3)],.8)
    # A cavity can be inspected from either side in VR.
    for g in [bolt,liner]:g[3]+=[list(reversed(t)) for t in list(g[3])]
    data=bytearray(z.read(r['model']));h=struct.unpack_from('<4si64s64s7i',data);cut=0
    invMatrix=inverse(matrix)
    for i in range(h[8]):
        at=164+struct.unpack_from('<i',data,164+4*i)[0];name=data[at:at+64].split(b'\0')[0].decode()
        p=h[7]+4+struct.unpack_from('<i',data,h[7]+4+4*i)[0];s=struct.unpack_from('<10i',data,p)
        if name.startswith('vr_bolt_'):struct.pack_into('<i',data,p+20,0)
        elif name.startswith('vr_body_'):
            vs,ts=vertices(bytes(data),i,world);keep=[]
            for t in ts:
                c=[sum(vs[n][0][k] for n in t)/3 for k in range(3)];raw=apply(invMatrix,c)
                normal=[sum(vs[n][1][k] for n in t)/3 for k in range(3)]
                if -.82<raw[0]<.38 and 2.0<raw[2]<2.30 and raw[1]>.07 and sum(normal[k]*outward[k] for k in range(3))>.25:cut+=1
                else:keep.append(t)
            struct.pack_into('<i',data,p+20,len(keep))
            for n,t in enumerate(keep):struct.pack_into('<3i',data,p+s[6]+12*n,*t)
    changes[r['model']]=append_surfaces(bytes(data),[('bolt',shader,4,bolt),('donor_interior',shader,1,liner)],world)
    changes[r['prefix']+'_bolt.md3']=multipart({shader:bolt},[0,0,0])
    r['bolt']=port;rep['sockets']['bolt']=port
    if 'donor_interior' not in rep['parts']:rep['parts'].append('donor_interior')
    changes['sof2vr-magazine-parts.json']=(json.dumps(reloads,indent=2)+'\n').encode()
    changes['sof2vr-weapon-replacements.json']=(json.dumps(replacements,indent=2)+'\n').encode()
    report=dict(version=1,weapon=8,source=row['viewModel'],sourceSHA256=hashlib.sha256(native).hexdigest(),
                boltTriangles=len(bolt[3]),linerTriangles=len(liner[3]),receiverFacesOpened=cut,actionAnchor=port,
                policy='Original SoF2 M3 donor bolt/chamber UVs, fitted inside the final calibrated receiver; hand/weapon scale unchanged')
    changes[marker]=(json.dumps(report,indent=2)+'\n').encode();return report

def repair():
    pack=ROOT/'resources/z_sof2vr_native.pk3';changes={}
    with zipfile.ZipFile(pack) as z:
        report=repair_m3(z,changes)
        buf=io.BytesIO()
        with wave.open(buf,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(22050);w.writeframes(b'\0\0'*220)
        for sound in ['swing01','swing02','swing03','slash01','slash02']:
            name=f'sound/weapons/knife/{sound}.wav';b=buf.getvalue()
            if name not in z.namelist() or z.read(name)!=b:changes[name]=b
        if changes:
            pending=pack.with_suffix('.feedback-pending')
            try:
                with zipfile.ZipFile(pending,'w') as out:
                    for entry in z.infolist():out.writestr(entry,changes.pop(entry.filename,z.read(entry)))
                    for name,b in changes.items():
                        entry=zipfile.ZipInfo(name,date_time=(2002,1,1,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED;out.writestr(entry,b)
            except BaseException:pending.unlink(missing_ok=True);raise
        else:pending=None
    if pending:os.replace(pending,pack)
    # Keep the runtime anchors in step with the patched asset metadata.
    with zipfile.ZipFile(pack) as z:rows=json.loads(z.read('sof2vr-magazine-parts.json'))['ready']
    text='/* Packed detachable magazines/actions, including native M3 donor repair. */\n#ifndef SOF2_MAGAZINE_PARTS_H\n#define SOF2_MAGAZINE_PARTS_H\n'
    text+='typedef struct {int id;const char *model,*prefix;float socket[3],bolt[3];int count,surfaces[16];} VRReloadParts;\nstatic const VRReloadParts vrReloadParts[]={\n'
    for r in rows:text+=' {%d,"%s","%s",{%s},{%s},%d,{%s}},\n'%(r['id'],r['model'],r['prefix'],','.join('%.7ff'%v for v in r['socket']),','.join('%.7ff'%v for v in r['bolt']),len(r['magSurfaces']),','.join(map(str,r['magSurfaces'])))
    text+='};\nstatic inline const VRReloadParts *ReloadParts(int id){for(unsigned i=0;i<sizeof(vrReloadParts)/sizeof(vrReloadParts[0]);++i)if(vrReloadParts[i].id==id)return &vrReloadParts[i];return 0;}\n#endif\n'
    header=ROOT/'src/magazine_parts.h'
    if not header.exists() or header.read_text()!=text:header.write_text(text)
    return report

if __name__=='__main__':print(json.dumps(repair(),indent=2))
