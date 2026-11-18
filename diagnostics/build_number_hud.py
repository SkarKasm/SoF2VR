"""Replace the three native status panels in the VR-only asset pack."""
import os
from pathlib import Path
import zipfile

from native_pose import read

ROOT = Path(__file__).resolve().parents[1]


def build():
    empty = read('huds/off.hud')
    left = b'<hud>\r\n<left>\r\n<font type lcd tint red>\r\n<ctext hud_time_display scale 0.33>\r\n</hud>\r\n'
    panels = {'huds/health.hud': empty, 'huds/right.hud': empty, 'huds/left.hud': left}
    pack = ROOT / 'resources/z_sof2vr_native.pk3'
    pending = pack.with_suffix('.hud-pending')
    if pending.exists():
        raise RuntimeError('Interrupted HUD build exists')
    try:
        with zipfile.ZipFile(pack) as source:
            changes = {n: data for n,data in panels.items() if n not in source.namelist() or source.read(n) != data}
            if not changes:
                return
            with zipfile.ZipFile(pending, 'x', zipfile.ZIP_DEFLATED, compresslevel=2) as target:
                for info in source.infolist():
                    target.writestr(info, changes.pop(info.filename) if info.filename in changes else source.read(info))
                for name, data in changes.items():
                    target.writestr(name, data)
        os.replace(pending, pack)
    finally:
        pending.unlink(missing_ok=True)


if __name__ == '__main__':
    build()
