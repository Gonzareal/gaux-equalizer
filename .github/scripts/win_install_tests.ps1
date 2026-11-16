# Gaux Equalizer - pruebas de instalacion, reinstalacion y desinstalacion en un Windows limpio de GitHub
param([string]$Phase, [string]$Setup)
$ErrorActionPreference = 'Continue'
$script:fails = 0; $script:checks = 0
function Check([string]$name, [scriptblock]$cond) {
    $script:checks++
    $ok = $false; try { $ok = [bool](& $cond) } catch { $ok = $false }
    if ($ok) { Write-Output "  ok    $name" } else { $script:fails++; Write-Output "  FALLA $name"; Write-Output "::error title=Instalacion Windows::$name" }
}
$vst3 = "$env:CommonProgramFiles\VST3\Gaux Equalizer.vst3"
$dll = "$vst3\Contents\x86_64-win\Gaux Equalizer.vst3"
$app = "${env:ProgramFiles}\Gaux\Gaux Equalizer"
$key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{A9FBBEB2-C13C-4A04-9CBF-76CA6DA1A452}_is1'
$menu = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs"
$presets = Join-Path ([Environment]::GetFolderPath('MyMusic')) 'Gaux EQ\Presets'
$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(GauxEqualizer VERSION ([0-9.]+)\)').Matches[0].Groups[1].Value

function Install-Silent([string]$args2) {
    $p = Start-Process -Wait -PassThru -FilePath $Setup -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- ' + $args2)
    return $p.ExitCode
}
function Uninstall-Silent {
    $u = Get-ChildItem "$app\unins*.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $u) { return 99 }
    $p = Start-Process -Wait -PassThru -FilePath $u.FullName -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART'
    # (el desinstalador de Inno se copia a TEMP y sigue solo: esperar a que termine de verdad)
    for ($i = 0; $i -lt 60 -and ((Test-Path $vst3) -or (Test-Path $key)); $i++) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 2
    return $p.ExitCode
}

if ($Phase -eq 'instalar') {
    $zip = Get-ChildItem dist\*.zip | Select-Object -First 1
    Check "Zip: existe y pesa menos de 150 MB ($([int]($zip.Length/1MB)) MB)" { $zip -and $zip.Length -gt 1MB -and $zip.Length -lt 150MB }
    Remove-Item -Recurse -Force zipx -ErrorAction SilentlyContinue
    Expand-Archive $zip.FullName -DestinationPath zipx
    Check 'Zip: se abre y trae el instalador' { (Get-ChildItem -Recurse zipx -Filter *_Windows_Setup.exe).Count -eq 1 }
    Check 'Zip: trae ANTES DE INSTALAR - Windows.txt' { (Get-ChildItem -Recurse zipx -Filter 'ANTES DE INSTALAR - Windows.txt').Count -eq 1 }
    Check 'Zip: trae la guia en PDF valida' { $g = Get-ChildItem -Recurse zipx -Filter 'Guia de Gaux Equalizer.pdf' | Select-Object -First 1; $g -and $g.Length -gt 20KB -and ([System.Text.Encoding]::ASCII.GetString((Get-Content $g.FullName -Encoding Byte -TotalCount 5)) -eq '%PDF-') }
    Check 'Zip: trae LICENCIA.txt y el codigo fuente' { (Get-ChildItem -Recurse zipx -Filter LICENCIA.txt).Count -ge 1 -and (Get-ChildItem -Recurse zipx -Filter GauxEQ_codigo_fuente.zip).Count -eq 1 }
    Check 'Zip: trae la version portable (VST3 sin instalador)' { Test-Path (Get-ChildItem -Recurse -Directory zipx -Filter 'Portable (sin instalador)' | Select-Object -First 1).FullName }

    $rc = Install-Silent ''
    Check 'Instalar: termina sin error' { $rc -eq 0 }
    Check 'Instalar: el VST3 queda en la carpeta comun de VST3' { Test-Path $dll }
    Check 'Instalar: el VST3 es de 64 bits' { $b = [IO.File]::ReadAllBytes($dll); $pe = [BitConverter]::ToInt32($b, 0x3C); [BitConverter]::ToUInt16($b, $pe + 4) -eq 0x8664 }
    Check 'Instalar: queda la app independiente' { Test-Path "$app\Gaux Equalizer.exe" }
    Check 'Instalar: queda la guia en PDF' { Test-Path "$app\Guia de Gaux Equalizer.pdf" }
    Check 'Instalar: accesos en el menu Inicio (app y guia)' { (Test-Path "$menu\Gaux Equalizer.lnk") -and (Test-Path "$menu\Gaux Equalizer - Guia de usuario.lnk") }
    Check "Instalar: aparece en Configuracion > Aplicaciones con la version $version" { (Get-ItemProperty $key).DisplayVersion -eq $version }
    Check 'Instalar: el editor es Aleksander Gaux' { (Get-ItemProperty $key).Publisher -eq 'Aleksander Gaux' }

    # el usuario guarda un preset propio; despues actualiza (instala encima)
    New-Item -ItemType Directory -Force -Path $presets | Out-Null
    Set-Content -Path (Join-Path $presets 'Mi preset de prueba.gauxpreset') -Value '<GauxPreset name="Mi preset"/>'
    $rc = Install-Silent ''
    Check 'Actualizar (instalar encima): termina sin error' { $rc -eq 0 }
    Check 'Actualizar: el VST3 sigue en su lugar' { Test-Path $dll }
    Check 'Actualizar: una sola entrada en Aplicaciones' { @(Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' | Where-Object { (Get-ItemProperty $_.PSPath).DisplayName -eq 'Gaux Equalizer' }).Count -eq 1 }
    Check 'Actualizar: los presets del usuario se conservan' { Test-Path (Join-Path $presets 'Mi preset de prueba.gauxpreset') }

    # instalacion "solo el plugin"
    Uninstall-Silent | Out-Null
    $rc = Install-Silent '/TYPE=plugin'
    Check 'Instalar "solo el plugin VST3": termina sin error' { $rc -eq 0 }
    Check 'Instalar "solo el plugin VST3": hay VST3 y no app' { (Test-Path $dll) -and -not (Test-Path "$app\Gaux Equalizer.exe") }
    Uninstall-Silent | Out-Null
    $rc = Install-Silent ''
    Check 'Instalacion completa otra vez (para pluginval)' { $rc -eq 0 -and (Test-Path $dll) }
}
elseif ($Phase -eq 'desinstalar') {
    $rc = Uninstall-Silent
    Check 'Desinstalar: termina sin error' { $rc -eq 0 }
    Check 'Desinstalar: no queda el VST3' { -not (Test-Path $vst3) }
    Check 'Desinstalar: no queda la carpeta del programa' { -not (Test-Path "$app\Gaux Equalizer.exe") -and -not (Test-Path "$app\unins000.exe") }
    Check 'Desinstalar: no quedan accesos en el menu Inicio' { -not (Test-Path "$menu\Gaux Equalizer.lnk") -and -not (Test-Path "$menu\Gaux Equalizer - Guia de usuario.lnk") }
    Check 'Desinstalar: desaparece de Configuracion > Aplicaciones' { -not (Test-Path $key) }
    Check 'Desinstalar: los presets del usuario NO se borran' { Test-Path (Join-Path $presets 'Mi preset de prueba.gauxpreset') }
    $rc = Install-Silent ''
    Check 'Reinstalar despues de desinstalar: funciona' { $rc -eq 0 -and (Test-Path $dll) }
    $rc = Uninstall-Silent
    Check 'Desinstalar otra vez: limpio' { -not (Test-Path $vst3) -and -not (Test-Path $key) }
}
Write-Output "RESUMEN INSTALACION Windows ($Phase): $script:checks comprobaciones, $script:fails fallas"
if ($script:fails -gt 0) { exit 1 } else { exit 0 }
