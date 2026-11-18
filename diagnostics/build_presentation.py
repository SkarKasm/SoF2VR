"""Local presentation overrides: M3 flash restored, no fire-root kick or idle fidgets.
Authored timing/notetracks are retained; camera kick is disabled separately.
"""
from pathlib import Path
import zipfile,json,struct,hashlib,re
from functools import lru_cache
ROOT=Path(__file__).resolve().parents[1];BASE=ROOT.parent/'Soldier of Fortune 2/base'
@lru_cache(maxsize=1)
def authored_idles():
 from build_weapon_profiles import parse,value
 result={}
 for archive in ['update103.pk3','update102.pk3','update101.PK3','therest.pk3']:
  with zipfile.ZipFile(BASE/archive) as z:
   if 'inview/SOF2.inview' not in z.namelist():continue
   for key,weapon in parse(z.read('inview/SOF2.inview').decode('cp1252')):
    if key!='weapon' or not isinstance(weapon,list):continue
    model=value(value(weapon,'weaponmodel',[]),'model');stem=Path(model).stem.lower()
    for key,group in weapon:
     if key!='anim' or not isinstance(group,list) or value(group,'name')!='idle':continue
     for key,info in group:
      if key=='info' and isinstance(info,list) and value(info,'type')=='weaponmodel' and value(info,'name')=='mainwpn':result[stem]=value(info,'anim').lower()+'.xsi'
   return result
 return result

def idle(sequences,stem):
 authored=authored_idles().get(stem.lower())
 found=next((a for a in sequences if a['name'].lower()==authored),None)
 if found:return found
 candidates=[a for a in sequences if 'idle' in a['name'].lower() and not any(s in a['name'].lower() for s in ['empty','prone','crawl'])]
 # Pistols contain separate left/right presentations. The right-hand native
 # model is the dominant assembly; left-handed VR reflects that assembly.
 for suffix in ['idle.xsi','ridle.xsi']:
  found=next((a for a in candidates if a['name'].lower()==stem.lower()+suffix),None)
  if found:return found
 right=next((a for a in candidates if a['name'].lower().endswith('ridle.xsi')),None)
 if right:return right
 return min(candidates,key=lambda a:len(a['name'])) if candidates else None
def build():
 entries={};report={'muzzleFlashes':[],'animationRoots':[]}
 flashNames=set()
 with zipfile.ZipFile(BASE/'therest.pk3') as z:
  flashNames.update(name for name in z.namelist() if name.endswith('.efx') and (name.startswith('effects/muzzle_flashes/mflash') or name.startswith('effects/muzzle_flash')))
 with zipfile.ZipFile(BASE/'update102.pk3') as z:
  text=z.read('ext_data/SOF2.wpn').decode('cp1252')
  flashNames.update(path+'.efx' for path in re.findall(r'(?im)^\s*(?:3rdPerson)?muzzleFlash\s+"([^"]+)"',text))
 # Weapon-root animation suppression does not disable the client camera's
 # independent random pitch/yaw kick. Preserve every other byte/weapon rule.
 # The parser accepts a pitch/yaw range: all four bounds must be zero.
 noKick,count=re.subn(r'(?im)^(\s*(?:mp_)?kickAngles\s+)"[^"\r\n]+"',r'\g<1>"0 0 0 0"',text)
 assert count>=20,'Expected native weapon camera-kick definitions'
 entries['ext_data/SOF2.wpn']=noKick.encode('cp1252')
 report['cameraKick']={'definitionsDisabled':count,'source':'update102.pk3/ext_data/SOF2.wpn','method':'zero only kickAngles ranges; other weapon rules unchanged'}
 report['muzzleFlashesRestored']=sorted(flashNames)
 report['effectPlacement']='Native local first-person FX calls redirected to the shared VR weapon muzzle/ejection locators'
 rows=json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons']
 profiles={w['id']:w for w in json.loads((ROOT/'resources/weapon_profiles.json').read_text())['weapons']}
 for row in rows:
  if profiles[row['id']]['category'] not in [1,2,3,4,5,6,7]:continue
  sequences=row['animations']['sequences'];rest=idle(sequences,Path(row['viewModel']).stem)
  if not rest:continue
  with zipfile.ZipFile(BASE/row['skeleton']['archive']) as z:original=z.read(row['skeleton']['path'])
  d=bytearray(original);nf,of,nb=struct.unpack_from('<3i',d,76);bones=[b['index'] for b in row['bones'] if b['name'] in ['model_root','gun']];count=0
  for seq in sequences:
   name=seq['name'].lower();static_idle='idle' in name or 'finger' in name
   physical_knife=row['id']==1
   if not physical_knife and not static_idle and 'fire' not in name:continue
   frozen=range(nb) if static_idle or physical_knife else bones
   for frame in range(seq['startFrame'],min(nf,seq['startFrame']+seq['duration'])):
    for bone in frozen:d[of+(frame*nb+bone)*3:of+(frame*nb+bone+1)*3]=original[of+(rest['startFrame']*nb+bone)*3:of+(rest['startFrame']*nb+bone+1)*3]
    count+=1
  entries[row['skeleton']['path']]=bytes(d);report['animationRoots'].append(dict(weapon=row['id'],path=row['skeleton']['path'],idle=rest['name'],frames=count,bones=bones,idleAndFidgetBones='all',originalSHA256=hashlib.sha256(original).hexdigest()))
 # Native inview data associates each weapon's hand actions with its grip pose.
 # Resolve these associations before replacing GLA frame references; never
 # replace every weapon with a single shared hand pose.
 from build_weapon_profiles import parse,value
 inview=None
 for archive in ['update103.pk3','update102.pk3','update101.PK3','therest.pk3']:
  with zipfile.ZipFile(BASE/archive) as z:
   if 'inview/SOF2.inview' in z.namelist():inview=parse(z.read('inview/SOF2.inview').decode('cp1252'));break
 handRest={'left':{},'right':{}}
 for key,weapon in inview or []:
  if key.lower()!='weapon' or not isinstance(weapon,list):continue
  groups=[b for k,b in weapon if k.lower()=='anim' and isinstance(b,list)]
  resting={}
  for group in groups:
   if value(group,'name','').lower()!='idle':continue
   for k,info in group:
    if k.lower()=='info' and isinstance(info,list) and value(info,'type','').lower()=='hands':
     side=value(info,'name','').lower();pose=value(info,'anim','').lower()
     if side in handRest and pose:resting[side]=pose+'.xsi'
  for group in groups:
   for k,info in group:
    if k.lower()!='info' or not isinstance(info,list) or value(info,'type','').lower()!='hands':continue
    side=value(info,'name','').lower()
    if side not in resting:continue
    for field,name in info:
     if isinstance(name,str) and (field.lower().startswith('anim') or field.lower().startswith('extra')):
      handRest[side].setdefault(name.lower()+'.xsi',resting[side])
 with zipfile.ZipFile(BASE/'therest.pk3') as z:
  for side,sideName in [('l','left'),('r','right')]:
   path=f'skeletons/weapons/{side}hand/{side}hand';original=z.read(path+'.gla');frames=z.read(path+'.frames').decode('cp1252')
   sequences=[]
   for key,block in parse(frames):
    if not isinstance(block,list) or not key.lower().endswith('.xsi'):continue
    sequences.append(dict(name=key.replace('\\','/').split('/')[-1],startFrame=int(value(block,'startframe','0')),duration=int(value(block,'duration','0'))))
   indexed={seq['name'].lower():seq for seq in sequences};rests=[seq for seq in sequences if 'idle' in seq['name'].lower() and not any(k in seq['name'].lower() for k in ['empty','prone','crawl'])]
   def rest_for(seq):
    name=seq['name'].lower()
    mapped=indexed.get(handRest[sideName].get(name,''))
    if mapped:return mapped
    if seq in rests:return seq
    def common(rest):
     stem=rest['name'].lower().split('idle')[0];return len(__import__('os').path.commonprefix([name,stem]))
    best=max(rests,key=common)
    if common(best)>=3:return best
    # Native right-hand rifle actions often share the M4 grip; grenade
    # variants without an idle sequence share the authored M67 hold.
    grenade=any(name.startswith(side+kind) for kind in ['anm14','f1','l2a2','mdn11'])
    return indexed.get(side+('m67' if grenade else 'm4')+'idle.xsi',best)
   d=bytearray(original);nf,of,nb=struct.unpack_from('<3i',d,76);count=0;mapping=[]
   for seq in sequences:
    rest=rest_for(seq);src=original[of+rest['startFrame']*nb*3:of+(rest['startFrame']+1)*nb*3]
    for frame in range(seq['startFrame'],min(nf,seq['startFrame']+seq['duration'])):d[of+frame*nb*3:of+(frame+1)*nb*3]=src;count+=1
    mapping.append(dict(sequence=seq['name'],pose=rest['name']))
   entries[path+'.gla']=bytes(d);report['animationRoots'].append(dict(hand=side,path=path+'.gla',frames=count,staticPoses=mapping,originalSHA256=hashlib.sha256(original).hexdigest()))
 entries['sof2vr-presentation.json']=(json.dumps(report,indent=2)+'\n').encode();return entries,report
