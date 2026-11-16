#!/bin/bash
# Gaux Equalizer - pruebas de descarga, instalacion, actualizacion y desinstalacion en un Mac limpio de GitHub.
# Fases:  descarga   -> baja el zip publicado (el link que reciben los usuarios), lo revisa y lo instala (version anterior)
#         nuevo <zip> -> revisa el zip recien creado
#         actualizado <version> -> despues de instalar la version nueva encima de la anterior
#         desinstalar -> corre el desinstalador y comprueba que no queden restos (y que los presets se conserven)
set -u
PHASE="$1"; ARG="${2:-}"
fails=0; checks=0
ok ()   { checks=$((checks+1)); echo "  ok    $1"; }
bad ()  { checks=$((checks+1)); fails=$((fails+1)); echo "  FALLA $1"; echo "::error title=Instalacion Mac::$1"; }
check () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

VST3="/Library/Audio/Plug-Ins/VST3/Gaux Equalizer.vst3"
AU="/Library/Audio/Plug-Ins/Components/Gaux Equalizer.component"
APP="/Applications/Gaux Equalizer.app"
SUP="/Library/Application Support/Gaux Equalizer"
PRESETS="$HOME/Music/Gaux EQ/Presets"
URL="https://raw.githubusercontent.com/Gonzareal/gaux-equalizer/instalador-mac/GauxEQ_Mac.zip"

zip_checks () {   # carpeta descomprimida
  local d="$1" what="$2"
  local pkg; pkg="$(find "$d" -name 'Gaux Equalizer.pkg' -maxdepth 3 | head -1)"
  check "$what: trae Gaux Equalizer.pkg" '[ -n "$pkg" ] && [ -f "$pkg" ]'
  check "$what: trae ANTES DE INSTALAR - Mac.txt" '[ -n "$(find "$d" -name "ANTES DE INSTALAR - Mac.txt" | head -1)" ]'
  check "$what: trae la guia en PDF" '[ -n "$(find "$d" -name "Guia de Gaux Equalizer.pdf" -size +20k | head -1)" ]'
  check "$what: la guia es un PDF valido" 'head -c 5 "$(find "$d" -name "Guia de Gaux Equalizer.pdf" | head -1)" | grep -q "%PDF-"'
  check "$what: trae LICENCIA.txt y el codigo fuente" '[ -n "$(find "$d" -name LICENCIA.txt | head -1)" ] && [ -n "$(find "$d" -name GauxEQ_codigo_fuente.zip -size +100k | head -1)" ]'
  check "$what: el codigo fuente se puede abrir" 'unzip -tq "$(find "$d" -name GauxEQ_codigo_fuente.zip | head -1)" >/dev/null'
  if [ -n "$pkg" ]; then
    rm -rf /tmp/pkgx; pkgutil --expand "$pkg" /tmp/pkgx >/dev/null 2>&1
    check "$what: el .pkg se abre (no esta danado)" '[ -f /tmp/pkgx/Distribution ]'
    check "$what: el .pkg trae VST3, AU y app" '[ -d /tmp/pkgx/vst3.pkg ] && [ -d /tmp/pkgx/au.pkg ] && [ -d /tmp/pkgx/app.pkg ]'
    check "$what: pide macOS 11 o mas nuevo" 'grep -q "os-version min=\"11.0\"" /tmp/pkgx/Distribution'
    check "$what: acepta Apple Silicon e Intel" 'grep -q "hostArchitectures=\"arm64,x86_64\"" /tmp/pkgx/Distribution'
    check "$what: no se reubica a copias viejas (BundleIsRelocatable)" '! grep -q "<key>BundleIsRelocatable</key><true/>" /tmp/pkgx/*/PackageInfo 2>/dev/null'
    echo "  (firma del .pkg: $(pkgutil --check-signature "$pkg" 2>&1 | head -1) - sin firma paga de Apple, como dice ANTES DE INSTALAR)"
  fi
}

case "$PHASE" in
descarga)
  rm -rf /tmp/descarga && mkdir -p /tmp/descarga && cd /tmp/descarga
  if ! curl -fsSL --retry 3 -o GauxEQ_Mac.zip "$URL"; then
    echo "  (todavia no hay instalador publicado: se salta la prueba de actualizacion)"; exit 0
  fi
  check "Descarga: el link publico responde y baja el zip" '[ -s GauxEQ_Mac.zip ]'
  size=$(stat -f%z GauxEQ_Mac.zip)
  check "Descarga: tamano razonable ($((size/1024/1024)) MB, menos de 150 MB)" '[ $size -gt 3000000 ] && [ $size -lt 157286400 ]'
  check "Descarga: el zip no esta danado" 'unzip -tq GauxEQ_Mac.zip >/dev/null'
  # como un usuario: el navegador le pone la marca de cuarentena
  xattr -w com.apple.quarantine "0081;$(printf %x $(date +%s));Safari;" GauxEQ_Mac.zip 2>/dev/null
  ditto -x -k GauxEQ_Mac.zip x
  zip_checks x "Zip publicado"
  pkg="$(find x -name 'Gaux Equalizer.pkg' | head -1)"
  sudo installer -pkg "$pkg" -target / >/tmp/inst_prev.log 2>&1; rc=$?
  check "Version publicada: se instala sin errores" '[ $rc -eq 0 ]'
  check "Version publicada: quedan VST3, AU y app" '[ -d "$VST3" ] && [ -d "$AU" ] && [ -d "$APP" ]'
  # el usuario ya tiene un preset propio antes de actualizar
  mkdir -p "$PRESETS" && echo '<GauxPreset name="Mi preset"/>' > "$PRESETS/Mi preset de prueba.gauxpreset"
  defaults read "$VST3/Contents/Info.plist" CFBundleShortVersionString > /tmp/prev_version.txt 2>/dev/null
  echo "  (version publicada instalada: $(cat /tmp/prev_version.txt))"
  ;;
nuevo)
  rm -rf /tmp/nuevo && mkdir -p /tmp/nuevo && ditto -x -k "$ARG" /tmp/nuevo
  zip_checks /tmp/nuevo "Zip nuevo"
  check "Zip nuevo: trae el desinstalador ejecutable" '[ -x "$(find /tmp/nuevo -name "Desinstalar Gaux Equalizer.command" | head -1)" ]'
  ;;
actualizado)
  check "Actualizacion: VST3 en su lugar" '[ -f "$VST3/Contents/MacOS/Gaux Equalizer" ]'
  check "Actualizacion: AU en su lugar" '[ -f "$AU/Contents/MacOS/Gaux Equalizer" ]'
  check "Actualizacion: app en su lugar" '[ -d "$APP" ]'
  for b in "$VST3" "$AU" "$APP"; do
    v=$(defaults read "$b/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null)
    check "Actualizacion: $(basename "$b") quedo en la version nueva ($v)" '[ "$v" = "$ARG" ]'
    check "Actualizacion: $(basename "$b") es universal (Apple Silicon + Intel)" 'lipo -archs "$(find "$b/Contents/MacOS" -type f | head -1)" | grep -q arm64 && lipo -archs "$(find "$b/Contents/MacOS" -type f | head -1)" | grep -q x86_64'
    check "Actualizacion: $(basename "$b") tiene firma local valida" 'codesign --verify --deep "$b" 2>/dev/null'
    check "Actualizacion: $(basename "$b") sin marca de cuarentena" '! xattr -r "$b" 2>/dev/null | grep -q quarantine'
  done
  check "Actualizacion: hay una sola copia del VST3 en el sistema" '[ ! -d "$HOME/Library/Audio/Plug-Ins/VST3/Gaux Equalizer.vst3" ]'
  check "Actualizacion: el desinstalador quedo en Application Support" '[ -x "$SUP/Desinstalar Gaux Equalizer.command" ]'
  check "Actualizacion: el sistema registra los 3 paquetes" '[ $(pkgutil --pkgs | grep -c "^com\.gaux\.equalizer") -eq 3 ]'
  if [ -f "$PRESETS/Mi preset de prueba.gauxpreset" ] || [ -s /tmp/prev_version.txt ]; then
    check "Actualizacion: los presets del usuario se conservan" '[ -f "$PRESETS/Mi preset de prueba.gauxpreset" ]'
  fi
  check "Actualizacion: Logic/GarageBand ven el AU (auval lo lista)" 'auval -a 2>/dev/null | grep -q "Gxeq"'
  ;;
desinstalar)
  bash "$SUP/Desinstalar Gaux Equalizer.command" --si > /tmp/uninst.log 2>&1
  code=$?
  cat /tmp/uninst.log | sed 's/^/    /'
  check "Desinstalar: el desinstalador termina bien" '[ $code -eq 0 ]'
  check "Desinstalar: no queda el VST3" '[ ! -e "$VST3" ]'
  check "Desinstalar: no queda el AU" '[ ! -e "$AU" ]'
  check "Desinstalar: no queda la app" '[ ! -e "$APP" ]'
  check "Desinstalar: no queda Application Support" '[ ! -e "$SUP" ]'
  check "Desinstalar: el sistema olvida los paquetes (sin recibos)" '! pkgutil --pkgs | grep -q "^com\.gaux\.equalizer"'
  killall -9 AudioComponentRegistrar 2>/dev/null; sleep 2
  check "Desinstalar: Logic/GarageBand ya no ven el AU" '! auval -a 2>/dev/null | grep -q "Gxeq"'
  if [ -f "$PRESETS/Mi preset de prueba.gauxpreset" ]; then
    check "Desinstalar: los presets del usuario NO se borran" '[ -f "$PRESETS/Mi preset de prueba.gauxpreset" ]'
  fi
  bash "packaging/mac/Desinstalar Gaux Equalizer.command" --si > /tmp/uninst2.log 2>&1; rc=$?
  check "Desinstalar dos veces: avisa que no esta instalado y no falla" '[ $rc -eq 0 ] && grep -q "no esta instalado" /tmp/uninst2.log'
  # reinstalar despues de desinstalar
  sudo installer -pkg "$ARG" -target / >/dev/null 2>&1
  check "Reinstalar despues de desinstalar: funciona" '[ -d "$VST3" ] && [ -d "$AU" ] && [ -d "$APP" ]'
  ;;
esac
echo "RESUMEN INSTALACION ($PHASE): $checks comprobaciones, $fails fallas"
[ $fails -eq 0 ]
