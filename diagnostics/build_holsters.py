"""Prepare/package visible holsters and low-detail stowed weapon derivatives."""
import argparse,gzip,hashlib,io,json,os,struct,sys,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,'E:/SoF2-asset-tools/python-packages')
import numpy as np
from PIL import Image
from build_magazine_assets import multipart

STAGE=ROOT/'build/holster-intake'
def prepare():
    with zipfile.ZipFile(ROOT/'resources/z_sof2vr_native.pk3') as z:
        grips={w['id']:w for w in json.loads(z.read('sof2vr-grip-poses.json'))};result=[];knife={}
        rep=next(w for w in json.loads(z.read('sof2vr-weapon-replacements.json'))['ready'] if w['id']==1)
        m=np.asarray(rep['calibration']['matrix']);cfg=json.loads((ROOT/'resources/knife_collision.json').read_text())
        base=m[:,:3]@cfg['bladeBase']+m[:,3];tip=m[:,:3]@cfg['bladeTip']+m[:,3]
        f=(tip-base)/np.linalg.norm(tip-base);u=m[:,2];side=np.cross(u,f);side/=np.linalg.norm(side);u=np.cross(f,side);bladeBasis=np.array([f,side,u]).T
        for row in json.loads(z.read('sof2vr-dropped-weapons.json')):
            id=row['id'];data=z.read(row['path']);h=struct.unpack_from('<4si64s9i',data);at=h[10];groups={}
            basis=bladeBasis if id==1 else np.asarray(grips[id]['basis']);anchor=base if id==1 else np.asarray(grips[id]['anchor'])
            for _ in range(h[6]):
                s=struct.unpack_from('<4s64s10i',data,at);shader=data[at+s[8]:at+s[8]+64].split(b'\0')[0].decode()
                g=groups.setdefault(shader,[[],[],[],[]]);offset=len(g[0])
                for i in range(s[5]):
                    x,y,zv,packed=struct.unpack_from('<3hH',data,at+s[10]+8*i);p=(np.array([x,y,zv])/64-anchor)@basis
                    lat=(packed>>8)*2*np.pi/255;lon=(packed&255)*2*np.pi/255;n=np.array([np.cos(lat)*np.sin(lon),np.sin(lat)*np.sin(lon),np.cos(lon)])@basis
                    g[0].append(p.tolist());g[1].append(n.tolist());g[2].append(struct.unpack_from('<2f',data,at+s[9]+8*i))
                g[3].extend([[v+offset for v in struct.unpack_from('<3i',data,at+s[7]+12*i)] for i in range(s[6])]);at+=s[-1]
            result.append(dict(id=id,groups=groups))
            if id==1:
                points=np.array([p for g in groups.values() for p in g[0] if p[0]>.15])
                knife=dict(length=float(np.linalg.norm(tip-base)+.35),width=float(np.ptp(points[:,2])),thickness=float(np.ptp(points[:,1])))
        with gzip.open(STAGE/'stowed-input.json.gz','wt') as f:json.dump(dict(weapons=result,knife=knife),f,separators=(',',':'))
    print('Prepared',len(result),'stowed derivatives; sheath target',knife)

def build():
    with gzip.open(STAGE/'exported.json.gz','rt') as f:exported=json.load(f)
    entries={};shaders=[];pistolFits=[]
    for key,groups in exported['models'].items():
        output={}
        for material,g in groups.items():
            if key=='pistol_holster':
                source=next((STAGE/'pistol/textures').glob(material+'_100*_BaseColor.png'));shader='sof2vr_holsters/'+material
            elif key=='knife_sheath':source=STAGE/'sheath/textures/ALL_Leather_BaseColor.png';shader='sof2vr_holsters/leather'
            else:source=None;shader=material
            if source and shader+'.png' not in entries:
                with Image.open(source) as im:
                    im=im.convert('RGB');im.thumbnail((1024,1024));buf=io.BytesIO();im.save(buf,format='PNG');entries[shader+'.png']=buf.getvalue()
                shaders.append(shader+'\n{\n cull disable\n {\n map '+shader+'.png\n rgbGen lightingDiffuse\n }\n}\n')
            dest=output.setdefault(shader,[[],[],[],[]]);offset=len(dest[0])
            for i in range(3):dest[i].extend(g[i])
            dest[3].extend([[v+offset for v in t] for t in g[3]])
        entries['models/sof2vr_holsters/'+key+'.md3']=multipart(output,[0,0,0])
        if key in ['weapon_2','weapon_3','weapon_4']:
            points=np.array([p for g in groups.values() for p in g[0]]);lo=points.min(axis=0);hi=points.max(axis=0)
            pistolFits.append(dict(id=int(key.split('_')[1]),side=float((lo[1]+hi[1])/2),front=float((lo[2]+hi[2])/2),scale=[float((hi[2]-lo[2]+.35)/4.71),float((hi[1]-lo[1]+.3)/1.53),float(max(1,(hi[0]-2.35+.35)/5.42))]))
    candidates=json.loads((ROOT/'resources/holster_asset_candidates.json').read_text())['candidates']
    credits={'purpose':'Visible pistol holster and a sheath adapted to the existing KA-BAR; stowed guns derive solely from the installed licensed/native meshes.',
        'assets':[dict(name=c['name'],author=c['author'],url=c['url'],license='CC-BY-4.0',licenseURL='https://creativecommons.org/licenses/by/4.0/',changes='Geometry reduction, coordinate conversion and fitting. Knife asset: sheath components only; existing KA-BAR unchanged.') for c in candidates[:2]],'sourceArchives':[json.loads((STAGE/p/'provenance.json').read_text()) for p in ['pistol','sheath']]}
    entries['licenses/HOLSTER_ASSET_CREDITS.json']=(json.dumps(credits,indent=2)+'\n').encode()
    report=dict(version=1,models=exported['stats'],credits=credits,pistolFits=pistolFits,stowedWeaponScale='Exact packed owner-edited geometry; no hand or weapon refitting')
    entries['sof2vr-holsters.json']=(json.dumps(report,indent=2)+'\n').encode()
    pack=ROOT/'resources/z_sof2vr_native.pk3';pending=pack.with_suffix('.holsters-pending')
    with zipfile.ZipFile(pack) as z,zipfile.ZipFile(pending,'w') as out:
        shader=z.read('shaders/weapons.shader').decode('cp1252');start='// BEGIN VISIBLE HOLSTERS';end='// END VISIBLE HOLSTERS'
        if start in shader:shader=shader[:shader.index(start)]+shader[shader.index(end)+len(end):]
        entries['shaders/weapons.shader']=(shader+'\n'+start+'\n'+''.join(shaders)+end+'\n').encode('cp1252')
        finish=json.loads(z.read('sof2vr-visual-finishes.json'));finish['outputs']['shaders/weapons.shader']=hashlib.sha256(entries['shaders/weapons.shader']).hexdigest()
        entries['sof2vr-visual-finishes.json']=(json.dumps(finish,indent=2)+'\n').encode()
        for info in z.infolist():out.writestr(info,entries.pop(info.filename,z.read(info)))
        for name,data in entries.items():out.writestr(name,data,compress_type=zipfile.ZIP_DEFLATED)
    os.replace(pending,pack)
    (ROOT/'build/holster-assets.json').write_text(json.dumps(report,indent=2)+'\n')
    (ROOT/'src/holster_fits.h').write_text('/* Fit only the container; held and stowed guns retain authored scale. */\n#ifndef SOF2_HOLSTER_FITS_H\n#define SOF2_HOLSTER_FITS_H\ntypedef struct {int id;float side,front,scale[3];} VRHolsterFit;\nstatic const VRHolsterFit vrHolsterFits[]={\n'+''.join('{%d,%.8ff,%.8ff,{%s}},\n'%(r['id'],r['side'],r['front'],','.join('%.8ff'%x for x in r['scale'])) for r in pistolFits)+'};\n#endif\n')
    print('Packed two holster/sheath models and',sum(k.startswith('weapon_') for k in exported['models']),'stowed weapons with credits.')

def rebuild():
    import subprocess
    prepare()
    subprocess.run(['E:/SoF2-asset-tools/blender-4.5.15-windows-x64/blender.exe','--background','--factory-startup','--python',str(ROOT/'diagnostics/export_holsters.py')],cwd=ROOT,check=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    build()

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--prepare',action='store_true');args=ap.parse_args()
    prepare() if args.prepare else build()
