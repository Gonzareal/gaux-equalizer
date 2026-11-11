import re, subprocess, sys
W,H=1160.0,760.0
param=[(38,32,"Encendido: bypass de todo el plugin"),(203,32,"Deshacer / rehacer"),(325,32,"PARAMETRIC / VINTAGE: cambiar de cara"),
(456,32,"A/B y flecha para copiar el ajuste"),(614,32,"Presets: anterior, menú, siguiente"),(762,32,"MATCH: igualar a una referencia"),
(822,32,"SC: sidechain general"),(883,32,"ZONE: limitar el EQ a una zona"),(944,32,"La calavera (golpes y saturación real)"),
(1029,32,"Calidad 1x / 2x / 4x"),(1116,32,"Tamaño de la interfaz"),(51,89,"SKETCH: dibujar la curva"),(139,89,"CHOQUE: comparar con otra pista"),
(580,85,"TP: true peak de la salida"),(190,226,"Punto de una banda (arrastrar, rueda = Q)"),(829,336,"Dinámica en vivo (relleno ámbar)"),
(306,398,"Armónicos de la saturación (2H, 3H…)"),(134,522,"Encender / bypass de la banda"),(461,522,"Tipo de filtro"),
(867,522,"Canal: ST, L, R, M, S"),(1000,522,"Solo y borrar banda"),(208,618,"FILTER: FREQ, GAIN, Q"),(541,618,"HARMONICS: modo, DRIVE, MIX"),
(870,618,"DYNAMIC: THRESH, RANGE, ATTACK, RELEASE"),(580,691,"Teclado de piano (clic = llevar la banda a la nota)"),
(280,731,"Bandas 1–24"),(668,731,"ANALYZER y AUTO GAIN"),(785,731,"Fase: MIN / NAT / LIN / LIN HQ"),(900,731,"SCALE: cuánto EQ en total"),
(1052,731,"OUT y medidores de entrada/salida")]
vint=[(80,174,"IN: encender / apagar cada unidad"),(125,177,"Piloto de la unidad"),(225,133,"LOW CUT (pasa altos)"),(740,133,"HIGH CUT (pasa bajos)"),
(875,130,"OUTPUT"),(980,116,"POWER: bypass del Vintage"),(1038,191,"AVANZADO: curva y analizador"),(319,250,"LOW PEAK: realce de medios-graves"),
(635,250,"DIP: recorte de medios"),(951,250,"HIGH PEAK: realce de medios-agudos"),(296,438,"LOW FREQUENCY: PUSH, CUT y frecuencia"),
(566,438,"HIGH FREQUENCY: PUSH, WIDTH y frecuencia"),(769,438,"SOFTEN: atenuar agudos"),(903,438,"AIR: brillo abierto"),(1038,438,"TUBE: GLOW e IRON")]
def mk(lst): return "".join(f'<div class="mk" style="left:{x/W*100:.2f}%;top:{y/H*100:.2f}%">{i+1}</div>' for i,(x,y,_) in enumerate(lst))
def lg(lst): return "".join(f'<div><span class="n">{i+1}</span><span>{t}</span></div>' for i,(_,_,t) in enumerate(lst))
s=open('/tmp/claude-0/guia/guia.html').read()
s=s.replace('<!--MARKERS:param-->',mk(param)).replace('<!--LEGEND:param-->',lg(param)).replace('<!--MARKERS:vintage-->',mk(vint)).replace('<!--LEGEND:vintage-->',lg(vint))
open('/tmp/claude-0/guia/guia_final.html','w').write(s)
