# ============================================================
#  Gaux Equalizer - Crear el instalador para Windows
#  Uso: doble clic en package_windows.bat (o: powershell -ExecutionPolicy Bypass -File package_windows.ps1)
#  Necesita: Visual Studio 2022 (C++), Git e Inno Setup 6 (https://jrsoftware.org/isdl.php)
#  Resultado: dist\GauxEQ_v<version>_Windows.zip  (listo para enviar)
# ============================================================
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$root = (Get-Location).Path

$m = Select-String -Path CMakeLists.txt -Pattern 'project\(GauxEqualizer VERSION ([0-9.]+)\)'
$version = $m.Matches[0].Groups[1].Value
$name  = "GauxEQ_v${version}_Windows"
$build = Join-Path $root 'build-package'
$out   = Join-Path $build 'GauxEqualizer_artefacts\Release'
$dist  = Join-Path $root "dist\$name"
Write-Host "`n=== Gaux Equalizer ${version}: creando el instalador para Windows ===`n"

# --- herramientas ---
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $c = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $c) { $cmake = $c }
    }
}
if (-not $cmake) { throw 'No encuentro CMake / Visual Studio con C++ (Desarrollo para el escritorio con C++).' }
$iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
foreach ($p in @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
    if (-not $iscc -and (Test-Path $p)) { $iscc = $p }
}
if (-not $iscc) { throw 'No encuentro Inno Setup 6. Instalalo gratis desde https://jrsoftware.org/isdl.php' }

# --- 1) compilar ---
& $cmake -S . -B $build -A x64 -DCMAKE_BUILD_TYPE=Release -DGAUX_COPY_AFTER_BUILD=FALSE
if ($LASTEXITCODE) { throw 'Fallo la configuracion' }
& $cmake --build $build --config Release --parallel --target GauxEqualizer_VST3 GauxEqualizer_Standalone
if ($LASTEXITCODE) { throw 'Fallo la compilacion' }
$vst3 = Join-Path $out 'VST3\Gaux Equalizer.vst3'
$exe  = Join-Path $out 'Standalone\Gaux Equalizer.exe'
foreach ($p in @($vst3, $exe)) { if (-not (Test-Path -LiteralPath $p)) { throw "No se genero $p" } }

# --- 2) instalador ---
if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null
& $iscc /Q "/DAppVersion=$version" "/DBuildDir=$out" "/DOutDir=$dist" 'packaging\windows\GauxEqualizer.iss'
if ($LASTEXITCODE) { throw 'Fallo Inno Setup' }

# --- 3) portable, textos, licencia y codigo fuente ---
$portable = Join-Path $dist 'Portable (sin instalador)'
New-Item -ItemType Directory -Path $portable | Out-Null
Copy-Item -LiteralPath $vst3 -Destination $portable -Recurse
Copy-Item -LiteralPath 'packaging\ANTES DE INSTALAR - Windows.txt' -Destination $dist
Copy-Item -LiteralPath 'packaging\LICENCIA.txt' -Destination $dist
try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 20 -Uri 'https://www.gnu.org/licenses/agpl-3.0.txt' -OutFile (Join-Path $dist 'AGPLv3.txt') } catch { }
$src = Join-Path $dist 'GauxEQ_codigo_fuente.zip'
$git = Get-Command git -ErrorAction SilentlyContinue
if ($git -and (Test-Path .git)) {
    & git archive --format=zip --prefix=GauxEQ/ -o $src HEAD
} else {
    $items = @('Source','Resources','Tests','packaging','CMakeLists.txt','README.md','build.sh','build_windows.bat','package_mac.sh','package_windows.ps1','package_windows.bat') | Where-Object { Test-Path $_ }
    Compress-Archive -Path $items -DestinationPath $src
}

# --- 4) zip final ---
$zip = Join-Path $root "dist\$name.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
Compress-Archive -Path $dist -DestinationPath $zip
Write-Host "`n=== Listo ===`nPara enviar: $zip`n"
