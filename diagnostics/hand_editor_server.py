"""Local-only SoF2 hand editor. Never launches the game or a VR runtime."""
import argparse, copy, gzip, hashlib, json, os, secrets, subprocess, sys, threading, time, webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit
from manual_hand_pose import ROOT, STATE, OVERRIDES, defaults, validate, saved_edits, atomic_json, load_cache, hand_preview
WEB = ROOT/'tools/hand-editor'
TOKEN = secrets.token_urlsafe(32)
LOCK = threading.RLock()
JOB = dict(application='sof2-hand-editor',running=False, message='Ready', log='')
CACHE = None

def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def inputs_digest():
    h=hashlib.sha256()
    for name in ['resources/hand_pose_overrides.json','build/hand-editor/poses.json.gz',
                 'resources/knife_collision.json','diagnostics/build_knife_collision.py',
                 'diagnostics/repair_reload_bodies.py','diagnostics/build_magazine_interactions.py',
                 'resources/magazine_interactions.json','diagnostics/repair_texture_formats.py','diagnostics/repair_feedback_assets.py',
                 'diagnostics/build_dropped_weapons.py','diagnostics/build_vr_options_menu.py',
                 'diagnostics/repair_visual_finishes.py','diagnostics/repair_m3_port.py',
                 'diagnostics/build_holsters.py','diagnostics/export_holsters.py',
                 'diagnostics/restore_native_wrists.py','diagnostics/close_glove_wrist.py','diagnostics/build_body_assets.py','diagnostics/build_number_hud.py',
                 'resources/holster_asset_candidates.json','diagnostics/repair_action_anchors.py',
                 'diagnostics/build_m4_materials.py','diagnostics/build_weapon_materials.py','diagnostics/build_weapon_source_maps.py']:
        path=ROOT/name
        if path.exists():h.update(path.read_bytes())
    # Compiled code and generated locator headers must still match the receipt.
    for path in sorted((ROOT/'src').glob('*')):
        if path.suffix in ['.c','.h']:h.update(path.read_bytes())
    return h.hexdigest()

def outputs():
    return [ROOT/'resources/z_sof2vr_native.pk3',ROOT/'build/sof2_vr_proxy.dll',ROOT/'build/sof2-vr-x64/sof2_vr_bridge64.exe']

def run_command(args, env=None):
    process=subprocess.Popen([str(v) for v in args],cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                             text=True,errors='replace',creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    for line in process.stdout:
        with LOCK:
            JOB['log']=(JOB['log']+line)[-14000:]
            JOB['message']=line.strip() or JOB['message']
        print(line,end='',flush=True)
    if process.wait():raise RuntimeError('Build command failed; see the log below')

def build(rebuild_assets=True):
    global CACHE
    saved_before=digest(OVERRIDES) if OVERRIDES.exists() else None
    env=os.environ.copy();env.pop('SOF2_VR_AUTHORING_WEAPONS',None)
    with LOCK: JOB.update(message='Building saved poses and weapon sizes…',log='')
    if rebuild_assets:
        run_command([sys.executable,'-u','diagnostics/build_native_assets.py'],env)
    else:
        # Runtime-only repairs keep the installed, user-reviewed meshes byte
        # identical. Reject a stale pack rather than relabeling it as current.
        import zipfile
        with zipfile.ZipFile(outputs()[0]) as z:
            if 'sof2vr-authoring-preview.txt' in z.namelist() or validate(json.loads(z.read('sof2vr-manual-hand-edits.json')))!=saved_edits():
                raise RuntimeError('Saved poses differ from the pack; rebuild assets first')
    from repair_feedback_assets import repair as repair_feedback_assets
    repair_feedback_assets()
    from build_magazine_interactions import generate as magazine_interactions
    magazine_interactions()
    from repair_texture_formats import repair as repair_texture_formats
    repair_texture_formats()
    from build_knife_collision import generate as knife_collision
    knife_collision()
    from repair_visual_finishes import repair as visual_finishes
    visual_finishes()
    from build_m4_materials import build as m4_materials
    m4_materials()
    from repair_m3_port import repair as clean_m3_port
    clean_m3_port()
    from repair_action_anchors import repair as action_contacts
    action_contacts()
    from repair_reload_bodies import repair as repair_reload_bodies
    repair_reload_bodies()
    from build_dropped_weapons import build as dropped_weapons
    dropped_weapons()
    from build_holsters import rebuild as holster_assets
    holster_assets()
    from restore_native_wrists import build as native_wrists
    native_wrists()
    from build_vr_options_menu import build as native_vr_options
    native_vr_options()
    from build_weapon_source_maps import build as source_maps
    source_maps()
    from build_number_hud import build as number_hud
    number_hud()
    bin=ROOT/'llvm-mingw-20261006-msvcrt-i686/bin'
    run_command([bin/'mingw32-make.exe','-C','build','-f','Makefile.mingw','sof2_vr_proxy.dll'])
    run_command([bin/'x86_64-w64-mingw32-clang.exe','-O2','-g','-Wall','-Wextra','-Wno-missing-field-initializers',
                 '-DWIN32','-D_WIN32_WINNT=0x0602','-Iinclude','-Isrc','src/bridge64.c','src/bridge_actions.c',
                 'src/weapon_wheel.c','-o','build/sof2-vr-x64/sof2_vr_bridge64.exe','-Lbuild/openxr-x64/src/loader',
                 '-lopenxr_loader','-lopengl32','-luser32','-lgdi32','-ladvapi32','-ld3d11','-ldxgi','-ldxguid',
                 '-Wl,--no-insert-timestamp'])
    import zipfile
    with zipfile.ZipFile(outputs()[0]) as z:
        if 'sof2vr-authoring-preview.txt' in z.namelist():raise RuntimeError('Partial pack cannot be installed')
        packed_edits=validate(json.loads(z.read('sof2vr-manual-hand-edits.json')))
    saved_after=digest(OVERRIDES) if OVERRIDES.exists() else None
    if saved_before!=saved_after or packed_edits!=saved_edits():
        raise RuntimeError('Saved poses changed during this build. Build again before installing.')
    atomic_json(STATE/'built.json',dict(inputs=inputs_digest(),files={str(p):digest(p) for p in outputs()}))
    CACHE=load_cache()
    with LOCK:JOB['message']='Build complete. Review your poses, close the game and launcher, then Install VR build.'

def install():
    """One original rollback plus temporary hardlinks, independent of install count."""
    import shutil
    game=ROOT.parent/'Soldier of Fortune 2'
    if (game/'sof2-launcher/flat-session.pending').exists():raise RuntimeError('Close the launcher and finish its flatscreen session first')
    running=subprocess.run(['powershell.exe','-NoProfile','-Command',"Get-Process -Name SoF2,SoF2MP,sof2_vr_bridge64,'SoF2 Launcher' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id"],capture_output=True,text=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if running.stdout.strip():raise RuntimeError('Close SoF2, its launcher and VR helper before installing')
    receipt_path=STATE/'built.json'
    if not receipt_path.exists():raise RuntimeError('Build saved poses first')
    receipt=json.loads(receipt_path.read_text())
    if receipt['inputs']!=inputs_digest():raise RuntimeError('Edits or source changed. Build saved poses again')
    for p in outputs():
        if digest(p)!=receipt['files'].get(str(p)):raise RuntimeError('Build output changed; rebuild before installing')
    destinations=[game/'base/z_sof2vr_native.pk3',game/'opengl32.dll',game/'sof2-vr-x64/sof2_vr_bridge64.exe']
    rollback=STATE/'rollback';rollback.mkdir(exist_ok=True)
    transaction=STATE/'transaction';transaction.mkdir(exist_ok=True)
    if any(transaction.iterdir()):raise RuntimeError('An interrupted install needs recovery; see build/hand-editor/transaction')
    original=rollback/'manifest.json'
    if not original.exists():
        manifest=[]
        for i,dest in enumerate(destinations):
            if not dest.exists():raise RuntimeError('Expected installed VR file missing: '+str(dest))
            saved=rollback/f'{i}-{dest.name}'
            if not saved.exists():os.link(dest,saved)
            manifest.append(dict(destination=str(dest),backup=str(saved),sha256=digest(saved)))
        atomic_json(original,manifest)
    rows=[]
    for i,(source,dest) in enumerate(zip(outputs(),destinations)):
        link=transaction/f'{i}-{dest.name}';os.link(dest,link)
        rows.append((source,dest,link))
    atomic_json(transaction/'manifest.json',[dict(destination=str(d),backup=str(b)) for s,d,b in rows])
    def replace(source,dest):
        pending=dest.with_name(dest.name+'.editor-pending')
        shutil.copy2(source,pending)
        if digest(source)!=digest(pending):raise RuntimeError('Staged hash mismatch')
        os.replace(pending,dest)
    restored=True
    try:
        for source,dest,backup in rows:replace(source,dest)
        for source,dest,backup in rows:
            if digest(source)!=digest(dest):raise RuntimeError('Installed hash mismatch')
    except BaseException:
        restored=False
        for source,dest,backup in rows:replace(backup,dest)
        restored=True
        raise
    finally:
        if restored:
            for source,dest,backup in rows:backup.unlink(missing_ok=True)
            (transaction/'manifest.json').unlink(missing_ok=True)
    atomic_json(STATE/'installed.json',dict(time=time.time(),files={str(p):digest(p) for p in destinations},manualEditor=True))
    with LOCK:JOB['message']='Installed into VR mode. Launch VR through the normal SoF2 launcher. No game was started.'

def start_job(action):
    with LOCK:
        if JOB['running']:raise ValueError('A build/install is already running')
        JOB.update(running=True,message='Starting '+action,log='')
    def worker():
        try: (build if action=='build' else install)()
        except Exception as error:
            import traceback
            with LOCK:JOB.update(message=str(error),log=JOB['log']+'\n'+traceback.format_exc())
        finally:
            with LOCK:JOB['running']=False
    threading.Thread(target=worker,daemon=True).start()

class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,value,status=200,mime='application/json'):
        data=json.dumps(value,separators=(',',':')).encode() if mime=='application/json' else value
        self.send_response(status);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(data)))
        self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff')
        self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self'; object-src 'none'; frame-ancestors 'none'")
        self.end_headers();self.wfile.write(data)
    def do_GET(self):
        try:
            path=urlsplit(self.path).path
            if self.headers.get('Host')!=f'127.0.0.1:{self.server.server_port}':return self.reply({'error':'Invalid host'},403)
            if path=='/api/state':
                catalog=json.loads((STATE/'catalog.json').read_text())
                return self.reply(dict(token=TOKEN,catalog=catalog,edits=saved_edits(),revision=digest(OVERRIDES) if OVERRIDES.exists() else None,job=copy.copy(JOB)))
            if path=='/api/status':return self.reply(copy.copy(JOB))
            if path.startswith('/api/weapon/'):
                id=int(path.rsplit('/',1)[1])
                with gzip.open(STATE/f'w{id:02d}.json.gz','rt') as f:data=json.load(f)
                from hand_editor_view import throwable_view
                data['editorView']=throwable_view(id)
                return self.reply(data)
            rel='index.html' if path=='/' else path.lstrip('/')
            target=(WEB/rel).resolve()
            if WEB.resolve() not in target.parents or not target.is_file():return self.reply({'error':'Not found'},404)
            mime={'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8','.txt':'text/plain'}.get(target.suffix,'application/octet-stream')
            self.reply(target.read_bytes(),mime=mime)
        except Exception as e:self.reply({'error':str(e)},400)
    def do_POST(self):
        try:
            origin=f'http://127.0.0.1:{self.server.server_port}'
            if self.headers.get('Origin')!=origin or self.headers.get('X-Editor-Token')!=TOKEN:
                return self.reply({'error':'Editor restarted. Export any unsaved edits, then refresh this tab.'},403)
            length=int(self.headers.get('Content-Length','0'))
            if not 0<length<1024*1024:raise ValueError('Invalid request size')
            data=json.loads(self.rfile.read(length));path=urlsplit(self.path).path
            if path=='/api/preview':
                id=str(int(data['id']));label=data['side'];side={'right':'r','left':'l'}[label]
                edit=validate(dict(version=1,weapons={id:dict(hands={label:data['edit']})}))['weapons'][id]['hands'][label]
                e=CACHE['poses'][id][side];grip=CACHE['grips'][id]
                return self.reply(hand_preview(e,side,grip,edit))
            if path=='/api/validate':return self.reply(validate(data))
            if path=='/api/shutdown':
                with LOCK:
                    if JOB['running']:raise ValueError('Wait for the build/install to finish before stopping the editor')
                self.reply(dict(message='Editor stopped'))
                threading.Thread(target=self.server.shutdown,daemon=True).start()
                return
            if path=='/api/save':
                if 'edits' not in data or 'revision' not in data:
                    return self.reply({'error':'Editor page is outdated. Export any unsaved edits, then refresh this tab.'},409)
                edits=validate(data['edits'])
                with LOCK:
                    if JOB['running']:raise ValueError('Wait for the active build/install to finish')
                    current=digest(OVERRIDES) if OVERRIDES.exists() else None
                    if current!=data['revision']:
                        return self.reply({'error':'Saved poses changed outside this tab. Export any unsaved edits, then refresh to load the latest poses.'},409)
                    if OVERRIDES.exists():atomic_json(OVERRIDES.with_name('hand_pose_overrides.previous.json'),saved_edits())
                    atomic_json(OVERRIDES,edits)
                return self.reply(dict(message='Saved to resources/hand_pose_overrides.json',revision=digest(OVERRIDES)))
            if path in ['/api/build','/api/install']:
                start_job(path.rsplit('/',1)[1]);return self.reply(dict(message='Started'))
            self.reply({'error':'Not found'},404)
        except Exception as e:self.reply({'error':str(e)},400)

def main():
    global CACHE
    parser=argparse.ArgumentParser();parser.add_argument('--no-browser',action='store_true');parser.add_argument('--port',type=int,default=8765);args=parser.parse_args()
    # Reuse the editor if its launcher is double-clicked twice.
    info=STATE/'server.json'
    if info.exists():
        from urllib.request import urlopen
        try:
            url=json.loads(info.read_text())['url'];parsed=urlsplit(url)
            if parsed.hostname=='127.0.0.1' and parsed.scheme=='http':
                with urlopen(url+'api/status',timeout=1) as response:old=json.load(response)
                if old.get('application')=='sof2-hand-editor':
                    print('Editor already running: '+url,flush=True)
                    if not args.no_browser:webbrowser.open(url)
                    return
        except (OSError,ValueError,KeyError):pass
    CACHE=load_cache()
    if not CACHE or not (STATE/'catalog.json').exists():
        print('Preparing the editor baseline. This can take several minutes the first time. No files will be installed.',flush=True)
        env=os.environ.copy();env.pop('SOF2_VR_AUTHORING_WEAPONS',None)
        run_command([sys.executable,'-u','diagnostics/build_native_assets.py'],env)
        CACHE=load_cache()
        if not CACHE:raise SystemExit('Baseline preparation failed. Keep this window open and read the build error above.')
    try:server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler)
    except OSError:
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
    url=f'http://127.0.0.1:{server.server_port}/'
    print('SoF2 hand editor: '+url,flush=True)
    print('Close this window to stop the editor. No game/VR runtime will be launched.',flush=True)
    atomic_json(STATE/'server.json',dict(url=url,pid=os.getpid()))
    if not args.no_browser:webbrowser.open(url)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close()

if __name__=='__main__':main()
