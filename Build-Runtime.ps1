param([Parameter(Mandatory=$true)][string]$ToolchainBin)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$out=Join-Path $root 'out'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$cc=Join-Path $ToolchainBin 'i686-w64-mingw32-gcc.exe'
$cc64=Join-Path $ToolchainBin 'x86_64-w64-mingw32-clang.exe'
$strip=Join-Path $ToolchainBin 'i686-w64-mingw32-strip.exe'
foreach($tool in @($cc,$cc64,$strip)){if(!(Test-Path -LiteralPath $tool)){throw "Missing compiler tool: $tool"}}
Push-Location $root
try {
    $common=@('-m32','-O2','-Wall','-Wextra','-std=c99','-Iinclude','-DWIN32','-D_WIN32_WINNT=0x0601','-DUNICODE','-D_UNICODE')
    $sources=@('proxy_bridge','game_vr','weapon_wheel','vr_holsters','vr_body_ik','vr_mechanics','opengl_forward')
    $objects=@()
    foreach($name in $sources) {
        $object=Join-Path $out ($name+'.o')
        & $cc @common -c "src/$name.c" -o $object
        if($LASTEXITCODE){throw "Compile failed: $name"};$objects+=$object
    }
    $assembly=Join-Path $out 'opengl_forward.S.o'
    & $cc @common -c src/opengl_forward.S -o $assembly
    if($LASTEXITCODE){throw 'OpenGL forwarding assembly compile failed'}
    & $cc -m32 -shared '-Wl,--kill-at' -lopengl32 -luser32 -lgdi32 -ld3d11 -ldxgi -ldxguid -lkernel32 -o "$out/opengl32.dll" @objects $assembly src/opengl_proxy.def
    if($LASTEXITCODE){throw 'Proxy link failed'}
    & $strip "$out/opengl32.dll"
    if($LASTEXITCODE){throw 'Proxy strip failed'}
    & $cc -O2 -Wall -Wextra -std=c99 -D_WIN32_WINNT=0x0601 -municode -mwindows src/launcher.c -o "$out/SoF2 Launcher.exe" -luser32 -lgdi32 '-Wl,--no-insert-timestamp'
    if($LASTEXITCODE){throw 'Launcher compile failed'}
    & $cc64 -O2 -g -Wall -Wextra -Wno-missing-field-initializers -DWIN32 -D_WIN32_WINNT=0x0602 -Iinclude -Isrc src/bridge64.c src/bridge_actions.c src/weapon_wheel.c -o "$out/sof2_vr_bridge64.exe" -Llib -lopenxr_loader -lopengl32 -luser32 -lgdi32 -ladvapi32 -ld3d11 -ldxgi -ldxguid '-Wl,--no-insert-timestamp'
    if($LASTEXITCODE){throw 'Helper compile failed'}
    Write-Host "Built runtime in $out. No game launched. Generated calibration headers are already included; do not regenerate the user's saved poses."
} finally {Pop-Location}
