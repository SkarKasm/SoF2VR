"""Extend the stereo material pass to the other replacement weapon atlases."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import zipfile

from build_m4_materials import FINISHES, ROOT

sys.path.insert(0, 'E:/SoF2-asset-tools/python-packages')
from PIL import Image, ImageOps

MANIFEST = 'sof2vr-weapon-material-maps.json'
VERSION = 1

# Normal, metallic and roughness filenames whose source names do not follow
# the BaseColor/Normal/Metallic/Roughness convention.
MAP_NAMES = {
    (2, 0): ('T_MK23_NormalOpenGL', 'T_MK23_Metallic', 'T_MK23_Roughness'),
    (3, 0): ('Normal', 'Metallic2', 'Roughness'),
    (4, 0): ('Normal', 'Metallic', 'Rouhness'),
    (6, 0): ('m590_receiver_Normal_opengl', 'm590_receiver_Metallic', 'm590_receiver_Roughness'),
    (6, 1): ('m590_furniture_Normal_opengl_plastic', 'm590_furniture_plastic_Metallic', 'm590_furniture_plastic_Roughness'),
    # The source blend references n3 and r2 and supplies no metallic map.
    (7, 0): ('uzi_n3', None, 'uzi_r2'),
    (10, 0): ('met_nrm', 'met_metallic', 'met_rough'),
}


def channels(weapon, slot, color, images):
    names = MAP_NAMES.get((weapon, slot))
    if names is None:
        pattern = re.compile('base_?color', re.I)
        names = [pattern.sub(kind, color.stem) for kind in ('normal', 'metallic', 'roughness')]
        names[0] = next((n for n in (names[0], names[0]+'_OpenGL', names[0]+'_DirectX')
                         if n.lower() in images), names[0])
    paths = [images.get(name.lower()) if name else None for name in names]
    if not paths[0] or not paths[2] or (names[1] and not paths[1]):
        raise ValueError(f'Incomplete source maps for weapon {weapon}, slot {slot}: {names}')
    return paths


def scaled(path, mode, size):
    with Image.open(path) as image:
        image = image.convert(mode)
        return image if image.size == size else image.resize(size, Image.Resampling.LANCZOS)


def write_maps(output, weapon):
    folder = ROOT / 'build/replacement-intake' / weapon['asset']
    with gzip.open(folder / 'selected-meshes.json.gz', 'rt') as file:
        selected = json.load(file)['materials']
    images = {path.stem.lower(): path for path in (folder / 'textures').iterdir() if path.is_file()}
    for slot, material in enumerate(weapon['materials']):
        color_path = folder / selected[material['material']]['file']
        normal_path, metal_path, rough_path = channels(weapon['id'], slot, color_path, images)
        size = tuple(material['size'])
        if min(size) < 1 or max(size) > 2048:
            raise ValueError(f'Unsupported material size for {material["shader"]}')
        color = scaled(color_path, 'RGB', size)
        normal = scaled(normal_path, 'RGB', size)
        directx = 'directx' in normal_path.stem.lower()
        if directx:
            red, green, blue = normal.split()
            normal = Image.merge('RGB', (red, ImageOps.invert(green), blue))
        normal.putalpha(scaled(rough_path, 'L', size))
        color.putalpha(scaled(metal_path, 'L', size) if metal_path else Image.new('L', size, 0))
        prefix = material['shader'].rsplit('/', 1)[0]
        header = struct.pack('<4s2I', b'S2M4', *size)
        output.writestr(f'{prefix}/p{slot}.bin', header + normal.tobytes())
        output.writestr(f'{prefix}/c{slot}.bin', header + color.tobytes())
        yield dict(weapon=weapon['id'], slot=slot, size=size, source=[color_path.name, normal_path.name,
                   metal_path.name if metal_path else None, rough_path.name], directxNormal=directx)


def build():
    pack = ROOT / 'resources/z_sof2vr_native.pk3'
    pending = pack.with_suffix('.weapon-materials-pending')
    if pending.exists():
        raise RuntimeError('Interrupted weapon material build exists')
    try:
        with zipfile.ZipFile(pack) as source:
            if MANIFEST in source.namelist() and json.loads(source.read(MANIFEST)).get('version') == VERSION:
                return
            replacements = json.loads(source.read('sof2vr-weapon-replacements.json'))['ready']
            shader_path = 'shaders/weapons.shader'
            shader = source.read(shader_path).decode('cp1252')
            # The SOCOM's previous additive approximation would brighten the
            # already shaded atlas again. Keep its lifted native fallback.
            shader = re.sub(r'\s*\{\s*map sof2vr_ext/w2/m0\.png\s+blendFunc GL_ONE GL_ONE\s+'
                            r'rgbGen const \( 0\.42 0\.42 0\.42 \)\s*\}', '', shader)
            entries = {shader_path: shader.encode('cp1252')}
            if FINISHES in source.namelist():
                finishes = json.loads(source.read(FINISHES))
                finishes['outputs'][shader_path] = hashlib.sha256(entries[shader_path]).hexdigest()
                entries[FINISHES] = (json.dumps(finishes, indent=2) + '\n').encode()
            with zipfile.ZipFile(pending, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=2) as output:
                for info in source.infolist():
                    if info.filename != MANIFEST:
                        output.writestr(info, entries.get(info.filename) or source.read(info))
                materials = []
                for weapon in replacements:
                    if weapon['id'] != 9:
                        materials.extend(write_maps(output, weapon))
                output.writestr(MANIFEST, json.dumps(dict(version=VERSION, materials=materials), indent=2)+'\n')
        os.replace(pending, pack)
    except BaseException:
        pending.unlink(missing_ok=True)
        raise
    print(f'Packed {len(materials)} additional weapon material pairs.', flush=True)


if __name__ == '__main__':
    build()
