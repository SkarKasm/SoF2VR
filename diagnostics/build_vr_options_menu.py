"""Add native SP main-menu VR settings without rebuilding weapon/hand assets.

RMF list/frame/font/command syntax is taken from the installed game's menus.
All overrides stay in the VR pack, which the mode launcher parks for flatscreen.
"""
import hashlib,json,os,re,zipfile
from pathlib import Path
from native_pose import read

ROOT=Path(__file__).resolve().parents[1]
PAGES={
    'general':[
        ('Weapon hand','LeftHanded','Right,Left','0,1'),
        ('Weapon grip','ToggleGrip','Hold,Toggle','0,1'),
        ('Playing position','Seated','Standing,Seated','0,1'),
        ('Reloading','ManualReload','Assisted,Manual','0,1'),
        ('Shotgun pump','ManualPump','Automatic,Manual','0,1'),
        ('Crouch button','CrouchToggle','Hold,Toggle','0,1'),
    ],
    'movement':[
        ('Turning','SnapAngle','Smooth,Snap 15,Snap 30,Snap 45,Snap 60,Snap 90','0,15,30,45,60,90'),
        ('Smooth speed (degrees/s)','TurnSpeed','60,90,120,150,180,210,240,270,300,330,360','60,90,120,150,180,210,240,270,300,330,360'),
        ('Walk direction','WalkDirection','Head,Support hand','0,1'),
        ('Swap movement/turn sticks','SwitchSticks','Off,On','0,1'),
        ('Stick deadzone','DeadzonePercent','10%,15%,25%,35%,50%,60%,70%','10,15,25,35,50,60,70'),
        ('Room-scale lean','RoomScale','Off,On','0,1'),
        ('Crouch by ducking','PhysicalCrouch','Off,On (standing)','0,1'),
    ],
    'comfort':[
        ('Controller vibration','HapticsPercent','Off,25%,50%,75%,100%','0,25,50,75,100'),
        ('Comfort vignette','VignettePercent','Off,25%,50%,75%,100%','0,25,50,75,100'),
        ('Enemy damage received','EnemyDamagePercent','25%,50%,75%,100%','25,50,75,100'),
        ('Aim reticle','Reticle','Hidden,Shown','0,1'),
        ('VR hands','ShowHands','Hidden,Shown','0,1'),
        ('Grip activation','GripThresholdPercent','20%,25%,40%,55%,65%,75%,90%','20,25,40,55,65,75,90'),
        ('Trigger activation','TriggerThresholdPercent','20%,25%,40%,55%,65%,75%,90%','20,25,40,55,65,75,90'),
    ],
    'holsters':[
        ('Holster reach','HolsterReachPercent','50%,65%,80%,100%,120%,150%','50,65,80,100,120,150'),
        ('Holster height (cm)','HolsterHeightCm','-30,-20,-10,0,+10,+20,+30','-30,-20,-10,0,10,20,30'),
        ('Wheel hold (ms)','HolsterHoldMs','Instant,150,250,350,500,750,1000,1500','0,150,250,350,500,750,1000,1500'),
    ],
}

def page_name(group):return 'vr_options' if group=='general' else 'vr_options_'+group
def generate():
    source=read('menus/m_game.rmf').decode('cp1252').replace('\r\r\n','\n').replace('\r\n','\n')
    anchor='<text "  &MENU_GAME_OPTIONS& " key mouse1 "menu keys;refresh" btint clickable><br>'
    if source.count(anchor)!=1:raise ValueError('Unexpected native main menu; refuse a blind edit')
    button='\n<text "  ">\n<image "icons/icon_options" scalex .20 scaley .20 tint smokey>\n<text "  VR OPTIONS " key mouse1 "set vr_uiReload 1;menu vr_options;refresh" btint clickable><br>\n'
    entries={'menus/m_game.rmf':source.replace(anchor,anchor+'\n'+button).encode('cp1252')}
    bound=[]
    for group,options in PAGES.items():
        name=page_name(group)
        entries['menus/'+name+'.rmf']=('\n'.join([
            '<stm>','<include mg_options>',
            f'<frame subtopic 0 sub_top_border page t_{name} cut main>',
            f'<frame submain 0 0 page m_{name} cut main>','</stm>',''])
        ).encode('ascii')
        tabs=['<stm>','<font type medium tint clickable atint reversed>']
        for target in PAGES:
            tabs.append(f'<text " {target.upper()} " key mouse1 "menu {page_name(target)};refresh" border 8 1 text>')
        tabs += ['<right>','<image "icons/submenu_back" alt "icons/submenu_back_alt" tint clickable border 12 1 text key mouse1 "set vr_uiReload 1;menu main;refresh" scale .5 proportional tip "Return to main menu">','<hr tint text line 4>','</stm>','']
        entries[f'menus/t_{name}.rmf']='\n'.join(tabs).encode('ascii')
        body=['<stm>','<vbar tint vbartint>','<center>','<font type title tint text dropshadow>',f'<text "VR OPTIONS - {group.upper()}">','<hr width 320 tint text>','<font type small tint text>','<text "Click a value to cycle it. SAVE AND APPLY keeps your changes."><hbr>',
              '<font type medium tint clickable atint reversed>']
        for label,key,labels,values in options:
            if len(labels.split(','))!=len(values.split(',')):raise ValueError('Mismatched list: '+key)
            bound.append(key)
            body += [f'<list "{labels}" match "{values}" cvar vr_ui_{key} atext "{label} : " btint clickable key mouse1 "set vr_uiEdited 1" key mouse2 "set vr_uiEdited 1"><hbr>']
        body += ['<hbr>','<font type small tint text>']
        if group=='holsters':body += ['<text "All weapon slots mirror with your weapon hand."><br>','<text "Right stick selects. Trigger equips. Release grip drops."><br>','<text "Click right stick for the save/load page."><br>']
        elif group=='general':body += ['<text "Hold: keep grip held to carry; release to drop."><br>','<text "Toggle: press again away from a holster to drop."><br>']
        elif group=='movement':body += ['<text "Hold the movement stick click while pushing forward to run."><br>','<text "Both stick clicks held for one second recenter the view."><br>','<text "Physical ducking is disabled in seated mode."><br>']
        else:body += ['<text "Lower activation values need less controller pressure."><br>']
        body += ['<hbr>','<font type medium tint clickable atint hilited>',
                 '<text "SAVE AND APPLY" key mouse1 "set vr_uiApply 1" border 12 1 text><hbr>',
                 '<text "BACK TO MAIN MENU" key mouse1 "set vr_uiReload 1;menu main;refresh" border 12 1 text><hbr>',
                 '<font type small tint text>','<ctext vr_uiMessage>',
                 '<hbr>','<blank 16 16>','</stm>','']
        entries[f'menus/m_{name}.rmf']='\n'.join(body).encode('ascii')
    header=(ROOT/'src/vr_accessibility.h').read_text()
    expected=re.findall(r'VR_ACCESS_OPTION\("([^"]+)"',header)
    if sorted(bound)!=sorted(expected) or len(set(bound))!=len(bound):raise ValueError('Menu must cover each accessibility setting exactly once')
    manifest=dict(version=1,entry='menus/m_game.rmf',pages=[page_name(p) for p in PAGES],settings=bound,canonical='sof2-vr-x64/quality.ini [Accessibility]',gameplayTested=False)
    entries['sof2vr-native-options.json']=(json.dumps(manifest,indent=2)+'\n').encode()
    return entries,manifest

def build():
    entries,manifest=generate();pack=ROOT/'resources/z_sof2vr_native.pk3';pending=pack.with_suffix('.menu-pending')
    with zipfile.ZipFile(pack) as source:
        protected={n:hashlib.sha256(source.read(n)).digest() for n in source.namelist() if n not in entries}
        try:
            with zipfile.ZipFile(pending,'w',compression=zipfile.ZIP_DEFLATED) as out:
                for info in source.infolist():
                    if info.filename not in entries:out.writestr(info,source.read(info))
                for name,data in entries.items():out.writestr(name,data)
            with zipfile.ZipFile(pending) as check:
                for name,digest in protected.items():
                    if hashlib.sha256(check.read(name)).digest()!=digest:raise ValueError('Unrelated packed asset changed: '+name)
        except BaseException:pending.unlink(missing_ok=True);raise
    os.replace(pending,pack)
    (ROOT/'build/native-vr-options-menu.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Packed native VR OPTIONS main-menu entry, four settings pages and all',len(manifest['settings']),'settings; other assets unchanged.')
if __name__=='__main__':build()
