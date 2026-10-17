#!/bin/bash
# Uso: pvstep.sh <plugin> <nombre> <args de pluginval...>
# Corre pluginval con limite de tiempo. Si se cuelga, toma una "foto" de lo que hace cada hilo (sample) y la
# muestra como aviso en GitHub para poder arreglarlo.
PLUG="$1"; NAME="$2"; shift 2
PV=pluginval.app/Contents/MacOS/pluginval
"$PV" validate "$PLUG" "$@" > "pv_$NAME.log" 2>&1 &
P=$!
for i in $(seq 1 120); do sleep 5; kill -0 $P 2>/dev/null || break; done
emit () { msg=$(cut -c1-300 | sed 's/%/%25/g' | awk '{printf "%s%%0A", $0}'); echo "::error title=$1::$msg"; }
if kill -0 $P 2>/dev/null; then
    for c in $(pgrep -f "pluginval"); do sample "$c" 3 -file "sample_$c.txt" >/dev/null 2>&1 || true; done
    pkill -9 -f pluginval || true
    tail -n 25 "pv_$NAME.log" | emit "pluginval $NAME: se colgo (final del registro)"
    for f in sample_*.txt; do
        [ -f "$f" ] || continue
        grep -E "gaux|Gaux|juce::|Thread|pthread|semaphore|wait" "$f" | grep -v "^ *$" | head -70 | emit "pluginval $NAME: donde estaba cada hilo ($f)"
    done
    exit 1
fi
wait $P; rc=$?
if [ $rc -ne 0 ]; then tail -n 40 "pv_$NAME.log" | emit "pluginval $NAME: fallo"; fi
tail -n 3 "pv_$NAME.log"
exit $rc
