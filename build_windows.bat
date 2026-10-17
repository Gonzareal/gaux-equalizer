@echo off
setlocal EnableExtensions
REM ============================================================
REM  Gaux Equalizer - Compilar e instalar en Windows (64 bits)
REM  Uso: doble clic en este archivo (o desde la consola: build_windows.bat)
REM  Necesita: Visual Studio 2022 (o mas nuevo) con "Desarrollo para el escritorio con C++",
REM            y Git (https://git-scm.com) para descargar JUCE la primera vez.
REM ============================================================
cd /d "%~dp0"

echo.
echo === Gaux Equalizer: compilando para Windows ===
echo (la primera vez descarga JUCE y tarda unos 5-15 minutos)
echo.

REM --- buscar CMake: primero en el PATH, luego el que trae Visual Studio ---
set "CMAKE=cmake"
where cmake >nul 2>nul
if not errorlevel 1 goto :cmake_ok
set "CMAKE="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :cmake_check
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if exist "%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
:cmake_check
if not "%CMAKE%"=="" goto :cmake_ok
echo ERROR: no encuentro CMake ni Visual Studio con C++.
echo Instala "Visual Studio 2022 Community" con la carga "Desarrollo para el escritorio con C++":
echo   https://visualstudio.microsoft.com/es/downloads/
pause
exit /b 1
:cmake_ok

if exist "JUCE\CMakeLists.txt" goto :git_ok
where git >nul 2>nul
if not errorlevel 1 goto :git_ok
echo ERROR: no encuentro Git; se necesita para descargar JUCE la primera vez.
echo Instalalo desde https://git-scm.com/download/win y vuelve a ejecutar este archivo.
pause
exit /b 1
:git_ok

"%CMAKE%" -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :fallo
"%CMAKE%" --build build --config Release --parallel
if errorlevel 1 goto :fallo

set "OUT=build\GauxEqualizer_artefacts\Release"
set "VST3DIR=%CommonProgramFiles%\VST3"

REM --- instalar el VST3 en la carpeta estandar (pide permiso de administrador si hace falta) ---
net session >nul 2>&1
if not errorlevel 1 goto :admin_copy
echo.
echo Instalando el VST3 en "%VST3DIR%"; Windows pedira permiso de administrador...
powershell -NoProfile -Command "Start-Process -Verb RunAs -Wait robocopy -ArgumentList '\"%CD%\%OUT%\VST3\Gaux Equalizer.vst3\"','\"%VST3DIR%\Gaux Equalizer.vst3\"','/E','/NFL','/NDL','/NJH','/NJS'"
goto :copied
:admin_copy
robocopy "%OUT%\VST3\Gaux Equalizer.vst3" "%VST3DIR%\Gaux Equalizer.vst3" /E /NFL /NDL /NJH /NJS >nul
:copied

echo.
if not exist "%VST3DIR%\Gaux Equalizer.vst3\Contents" goto :not_copied
echo === Listo ===
echo VST3 instalado en: %VST3DIR%\Gaux Equalizer.vst3
goto :final
:not_copied
echo === Compilado, pero no se pudo copiar el VST3 ===
echo Copia a mano esta carpeta: %CD%\%OUT%\VST3\Gaux Equalizer.vst3
echo dentro de: %VST3DIR%
:final
echo App independiente para probarlo sin DAW:
echo   %CD%\%OUT%\Standalone\Gaux Equalizer.exe
echo.
echo Ahora abre tu DAW y vuelve a escanear los plug-ins
echo (Ableton: Opciones ^> Preferencias ^> Plug-ins ^> Volver a escanear).
pause
exit /b 0

:fallo
echo.
echo ERROR: la compilacion fallo. Revisa los mensajes de arriba.
pause
exit /b 1
