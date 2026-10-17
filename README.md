# Gaux Equalizer v0.21

EQ para producción de música electrónica, desde la mezcla de cada instrumento hasta el mastering. Dos caras:

## Novedades v0.21 (el Paramétrico al nivel de los mejores EQ del mundo, con identidad propia)
- **24 bandas** (antes 16). Las sesiones viejas abren igual que antes.
- **Pendientes de 6 a 96 dB/oct y BRICK** (pared de 144 dB/oct) en los cortes. **Filtros nuevos**: Band Pass, Tilt (inclina todo el espectro sobre un pivote) y All Pass (gira la fase sin cambiar la curva: para alinear un kick con el bajo).
- **Dinámica ESPECTRAL** (botón SPECTRAL en el panel de dinámica): la banda actúa frecuencia por frecuencia, solo donde el sonido se pasa (una resonancia que aparece en una nota de la voz, el silbido de una "s"), sin tocar lo que hay al lado. Con AUTO el umbral sigue solo al material. Funciona en L/R/M/S y con sidechain.
- **EQ MATCH** (botón MATCH arriba): que tu pista suene con el balance de una referencia (un tema en WAV/AIFF/MP3/FLAC o lo que entra por el sidechain). Aprende tu pista mientras suena, dibuja en turquesa la curva que falta y crea las bandas (AMOUNT, número de bandas y DETALLE suave/medio/fino). Cmd+Z deshace.
- **SKETCH: dibuja la curva** (botón lima arriba a la izquierda de la pantalla, o Cmd/Ctrl + arrastrar). Mientras dibujas ves en vivo la curva que va a quedar y dónde nacerán las bandas (rombos); al soltar, la tinta se evapora y aparecen las bandas. Fuera de lo que dibujaste no cambia nada.
- **Caza de picos**: el analizador marca con un "colmillo" las resonancias que sobresalen. Pasa el mouse para ver frecuencia, nota y cuánto sobresalen; **clic** = domarla (corte sugerido con el ancho exacto del pico), **arrastrar** = recortarla a mano.
- **Fase NATURAL** (selector MIN/NAT/LIN/LIN HQ): suena como el EQ analógico ideal, con la misma curva y la misma fase hasta 20 kHz, y solo 1.3 ms de latencia (4.7 ms con HQ). A diferencia de la fase lineal, la dinámica, la saturación y el sidechain siguen funcionando.
- **CHOQUE** (botón junto a SKETCH): compara esta pista con otra que tenga un Gaux (kick vs bajo, voz vs sintes). Su espectro aparece en su color y las zonas donde se tapan quedan rayadas abajo; **clic en una zona** = abrir espacio en esta pista con una campana suave. Los nombres llegan del DAW o los pones tú (menú del botón). Si nadie está mirando, no gasta nada.
- **Varias bandas a la vez**: Shift + clic en los puntos, o arrastra un recuadro en la zona vacía; arrastrar mueve el grupo junto; Supr las borra.
- **Copiar / pegar la curva** (Cmd+C / Cmd+V en la pantalla o clic derecho): entre pistas, entre sesiones, entre proyectos.
- **Escribir valores**: clic en el número de una perilla y escribe. La frecuencia acepta notas: `A4` (440 Hz), `C#2`, `Eb1`, y también `2k5`, `1.2 kHz`.
- **Opciones de la pantalla** (clic derecho en la zona vacía): velocidad del analizador, inclinación (0 a 6 dB/oct), ver o no el espectro antes del EQ, **congelar** el analizador, rango (±6 a ±36 dB) y un **teclado de piano** abajo que ilumina las notas de los picos y de la banda elegida. Se guardan con la sesión.
- **Más liviano**: FFT nueva (8 a 13 veces más rápida que la anterior), las bandas apagadas no cuestan nada aunque sean 24; en todas las pruebas de CPU la v0.21 gasta igual o menos que la v0.20.

## Novedades v0.20
- **Pantalla a la velocidad del monitor**: 120 FPS en un MacBook Pro con ProMotion, 144 en monitores gamer, 60 en monitores normales (antes 30-40). Todo sincronizado con el refresco real de la pantalla.
- **Más fluido y más barato a la vez**: dibujar un cuadro del Paramétrico cuesta ~6 veces menos que antes (fondo y grilla en caché, cada curva se recalcula solo cuando cambia su banda, trazos reutilizados).
- **Cero CPU de pantalla en reposo**: sin audio y sin tocar nada, no se dibuja nada.
- **Protección del DAW**: si un PC es lento, la pantalla baja sola a 60 FPS (o menos solo si ni así alcanza) para no trabar la interfaz del DAW.
- Medidor de FPS: clic derecho en una zona vacía de la pantalla > *Mostrar FPS*.

## Novedades v0.19
- **Optimizado para usarlo en todos los canales**: con la ventana cerrada el plugin no gasta CPU en medidores ni analizador; las bandas apagadas no cuestan nada; en una pista en silencio el plugin "duerme" (~0.006 % de un núcleo) y despierta en la primera muestra sin perder nada. Ver la tabla de CPU abajo.
- **Fase lineal** (Paramétrico): selector **MIN / LIN / LIN HQ** abajo, junto a SCALE. Sin corrimiento de fase, ideal para mastering y buses. Debajo muestra la latencia (el DAW la compensa sola).
- **A/B**: botón junto al selector de modo. Dos ajustes completos para comparar al instante; se guardan con la sesión. La flechita al lado copia el ajuste que suena al otro lado (A→B o B→A).
- **True peak** (dBTP): arriba en la pantalla, medido entre muestras como los medidores de mastering (rojo sobre -1 dBTP). Clic para reiniciar.
- **SC RELEASE**: tiempo de vuelta del sidechain general.
- **Windows** (VST3 64 bits) además de Mac (VST3 + AU), y Mac Intel con el binario universal.

## Arriba, en los dos modos
- **Deshacer / rehacer**: flechas junto al logo (también Cmd+Z y Cmd+Shift+Z). Cada movimiento que haces con el mouse es un paso; la automatización del DAW no llena el historial.
- **Presets**: barra central con flechas para pasar de uno a otro. **Cada modo tiene su propia lista**: en Paramétrico solo ves presets del paramétrico y en Vintage solo los del vintage; cargar un preset nunca cambia de modo ni toca los ajustes del otro. 40 presets de fábrica diseñados para GAUX:
  - **Voces**: lead clara, rap / presencia, aire y brillo, coros anchos, de-esser dinámico, lo-fi teléfono, cálida con tubo.
  - **Baterías**: kick tech house, kick sub profundo, kick dubstep, snare / clap, hats, percusión, drum bus punch, drum bus control.
  - **Bajo**: sub limpio (lados en mono), growl dubstep, bajo house redondo.
  - **Sintes**: lead, pad ancho (M/S), pluck / arp, synth grueso saturado.
  - **Buses**: mix bus, bus instrumental, graves en mono, bus de voces.
  - **Mastering**: tech house, dubstep / bass, aire suave, graves controlados, ancho M/S (todos con Auto Gain para comparar justo).
  - **Vintage**: truco de graves, kick con click, voz, snare, bajo, synth, bus, master, lo-fi radio.
  - **Mis presets**: *Guardar preset...* guarda el sonido actual en `Música/Gaux EQ/Presets`. Los presets son puntos de partida: ajusta a tu sonido.

- **SC (sidechain general)**, junto a ZONE, sirve en los dos modos. Haz clic para abrir su panel:
  - **ON**: lo prende. Cuando suena la otra pista (por ejemplo el kick), toda la curva del EQ se aplana y vuelve al soltar. En el Paramétrico, además, todas las bandas dinámicas escuchan al sidechain.
  - **DUCK**: cuánto se aplana (100 % = queda plana mientras suena el kick).
  - **THRESH**: desde qué nivel del sidechain empieza a actuar. Truco: ponlo unos 8 dB debajo del pico del kick y la curva se aplana del todo en cada golpe.
  - **RELEASE**: cuánto tarda la curva en volver después de cada golpe (corto para techno/psy rápido, largo para un "pumping" más lento).
  - El puntito del botón se enciende cuando llega señal, y la barrita amarilla muestra cuánto está actuando.
  - En Ableton: sección *Sidechain* del dispositivo > *Audio From* > elige la pista.

## Paramétrico (negro/rojo infrarrojo)
- **24 bandas**: Bell, Low/High Shelf, Low/High Cut (6, 12, 18, 24, 36, 48, 72, 96 dB/oct y BRICK), Notch, Band Pass, Tilt y All Pass.
- **HARMONICS**: cada Bell o Shelf satura solo su zona con Drive y Mix. La pantalla marca dónde caen los armónicos (2H, 3H...). Cada modo tiene su propio carácter:
  - **TUBE**: válvula; domina el 2do armónico (calidez, cuerpo) a cualquier drive.
  - **TAPE**: cinta; satura antes y muy suave, agudos redondos (pegamento).
  - **SOFT**: consola; limpio hasta la rodilla y luego redondea.
  - **HARD**: clip duro; limpio hasta que muerde, mucho ataque.
  - **FUZZ**: fuzz de silicio; pares e impares, agresivo (growls).
  - Con antialiasing (ADAA) y compensación de nivel: al subir el drive la banda gana densidad sin hundirse.
- **DYNAMIC** (todo en amarillo ámbar): EQ dinámica por banda (Threshold, Range, Attack, Release). La banda solo actúa cuando la señal supera el umbral. En la pantalla, el relleno amarillo muestra en vivo lo que se está comprimiendo o expandiendo, y la línea punteada hasta dónde puede llegar. Al lado del punto seleccionado hay un **tirador amarillo para arrastrar el Range** directamente.
  - **AUTO**: el threshold se ubica solo, justo encima del nivel medio de la banda.
  - **EXT**: la banda reacciona a otra pista (sidechain). En Ableton, elige la pista en el menú *Sidechain* del plugin. Si no llega señal, el medidor dice "SIN SC".
- Espectro **antes** (gris) y **después** (rojo) del EQ.
- El panel de la banda queda **fijo abajo**, así nada se mueve mientras ajustas.
- **Canal por banda, con color propio**: ST (rojo/naranja normal), **L azul**, **R verde**, **M blanco**, **S violeta**. El punto lleva además su letra debajo, igual que el botón de la banda abajo.
- **ZONE** (arriba, sirve en los dos modos): limita todo el plugin a una zona de frecuencias. Arrastra las pestañas de los bordes en la pantalla; lo que queda afuera (rayado) pasa intacto, sin EQ ni saturación. Ejemplo: proteger el sub-bajo de un kick mientras saturas los medios.
- **Solo**: botón de audífonos en el panel, o **Alt + clic sostenido** sobre un punto.
- **Auto Gain**: compensa el cambio de volumen para comparar en igualdad.
- **Scale**: escala la ganancia de todas las bandas a la vez (0–200 %).
- Nota musical de cada frecuencia (útil para afinar kicks y bajos), y medidores de entrada y salida.
- **Curvas como un EQ analógico también a 44.1/48 kHz (sin HQ)**: las campanas (también las muy anchas), los shelf de agudos y los High Cut no se "aplastan" cerca de los 20 kHz como en un EQ digital clásico. Un shelf en 12 kHz o una campana ancha de aire suenan como deben, y al barrer cualquier perilla la curva cambia de forma continua, sin saltos.

## Vintage GX-1 (rack de tubos GAUX)
Rack de 3 unidades en esmalte rojo sangre de toro, perillas de baquelita negra (falda con índice crema y selectores "cabeza de gallo"). Cada unidad tiene su interruptor **IN** y su piloto rojo para compararla encendida y apagada.

**FILTER** (arriba, con el logo GAUX): pasa altos **LOW CUT** (OFF, 50 Hz a 2 kHz) y pasa bajos **HIGH CUT** (1.5 kHz a 15 kHz, OFF), de 12 dB/oct con curva suave de filtro pasivo. Útil para limpiar o para sonidos lo-fi. Aquí también están OUTPUT, POWER y AVANZADO.

**MID** (medios): **LOW PEAK** realza medios-graves (200 Hz a 1 kHz), **DIP** recorta medios (200 Hz a 7 kHz) y **HIGH PEAK** realza medios-agudos (1.5 a 5 kHz). Campanas anchas y musicales para voces, sintes, guitarras y teclados.

**PROGRAM** (el clásico). Afinado contra el comportamiento del equipo pasivo original:
- **LOW FREQUENCY**: elige 20, 30, 60 o 100 Hz. PUSH realza hasta +13.5 dB y CUT atenúa hasta -17.5 dB (como el original). Usados a la vez dan el truco de graves: con 60 Hz, PUSH 5 + CUT 3 hace una joroba de +3.6 dB en ~70 Hz y un hueco suave de -1 dB en ~215 Hz: sub profundo y pegado sin embarrar. Como el equipo original (acoplado por transformador), al realzar graves no deja pasar DC ni subsónicos: el realce nunca infla un DC que venga en la señal.
- **HIGH FREQUENCY**: realce de 3 a 16 kHz con **WIDTH** (de SHARP a BROAD).
- **SOFTEN**: atenúa agudos en 5, 10 o 20 kHz.
- **AIR**: brillo extra muy abierto (propio de GAUX).
- **TUBE**: **GLOW** (saturación de válvula) e **IRON** (transformador: satura sobre todo los graves y agrega armónicos pares, calidez y pegada; los agudos casi no se tocan). Calibrados como el hardware: casi limpios con señal baja y cada vez más color cuanto más fuerte entras (gain staging analógico).

**AVANZADO** despliega la curva de las tres unidades y el analizador (ahí también se ajusta la ZONE). La curva es exactamente lo que suena (verificado con tonos de prueba en las tres calidades, con diferencias menores a 0.2 dB), incluso con los interruptores IN.

**Fidelidad analógica**: todos los filtros del Vintage están "emparejados con el analógico": las curvas de agudos no se aplastan cerca de los 20 kHz (HIGH 16 kHz BROAD mantiene +16 dB en 20 kHz incluso en 1x) y suenan igual a 44.1, 48, 96 o 192 kHz. SOFTEN y AIR también en 20 y 40 kHz. Las perillas cambian la curva de forma continua (sin escalones al girarlas) y los selectores se pueden girar en plena reproducción sin clics (cambian con un fundido imperceptible).

## La calavera
La calavera con audífonos (arriba) vive con tu música:
- **Cabecea con cada kick**: detecta los golpes reales en los graves de la música que entra (no se confunde con las notas del bajo y sigue el groove aunque estés saturando fuerte).
- **Su ánimo depende de la saturación que de verdad está sonando**, no de dónde está la perilla: tranquila con el sonido limpio, cálida con saturación moderada, enojada (se le levanta la tapa) con saturación fuerte, y con CLIP la tapa sale volando y se ve el cerebro en llamas. Si subes el drive en una zona donde no hay sonido, no se enoja.
- Cambia de ánimo con calma (sin parpadeos) y se duerme cuando no hay audio o el plugin está en bypass.

## Controles
| Acción | Resultado |
|---|---|
| Doble clic en zona vacía | Crea una banda (y puedes arrastrarla de una vez) |
| Arrastrar un punto | Frecuencia y ganancia |
| Shift + arrastrar | Ajuste fino |
| Shift + clic en puntos / recuadro en zona vacía | Elegir varias bandas (se mueven juntas; Supr las borra) |
| Cmd/Ctrl + arrastrar en zona vacía | Dibujar la curva (SKETCH) |
| Clic / arrastrar un colmillo del analizador | Domar / recortar esa resonancia |
| Clic en una zona rayada (CHOQUE) | Abrir espacio para la otra pista |
| Cmd+C / Cmd+V (con la pantalla elegida) | Copiar / pegar la curva |
| Clic derecho en zona vacía | Opciones del analizador y de la pantalla, piano, copiar/pegar |
| Clic en el número de una perilla | Escribir el valor (frecuencias: también notas, `A4`, `C#2`) |
| Rueda del mouse sobre un punto | Q (ancho) |
| Alt + clic sostenido | Solo de esa banda |
| Doble clic en un punto | Borra la banda |
| Clic derecho en un punto | Tipo, reset, quitar saturación |
| Doble clic en una perilla | Valor por defecto |
| Botón de encendido (arriba a la izquierda) | Bypass del plugin |
| Botón de % (arriba a la derecha) | Tamaño de la interfaz (50–200 %) |
| Esquina inferior derecha | Arrastra para cambiar el tamaño |

## Robustez (probado)
- **Estudio virtual de mezcla y master (MIXLAB)**: sesiones de tech house, dubstep, pop, rock, reggaetón, jazz, orquesta (96 kHz), trap/hip hop, metal, folk acústico, podcast, ambient/cine, salsa y drum & bass, con los movimientos típicos de un ingeniero en cada pista y en el master (Paramétrico y Vintage). 768 comprobaciones (incluye fase lineal y natural, dinámica espectral, EQ Match, SKETCH, true peak y A/B): lo que suena es exactamente lo que dibuja la pantalla (medido con la respuesta al impulso real, < 0.02 dB), las curvas son las de un EQ analógico, la dinámica, el M/S, el sidechain, el Auto Gain y el nivel hacen lo que dicen.
- **Los 40 presets probados sobre la pista para la que fueron hechos** (voz en la voz, kick en el kick, master en la mezcla...): hacen lo que dice su nombre y no "ganan" solo por subir el volumen.
- **LFO rápidos**: la frecuencia y la Q siguen un LFO de 8 Hz (1/16 a 120 BPM, como LFO Tool o el LFO de Ableton) en un 95 % del recorrido, sin "zipper" aunque la automatización llegue en bloques grandes.
- **En vivo**: barridos de filtro de build-up (low cut y high cut resonantes, campana buscando resonancias) suenan igual con automatización gruesa o fina; bypass, encender bandas, cambiar tipo, pendiente, canal, modo Paramétrico/Vintage, selectores del Vintage, GLOW, ZONE, dinámica y **cambiar la calidad HQ** en plena reproducción: sin clics.
- Saturación con aliasing controlado: en HQ 4x el aliasing audible queda 66-84 dB por debajo (en Normal, ~30 dB con agudos muy saturados: para eso está el HQ).
- Bypass y HQ alineados con la latencia que se reporta al DAW (prender/apagar no corre el audio). Bloques gigantes al exportar, re-preparar el plugin, 44.1 a 192 kHz: todo correcto.
- Cambios en vivo sin clics en la saturación (fundido de 30 ms entre modos, DRIVE, MIX, GAIN, Q y FREQ sin "escalones").
- **Huella de regresión**: 260 ajustes (los 40 presets + 220 al azar, en 44.1/48/96 kHz y varios buffers) suenan bit a bit igual que en la versión anterior: lo que ya funcionaba no cambió.
- **Interfaz probada con el mouse simulado**: dibujar con SKETCH, cazar picos, choques entre dos instancias (y borrar una mientras se mira), seleccionar varias bandas, copiar/pegar, congelar, A/B, presets, Match con un archivo real.
- Probado con AddressSanitizer (detector de errores de memoria): 0 errores.
- **pluginval** (el validador estándar de la industria) al nivel máximo, 10, ahora también con las pruebas de la ventana abierta mientras suena: estado, automatización, fuzz de parámetros, buses, 44.1 a 192 kHz y buffers de 1 a 4096 muestras: sin fallas.
- 50 pruebas de sonido del motor + pruebas del plugin completo (curvas, saturaciones, dinámica, sidechain, ZONE, presets, interfaz, calavera).
- Estrés: miles de cambios aleatorios de parámetros por segundo, bloques irregulares, +12 dBFS, DC, impulsos, silencio largo, pistas mono: nunca da NaN ni se descontrola.
- Filtros con estructura SVF (estables al automatizar), shelf con Q máx. 2 y ganancia de banda máx. +30 dB por seguridad.
- A 88.2/96 kHz el HQ trabaja internamente hasta ~192 kHz (mismo sonido, la mitad de CPU).

## Fase: mínima, natural o lineal (Paramétrico)
| | MIN (normal) | NAT (natural) | LIN | LIN HQ |
|---|---|---|---|---|
| Fase | mínima (digital) | **igual al analógico ideal hasta 20 kHz** | lineal (sin corrimiento) | lineal |
| Latencia a 48 kHz | 0 | 1.3 ms (4.7 ms con HQ) | ~171 ms | ~683 ms |
| Dinámica, saturación, sidechain | sí | **sí** | en pausa | en pausa |
| CPU (48 kHz, 4 bandas) | ~0.25 % | ~0.5 % | ~1.7 % | ~3.2 % |
| Para qué | mezcla, en todas las pistas | agudos que suenen "analógicos" (aire, cortes en 18-20 kHz), buses | buses, master | master final |

Cerca de los 20 kHz un EQ digital normal (fase mínima) tuerce la fase (hasta 170° en un corte de agudos a 18 kHz); la fase natural la deja como en un EQ analógico perfecto (error medido 0.1°, curva 0.01 dB). En fase lineal la saturación, la dinámica y el sidechain general se pausan; la curva y el M/S funcionan completos. No usar LIN en pistas que tocas en vivo (latencia).

## CPU por instancia (48 kHz, buffer 256, ventana cerrada, un núcleo de un PC x86 normal)
| Uso | CPU | Instancias por núcleo |
|---|---|---|
| Insertado sin bandas | 0.05 % | ~2000 |
| 4 bandas típicas | 0.25 % | ~400 |
| Pista en silencio (duerme) | 0.009 % | ~10000 |
| 8 bandas | 0.45 % | ~220 |
| 24 bandas | 1.1 % | ~85 |
| 4 bandas, 2 dinámicas | 0.55 % | ~180 |
| 4 bandas en fase NATURAL | 0.53 % | ~190 |
| Vintage | 0.20 % | ~500 |
| Vintage con GLOW + IRON | 0.81 % | ~120 |
| 4 bandas en HQ 4x | 1.5 % | ~65 |

(Medido en esta versión en la máquina de pruebas, que es más lenta que la del README anterior; frente a la v0.20 en la misma máquina, la v0.21 gasta igual o menos en todos los casos.)

Es decir: 100 pistas con 4 bandas cada una usan ~25 % de **un** núcleo. Un Mac M-series es más rápido aún. Consejo: deja HQ y fase lineal para el master y los buses.

## Compilar e instalar
### macOS (Apple Silicon)
```
cd ~/Downloads/GauxEQ
bash build.sh
```
Para que también corra en Macs Intel: `bash build.sh universal`. El script firma el plugin localmente (ad-hoc) para que macOS no lo bloquee en tu Mac.
Después, en Ableton: Preferencias > Plug-ins > Volver a escanear.

### Windows 10/11 (64 bits)
1. Instala **Visual Studio 2022 Community** (gratis) marcando *Desarrollo para el escritorio con C++*.
2. Instala **Git** (https://git-scm.com/download/win).
3. Doble clic en **`build_windows.bat`**. Al final pide permiso de administrador para copiar el plugin a `C:\Program Files\Common Files\VST3`.
4. En el DAW: volver a escanear plug-ins.

El plugin compilado en Windows no necesita instalar nada más (lleva el runtime de C++ dentro), así que la carpeta `Gaux Equalizer.vst3` se puede copiar a cualquier PC Windows de 64 bits.

## Crear los instaladores para compartir
Cada sistema recibe un zip listo para enviar, con el instalador, un archivo **ANTES DE INSTALAR** (explica las alertas de Apple y de Windows y cómo pasarlas), la licencia y el código fuente.

- **Automático (Mac + Windows a la vez)**: en GitHub, pestaña *Actions* > *Instaladores* > *Run workflow*. Compila en máquinas Mac y Windows reales, instala cada paquete, valida el plugin con **pluginval** al nivel máximo de exigencia (y `auval` en Mac), prueba el desinstalador de Windows y deja los dos zip para descargar. Con una etiqueta `vX.Y.Z` además publica una *Release*.
- **Solo Mac, en tu Mac**: `bash package_mac.sh` → `dist/GauxEQ_v…_Mac.zip` (universal: Apple Silicon + Intel).
- **Solo Windows, en un PC**: doble clic en `package_windows.bat` (necesita Visual Studio 2022 con C++, Git e Inno Setup 6) → `dist\GauxEQ_v…_Windows.zip`.

| Paquete | Contenido |
|---|---|
| Mac | `Gaux Equalizer.pkg` (VST3 + AU + app, elegibles), ANTES DE INSTALAR, LICENCIA, código fuente |
| Windows | `…_Windows_Setup.exe` (VST3 + app, con desinstalador), carpeta *Portable* con el VST3 suelto, ANTES DE INSTALAR, LICENCIA, código fuente |

## Para venderlo / distribuirlo (lo que falta y depende de cuentas tuyas)
- **Mac**: firma y "notarización" de Apple para que otros Macs lo abran sin avisos: requiere una cuenta Apple Developer (99 USD/año).
- **Windows**: opcional firmar el instalador con un certificado de código para evitar el aviso de SmartScreen.
- **Pro Tools (AAX)**: requiere cuenta de desarrollador de Avid, el SDK de AAX y la firma PACE/iLok.
- **Licencia de JUCE**: JUCE es AGPLv3 o licencia comercial. Para vender el plugin con código cerrado necesitas una licencia comercial de JUCE (JUCE ha tenido un plan gratuito para ingresos pequeños; revisa los planes vigentes en juce.com). Bajo AGPLv3, si lo distribuyes debes publicar el código fuente.

## Código
- `Source/DSP/`: motor de audio (sin JUCE), con pruebas en `Tests/`.
- `Source/UI/`: interfaz (logo, mascota, pantalla, paneles, perillas).
- `Resources/Fonts/`: tipografías de licencia libre (OFL). El logo usa Syne ExtraBold (`LogoFont.ttf`).
