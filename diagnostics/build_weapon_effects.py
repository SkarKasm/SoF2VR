"""World-FX locators in the same model frame as fitted guns and hands."""
import json,struct
from pathlib import Path
from native_pose import read,pose,bolt
from build_weapon_profiles import parse,value
from build_presentation import idle
ROOT=Path(__file__).resolve().parents[1]
def build(entries,replacements,config,rows):
 from build_weapon_replacements import apply,unit,frame
 definitions={value(b,'name').lower():b for k,b in parse(read('inview/SOF2.inview').decode('cp1252')) if k=='weapon'}
 attacks={value(b,'name').lower():value(b,'attack',[]) for k,b in parse(read('ext_data/SOF2.wpn').decode('cp1252')) if k=='weapon'}
 fitted={w['id']:w for w in replacements};settings={w['id']:w for w in config};result=[];alternate=[]
 for id,w in settings.items():
  if id in fitted and 'alternateFlashPoint' in w:
   transform=fitted[id]['calibration']['matrix']
   alternate.append(dict(id=id,point=apply(transform,w['alternateFlashPoint']),direction=unit(apply(transform,w['forward'],True)),up=unit(apply(transform,w['up'],True))))
 for id,row in rows.items():
  definition=definitions.get(row['name'].lower());rest=idle(row['animations']['sequences'],Path(row['viewModel']).stem)
  if not definition or not rest:continue
  buffer=value(value(definition,'weaponmodel',[]),'buffer',[])
  if not value(buffer,'model'):continue
  model=read(value(buffer,'model'));path=struct.unpack_from('<4si64s64s7i',model)[3].split(b'\0')[0].decode()+'.gla'
  wp=pose(read(row['skeleton']['path']),rest['startFrame']);bp=pose(read(path),0,bolt(wp,value(buffer,'bolttobone')))
  stem=Path(row['viewModel']).stem.lower()
  requested=value(attacks.get(row['name'].lower(),[]),'EjectBone','ejection_'+stem).lower()
  tag=next((n for n in bp[2] if n.lower()==requested),None)
  if not tag:continue
  native=bolt(bp,tag);point=[r[3] for r in native];direction=unit([r[0] for r in native]);up=unit([r[2] for r in native]);method='native buffer ejection bone'
  w=settings.get(id);fit=fitted.get(id)
  if w and fit:
   transform=fit['calibration']['matrix'];direction=unit([-x for x in apply(transform,frame(w['forward'],w['up'])[1],True)]);up=unit(apply(transform,w['up'],True))
   if 'ejectionPoint' in w:point=apply(transform,w['ejectionPoint']);method='reviewed source ejection-port locator'
   elif 'bolt' in fit['sockets']:point=fit['sockets']['bolt'];method='converted source action-mesh locator'
   else:continue # Do not retain a mismatched flatscreen locator on a replacement.
  if not (w and fit):
   from manual_hand_pose import native_effect_point
   point=native_effect_point(id,point)
  result.append(dict(id=id,point=point,direction=direction,up=up,tag=tag,method=method))
 floats=lambda v:','.join(f'{x:.8f}f' for x in v)
 lines=['/* Generated weapon-local FX locators; never camera offsets. */','#ifndef SOF2_WEAPON_EFFECTS_H','#define SOF2_WEAPON_EFFECTS_H','typedef struct {int id;float point[3],direction[3],up[3];} VRWeaponEjection;','static const VRWeaponEjection vrWeaponEjections[]={']
 lines += ['{%d,{%s},{%s},{%s}},'%(r['id'],floats(r['point']),floats(r['direction']),floats(r['up'])) for r in result]
 lines += ['};','static const VRWeaponEjection vrWeaponAlternateFlashes[]={']
 lines += ['{%d,{%s},{%s},{%s}},'%(r['id'],floats(r['point']),floats(r['direction']),floats(r['up'])) for r in alternate]
 lines += ['};','#endif',''];(ROOT/'src/weapon_effects.h').write_text('\n'.join(lines))
 entries['sof2vr-weapon-effects.json']=(json.dumps(result,indent=2)+'\n').encode()
 entries['sof2vr-alternate-flashes.json']=(json.dumps(alternate,indent=2)+'\n').encode()
 return result
