#!/bin/bash
# ============================================================
#  Gaux Equalizer - Crear el instalador para Mac (.pkg)
#  Uso (en la Terminal):  bash package_mac.sh
#  Resultado: dist/GauxEQ_v<version>_Mac.zip  (listo para enviar)
#  Compila un binario universal (Apple Silicon + Intel).
# ============================================================
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(pwd)"

VERSION="$(sed -n 's/^project(GauxEqualizer VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)"
NAME="GauxEQ_v${VERSION}_Mac"
ARCHS="${GAUX_ARCHS:-arm64;x86_64}"
BUILD="build-package"
OUT="$BUILD/GauxEqualizer_artefacts/Release"
WORK="$BUILD/pkg"
DIST="dist/$NAME"

echo ""
echo "=== Gaux Equalizer $VERSION: creando el instalador para Mac ==="
echo ""
command -v cmake >/dev/null || { echo "ERROR: falta cmake (brew install cmake)"; exit 1; }

# 1) Compilar (universal)
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCHS" -DGAUX_COPY_AFTER_BUILD=FALSE
cmake --build "$BUILD" --config Release --target GauxEqualizer_VST3 GauxEqualizer_AU GauxEqualizer_Standalone -j "$(sysctl -n hw.ncpu)"

VST3="$OUT/VST3/Gaux Equalizer.vst3"
AU="$OUT/AU/Gaux Equalizer.component"
APP="$OUT/Standalone/Gaux Equalizer.app"
for p in "$VST3" "$AU" "$APP"; do [ -e "$p" ] || { echo "ERROR: no se genero $p"; exit 1; }; done

# 2) Firma local (ad-hoc): obligatoria para que el codigo arm64 cargue
for p in "$VST3" "$AU" "$APP"; do
    xattr -cr "$p" 2>/dev/null || true
    codesign --force --deep --sign - "$p"
    codesign --verify --deep "$p"
done

# 3) Un paquete por formato (cada uno instala en su carpeta del sistema)
chmod +x packaging/mac/scripts/postinstall
rm -rf "$WORK"; mkdir -p "$WORK"
make_component () {   # id  bundle  destino  nombre.pkg  [scripts]
    local id="$1" src="$2" dest="$3" pkg="$4" scripts="${5:-}"
    local root="$WORK/root-$id"
    mkdir -p "$root$dest"
    ditto "$src" "$root$dest/$(basename "$src")"
    # que el instalador no "mueva" la actualizacion a donde estaba una copia vieja
    pkgbuild --analyze --root "$root" "$WORK/$id.plist" >/dev/null
    local i=0
    while /usr/libexec/PlistBuddy -c "Print :$i" "$WORK/$id.plist" >/dev/null 2>&1; do
        # (en macOS nuevos la clave puede no venir: entonces se agrega)
        /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$WORK/$id.plist" 2>/dev/null \
            || /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$WORK/$id.plist"
        i=$((i + 1))
    done
    if [ -n "$scripts" ]; then
        pkgbuild --root "$root" --component-plist "$WORK/$id.plist" --identifier "$id" --version "$VERSION" \
                 --install-location "/" --scripts "$scripts" "$WORK/$pkg"
    else
        pkgbuild --root "$root" --component-plist "$WORK/$id.plist" --identifier "$id" --version "$VERSION" \
                 --install-location "/" "$WORK/$pkg"
    fi
}
make_component com.gaux.equalizer.vst3 "$VST3" "/Library/Audio/Plug-Ins/VST3" vst3.pkg packaging/mac/scripts
make_component com.gaux.equalizer.au   "$AU"   "/Library/Audio/Plug-Ins/Components" au.pkg packaging/mac/scripts
make_component com.gaux.equalizer.app  "$APP"  "/Applications" app.pkg

# Arquitecturas que el instalador acepta (segun lo compilado)
HOSTS="arm64"; [[ "$ARCHS" == *x86_64* ]] && HOSTS="arm64,x86_64"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@ARCHS@/$HOSTS/g" packaging/mac/distribution.xml > "$WORK/distribution.xml"

rm -rf "$DIST"; mkdir -p "$DIST"
productbuild --distribution "$WORK/distribution.xml" --resources packaging/mac --package-path "$WORK" "$DIST/Gaux Equalizer.pkg"

# 4) Textos, licencia y codigo fuente (la licencia AGPLv3 pide incluirlo)
cp "packaging/ANTES DE INSTALAR - Mac.txt" "$DIST/"
cp packaging/LICENCIA.txt "$DIST/"
curl -fsSL --max-time 20 https://www.gnu.org/licenses/agpl-3.0.txt -o "$DIST/AGPLv3.txt" 2>/dev/null || rm -f "$DIST/AGPLv3.txt"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git archive --format=zip --prefix=GauxEQ/ -o "$DIST/GauxEQ_codigo_fuente.zip" HEAD
else
    (cd .. && zip -qr "$ROOT/$DIST/GauxEQ_codigo_fuente.zip" "$(basename "$ROOT")" \
        -x "*/build*" "*/dist/*" "*/JUCE/*" "*/.git/*" "*.DS_Store")
fi

# 5) Zip final
(cd dist && rm -f "$NAME.zip" && ditto -c -k --sequesterRsrc --keepParent "$NAME" "$NAME.zip")

echo ""
echo "=== Listo ==="
echo "Para enviar:  $ROOT/dist/$NAME.zip"
echo ""
