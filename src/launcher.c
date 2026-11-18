/* Native single-player mode launcher. No runtime registry changes. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <wctype.h>
#include <stdio.h>
#include <string.h>

static WCHAR root[MAX_PATH],game[MAX_PATH],pack[MAX_PATH],park[MAX_PATH];
static WCHAR cfg[MAX_PATH],backup[MAX_PATH],flatCfg[MAX_PATH],journal[MAX_PATH];
static WCHAR assetJournal[MAX_PATH];
#define MAX_MODE_ASSETS 64
static WCHAR assetSources[MAX_MODE_ASSETS][MAX_PATH],assetParks[MAX_MODE_ASSETS][MAX_PATH];
static unsigned assetCount;
static HWND window,statusLabel,flatButton,vrButton;
static HANDLE child;
static int playingFlat;

static void Path(WCHAR *out,const WCHAR *relative){_snwprintf(out,MAX_PATH,L"%ls\\%ls",root,relative);out[MAX_PATH-1]=0;}
static int Exists(const WCHAR *p){return GetFileAttributesW(p)!=INVALID_FILE_ATTRIBUTES;}
static void Error(const WCHAR *what){
    WCHAR text[1024],detail[512];DWORD code=GetLastError();
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,code,0,detail,512,NULL);
    _snwprintf(text,1024,L"%ls\n\n%ls (error %lu)\n\nYour mod files and saved games have not been deleted.",what,detail,code);
    MessageBoxW(window,text,L"SoF2 Launcher",MB_OK|MB_ICONERROR);
}
/* Refuse switches while either single player or multiplayer owns the assets. */
static int GameRunning(void){
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W e={0};
    if(snap==INVALID_HANDLE_VALUE)return 1;
    e.dwSize=sizeof(e);int found=0;
    if(Process32FirstW(snap,&e))do{
        if(!_wcsicmp(e.szExeFile,L"SoF2.exe") || !_wcsicmp(e.szExeFile,L"SoF2MP.exe")){found=1;break;}
    }while(Process32NextW(snap,&e));
    CloseHandle(snap);return found;
}
static int AddFlatAsset(const WCHAR *relative){
    if(assetCount>=MAX_MODE_ASSETS){SetLastError(ERROR_BUFFER_OVERFLOW);return 0;}
    WCHAR source[MAX_PATH],destination[MAX_PATH],section[32],parkRelative[MAX_PATH];
    Path(source,relative);if(!Exists(source))return 1;
    const WCHAR *name=wcsrchr(relative,L'\\');name=name?name+1:relative;
    _snwprintf(parkRelative,MAX_PATH,L"sof2-launcher\\flat-assets\\%ls.disabled",name);
    Path(destination,parkRelative);
    if(Exists(destination)){SetLastError(ERROR_ALREADY_EXISTS);return 0;}
    _snwprintf(section,32,L"Asset%u",assetCount);
    if(!WritePrivateProfileStringW(section,L"Source",relative,assetJournal) ||
       !WritePrivateProfileStringW(section,L"Park",parkRelative,assetJournal))return 0;
    wcscpy(assetSources[assetCount],source);wcscpy(assetParks[assetCount],destination);++assetCount;return 1;
}
static int CollectFlatAssets(void){
    assetCount=0;
    if(Exists(assetJournal) && !DeleteFileW(assetJournal))return 0;
    WCHAR directory[MAX_PATH];Path(directory,L"sof2-launcher\\flat-assets");
    if(!CreateDirectoryW(directory,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS)return 0;
    if(!AddFlatAsset(L"opengl32.dll"))return 0;
    WCHAR pattern[MAX_PATH];Path(pattern,L"base\\*sof2vr*.pk3");
    WIN32_FIND_DATAW data;HANDLE find=FindFirstFileW(pattern,&data);
    if(find!=INVALID_HANDLE_VALUE){
        do{if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){
            WCHAR relative[MAX_PATH];_snwprintf(relative,MAX_PATH,L"base\\%ls",data.cFileName);
            if(!AddFlatAsset(relative)){FindClose(find);return 0;}
        }}while(FindNextFileW(find,&data));FindClose(find);
    }else if(GetLastError()!=ERROR_FILE_NOT_FOUND)return 0;
    WCHAR count[16];_snwprintf(count,16,L"%u",assetCount);
    if(!WritePrivateProfileStringW(L"Assets",L"Count",count,assetJournal))return 0;
    /* The all-NULL profile call flushes the INI cache; its return value
     * is not a file-write success result. Check the actual file instead. */
    WritePrivateProfileStringW(NULL,NULL,NULL,assetJournal);
    HANDLE manifest=CreateFileW(assetJournal,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(manifest==INVALID_HANDLE_VALUE)return 0;
    BOOL flushed=FlushFileBuffers(manifest);DWORD error=GetLastError();CloseHandle(manifest);
    if(!flushed){SetLastError(error);return 0;}
    return 1;
}
static int LoadFlatAssets(void){
    assetCount=GetPrivateProfileIntW(L"Assets",L"Count",0,assetJournal);
    if(!assetCount || assetCount>MAX_MODE_ASSETS){SetLastError(ERROR_INVALID_DATA);return 0;}
    for(unsigned i=0;i<assetCount;++i){
        WCHAR section[32],source[MAX_PATH],parked[MAX_PATH];_snwprintf(section,32,L"Asset%u",i);
        GetPrivateProfileStringW(section,L"Source",L"",source,MAX_PATH,assetJournal);
        GetPrivateProfileStringW(section,L"Park",L"",parked,MAX_PATH,assetJournal);
        /* Accept only generated paths inside this installation. */
        if(!*source || wcsstr(source,L"..") || wcschr(source,L':') || source[0]==L'\\' || source[0]==L'/' ||
           (wcscmp(source,L"opengl32.dll") && wcsncmp(source,L"base\\",5)) ||
           wcsncmp(parked,L"sof2-launcher\\flat-assets\\",26) || wcsstr(parked,L"..") || wcschr(parked,L':')){
            SetLastError(ERROR_INVALID_DATA);return 0;
        }
        Path(assetSources[i],source);Path(assetParks[i],parked);
        if(Exists(assetParks[i]) && Exists(assetSources[i])){SetLastError(ERROR_ALREADY_EXISTS);return 0;}
        if(!Exists(assetParks[i]) && !Exists(assetSources[i])){SetLastError(ERROR_FILE_NOT_FOUND);return 0;}
    }return 1;
}
static int VRSetting(const char *line){
    char command[16],name[100];if(sscanf(line,"%15s %99s",command,name)!=2)return 0;
    if(_stricmp(command,"set") && _stricmp(command,"seta") && _stricmp(command,"sets"))return 0;
    const char *names[]={"cg_bobup","cg_bobpitch","cg_bobroll","cg_runpitch","cg_runroll","cg_drawCrosshair","com_maxfps","r_swapInterval","r_finish","r_glDriver","fs_game","r_gamma","r_ignorehwgamma","r_overBrightBits","r_intensity"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i)if(!_stricmp(name,names[i]))return 1;return 0;
}
static int CreateFlatSettings(void){
    FILE *input=_wfopen(cfg,L"rb"),*output=_wfopen(flatCfg,L"wb");
    if(!input || !output){if(input)fclose(input);if(output)fclose(output);SetLastError(ERROR_OPEN_FAILED);return 0;}
    char line[4096];int ok=1;
    while(fgets(line,sizeof(line),input))if(!VRSetting(line) && fputs(line,output)==EOF){ok=0;break;}
    ok=ok && !ferror(input);if(fclose(output))ok=0;fclose(input);return ok;
}
/* The journal is written before any shared game file is changed. Restore the
 * original VR config and asset on normal exit or next-launch crash recovery.
 * MoveFile never overwrites an unexpected second copy of the asset. */
static int Recover(int saveFlat){
    if(!Exists(journal))return 1;
    /* Validate config and all file destinations before restoring any asset. */
    if(!Exists(backup)){SetLastError(ERROR_FILE_NOT_FOUND);Error(L"The launcher recovery config is missing. Recovery stopped to protect your settings.");return 0;}
    if(Exists(assetJournal)){
        if(!LoadFlatAssets()){Error(L"Could not validate the flatscreen asset recovery list.");return 0;}
        for(unsigned i=0;i<assetCount;++i)if(Exists(assetParks[i]) && !MoveFileW(assetParks[i],assetSources[i])){
            Error(L"Could not restore a parked VR file. Reopen the launcher after closing the game.");return 0;
        }
    }else if(Exists(park)){
        if(Exists(pack)){SetLastError(ERROR_ALREADY_EXISTS);Error(L"Both copies of the VR asset pack exist. Resolve the duplicate before switching modes.");return 0;}
        if(!MoveFileW(park,pack)){Error(L"Could not restore the VR asset pack.");return 0;}
    }
    if(saveFlat && Exists(cfg) && !CopyFileW(cfg,flatCfg,FALSE)){Error(L"Could not save flatscreen settings.");return 0;}
    if(!CopyFileW(backup,cfg,FALSE)){Error(L"Could not restore VR settings.");return 0;}
    if(!DeleteFileW(journal)){Error(L"Could not finish mode recovery.");return 0;}
    if(Exists(assetJournal))DeleteFileW(assetJournal);
    return 1;
}
static int PrepareFlat(void){
    if(!CopyFileW(cfg,backup,FALSE)){Error(L"Could not back up VR settings.");return 0;}
    if(!Exists(flatCfg) && !CreateFlatSettings()){Error(L"Could not create separate native flatscreen settings.");return 0;}
    if(!CollectFlatAssets()){Error(L"Could not prepare complete VR isolation for flatscreen.");return 0;}
    HANDLE mark=CreateFileW(journal,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,NULL);
    if(mark==INVALID_HANDLE_VALUE){Error(L"Could not begin mode switch.");return 0;}
    const char text[]="SoF2 flat session: restore vr-session.cfg and all files in flat-assets.ini before VR launch.\r\n";DWORD written;
    BOOL ok=WriteFile(mark,text,sizeof(text)-1,&written,NULL) && written==sizeof(text)-1 && FlushFileBuffers(mark);CloseHandle(mark);
    if(!ok){Error(L"Could not save recovery information.");return 0;}
    for(unsigned i=0;i<assetCount;++i)if(!MoveFileW(assetSources[i],assetParks[i])){
        Error(L"Could not disable a VR file for flatscreen.");Recover(0);return 0;
    }
    if(!CopyFileW(flatCfg,cfg,FALSE)){Error(L"Could not load flatscreen settings.");Recover(0);return 0;}
    return 1;
}
static void Launch(int flat){
    if(child)return;
    if(GameRunning()){MessageBoxW(window,L"Close Soldier of Fortune II before changing modes.",L"SoF2 Launcher",MB_OK|MB_ICONINFORMATION);return;}
    if(!Recover(0))return;
    if(!Exists(game) || !Exists(pack)){SetLastError(ERROR_FILE_NOT_FOUND);Error(L"Place this launcher beside SoF2.exe with the VR mod installed.");return;}
    if(flat && !PrepareFlat())return;
    /* Environment edits belong only to this launcher and its child, never the
     * user's machine. Explicit VR mode overrides an inherited flat setting. */
    if(!SetEnvironmentVariableW(L"SOF2_VR_MODE",flat?L"flat":L"vr")){
        Error(L"Could not select the game mode.");if(flat)Recover(0);return;
    }
    WCHAR command[MAX_PATH+128];
    /* The native startup checks com_introplayed before queuing the two logos.
     * Skip those in VR only; story cinematics and the flat profile keep theirs. */
    _snwprintf(command,MAX_PATH+128,L"\"%ls\" +set r_glDriver opengl32 +set cg_drawCrosshair %d%ls",game,flat?1:0,flat?L"":L" +set com_introplayed 1");
    STARTUPINFOW si={0};PROCESS_INFORMATION pi={0};si.cb=sizeof(si);
    if(!CreateProcessW(game,command,NULL,NULL,FALSE,0,NULL,root,&si,&pi)){
        Error(L"Could not start Soldier of Fortune II.");if(flat)Recover(0);return;
    }
    CloseHandle(pi.hThread);child=pi.hProcess;playingFlat=flat;
    EnableWindow(flatButton,FALSE);EnableWindow(vrButton,FALSE);
    SetWindowTextW(statusLabel,flat?L"Flatscreen is running. Settings will be saved when it exits.":L"VR is running. SteamVR must be your active OpenXR runtime.");
    SetTimer(window,1,500,NULL);ShowWindow(window,SW_MINIMIZE);
}
#include "launcher_accessibility.h"
static LRESULT CALLBACK Proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_COMMAND: if(LOWORD(wp)==101)Launch(1);if(LOWORD(wp)==102)Launch(0);if(LOWORD(wp)==103)OpenVROptions();return 0;
    case WM_TIMER:
        if(child && WaitForSingleObject(child,0)==WAIT_OBJECT_0){
            KillTimer(w,1);CloseHandle(child);child=NULL;
            int ok=!playingFlat || Recover(1);
            EnableWindow(flatButton,ok);EnableWindow(vrButton,ok);
            SetWindowTextW(statusLabel,ok?L"Choose how you want to play.":L"Recovery needs attention. Reopen the launcher after resolving the error.");
            ShowWindow(w,SW_RESTORE);
        }return 0;
    case WM_CLOSE:
        if(child){MessageBoxW(w,L"Leave the launcher open until the game exits so it can restore your VR files and save settings.",L"SoF2 Launcher",MB_OK|MB_ICONINFORMATION);return 0;}
        DestroyWindow(w);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }return DefWindowProcW(w,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR args,int show){
    (void)previous;(void)args;(void)show;
    DWORD n=GetModuleFileNameW(NULL,root,MAX_PATH);
    if(!n || n>=MAX_PATH-90){MessageBoxW(NULL,L"Install the launcher in a shorter game folder path.",L"SoF2 Launcher",MB_OK);return 1;}
    WCHAR *slash=wcsrchr(root,L'\\');if(!slash)return 1;*slash=0;
    Path(game,L"SoF2.exe");Path(pack,L"base\\z_sof2vr_native.pk3");
    Path(park,L"sof2-launcher\\z_sof2vr_native.pk3.disabled");
    Path(cfg,L"base\\sof2sp.cfg");Path(backup,L"sof2-launcher\\vr-session.cfg");
    Path(flatCfg,L"sof2-launcher\\flatscreen.cfg");Path(journal,L"sof2-launcher\\flat-session.pending");
    Path(assetJournal,L"sof2-launcher\\flat-assets.ini");
    /* Per-install mutex also protects startup recovery from a second launcher. */
    unsigned hash=2166136261u;for(const WCHAR *c=root;*c;++c)hash=(hash^(unsigned)towlower(*c))*16777619u;
    WCHAR mutexName[80];_snwprintf(mutexName,80,L"Local\\SoF2ModeLauncher-%08x",hash);
    HANDLE mutex=CreateMutexW(NULL,FALSE,mutexName);
    if(!mutex || GetLastError()==ERROR_ALREADY_EXISTS){MessageBoxW(NULL,L"The launcher is already open.",L"SoF2 Launcher",MB_OK);if(mutex)CloseHandle(mutex);return 1;}
    if(GameRunning()){MessageBoxW(NULL,L"Close Soldier of Fortune II before opening the mode launcher.",L"SoF2 Launcher",MB_OK);CloseHandle(mutex);return 1;}
    WCHAR stateDir[MAX_PATH];Path(stateDir,L"sof2-launcher");
    if(!CreateDirectoryW(stateDir,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS){Error(L"Could not create launcher settings folder.");CloseHandle(mutex);return 1;}
    if(!Recover(0)){CloseHandle(mutex);return 1;}
    WNDCLASSW wc={0};wc.lpfnWndProc=Proc;wc.hInstance=instance;wc.lpszClassName=L"SoF2ModeLauncher";
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc);
    window=CreateWindowW(wc.lpszClassName,L"Soldier of Fortune II — Flatscreen / VR",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,540,330,NULL,NULL,instance,NULL);
    if(!window){CloseHandle(mutex);return 1;}
    HWND title=CreateWindowW(L"STATIC",L"Soldier of Fortune II",WS_CHILD|WS_VISIBLE,24,20,470,25,window,NULL,instance,NULL);
    statusLabel=CreateWindowW(L"STATIC",L"Choose how you want to play.",WS_CHILD|WS_VISIBLE,24,57,470,42,window,NULL,instance,NULL);
    flatButton=CreateWindowW(L"BUTTON",L"Play Flatscreen",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,24,110,225,48,window,(HMENU)101,instance,NULL);
    vrButton=CreateWindowW(L"BUTTON",L"Play VR",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,267,110,225,48,window,(HMENU)102,instance,NULL);
    optionsButton=CreateWindowW(L"BUTTON",L"VR Accessibility && Controls",WS_CHILD|WS_VISIBLE|WS_TABSTOP,24,170,468,32,window,(HMENU)103,instance,NULL);
    HWND note=CreateWindowW(L"STATIC",L"VR: connect Quest Link and start SteamVR first.\nKeep this launcher open while playing. Both modes share saved games.",WS_CHILD|WS_VISIBLE,24,218,475,48,window,NULL,instance,NULL);
    HWND controls[]={title,statusLabel,flatButton,vrButton,note,optionsButton};
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);++i)SendMessageW(controls[i],WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    ShowWindow(window,SW_SHOW);MSG msg;
    while(GetMessageW(&msg,NULL,0,0)>0){if(!IsDialogMessageW(optionsWindow?optionsWindow:window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    CloseHandle(mutex);return 0;
}
