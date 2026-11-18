"""Catalog every native definition from the active original SoF2 SP weapon data."""
from pathlib import Path
import re,json,zipfile,hashlib
ROOT=Path(__file__).resolve().parents[1]
def parse(text):
 tokens=re.findall(r'"[^"\r\n]*"|[{}\[\]]|[^\s{}\[\]]+',re.sub(r'//[^\r\n]*','',text));at=0
 def group(close=None):
  nonlocal at
  out=[]
  while at<len(tokens):
   key=tokens[at];at+=1
   if key==close:return out
   if at>=len(tokens):break
   value=tokens[at];at+=1
   if value=='{':value=group('}')
   elif value=='[':
    value=[]
    while at<len(tokens) and tokens[at]!=']':value.append(tokens[at].strip(chr(34)));at+=1
    at+=1
   else:value=value.strip('"')
   out.append((key.strip('"'),value))
  return out
 return group()
def value(block,key,default=''):
 return next((v for k,v in block if k==key),default)
def build():
 source=ROOT.parent/'Soldier of Fortune 2/base/update102.pk3'
 with zipfile.ZipFile(source) as z:data=z.read('ext_data/SOF2.wpn')
 rows=[]
 for block in [v for k,v in parse(data.decode('cp1252')) if k=='weapon']:
  name=value(block,'name');category=int(value(block,'category','0'));attacks=[value(block,k,[]) for k in ('attack','altattack')]
  projectiles=[value(a,'projectile',[]) for a in attacks]
  row={'id':len(rows)+1,'name':name,'category':category,'playerSelectable':1<=category<=9,'pellets':int(value(attacks[0],'pellets','1')),'clipCapacity':int(value(attacks[0],'clipSize','0')),'speed':[float(value(p,'speed','0')) for p in projectiles],'gravityScale':[float(value(p,'gravity','1')) if p else 0 for p in projectiles],'model':value(block,'model'),'icon':value(block,'menuImage'),'reload':{'Knife':'knife-replenish','M590':'pump-shells','M60':'belt-cover','MM1':'revolving-drum','RPG7':'single-rocket','OICW':'dual-magazine'}.get(name,'grenade' if category==8 else 'magazine' if category in (2,3,4,5,6,7) else 'none'),'validation':'pending-native-test'}
  rows.append(row)
 assert len(rows)==36 and rows[5]['name']=='M590' and rows[17]['name']=='M67'
 manifest={'sourceArchive':source.name,'path':'ext_data/SOF2.wpn','sourceSHA256':hashlib.sha256(data).hexdigest(),'weapons':rows}
 (ROOT/'resources/weapon_profiles.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf8')
 header=['/* Generated from original SP weapon data; run diagnostics/build_weapon_profiles.py. */','#ifndef SOF2_WEAPON_PROFILES_H','#define SOF2_WEAPON_PROFILES_H','typedef struct {int id,category,pellets,clip,pump;float speed[2],gravity[2];const char *name;} VRWeaponProfile;','static const VRWeaponProfile vrWeaponProfiles[]={']
 for w in rows:
  fmt=lambda x:format(x,'.6f')+'f'
  header.append(' {%d,%d,%d,%d,%d,{%s},{%s},%s},'%(w['id'],w['category'],w['pellets'],w['clipCapacity'],w['reload']=='pump-shells',','.join(map(fmt,w['speed'])),','.join(map(fmt,w['gravityScale'])),json.dumps(w['name'])))
 header+=['};','static inline const VRWeaponProfile *VR_Profile(int id){return id>=1 && id<=36?&vrWeaponProfiles[id-1]:0;}','#endif','']
 (ROOT/'src/weapon_profiles.h').write_text('\n'.join(header),encoding='utf8')
 print('Built all 36 native definitions: '+str(sum(w['playerSelectable'] for w in rows))+' player definitions; NPC/emplaced/body definitions identified.')
if __name__=='__main__':build()
