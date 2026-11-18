"""Outfit hints from authored Mullins NPC classes in original campaign maps."""
from pathlib import Path
import zipfile,struct,re,json
ROOT=Path(__file__).resolve().parents[1]
NAMES=['urban','jungle','snow','suit','noncombat','young','chem']
def class_outfit(name):
 name=name.lower()
 if 'chem' in name:return 'chem'
 if 'non_combat' in name:return 'noncombat'
 for item in NAMES:
  if item in name:return item
 return None
def build():
 rows=[];families={}
 with zipfile.ZipFile(ROOT.parent/'Soldier of Fortune 2/base/maps.pk3') as z:
  for path in z.namelist():
   match=re.fullmatch(r'maps/((?:air|arm|cem|col|finca|hk|hos|kam|liner|pra|shop|tut)\d+[a-z]?)\.bsp',path)
   if not match:continue
   with z.open(path) as f:h=f.read(152);off,size=struct.unpack_from('<II',h,8);f.seek(off);text=f.read(size).decode('cp1252')
   classes=sorted(set(re.findall(r'"classname"\s*"([^"]*mullins[^"]*)"',text,re.I)))
   outfits={class_outfit(n) for n in classes}-{None};chosen=next(iter(outfits)) if len(outfits)==1 else None
   family=re.match('[a-z]+',match[1])[0]
   if chosen:families.setdefault(family,set()).add(chosen)
   rows.append({'map':match[1],'outfit':chosen,'source':'authored Mullins NPC class' if chosen else 'pending','classes':classes,'archivePath':path})
 for row in rows:
  family=re.match('[a-z]+',row['map'])[0]
  if row['outfit']:continue
  possible=families.get(family,set())
  if len(possible)==1:row.update(outfit=next(iter(possible)),source='same campaign region; inferred from authored classes')
  elif family=='col':row.update(outfit='jungle' if int(re.search(r'\d+',row['map'])[0])<9 else 'urban',source='campaign continuity fallback; verify in headset')
  elif family=='finca':row.update(outfit='jungle',source='campaign continuity fallback; verify in headset')
  else:row.update(outfit='urban',source='urban fallback; manual override available')
 rows.sort(key=lambda r:r['map']);(ROOT/'resources/outfit_profiles.json').write_text(json.dumps({'policy':'Native skin observations override map hints; explicit Hands.Outfit overrides both. Region inference is not exact player-skin tracking.','maps':rows},indent=2)+'\n',encoding='utf8')
 h=['/* Generated authored campaign outfit hints; see resources/outfit_profiles.json. */','#ifndef SOF2_OUTFIT_PROFILES_H','#define SOF2_OUTFIT_PROFILES_H','typedef struct {const char *map;int outfit;} VROutfitMap;','static const VROutfitMap vrOutfitMaps[]={']
 h += [' {"%s",%d},'%(r['map'],NAMES.index(r['outfit'])) for r in rows];h+=['};','#endif',''];(ROOT/'src/outfit_profiles.h').write_text('\n'.join(h),encoding='utf8')
 print('Built '+str(len(rows))+' campaign outfit hints from original map entities; inference labeled.')
if __name__=='__main__':build()
