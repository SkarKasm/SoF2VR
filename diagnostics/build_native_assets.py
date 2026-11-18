"""Prepare original SoF2 first-person hand meshes and stock assets.
Original archives stay untouched. Generated SoF2 content remains local.
"""
from pathlib import Path
import hashlib, json, struct, zipfile, math

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT.parent / 'Soldier of Fortune 2/base'
OUTPUT = ROOT / 'resources/z_sof2vr_native.pk3'

def tracked_glove(data, side, curl=0):
    from native_pose import pose,bolt,read,vertices,IDENTITY,ROOT as handRoot
    from build_reload_assets import compose,transform
    from build_weapon_profiles import parse,value
    gla=read(f'skeletons/weapons/{side}hand/{side}hand.gla')
    sequences=parse(read(f'skeletons/weapons/{side}hand/{side}hand.frames').decode('cp1252'))
    frame=next(int(value(b,'startframe')) for k,b in sequences if k.replace('\\','/').split('/')[-1].lower()==side+'m4idle.xsi')
    evaluated=pose(gla,frame,IDENTITY);palm=bolt(evaluated,side+'_metacarple')
    # Remove the authored wrist placement while retaining finger articulation.
    # This keeps the controller wrist fixed throughout the pressure blend.
    inverse=[[palm[j][i] for j in range(3)]+[-sum(palm[j][i]*palm[j][3] for j in range(3))] for i in range(3)]
    index=evaluated[2].index(side+'_metacarple');align=compose(evaluated[1][index],inverse)
    closed=[compose(handRoot,compose(align,m)) for m in evaluated[0]]
    header=struct.unpack_from('<4si64s64s7i',data);lod=header[7]
    # Complete native hand and cuff, including authored back/underside surfaces.
    points=[];normals=[];uv=[];triangles=[];surface_names=[];binds=[]
    for index in range(3):
        posed,_=vertices(data,index,closed)
        surface=lod+4+struct.unpack_from('<i',data,lod+4+index*4)[0]
        h=struct.unpack_from('<10i',data,surface);base=len(points)
        hierarchy=164+struct.unpack_from('<i',data,164+index*4)[0]
        surface_names.append(data[hierarchy:hierarchy+64].split(b'\0')[0].decode('ascii'))
        for v in range(h[3]):
            nx,ny,nz,x,y,z=struct.unpack_from('<6f',data,surface+h[4]+32*v)
            bind=(-y,x,z);normal=(-ny,nx,nz);binds.append((x,y,z))
            points.append(tuple(bind[k]+(posed[v][0][k]-bind[k])*curl for k in range(3)))
            normals.append(tuple(normal[k]+(posed[v][1][k]-normal[k])*curl for k in range(3)))
            uv.append(struct.unpack_from('<2f',data,surface+h[4]+32*h[3]+8*v))
        for n in range(h[5]):
            tri=tuple(base+v for v in struct.unpack_from('<3i',data,surface+h[6]+n*12))
            triangles.append(tri)
    from close_glove_wrist import close_wrist,CUFF_PLANE
    from build_hand_assets import make_md3
    points,normals,uv,triangles=close_wrist(points,normals,uv,triangles,binds)
    from native_hand_size import points_size
    from build_weapon_grips import palm_points
    points=points_size(points,palm_points(data,[handRoot for _ in evaluated[2]]),side)
    model=make_md3(points,normals,uv,triangles,'models/hands/hands',side)
    from native_hand_size import LEFT_SCALE
    return model,{'vertices':len(points),'triangles':len(triangles),'surfaces':surface_names,'trimmed':True,'wristPlane':CUFF_PLANE,'wristClosed':True,'uniformHandScale':LEFT_SCALE if side=='l' else 1.,'pose':'native bind to authored M4 finger curl; palm frame held fixed','curl':curl}

def glove_only(data):
    data=bytearray(data)
    header=struct.unpack_from('<4si64s64s7i',data)
    assert header[0]==b'2LGM' and header[1]==6 and header[10]==len(data)
    lod_count,lod_offset,surface_count=header[6:9]
    assert 1<=lod_count<=8 and 1<=surface_count<=64
    removed=[]
    for index in range(surface_count):
        hierarchy=164+struct.unpack_from('<i',data,164+index*4)[0]
        name=bytes(data[hierarchy:hierarchy+64]).split(b'\0')[0].decode('ascii')
        hide='sleeve' in name or 'button' in name
        if hide:
            removed.append(name)
            flags=struct.unpack_from('<I',data,hierarchy+64)[0]
            struct.pack_into('<I',data,hierarchy+64,flags|2)
        lod=lod_offset
        for _ in range(lod_count):
            lod_end=struct.unpack_from('<i',data,lod)[0]
            surface=lod+4+struct.unpack_from('<i',data,lod+4+index*4)[0]
            h=struct.unpack_from('<10i',data,surface)
            assert h[1]==index and surface+h[2]==0 and surface+h[9]<=len(data)
            # Keep hierarchy, skeleton, vertices and offsets intact. Empty
            # triangle sets remain invisible even if game code enables a sleeve.
            if hide:struct.pack_into('<i',data,surface+20,0)
            else:
                # The default glove itself includes a long tapered wrist
                # extension. Retain only triangles entirely on the hand side
                # of its bind-space wrist plane, across every LOD/skin variant.
                kept=[]
                for triangle in range(h[5]):
                    indices=struct.unpack_from('<3i',data,surface+h[6]+triangle*12)
                    assert all(0<=v<h[3] for v in indices)
                    if all(struct.unpack_from('<f',data,surface+h[4]+v*32+16)[0]<=2.7 for v in indices):kept.append(indices)
                for triangle,indices in enumerate(kept):struct.pack_into('<3i',data,surface+h[6]+triangle*12,*indices)
                struct.pack_into('<i',data,surface+20,len(kept))
            lod+=lod_end
    assert len(removed)==3
    return bytes(data),removed

def build():
    (ROOT/'build/candidate-rebuild.pending').write_text('Unreviewed generated grip/weapon assets. Complete multi-angle render review before installation.\n')
    entries={};manifest={'policy':'Original SoF2 hands, licensed replacement weapons. No RealRTCW hand assets. Original game content remains local; third-party assets retain their individual licenses.','models':[]}
    for side in ['l','r']:
        name=f'models/weapons/{side}hand/{side}hand.glm'
        for archive in ['update101.PK3','models.pk3']:
            with zipfile.ZipFile(BASE/archive) as source:
                if name not in source.namelist():continue
                original=source.read(name)
                # Keep the game's topology, UVs, materials, skeleton and LODs intact.
                modified,surfaces=glove_only(original)
                entries[name]=modified
                model,details=tracked_glove(original,side)
                entries[f'models/sof2vr_native/{side}hand.md3']=model
                for step in range(17):
                    animated,_=tracked_glove(original,side,step/16)
                    entries[f'models/sof2vr_native/{side}hand_grip{step:02d}.md3']=animated
                details['pressureSteps']=17
                manifest['models'].append({'path':name,'sourceArchive':archive,'sourceSHA256':hashlib.sha256(original).hexdigest(),'outputSHA256':hashlib.sha256(modified).hexdigest(),'hiddenSurfaces':surfaces})
                manifest['models'][-1]['trackedGlove']=details
                break
        else:raise RuntimeError(f'Missing original SoF2 model: {name}')
    from build_reload_assets import generate
    reloadEntries,reloadMeta=generate();entries.update(reloadEntries);manifest['manualReload']=reloadMeta
    from build_magazine_assets import build as magazines
    magazineEntries,magazineMeta=magazines();entries.update(magazineEntries);manifest['magazineReloads']=magazineMeta
    from build_shotgun_assets import build as shotguns
    shotgunEntries,shotgunMeta=shotguns();entries.update(shotgunEntries);manifest['shotgunLoading']=shotgunMeta
    from build_launcher_sides import build as launcher_sides
    launcherEntries,launcherMeta=launcher_sides();entries.update(launcherEntries);manifest['launcherSides']=launcherMeta
    from build_presentation import build as presentation
    presentationEntries,presentationMeta=presentation();entries.update(presentationEntries);manifest['presentation']=presentationMeta
    from build_weapon_materials import build as materials
    manifest['weaponMaterials']=materials(entries)
    from build_weapon_grips import generate as grips
    gripEntries,gripMeta=grips(with_assets=True);entries.update(gripEntries);manifest['gripPoses']=gripMeta
    from build_weapon_replacements import build as replacements
    manifest['weaponReplacements']=replacements(entries)
    from build_body_assets import build as body
    bodyEntries,bodyMeta=body(entries);entries.update(bodyEntries);manifest['bodyPrototype']=bodyMeta
    manifest['gripPoses']=json.loads(entries['sof2vr-grip-poses.json'])
    manifest['magazineReloads']=json.loads(entries['sof2vr-magazine-parts.json'])
    manifest['manualReload']=json.loads(entries['sof2vr-reload-parts.json'])
    manifest['shotgunLoading']=json.loads(entries['sof2vr-shotgun-loading.json'])
    entries['sof2vr-native-assets.json']=(json.dumps(manifest,indent=2)+'\n').encode()
    temporary=OUTPUT.with_suffix('.pk3.tmp')
    with zipfile.ZipFile(temporary,'w',compression=zipfile.ZIP_DEFLATED) as archive:
        for name,data in sorted(entries.items()):
            info=zipfile.ZipInfo(name,date_time=(2002,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
            archive.writestr(info,data)
    temporary.replace(OUTPUT)
    from repair_feedback_assets import repair as repair_feedback_assets
    repair_feedback_assets()
    from repair_reload_bodies import repair as repair_reload_bodies
    repair_reload_bodies()
    from build_magazine_interactions import generate as magazine_interactions
    magazine_interactions()
    from repair_texture_formats import repair as repair_texture_formats
    repair_texture_formats()
    from build_knife_collision import generate as knife_collision
    knife_collision()
    from repair_visual_finishes import repair as visual_finishes
    visual_finishes()
    from build_m4_materials import build as m4_materials
    m4_materials()
    from repair_m3_port import repair as clean_m3_port
    clean_m3_port()
    from repair_action_anchors import repair as action_contacts
    action_contacts()
    repair_reload_bodies()
    print(f'Built SoF2 VR hand/weapon pack: {OUTPUT}')

if __name__=='__main__':build()
