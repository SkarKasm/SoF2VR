"""Original M590 receiver, moving fore-end, and shell reference geometry."""
from pathlib import Path
import json
from native_pose import read,pose,vertices
from build_presentation import idle
from build_magazine_assets import multipart
ROOT=Path(__file__).resolve().parents[1]

def build():
 row=next(r for r in json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons'] if r['id']==6)
 rest=idle(row['animations']['sequences'],'m590');world=pose(read(row['skeleton']['path']),rest['startFrame'])[0];data=read(row['viewModel'])
 groups={p:{} for p in ['body','pump']};receiver=[];foreend=[]
 for i,s in enumerate(row['surfaces']):
  vs,tris=vertices(data,i,world);part='pump' if 'pump' in s['name'].lower() else 'body'
  if s['name'].lower()=='m590body':receiver=[v[0] for v in vs]
  if part=='pump':foreend.extend(v[0] for v in vs)
  out=groups[part].setdefault('sof2vr/'+s['shader'],[[],[],[],[]]);base=len(out[0])
  for point,normal,uv,weights,bind in vs:out[0].append(point);out[1].append(normal);out[2].append(uv)
  out[3].extend(tuple(base+v for v in t) for t in tris)
 def bounds(points):return [[fn(p[k] for p in points) for k in range(3)] for fn in [min,max]]
 low,high=bounds(receiver);pumpLow,pumpHigh=bounds(foreend)
 # Feed port lies on the underside of the original receiver. This uses an
 # explicitly inferred underside anchor, not a claimed authored bolt/tag.
 port=[(low[0]+high[0])*.5,(low[1]+high[1])*.5,low[2]]
 pump=[(a+b)*.5 for a,b in zip(pumpLow,pumpHigh)]
 entries={f'models/sof2vr_loading/m590_{part}.md3':multipart(g,[0,0,0]) for part,g in groups.items()}
 entries['models/sof2vr_loading/shell.md3']=read('models/weapons/shells/shell_shotgun.md3')
 header=['/* Original M590 geometry; loading port inferred from receiver underside. */','#ifndef SOF2_LOADING_PARTS_H','#define SOF2_LOADING_PARTS_H']
 for name,anchor in [('m590LoadPort',port),('m590PumpSocket',pump)]:header.append('static const float '+name+'[3]={'+','.join('%.8ff'%x for x in anchor)+'};')
 header+=['#define M590_PUMP_TRAVEL 3.5433071f /* 9 cm, game units */','#endif','']
 (ROOT/'src/loading_parts.h').write_text('\n'.join(header))
 meta=dict(weapon=6,idle=rest['name'],port=port,portMethod='inferred original receiver underside centre',pump=pump,pumpTravelCentimetres=9,shell='original shell_shotgun.md3',status='source-built; no runtime evaluation')
 entries['sof2vr-shotgun-loading.json']=(json.dumps(meta,indent=2)+'\n').encode();return entries,meta
