"""Authored magazine and action geometry for detachable-magazine firearms."""
import json,struct
from pathlib import Path
from native_pose import read,pose,vertices
from build_presentation import idle
from build_hand_assets import make_md3
ROOT=Path(__file__).resolve().parents[1]

def multipart(groups,anchor):
 surfaces=[];allpoints=[]
 for shader,(points,normals,uv,triangles) in groups.items():
  if not triangles:continue
  points=[tuple(p[k]-anchor[k] for k in range(3)) for p in points];allpoints+=points
  model=make_md3(points,normals,uv,triangles,shader,'native_reload')
  h=struct.unpack_from('<4si64s9i',model);at=h[10]
  for _ in range(h[6]):
   size=struct.unpack_from('<i',model,at+104)[0];surfaces.append(model[at:at+size]);at+=size
 if not surfaces:raise ValueError('Missing detachable part')
 lo=[min(p[k] for p in allpoints) for k in range(3)];hi=[max(p[k] for p in allpoints) for k in range(3)]
 frame=struct.pack('<10f16s',*lo,*hi,0,0,0,max(sum(v*v for v in p)**.5 for p in allpoints),b'native_reload')
 body=b''.join(surfaces)
 return struct.pack('<4si64s9i',b'IDP3',15,b'sof2_native_reload',0,1,0,len(surfaces),0,108,164,164,164+len(body))+frame+body

def build():
 entries={};report=[];pending=[]
 sides={r['viewModel']:{x['index']:x['base'] for x in r['restored']} for r in json.loads((ROOT/'build/weapon-side-audit.json').read_text())}
 for row in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons']:
  if row['id'] not in [2,3,4,5,7,8,10,11,12,35,36]:continue
  rest=idle(row['animations']['sequences'],Path(row['viewModel']).stem);world=pose(read(row['skeleton']['path']),rest['startFrame'])[0]
  d=read(row['viewModel']);h=struct.unpack_from('<4si64s64s7i',d)
  flags=[struct.unpack_from('<I',d,164+struct.unpack_from('<i',d,164+4*i)[0]+64)[0] for i in range(h[8])]
  groups={p:{} for p in ['body','magazine','bolt']};magSurfaces=[];boltSurfaces=[]
  for i,surface in enumerate(row['surfaces']):
   name=surface['name'].lower();base=sides.get(row['viewModel'],{}).get(i,i)
   if '_off' in name or (flags[i if base<0 else base]&2):continue
   verts,tris=vertices(d,i,world)
   if not tris:continue
   ismag='clip' in name and 'cliphandle' not in name
   isbolt=not ismag and ('slide' in name or 'bolt' in name or 'chamber' in name or name=='forwardassist')
   # Some actions are unnamed but their triangles are weighted to bolt1.
   boltBones={b['index'] for b in row['bones'] if b['name'].lower().startswith('bolt')}
   clipBones={b['index'] for b in row['bones'] if b['name'].lower()=='clip1'}
   alternateClips={b['index'] for b in row['bones'] if b['name'].lower().startswith('clip') and b['name'].lower()!='clip1'}
   if ismag:magSurfaces.append(i)
   for tri in tris:
    primary=sum(sum(verts[v][3].get(b,0) for b in clipBones) for v in tri)>1.5
    alternate=sum(sum(verts[v][3].get(b,0) for b in alternateClips) for v in tri)>1.5
    magazine=primary or (ismag and not alternate)
    action=not magazine and (isbolt or sum(sum(verts[v][3].get(b,0) for b in boltBones) for v in tri)>1.5)
    part='magazine' if magazine else 'bolt' if action else 'body'
    if magazine and i not in magSurfaces:magSurfaces.append(i)
    if action and i not in boltSurfaces:boltSurfaces.append(i)
    out=groups[part].setdefault('sof2vr/'+surface['shader'],[[],[],[],[]]);start=len(out[0])
    for v in tri:
     point,normal,uv,weights,bind=verts[v];out[0].append(point);out[1].append(normal);out[2].append(uv)
    out[3].append((start,start+1,start+2))
  def centre(part):
   points=[p for group in groups[part].values() for p in group[0]]
   if not points:raise ValueError('Missing '+part+' for '+row['name'])
   return [(min(p[k] for p in points)+max(p[k] for p in points))*.5 for k in range(3)]
  missing=[part for part in groups if not any(group[3] for group in groups[part].values())]
  if missing:
   pending.append(dict(id=row['id'],name=row['name'],reason='Native action/bolton geometry needs separate extraction',missing=missing));continue
  socket=centre('magazine');action=centre('bolt');prefix=f"models/sof2vr_reload/w{row['id']}"
  for part in groups:entries[prefix+'_'+part+'.md3']=multipart(groups[part],socket if part=='magazine' else [0,0,0])
  report.append(dict(id=row['id'],model=row['viewModel'],prefix=prefix,socket=socket,bolt=action,magSurfaces=magSurfaces,pose=rest['name']))
 # The existing physically evaluated M4 parts/anchors remain unchanged.
 import re
 old=(ROOT/'src/reload_parts.h').read_text()
 def oldanchor(name):return [float(v.rstrip('f')) for v in re.search(name+r'\[3\]=\{([^}]+)',old)[1].split(',')]
 report.append(dict(id=9,model='models/weapons/m4/m4.glm',prefix='models/sof2vr_reload/m4',socket=oldanchor('m4MagazineSocket'),bolt=oldanchor('m4BoltSocket'),magSurfaces=[4,5],pose='m4idle.xsi'))
 header=['/* Original authored detachable magazine/action geometry. */','#ifndef SOF2_MAGAZINE_PARTS_H','#define SOF2_MAGAZINE_PARTS_H','typedef struct {int id;const char *model,*prefix;float socket[3],bolt[3];int count,surfaces[16];} VRReloadParts;','static const VRReloadParts vrReloadParts[]={']
 for r in report:
  assert len(r['magSurfaces'])<=16
  header.append(' {%d,"%s","%s",{%s},{%s},%d,{%s}},'%(r['id'],r['model'],r['prefix'],','.join('%.7ff'%v for v in r['socket']),','.join('%.7ff'%v for v in r['bolt']),len(r['magSurfaces']),','.join(map(str,r['magSurfaces']))))
 header+=['};','static inline const VRReloadParts *ReloadParts(int id){for(unsigned i=0;i<sizeof(vrReloadParts)/sizeof(vrReloadParts[0]);++i)if(vrReloadParts[i].id==id)return &vrReloadParts[i];return 0;}','#endif','']
 (ROOT/'src/magazine_parts.h').write_text('\n'.join(header));metadata=dict(ready=report,pending=pending)
 entries['sof2vr-magazine-parts.json']=(json.dumps(metadata,indent=2)+'\n').encode()
 print('Prepared',len(report),'manual magazine profiles;',len(pending),'require separate native bolton/action extraction.')
 return entries,metadata
