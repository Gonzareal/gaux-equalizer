#!/bin/bash
# Uso: pvstep.sh <plugin> <nombre> <args de pluginval...>
# Corre pluginval con limite de tiempo. Si se cuelga, toma una "foto" de lo que hace cada hilo (sample) y la
# muestra como aviso en GitHub para poder arreglarlo.
PLUG="$1"; NAME="$2"; shift 2
PV=pluginval.app/Contents/MacOS/pluginval
"$PV" "$@" --validate "$PLUG" > "pv_$NAME.log" 2>&1 &   # (sintaxis de pluginval 1.x, la que publica Tracktion)
P=$!
for i in $(seq 1 120); do sleep 5; kill -0 $P 2>/dev/null || break; done
emit () { msg=$(cut -c1-300 | sed 's/%/%25/g' | awk '{printf "%s%%0A", $0}'); echo "::error title=$1::$msg"; }
if kill -0 $P 2>/dev/null; then
    for c in $(pgrep -f "pluginval"); do sample "$c" 3 -file "sample_$c.txt" >/dev/null 2>&1 || true; done
    pkill -9 -f pluginval || true
    (echo "registro: $(wc -c < pv_$NAME.log) bytes"; tail -n 25 "pv_$NAME.log") | emit "pluginval $NAME: se colgo (final del registro)"
    for f in sample_*.txt; do
        [ -f "$f" ] || continue
        # los hilos donde aparece el plugin: cada linea con "(in Gaux Equalizer)" y las 6 de antes (quien la llamo)
        grep -n "in Gaux Equalizer" "$f" | head -1 >/dev/null || continue
        python3 - "$f" > "$f.gaux" <<'PY'
import sys, re
L = open(sys.argv[1], errors='replace').read().splitlines()
keep = set()
for i, l in enumerate(L):
    if 'Gaux Equalizer' in l or 'gaux::' in l:
        for k in range(max(0, i - 4), i + 1): keep.add(k)
    if re.match(r'^ +\d+ Thread_', l): keep.add(i)
out = [re.sub(r'\s+\[0x[0-9a-f]+\]', '', re.sub(r'^[ +!:|]+', '', L[i])) for i in sorted(keep)]
print('\n'.join(out))
PY
        split -b 3500 "$f.gaux" "$f.part_"
        n=0; for part in "$f".part_*; do n=$((n+1)); [ $n -le 6 ] || break; emit "pluginval $NAME: hilos con el plugin ($n)" < "$part"; done
    done
    exit 1
fi
wait $P; rc=$?
grep -q "SUCCESS" "pv_$NAME.log" || rc=1
if [ $rc -ne 0 ]; then tail -n 40 "pv_$NAME.log" | emit "pluginval $NAME: fallo"; fi
tail -n 3 "pv_$NAME.log"
exit $rc
