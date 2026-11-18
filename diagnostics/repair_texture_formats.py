"""Normalize unsupported replacement PNG bit depths without touching geometry."""
from pathlib import Path
import hashlib,io,json,os,struct,sys,zipfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,'E:/SoF2-asset-tools/python-packages')
from PIL import Image

def normalized(data):
    depth,colour=struct.unpack_from('>BB',data,24)
    if depth==8 and colour in [2,6]:return data
    with Image.open(io.BytesIO(data)) as image:
        alpha='A' in image.getbands() or 'transparency' in image.info
        output=io.BytesIO();image.convert('RGBA' if alpha else 'RGB').save(output,format='PNG')
    result=output.getvalue()
    if result[24]!=8:raise ValueError('PNG normalization did not produce 8-bit samples')
    return result

def repair():
    pack=ROOT/'resources/z_sof2vr_native.pk3';changes={};records=[]
    with zipfile.ZipFile(pack) as z:
        for info in z.infolist():
            if not info.filename.startswith('sof2vr_ext/') or not info.filename.endswith('.png'):continue
            original=z.read(info);fixed=normalized(original)
            if original==fixed:continue
            changes[info.filename]=fixed
            records.append(dict(path=info.filename,originalBitDepth=original[24],bitDepth=fixed[24],
                width=struct.unpack_from('>I',original,16)[0],height=struct.unpack_from('>I',original,20)[0],
                originalSHA256=hashlib.sha256(original).hexdigest(),sha256=hashlib.sha256(fixed).hexdigest()))
        if not changes:return []
        changes['sof2vr-texture-compatibility.json']=(json.dumps(dict(method='RGB/RGBA 8-bit PNG samples for the native image loader; dimensions/UVs unchanged',textures=records),indent=2)+'\n').encode()
        pending=pack.with_suffix('.texture-pending')
        if pending.exists():raise RuntimeError('Interrupted texture repair exists')
        try:
            with zipfile.ZipFile(pending,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as out:
                for info in z.infolist():out.writestr(info,changes.pop(info.filename,z.read(info)))
                for name,data in changes.items():out.writestr(name,data)
        except BaseException:
            pending.unlink(missing_ok=True);raise
    os.replace(pending,pack)
    print('Normalized native PNG sample format:',', '.join(row['path'] for row in records),flush=True)
    return records

if __name__=='__main__':repair()
