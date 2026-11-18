"""Keep original view-model materials but make their surfaces visible from both sides."""
from pathlib import Path
import json,re,struct,zipfile
from native_pose import read,BASE
ROOT=Path(__file__).resolve().parents[1]

def build(entries):
 definitions={};sources={}
 for archive in ['therest.pk3','models.pk3','update101.PK3','update102.pk3','update103.pk3']:
  with zipfile.ZipFile(BASE/archive) as z:
   for name in z.namelist():
    if not name.lower().endswith('.shader'):continue
    text=z.read(name).decode('cp1252');sources[name.lower()]=text
    # Shader braces can contain stages; retain the complete original block.
    for match in re.finditer(r'(?m)^\s*([^\s{}]+)\s*\{',text):
     depth=1;at=match.end()
     while at<len(text) and depth:
      if text[at]=='{':depth+=1
      elif text[at]=='}':depth-=1
      at+=1
     if not depth:definitions[match[1].lower()]=text[match.end():at-1]
 rows=json.loads((ROOT/'resources/weapon_parts.json').read_text())['weapons']
 rows=rows+[dict(id=9,name='M203',viewModel='models/weapons/m203/m203.glm')]
 aliases={};report=[]
 for row in rows:
  path=row['viewModel'];d=bytearray(entries.get(path,read(path)));h=struct.unpack_from('<4si64s64s7i',d);mappings=[]
  for i in range(h[8]):
   at=164+struct.unpack_from('<i',d,164+4*i)[0];shader=bytes(d[at+68:at+132]).split(b'\0')[0].decode()
   if not shader:continue
   alias='sof2vr/'+shader
   if len(alias)>=64:raise ValueError('Material alias exceeds native GLM path field')
   body=definitions.get(shader.lower())
   if body is None:body='\n {\n map '+shader+'\n rgbGen lightingDiffuse\n }\n'
   body=re.sub(r'(?im)^\s*cull\s+\S+[^\r\n]*','',body)
   aliases[alias]='\n'+alias+'\n{\n cull disable\n'+body+'\n}\n'
   d[at+68:at+132]=alias.encode().ljust(64,b'\0');mappings.append(dict(original=shader,material=alias))
  # Knife motion is tracked, so the flat game's baked motion-blur fans are
  # never appropriate even when its native attack notetrack enables them.
  if row['id']==1:
   lod=h[7]
   for _ in range(h[6]):
    end=struct.unpack_from('<i',d,lod)[0]
    for i in range(h[8]):
     at=164+struct.unpack_from('<i',d,164+4*i)[0]
     if bytes(d[at:at+64]).split(b'\0')[0].startswith(b'blur'):
      surface=lod+4+struct.unpack_from('<i',d,lod+4+4*i)[0];struct.pack_into('<i',d,surface+20,0)
    lod+=end
  entries[path]=bytes(d);report.append(dict(weapon=row['name'],model=path,materials=mappings))
 # Extend an existing, engine-listed shader file so discovery is deterministic.
 entries['shaders/weapons.shader']=(sources['shaders/weapons.shader']+'\n'+''.join(aliases.values())).encode('cp1252')
 entries['sof2vr-weapon-materials.json']=(json.dumps(report,indent=2)+'\n').encode()
 return report
