#!/bin/bash
# ============================================================
#  Gaux Equalizer - Compilar e instalar en macOS
#  Uso (en la Terminal):  bash build.sh
#  Para Macs Intel y Apple Silicon a la vez:  bash build.sh universal
# ============================================================
set -e
cd "$(dirname "$0")"

echo ""
echo "=== Gaux Equalizer: compilando ==="
echo "(la primera vez descarga JUCE y tarda unos 5-10 minutos)"
echo ""

if ! command -v cmake >/dev/null 2>&1; then
    echo "ERROR: no encuentro cmake. Instalalo con:  brew install cmake"
    exit 1
fi

ARCHS="arm64"
if [ "$1" = "universal" ]; then
    ARCHS="arm64;x86_64"
    echo "(binario universal: Apple Silicon + Intel)"
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCHS"
cmake --build build --config Release -j "$(sysctl -n hw.ncpu)"

# Firma local (ad-hoc): macOS y los DAW cargan el plugin sin avisos de "danado"
for p in "$HOME/Library/Audio/Plug-Ins/VST3/Gaux Equalizer.vst3" \
         "$HOME/Library/Audio/Plug-Ins/Components/Gaux Equalizer.component" \
         "build/GauxEqualizer_artefacts/Release/Standalone/Gaux Equalizer.app"; do
    if [ -e "$p" ]; then
        xattr -cr "$p" 2>/dev/null || true
        codesign --force --deep -s - "$p" >/dev/null 2>&1 || true
    fi
done

# Refrescar la cache de Audio Units para que Ableton/Logic lo vean
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true

echo ""
echo "=== Listo ==="
echo "Plugin instalado en:"
echo "  VST3: ~/Library/Audio/Plug-Ins/VST3/Gaux Equalizer.vst3"
echo "  AU:   ~/Library/Audio/Plug-Ins/Components/Gaux Equalizer.component"
echo "App independiente para probarlo sin DAW:"
echo "  $(pwd)/build/GauxEqualizer_artefacts/Release/Standalone/Gaux Equalizer.app"
echo ""
echo "Ahora abre Ableton > Preferencias > Plug-ins y dale 'Volver a escanear'."
