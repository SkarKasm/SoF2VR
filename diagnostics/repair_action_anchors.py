"""Move pistol slide grasp points to their rear serrations; hands stay authored."""
import json,os,struct,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def repair():
    pack=ROOT/'resources/z_sof2vr_native.pk3';changes={};report=[]
    with zipfile.ZipFile(pack) as z:
        reloads=json.loads(z.read('sof2vr-magazine-parts.json'));replacements=json.loads(z.read('sof2vr-weapon-replacements.json'))
        for r in reloads['ready']:
            if r['id'] not in [2,3,4]:continue
            d=z.read(r['prefix']+'_bolt.md3');h=struct.unpack_from('<4si64s9i',d);at=h[10];points=[]
            for _ in range(h[6]):
                s=struct.unpack_from('<4s64s10i',d,at)
                points.extend([[v/64 for v in struct.unpack_from('<3h',d,at+s[10]+8*i)] for i in range(s[5])]);at+=s[-1]
            lo=[min(p[k] for p in points) for k in range(3)];hi=[max(p[k] for p in points) for k in range(3)]
            point=[lo[0]+.15*(hi[0]-lo[0]),(lo[1]+hi[1])*.5,hi[2]-.08]
            report.append(dict(id=r['id'],contact='rear slide serrations, above the slide; not the ejection port',point=point));r['bolt']=point
            for rep in replacements['ready']:
                if rep['id']==r['id']:rep['sockets']['bolt']=point
        changes['sof2vr-magazine-parts.json']=(json.dumps(reloads,indent=2)+'\n').encode();changes['sof2vr-weapon-replacements.json']=(json.dumps(replacements,indent=2)+'\n').encode()
        changes['sof2vr-action-contacts.json']=(json.dumps(dict(ready=report,policy='Locator-only repair; no edited hand or weapon mesh/scale changes'),indent=2)+'\n').encode()
        pending=pack.with_suffix('.actions-pending')
        with zipfile.ZipFile(pending,'w') as out:
            for info in z.infolist():out.writestr(info,changes.pop(info.filename,z.read(info)))
            for path,data in changes.items():out.writestr(path,data,compress_type=zipfile.ZIP_DEFLATED)
    os.replace(pending,pack)
    # Keep the generated runtime table synchronized with the same pack.
    text=['/* Packed detachable magazines and model-based action contacts. */','#ifndef SOF2_MAGAZINE_PARTS_H','#define SOF2_MAGAZINE_PARTS_H','typedef struct {int id;const char *model,*prefix;float socket[3],bolt[3];int count,surfaces[16];} VRReloadParts;','static const VRReloadParts vrReloadParts[]={']
    for r in reloads['ready']:text.append(' {%d,"%s","%s",{%s},{%s},%d,{%s}},'%(r['id'],r['model'],r['prefix'],','.join('%.7ff'%v for v in r['socket']),','.join('%.7ff'%v for v in r['bolt']),len(r['magSurfaces']),','.join(map(str,r['magSurfaces']))))
    text+=['};','static inline const VRReloadParts *ReloadParts(int id){for(unsigned i=0;i<sizeof(vrReloadParts)/sizeof(vrReloadParts[0]);++i)if(vrReloadParts[i].id==id)return &vrReloadParts[i];return 0;}','#endif','']
    (ROOT/'src/magazine_parts.h').write_text('\n'.join(text));return report
if __name__=='__main__':print(json.dumps(repair(),indent=2))
