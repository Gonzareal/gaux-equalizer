#!/bin/bash
# ============================================================
#  Gaux Equalizer - Desinstalar (Mac)
#  Doble clic (si el Mac lo bloquea: clic derecho > Abrir > Abrir).
#  Borra el VST3, el Audio Unit y la app. Tus presets NO se borran
#  (quedan en Musica > Gaux EQ > Presets).
#  Uso sin preguntas (para pruebas):  bash "Desinstalar Gaux Equalizer.command" --si
# ============================================================
ITEMS=(
  "/Library/Audio/Plug-Ins/VST3/Gaux Equalizer.vst3"
  "/Library/Audio/Plug-Ins/Components/Gaux Equalizer.component"
  "/Applications/Gaux Equalizer.app"
  "/Library/Application Support/Gaux Equalizer"
)
# (copias viejas instaladas a mano en la carpeta del usuario)
for d in "$HOME/Library/Audio/Plug-Ins/VST3/Gaux Equalizer.vst3" "$HOME/Library/Audio/Plug-Ins/Components/Gaux Equalizer.component"; do
  ITEMS+=("$d")
done

echo ""
echo "=== Desinstalar Gaux Equalizer ==="
echo ""
found=0
for p in "${ITEMS[@]}"; do [ -e "$p" ] && { echo "  - $p"; found=1; }; done
if [ $found -eq 0 ] && ! pkgutil --pkgs 2>/dev/null | grep -q '^com\.gaux\.equalizer'; then
  echo "Gaux Equalizer no esta instalado en este Mac."
  [ "$1" = "--si" ] || read -r -p "Pulsa Enter para cerrar..." _
  exit 0
fi
echo ""
if [ "$1" != "--si" ]; then
  echo "Cierra tu DAW antes de seguir. Tus presets no se borran."
  read -r -p "Escribe S y pulsa Enter para desinstalar: " r
  case "$r" in s|S|si|SI|Si) ;; *) echo "Cancelado."; exit 0;; esac
  echo "El Mac te pedira tu contrasena (no se ve mientras la escribes)."
fi

SUDO=sudo; [ "$(id -u)" = "0" ] && SUDO=
for p in "${ITEMS[@]}"; do
  [ -e "$p" ] || continue
  case "$p" in "$HOME"*) rm -rf "$p" ;; *) $SUDO rm -rf "$p" ;; esac
done
for id in com.gaux.equalizer.vst3 com.gaux.equalizer.au com.gaux.equalizer.app; do
  pkgutil --pkg-info "$id" >/dev/null 2>&1 && $SUDO pkgutil --forget "$id" >/dev/null
done
killall -9 AudioComponentRegistrar >/dev/null 2>&1

left=0
for p in "${ITEMS[@]}"; do [ -e "$p" ] && { echo "No se pudo borrar: $p"; left=1; }; done
echo ""
if [ $left -eq 0 ]; then
  echo "Listo: Gaux Equalizer quedo desinstalado. Tus presets siguen en Musica > Gaux EQ > Presets."
else
  echo "Algo quedo sin borrar (arriba). Cierra el DAW y vuelve a intentarlo."
fi
[ "$1" = "--si" ] || read -r -p "Pulsa Enter para cerrar..." _
exit $left
