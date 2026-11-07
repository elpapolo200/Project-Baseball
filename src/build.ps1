# Compila el port de MLB 15.
#   .\build.ps1            -> compila el anfitrión (host\*.c) y enlaza
#   .\build.ps1 -Partes    -> además genera datos\juego.dll (el código del juego) con el MISMO proceso que
#                            usa el launcher en el PC del jugador (recompilar.c: armrecomp + clang + lld)
#   .\build.ps1 -Run       -> enlaza y ejecuta (60 s máximo), mostrando el final de la salida
#   .\build.ps1 -Regenerar -> igual que -Partes (se mantiene por costumbre)
#   .\build.ps1 -Distribuir -> crea VitaRE\MLB15_Distribuible\ (lo que se puede pasar: sin nada del juego)
#   .\build.ps1 -Paquete   -> además crea VitaRE\MLB15_Port\ (ProjectBaseball.exe + SDL2.dll + LEEME.txt) para jugar
param([switch]$Partes, [switch]$Run, [switch]$Regenerar, [switch]$Paquete, [switch]$Distribuir, [switch]$Od, [int]$Segundos = 60, [int]$Hilos = 4)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$inc = Join-Path $root '..\..\vitarecomp\include'
$rtlib = Join-Path $root '..\..\vitarecomp\build\Release\vitarecomp.lib'

function Invoke-VS([string]$cmd) {
    $out = cmd /c "`"$vcvars`" >nul 2>&1 && $cmd 2>&1"
    $errors = $out | Select-String -Pattern ' error |fatal error'
    if ($errors) { $errors | Select-Object -First 15 | ForEach-Object { Write-Host $_ }; throw "falló: $cmd" }
    return $out
}

if ($Regenerar) { $Partes = $true }
# (Hasta el 2026-11-06 las partes se compilaban aquí con cl /O2 y se enlazaban dentro del exe. Ahora el
#  juego va en datos\juego.dll, generado por el propio exe: ver más abajo, tras el enlace.)

# Shaders GLSL -> SPIR-V (cabeceras .h) cuando cambian. glslang está en deps\.
$deps = Join-Path $root '..\..\deps'
$glslang = Join-Path $deps 'glslang\bin\glslang.exe'
Push-Location (Join-Path $root 'host\shaders')
foreach ($s in Get-ChildItem *.vert, *.frag) {
    $h = "$($s.BaseName)_$($s.Extension.TrimStart('.')).h"
    if (-not (Test-Path $h) -or (Get-Item $h).LastWriteTime -lt $s.LastWriteTime) {
        $vn = "spv_$($s.BaseName)_$($s.Extension.TrimStart('.'))"
        # Por cmd: glslang escribe el nombre del archivo en stderr y con 'Stop' eso aborta.
        cmd /c "`"$glslang`" -V --target-env vulkan1.1 --vn $vn -o $h $($s.Name) >nul 2>&1"
        if ($LASTEXITCODE) { throw "glslang falló con $($s.Name)" }
    }
}
Pop-Location

$vkinc = Join-Path $deps 'vulkan-headers\Vulkan-Headers-vulkan-sdk-1.4.363.0\include'
$sdl = Join-Path $deps 'sdl2\SDL2-2.32.10'
$sdlinc = Join-Path $sdl 'include'
$sdllib = Join-Path $sdl 'lib\x64'

# Compilar recurso .rc (icono + versión)
Push-Location (Join-Path $root 'host')
$rc = "$vcvars >nul 2>&1 && rc /nologo /I`"$inc`" /I`"$vkinc`" /I`"$sdlinc`" mlb15.rc"
Invoke-VS $rc
Pop-Location

# LibAtrac9 (MIT, Alex Barney; deps\LibAtrac9): decodificador AT9 para SceNgs. Se compila una vez.
$at9src = Join-Path $deps 'LibAtrac9\C\src'
$at9lib = Join-Path $deps 'LibAtrac9\at9.lib'
if (-not (Test-Path $at9lib)) {
    Push-Location $at9src
    Invoke-VS "cl /nologo /c /O2 /MD /W0 /DCOMPILING_DLL *.c" | Out-Null
    Invoke-VS "lib /nologo /OUT:`"$at9lib`" *.obj" | Out-Null
    Remove-Item *.obj
    Pop-Location
}

# Traductor de shaders de Vita3K (GPLv2; v3k\, glslang fc9889c en deps\glslang-v3k, fmt en deps\fmt).
# Se compila si falta v3k.lib o si cambió algún fuente de v3k\src / v3k\include.
$v3klib = Join-Path $root 'v3k\v3k.lib'
$v3knew = $false
if (Test-Path $v3klib) {
    $t = (Get-Item $v3klib).LastWriteTime
    $v3knew = [bool](Get-ChildItem (Join-Path $root 'v3k\src'), (Join-Path $root 'v3k\include') -Recurse -File | Where-Object { $_.LastWriteTime -gt $t } | Select-Object -First 1)
}
if (-not (Test-Path $v3klib) -or $v3knew) {
    $out = cmd /c "`"$(Join-Path $root 'v3k\build_v3k.bat')`" 2>&1"
    if ($LASTEXITCODE -ne 0) { $out | Select-String ' error ' | Select-Object -First 15 | ForEach-Object { Write-Host $_ }; throw 'falló v3k.lib' }
}

Push-Location (Join-Path $root 'host')
$glinc = Join-Path $deps 'glslang\include'
$gllib = Join-Path $deps 'glslang\lib'
Invoke-VS "cl /nologo /c /O2 /MD /W3 /DVITA_CPU_REMOTA /I`"$inc`" /I`"$vkinc`" /I`"$sdlinc`" /I`"$glinc`" /I`"$at9src`" /Ishaders *.c" | Out-Null
Pop-Location

# Runtime de vitarecomp con los registros en juego.dll (cpu.h, VITA_CPU_REMOTA): se compila aquí en vez de
# usar vitarecomp.lib (que sigue siendo la versión clásica para armrecomp y los tests).
$rtsrc = Join-Path $root '..\..\vitarecomp\src'
$rtobj = Join-Path $root 'host\rt'
New-Item -ItemType Directory -Force $rtobj | Out-Null
Push-Location $rtobj
Invoke-VS "cl /nologo /c /O2 /MD /W3 /D_CRT_SECURE_NO_WARNINGS /DVITA_CPU_REMOTA /I`"$inc`" `"$rtsrc\*.c`"" | Out-Null
Pop-Location

Push-Location $root
# Reintento: tras matar una ejecución, Windows tarda un momento en soltar el .exe (1,2 GB).
for ($n = 1; ; $n++) {
    try { Invoke-VS "link /nologo /OUT:mlb15_recomp.exe /MAP:mlb15_recomp.map /STACK:16777216 /DEF:dist\exports.def host\*.obj host\rt\*.obj host\mlb15.res hle_defaults.lib `"$at9lib`" `"$v3klib`" `"$sdllib\SDL2.lib`" `"$gllib\glslang.lib`" `"$gllib\glslang-default-resource-limits.lib`" `"$gllib\SPIRV.lib`" `"$gllib\MachineIndependent.lib`" `"$gllib\GenericCodeGen.lib`" `"$gllib\OSDependent.lib`" `"$gllib\SPIRV-Tools-opt.lib`" `"$gllib\SPIRV-Tools.lib`" shell32.lib ole32.lib user32.lib comdlg32.lib" | Out-Null; break }
    catch { if ($n -ge 4) { throw }; Start-Sleep -Seconds 3 }
}
try { Copy-Item (Join-Path $sdllib 'SDL2.dll') (Join-Path $root 'SDL2.dll') -Force } catch { Write-Host 'SDL2.dll en uso: se deja la que hay' }
Write-Host 'enlazado: mlb15_recomp.exe'

# herramientas\ : lo que el launcher necesita para generar juego.dll en el PC del jugador (todo libre o propio).
$her = Join-Path $root 'herramientas'
$llvm = Join-Path $deps 'llvm'
New-Item -ItemType Directory -Force $her, "$her\db", "$her\include\vitarecomp", "$her\include\shim", "$her\include\clang" | Out-Null
Copy-Item (Join-Path $root '..\..\vitarecomp\build\tools\armrecomp\Release\armrecomp.exe') $her -Force
foreach ($x in 'clang.exe', 'lld-link.exe', 'llvm-dlltool.exe') {
    $d = Join-Path $her $x
    if (-not (Test-Path $d) -or (Get-Item $d).Length -ne (Get-Item "$llvm\bin\$x").Length) { Copy-Item "$llvm\bin\$x" $d -Force }
}
Copy-Item (Join-Path $root '..\..\vita-headers\db\360\*.yml') "$her\db" -Force
Copy-Item (Join-Path $root '..\..\vitarecomp\include\vitarecomp\*.h') "$her\include\vitarecomp" -Force
Copy-Item (Join-Path $root 'dist\shim\*.h') "$her\include\shim" -Force
Copy-Item "$llvm\lib\clang\19\include\*" "$her\include\clang" -Recurse -Force
Copy-Item (Join-Path $root 'dist\juego_base.c'), (Join-Path $root 'dist\exports.def') $her -Force

# El código del juego: datos\juego.dll, generado por el exe igual que en el PC de un jugador.
if ($Partes) {
    $t = Get-Date
    $env:MLB15_RECOMPILAR = '1'
    & .\mlb15_recomp.exe ..\eboot.elf | Select-String 'recompilar\]' | Select-Object -Last 8 | ForEach-Object { Write-Host $_ }
    Remove-Item env:MLB15_RECOMPILAR
    if (-not (Test-Path 'datos\juego.dll')) { throw 'no se generó datos\juego.dll' }
    Write-Host ("juego.dll generado en {0:N0} s" -f ((Get-Date) - $t).TotalSeconds)
}

# Carpeta lista para jugar: VitaRE\MLB15_Port\ con ProjectBaseball.exe, SDL2.dll y LEEME.txt.
# MLB15.ini y datos\ se crean solos en el primer arranque (no se pisan).
if ($Paquete) {
    # No "MLB15": en Windows es la misma carpeta que VitaRE\mlb15 (el taller).
    $dst = Join-Path $root '..\..\MLB15_Port'
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item (Join-Path $root 'mlb15_recomp.exe') (Join-Path $dst 'ProjectBaseball.exe') -Force
    Copy-Item (Join-Path $sdllib 'SDL2.dll') (Join-Path $dst 'SDL2.dll') -Force
    Copy-Item (Join-Path $root 'paquete\LEEME.txt') (Join-Path $dst 'LEEME.txt') -Force
    # el CONTENIDO de herramientas (copiar la carpeta sobre una existente la metía dentro: herramientas\herramientas,
    # y el launcher seguía usando el armrecomp viejo)
    New-Item -ItemType Directory -Force (Join-Path $dst 'herramientas') | Out-Null
    Copy-Item "$her\*" (Join-Path $dst 'herramientas') -Recurse -Force
    Remove-Item (Join-Path $dst 'herramientas\herramientas') -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $deps 'realesrgan\realesrgan-ncnn-vulkan.exe'), (Join-Path $deps 'realesrgan\vcomp140.dll') (New-Item -ItemType Directory -Force (Join-Path $dst 'herramientas\realesrgan')) -Force
    New-Item -ItemType Directory -Force (Join-Path $dst 'herramientas\realesrgan\models') | Out-Null
    Copy-Item (Join-Path $deps 'realesrgan\models\realesrgan-x4plus.*') (Join-Path $dst 'herramientas\realesrgan\models') -Force
    Write-Host "paquete: $((Resolve-Path $dst).Path)"
}

# Carpeta para pasar a otros: VitaRE\MLB15_Distribuible\. NO lleva nada del juego: el exe es solo el port y
# juego.dll se genera en el PC de cada uno (recompilar.c) a partir de su copia. Lleva herramientas\ (armrecomp,
# clang, lld, Real-ESRGAN), las DLL de Visual C++, las licencias y el código fuente (GPL).
if ($Distribuir) {
    $dd = Join-Path $root '..\..\MLB15_Distribuible'
    if (Test-Path $dd) { Remove-Item $dd -Recurse -Force }
    New-Item -ItemType Directory -Force $dd, "$dd\LICENCIAS", "$dd\herramientas\realesrgan\models" | Out-Null
    Copy-Item (Join-Path $root 'mlb15_recomp.exe') "$dd\ProjectBaseball.exe"
    Copy-Item (Join-Path $sdllib 'SDL2.dll') $dd
    $crt = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT' -Directory | Select-Object -Last 1
    foreach ($x in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item (Join-Path $crt.FullName $x) $dd; Copy-Item (Join-Path $crt.FullName $x) "$dd\herramientas" }
    Copy-Item "$her\*" "$dd\herramientas" -Recurse -Force
    $esr = Join-Path $deps 'realesrgan'
    Copy-Item "$esr\realesrgan-ncnn-vulkan.exe", "$esr\vcomp140.dll" "$dd\herramientas\realesrgan"
    Copy-Item "$esr\models\realesrgan-x4plus.*" "$dd\herramientas\realesrgan\models"
    Copy-Item (Join-Path $root 'dist\LEEME_AMIGOS.txt') "$dd\LEEME.txt"
    Copy-Item (Join-Path $root 'dist\LICENCIAS_TERCEROS.txt') "$dd\LICENCIAS\LEEME.txt"
    Copy-Item (Join-Path $root 'dist\GPL-2.0.txt') "$dd\LICENCIAS\GPL-2.0.txt"
    Copy-Item (Join-Path $root '..\..\vitarecomp\LICENSE') "$dd\LICENCIAS\vitarecomp.txt"
    Copy-Item (Join-Path $root '..\..\vita-headers\LICENSE.md') "$dd\LICENCIAS\vita-headers.txt"
    Copy-Item (Join-Path $sdl 'LICENSE.txt') "$dd\LICENCIAS\SDL2.txt"
    Copy-Item (Join-Path $deps 'LibAtrac9\LICENSE') "$dd\LICENCIAS\LibAtrac9.txt"
    Copy-Item (Join-Path $deps 'glslang-v3k\LICENSE.txt') "$dd\LICENCIAS\glslang.txt"
    Copy-Item (Join-Path $deps 'fmt\LICENSE') "$dd\LICENCIAS\fmt.txt"
    # código fuente (GPL): la copia del repositorio aparte, sin .git
    $repo = Join-Path $root '..\..\..\VitaRE_repo'   # copia del código (tools\copiar_repo.ps1)
    if (Test-Path $repo) {
        $tmpz = Join-Path $env:TEMP 'mlb15_fuente'
        if (Test-Path $tmpz) { Remove-Item $tmpz -Recurse -Force }
        robocopy $repo $tmpz /E /XD .git /NFL /NDL /NJH /NJS | Out-Null
        Compress-Archive -Path "$tmpz\*" -DestinationPath "$dd\codigo_fuente.zip" -Force
        Remove-Item $tmpz -Recurse -Force
    }
    $mb = [math]::Round(((Get-ChildItem $dd -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 0)
    Write-Host "distribuible: $((Resolve-Path $dd).Path) ($mb MB)"
}

if ($Run) {
    $p = Start-Process .\mlb15_recomp.exe -ArgumentList '..\eboot.elf' -NoNewWindow -PassThru `
        -RedirectStandardOutput run_out.txt -RedirectStandardError run_err.txt
    if (-not $p.WaitForExit($Segundos * 1000)) { $p.Kill(); Write-Host "(detenido tras $Segundos s)" }
    Get-Content run_out.txt -Tail 25
    Get-Content run_err.txt -Tail 10
}
Pop-Location
