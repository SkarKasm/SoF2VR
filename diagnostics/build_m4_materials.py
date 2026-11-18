"""Pack the M4 source material maps for the stereo renderer."""
import gzip
import hashlib
import json
import os
import re
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'build/replacement-intake/a-colt-m4a1m203'
PREFIX = 'sof2vr_ext/w9/'
MANIFEST = 'sof2vr-m4-materials.json'
FINISHES = 'sof2vr-visual-finishes.json'
VERSION = 4


def material_maps():
    sys.path.insert(0, 'E:/SoF2-asset-tools/python-packages')
    from PIL import Image

    with gzip.open(SOURCE / 'selected-meshes.json.gz', 'rt') as file:
        materials = json.load(file)['materials']
    images = {p.stem.lower(): p for p in (SOURCE / 'textures').iterdir() if p.is_file()}
    entries, report = {}, []
    for index, (name, material) in enumerate(materials.items()):
        original = SOURCE / material['file']
        stem = original.stem.lower().removesuffix('_basecolor')
        paths = [images.get(stem + '_' + kind) for kind in ('normal', 'metallic', 'roughness')]
        # The source omits the mask for the steel bolt. Its normal and
        # roughness textures are present and remain authoritative.
        if paths[0] is None or paths[2] is None or (paths[1] is None and name != 'ACR16_Bolt_D'):
            raise ValueError('Incomplete material maps for ' + name)
        with Image.open(paths[0]) as image:
            normal = image.convert('RGB')
        with Image.open(paths[2]) as image:
            roughness = image.convert('L')
        if paths[1]:
            with Image.open(paths[1]) as image:
                metallic = image.convert('L')
        else:
            metallic = Image.new('L', normal.size, 255)
        albedo = f'{PREFIX}m{index}.png'
        prepared = SOURCE / material['prepared']
        entries[albedo] = prepared.read_bytes()
        # The native renderer adjusts base maps for r_gamma and r_intensity
        # on upload. Keep a separate ungraded sRGB copy for linear shading.
        with Image.open(prepared) as image:
            color = image.convert('RGB')
        if not (normal.size == color.size == roughness.size == metallic.size):
            raise ValueError('Mismatched source map sizes for ' + name)
        if max(normal.size) > 2048:
            raise ValueError('Source map exceeds the M4 loader limit for ' + name)
        # Keep the original RGB normals and put each linear scalar in an
        # alpha channel. Separate files bound each legacy FS_ReadFile call.
        normal.putalpha(roughness)
        color.putalpha(metallic)
        header = struct.pack('<4s2I', b'S2M4', *normal.size)
        path = f'{PREFIX}p{index}.bin'
        raw_albedo = f'{PREFIX}c{index}.bin'
        entries[path] = header + normal.tobytes()
        entries[raw_albedo] = header + color.tobytes()
        report.append(dict(material=name, albedo=albedo, raw_albedo=raw_albedo, parameters=path, size=normal.size,
                           sources=[original.name, *(source.name if source else 'steel metallic=1' for source in paths)]))
    if len(report) != 8:
        raise ValueError('Expected all eight M4 material slots')
    return entries, report


def build():
    pack = ROOT / 'resources/z_sof2vr_native.pk3'
    pending = pack.with_suffix('.m4-materials-pending')
    with zipfile.ZipFile(pack) as source:
        if MANIFEST in source.namelist() and json.loads(source.read(MANIFEST)).get('version') == VERSION:
            return
        entries, materials = material_maps()
        path = 'shaders/weapons.shader'
        shader = source.read(path).decode('cp1252')
        # Retire the prior additive tint. The same native base stage serves as
        # the fallback and identifies these surfaces at the stereo draw hook.
        pattern = re.compile(r'(?m)^(' + PREFIX + r'm\d+)\s*\{[^{}]*(?:\{[^{}]*\}\s*)+\}')
        def native_stage(match):
            name = match[1]
            return f'{name}\n{{\n cull disable\n {{\n map {name}.png\n rgbGen lightingDiffuse\n }}\n}}'
        shader, count = pattern.subn(native_stage, shader)
        if count != len(materials):
            raise ValueError('M4 shader slots do not match the source materials')
        entries[path] = shader.encode('cp1252')
        if FINISHES in source.namelist():
            finishes = json.loads(source.read(FINISHES))
            for name, data in entries.items():
                if name in finishes['outputs'] or name == path:
                    finishes['outputs'][name] = hashlib.sha256(data).hexdigest()
            entries[FINISHES] = (json.dumps(finishes, indent=2) + '\n').encode()
        entries[MANIFEST] = (json.dumps(dict(version=VERSION, materials=materials), indent=2) + '\n').encode()
        with zipfile.ZipFile(pending, 'w', compression=zipfile.ZIP_DEFLATED) as output:
            for info in source.infolist():
                output.writestr(info, entries.pop(info.filename, source.read(info)))
            for name, data in entries.items():
                output.writestr(name, data)
    os.replace(pending, pack)
    print('Packed eight M4 materials at their original source resolutions.')


if __name__ == '__main__':
    build()
