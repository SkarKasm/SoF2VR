"""Extract original mission-equipment icons to a small helper texture format.
PNG decoding here keeps local builds independent of imaging packages.
"""
from pathlib import Path
import json,struct,zlib
from native_pose import read
ROOT=Path(__file__).resolve().parents[1]

def rgba_png(data):
 assert data[:8]==b'\x89PNG\r\n\x1a\n'
 at=8;compressed=bytearray();palette=None;alpha=b''
 while at<len(data):
  count=struct.unpack_from('>I',data,at)[0];kind=data[at+4:at+8];chunk=data[at+8:at+8+count];at+=12+count
  if kind==b'IHDR':w,h,depth,color,compression,filtering,interlace=struct.unpack('>IIBBBBB',chunk)
  elif kind==b'IDAT':compressed+=chunk
  elif kind==b'PLTE':palette=chunk
  elif kind==b'tRNS':alpha=chunk
 assert depth==8 and color in [2,3,6] and not interlace and not compression and not filtering
 bpp={2:3,3:1,6:4}[color];raw=zlib.decompress(compressed);stride=w*bpp;previous=bytearray(stride);out=bytearray();at=0
 for y in range(h):
  mode=raw[at];at+=1;line=bytearray(raw[at:at+stride]);at+=stride
  for i in range(stride):
   a=line[i-bpp] if i>=bpp else 0;b=previous[i];c=previous[i-bpp] if i>=bpp else 0
   if mode==1:value=a
   elif mode==2:value=b
   elif mode==3:value=(a+b)//2
   elif mode==4:
    p=a+b-c;da,db,dc=abs(p-a),abs(p-b),abs(p-c);value=a if da<=db and da<=dc else b if db<=dc else c
   elif mode==0:value=0
   else:raise ValueError('Unsupported PNG row filter')
   line[i]=(line[i]+value)&255
  for i in range(w):
   p=line[i*bpp:(i+1)*bpp]
   if color==6:out+=p
   elif color==2:out+=p+b'\xff'
   else:out+=palette[p[0]*3:p[0]*3+3]+bytes([alpha[p[0]] if p[0]<len(alpha) else 255])
  previous=line
 assert len(out)==w*h*4
 return w,h,out

def build():
 target=ROOT/'build/sof2-vr-x64/icons';target.mkdir(parents=True,exist_ok=True);report=[]
 for row in json.loads((ROOT/'resources/weapon_profiles.json').read_text())['weapons']:
  if not row['playerSelectable']:continue
  path='gfx/menus/'+row['icon']+'.png';w,h,pixels=rgba_png(read(path))
  (target/f"weapon-{row['id']:02d}.rgba").write_bytes(struct.pack('<4sII',b'S2IC',w,h)+pixels)
  report.append(dict(id=row['id'],path=path,width=w,height=h))
 (ROOT/'build/weapon-icon-assets.json').write_text(json.dumps(report,indent=2)+'\n')
 (target/'sources.json').write_text(json.dumps(report,indent=2)+'\n')
 print('Prepared',len(report),'original weapon/equipment icons.')
if __name__=='__main__':build()
