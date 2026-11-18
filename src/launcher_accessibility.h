/* Native keyboard-accessible VR options, inspired by the creator's RTCW menu. */
#include "vr_accessibility.h"
#include <stddef.h>
static HWND optionsWindow,optionsButton;
typedef struct {const WCHAR *label;const char *key;size_t offset;int low,high;const WCHAR *choices;} VROptionControl;
#define VO(label,key,member,low,high,choices) {L##label,key,offsetof(VRAccessibility,member),low,high,choices}
static const VROptionControl optionControls[]={
    VO("Weapon hand","LeftHanded",leftHanded,0,1,L"Right|Left"),
    VO("Grip style","ToggleGrip",toggleGrip,0,1,L"Hold to hold / release to drop|Press to grab / press to drop"),
    VO("Snap turn angle (0 = smooth)","SnapAngle",snapAngle,0,90,NULL),
    VO("Smooth turn speed (degrees/s)","TurnSpeed",turnSpeed,60,360,NULL),
    VO("Walk direction","WalkDirection",walkDirection,0,1,L"Head|Support hand"),
    VO("Swap movement / turn sticks","SwitchSticks",switchSticks,0,1,L"Off|On"),
    VO("Stick deadzone (%)","DeadzonePercent",deadzone,10,70,NULL),
    VO("Grip activation (%)","GripThresholdPercent",gripThreshold,20,90,NULL),
    VO("Trigger activation (%)","TriggerThresholdPercent",triggerThreshold,20,90,NULL),
    VO("Holster reach (%)","HolsterReachPercent",holsterReach,50,150,NULL),
    VO("Holster height offset (cm)","HolsterHeightCm",holsterHeight,-30,30,NULL),
    VO("Holster wheel hold (ms)","HolsterHoldMs",holdMs,0,1500,NULL),
    VO("Playing position","Seated",seated,0,1,L"Standing|Seated"),
    VO("Crouch button","CrouchToggle",crouchToggle,0,1,L"Hold|Toggle"),
    VO("Crouch by ducking","PhysicalCrouch",physicalCrouch,0,1,L"Off|On (standing only)"),
    VO("Reloading","ManualReload",manualReload,0,1,L"Assisted (A reloads)|Manual (A ejects)"),
    VO("Shotgun pump","ManualPump",manualPump,0,1,L"Automatic|Manual"),
    VO("Controller vibration (%)","HapticsPercent",haptics,0,100,NULL),
    VO("Aim reticle","Reticle",reticle,0,1,L"Hidden|Shown"),
    VO("VR hands","ShowHands",hands,0,1,L"Hidden|Shown"),
    VO("Comfort vignette (%)","VignettePercent",vignette,0,100,NULL),
    VO("Enemy damage received (%)","EnemyDamagePercent",enemyDamage,25,100,NULL),
    VO("Room-scale lean","RoomScale",roomScale,0,1,L"Off|On")
};
#undef VO
static char optionsPath[MAX_PATH];
static LRESULT CALLBACK OptionsProc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_COMMAND && (LOWORD(wp)==502 || LOWORD(wp)==IDCANCEL)){DestroyWindow(w);return 0;}
    if(msg==WM_COMMAND && (LOWORD(wp)==501 || LOWORD(wp)==IDOK)){
        WCHAR text[64];int values[sizeof(optionControls)/sizeof(optionControls[0])];
        for(unsigned i=0;i<sizeof(optionControls)/sizeof(optionControls[0]);++i){
            const VROptionControl *c=&optionControls[i];HWND input=GetDlgItem(w,600+i);WCHAR *end;
            if(c->choices)values[i]=(int)SendMessageW(input,CB_GETCURSEL,0,0);
            else{GetWindowTextW(input,text,64);values[i]=(int)wcstol(text,&end,10);
                if(end==text || *end)values[i]=c->low-1;}
            if(values[i]<c->low || values[i]>c->high || (!strcmp(c->key,"SnapAngle") && values[i]>0 && values[i]<15)){
                WCHAR warning[200];_snwprintf(warning,200,L"%ls: enter a value from %d to %d.%ls",c->label,c->low,c->high,!strcmp(c->key,"SnapAngle")?L" Use 0 for smooth or 15-90 for snap.":L"");
                MessageBoxW(w,warning,L"VR options",MB_OK|MB_ICONINFORMATION);SetFocus(input);return 0;
            }
        }
        VRAccessibility saved=VR_ReadAccessibility(optionsPath);
        for(unsigned i=0;i<sizeof(optionControls)/sizeof(optionControls[0]);++i)*(int *)((char *)&saved+optionControls[i].offset)=values[i];
        if(!VR_WriteAccessibility(optionsPath,&saved)){
            MessageBoxW(w,L"Could not save VR options. Check folder write access.",L"VR options",MB_OK|MB_ICONERROR);return 0;
        }
        DestroyWindow(w);return 0;
    }
    if(msg==WM_CLOSE){DestroyWindow(w);return 0;}
    if(msg==WM_DESTROY){optionsWindow=NULL;EnableWindow(window,TRUE);SetForegroundWindow(window);return 0;}
    return DefWindowProcW(w,msg,wp,lp);
}
static void OpenVROptions(void){
    if(child || GameRunning()){MessageBoxW(window,L"Close the game before changing VR options.",L"VR options",MB_OK);return;}
    if(optionsWindow){SetForegroundWindow(optionsWindow);return;}
    WCHAR widePath[MAX_PATH];Path(widePath,L"sof2-vr-x64\\quality.ini");
    if(!WideCharToMultiByte(CP_ACP,0,widePath,-1,optionsPath,MAX_PATH,NULL,NULL))return;
    VRAccessibility a=VR_ReadAccessibility(optionsPath);
    HINSTANCE instance=GetModuleHandleW(NULL);WNDCLASSW wc={0};wc.lpfnWndProc=OptionsProc;wc.hInstance=instance;
    wc.lpszClassName=L"SoF2VROptions";wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);
    optionsWindow=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"SoF2 VR - Accessibility & Controls",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT,960,705,window,NULL,instance,NULL);
    if(!optionsWindow)return;
    HWND intro=CreateWindowW(L"STATIC",L"VR options apply on the next launch. Holsters mirror with your weapon hand. Right stick always selects wheel items.",WS_CHILD|WS_VISIBLE,20,15,910,32,optionsWindow,NULL,instance,NULL);
    SendMessageW(intro,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    for(unsigned i=0;i<sizeof(optionControls)/sizeof(optionControls[0]);++i){
        const VROptionControl *c=&optionControls[i];int x=20+(i/12)*465,y=56+(i%12)*46;
        HWND label=CreateWindowW(L"STATIC",c->label,WS_CHILD|WS_VISIBLE,x,y,235,20,optionsWindow,NULL,instance,NULL);
        HWND input=CreateWindowW(c->choices?L"COMBOBOX":L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|(c->choices?CBS_DROPDOWNLIST|WS_VSCROLL:WS_BORDER|ES_AUTOHSCROLL),
            x+240,y-2,205,c->choices?140:25,optionsWindow,(HMENU)(INT_PTR)(600+i),instance,NULL);
        int value=*(int *)((char *)&a+c->offset);
        if(c->choices){WCHAR choices[180];wcsncpy(choices,c->choices,179);choices[179]=0;WCHAR *begin=choices;
            for(WCHAR *p=choices;;++p)if(*p==L'|' || !*p){int last=!*p;*p=0;SendMessageW(input,CB_ADDSTRING,0,(LPARAM)begin);if(last)break;begin=p+1;}
            SendMessageW(input,CB_SETCURSEL,value,0);
        }else{WCHAR valueText[32];_snwprintf(valueText,32,L"%d",value);SetWindowTextW(input,valueText);}
        SendMessageW(label,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);SendMessageW(input,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    }
    HWND note=CreateWindowW(L"STATIC",L"At a holster: hold grip, point the right stick at an icon, trigger to equip. Right stick click changes pages (including save/load).\nHold grip to retain the weapon; release to drop. Toggle grip is available above. Smaller reach brings slots closer.",WS_CHILD|WS_VISIBLE,20,590,910,44,optionsWindow,NULL,instance,NULL);
    HWND save=CreateWindowW(L"BUTTON",L"Save",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,700,635,100,28,optionsWindow,(HMENU)501,instance,NULL);
    HWND cancel=CreateWindowW(L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,815,635,100,28,optionsWindow,(HMENU)502,instance,NULL);
    SendMessageW(note,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);SendMessageW(save,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);SendMessageW(cancel,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    EnableWindow(window,FALSE);ShowWindow(optionsWindow,SW_SHOW);SetFocus(GetDlgItem(optionsWindow,600));
}
