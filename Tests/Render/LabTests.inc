// (Incluido desde MixLab.cpp)
//==============================================================================
// LABORATORIO v0.24: las mediciones que hacen las herramientas de la industria sobre un EQ, cada una pensada para
// algo que pasa de verdad en una sesion:
//  - Plugin Doctor (DDMF): analisis lineal a distintos niveles, distorsion armonica, aliasing, respuesta al impulso
//  - Validador VST3 de Steinberg / pluginval: textos de parametros, render offline, latencia, mono/estereo
//  - Mediciones de hardware (Pultec EQP-1A / MEQ-5): donde caen las frecuencias de cada selector, el truco Pultec
//  - Ingenieria de mezcla: nulos (subir y bajar lo mismo), M/S, balistica de la dinamica, ruido de cremallera al
//    automatizar, medidores exactos
//==============================================================================

namespace lab
{
using namespace ParamID;

// Respuesta en frecuencia del plugin completo midiendo su respuesta al impulso (latencia compensada).
// chMode: 0 = impulso igual en L y R, 1 = solo en L, 2 = L = +x y R = -x (senal solo de lados)
struct Resp { std::vector<Cx> l, r; double fs = 48000; size_t n = 0; int lat = 0; std::vector<double> irl, irr; };
static Resp measure (double fs, const std::function<void (GauxEqProcessor&)>& set, size_t n = 65536, double amp = 0.01, int chMode = 0)
{
    // (el impulso entra a la mitad de la ventana: asi tambien se mide el "pre-ringing" de la fase lineal, que sale
    //  antes del pico. La respuesta se rota para que el pico quede en el 0: fase sin el retraso)
    const size_t pre = n / 2;
    St in (n + 140000 + pre);
    in.l[pre] = amp; in.r[pre] = chMode == 0 ? amp : chMode == 1 ? 0.0 : -amp;
    int lat = 0;
    const St y = renderLayout (fs, in, 512, false, set, {}, &lat);
    Resp r; r.fs = fs; r.n = n; r.lat = lat; r.l.assign (n, Cx (0)); r.r.assign (n, Cx (0));
    for (size_t i = 0; i < n; ++i)
    {
        const size_t src = pre + (size_t) lat + i - (i >= n / 2 ? n : 0);
        r.l[i] = y.l[src] / amp; r.r[i] = y.r[src] / amp;
    }
    r.irl.resize (n); r.irr.resize (n);
    for (size_t i = 0; i < n; ++i) { r.irl[i] = r.l[i].real(); r.irr[i] = r.r[i].real(); }
    fft (r.l); fft (r.r);
    return r;
}
static double magDb (const std::vector<Cx>& h, double fs, double f) { const size_t k = (size_t) std::lround (f * (double) h.size() / fs); return dB (std::abs (h[std::min (k, h.size() / 2)])); }
// Respuesta exacta en la frecuencia f (transformada directa de la respuesta al impulso: sin el error de "caer" en el
// bin mas cercano, que en un corte de 24 dB/oct a 20 Hz y 192 kHz ya son 0.6 dB)
static double dtftDb (const std::vector<double>& ir, double fs, double f)
{
    const size_t n = ir.size(); Cx acc (0), w (1); const double ang = -2 * kPi * f / fs; const Cx step (std::cos (ang), std::sin (ang));
    // indices 0..n/2-1 son tiempos positivos; n/2..n-1 son tiempos negativos (-n/2..-1)
    for (size_t i = 0; i < n / 2; ++i) { acc += ir[i] * w; w *= step; }
    Cx wn (std::cos (ang * -(double) (n / 2)), std::sin (ang * -(double) (n / 2)));
    for (size_t i = n / 2; i < n; ++i) { acc += ir[i] * wn; wn *= step; }
    return dB (std::abs (acc));
}
static double magL (const Resp& r, double f) { return dtftDb (r.irl, r.fs, f); }
static double magR (const Resp& r, double f) { return dtftDb (r.irr, r.fs, f); }
static std::vector<double> logFreqs (double f0, double f1, int n) { std::vector<double> v; for (int i = 0; i < n; ++i) v.push_back (f0 * std::pow (f1 / f0, i / (double) (n - 1))); return v; }

// Pico de una curva (frecuencia del maximo o del minimo) entre f0 y f1
static double peakFreq (const Resp& r, double f0, double f1, bool findMin = false)
{
    double best = findMin ? 1e9 : -1e9, bf = f0;
    for (double f = f0; f <= f1; f *= 1.01) { const double m = magL (r, f); if (findMin ? m < best : m > best) { best = m; bf = f; } }
    return bf;
}

// Amplitud de un tono a la frecuencia f (proyeccion por minimos cuadrados, ventana entera de periodos)
static double toneAmp (const std::vector<double>& y, size_t start, size_t len, double fs, double f)
{
    double c = 0, s = 0;
    for (size_t i = 0; i < len; ++i) { const double w = 2 * kPi * f * (double) (start + i) / fs; c += y[start + i] * std::cos (w); s += y[start + i] * std::sin (w); }
    return 2.0 * std::sqrt (c * c + s * s) / (double) len;
}

// Armonicos (en dB respecto de la fundamental) de un tono f0 que pasa por el plugin
struct Harm { double h[12] {}; double thdDb = -300; double fund = 0; };
static Harm harmonics (double fs, double f0, double ampIn, const std::function<void (GauxEqProcessor&)>& set)
{
    const size_t n = (size_t) (fs * 1.2);
    St in (n);
    for (size_t i = 0; i < n; ++i) in.l[i] = in.r[i] = ampIn * std::sin (2 * kPi * f0 * (double) i / fs);
    const St y = renderLayout (fs, in, 512, false, set);
    const size_t len = (size_t) (std::floor (fs * 0.5 / (fs / f0)) * (fs / f0)), start = (size_t) (fs * 0.6);
    Harm hm; hm.fund = toneAmp (y.l, start, len, fs, f0);
    double sum = 0;
    for (int k = 2; k < 12; ++k)
    {
        if (f0 * k >= fs * 0.45) { hm.h[k] = -300; continue; }
        const double a = toneAmp (y.l, start, len, fs, f0 * k);
        hm.h[k] = dB (a) - dB (hm.fund); sum += a * a;
    }
    hm.thdDb = dB (std::sqrt (sum)) - dB (hm.fund);
    return hm;
}

// THD+N: todo lo que no es la fundamental (despues de restarla), en dB respecto de ella
static double thdN (const std::vector<double>& y, size_t start, size_t len, double fs, double f)
{
    // ajuste exacto por minimos cuadrados de a*cos + b*sin + DC (como un analizador de THD+N con filtro de rechazo ideal)
    double A[3][3] {}, B[3] {};
    for (size_t i = 0; i < len; ++i)
    {
        const double w = 2 * kPi * f * (double) (start + i) / fs, v[3] = { std::cos (w), std::sin (w), 1.0 };
        for (int a = 0; a < 3; ++a) { B[a] += v[a] * y[start + i]; for (int b = 0; b < 3; ++b) A[a][b] += v[a] * v[b]; }
    }
    for (int c = 0; c < 3; ++c) for (int r = c + 1; r < 3; ++r) { const double m = A[r][c] / A[c][c]; for (int k = 0; k < 3; ++k) A[r][k] -= m * A[c][k]; B[r] -= m * B[c]; }
    double x[3]; for (int r = 2; r >= 0; --r) { double t = B[r]; for (int k = r + 1; k < 3; ++k) t -= A[r][k] * x[k]; x[r] = t / A[r][r]; }
    double e = 0, p = 0;
    for (size_t i = 0; i < len; ++i)
    {
        const double w = 2 * kPi * f * (double) (start + i) / fs;
        const double fit = x[0] * std::cos (w) + x[1] * std::sin (w);
        e += (y[start + i] - fit - x[2]) * (y[start + i] - fit - x[2]); p += fit * fit;
    }
    return 10.0 * std::log10 (std::max (1e-300, e) / std::max (1e-300, p));
}

struct Mode { const char* name; std::function<void (GauxEqProcessor&)> set; };
static std::vector<Mode> modes()
{
    return {
        { "MIN", [] (auto&) {} },
        { "MIN HQ 2x", [] (auto& p) { setP (p, quality, 1); } },
        { "MIN HQ 4x", [] (auto& p) { setP (p, quality, 2); } },
        { "NAT", [] (auto& p) { setP (p, phase, 3); } },
        { "LIN", [] (auto& p) { setP (p, phase, 1); } },
        { "LIN HQ", [] (auto& p) { setP (p, phase, 2); } },
    };
}
static void busyStatic (GauxEqProcessor& p)
{
    applyBand (p, 0, lcut (30, 1)); applyBand (p, 1, bell (90, 4, 1.4)); applyBand (p, 2, bell (350, -3, 2));
    applyBand (p, 3, bell (2500, 2.5, 0.8)); applyBand (p, 4, hshelf (9000, 3)); applyBand (p, 5, notch (5200, 6));
    applyBand (p, 6, lshelf (150, -2)); applyBand (p, 7, hcut (19000, 1)); applyBand (p, 8, tilt (1000, 1.5));
}
} // namespace lab

static void labTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("laboratorio lab v024").contains (gFilter)) return;
    using namespace lab;
    std::cout << "\n== LABORATORIO v0.24: mediciones tipo Plugin Doctor / validador VST3 / hardware, en escenarios reales ==" << std::endl;
    const double fs = 48000;

    //--------------------------------------------------------------------------------------------------------------
    // 1) LINEALIDAD (Plugin Doctor, analisis lineal a varios niveles): un EQ limpio hace lo mismo a -60 dBFS (una
    //    reverb que se apaga) que a +12 dBFS (un bus caliente). Sin saturacion ni dinamica la curva no puede cambiar.
    for (auto& m : modes())
    {
        auto set = [&] (GauxEqProcessor& p) { busyStatic (p); m.set (p); };
        // (el modo dormido corta la cola por debajo de -160 dBFS: con un impulso de -60 dBFS eso se notaria en la
        //  medicion de 20 Hz, aunque nadie lo pueda oir. Aqui se mide la matematica del EQ, sin dormir)
        GauxEqProcessor::allowSleep = false;
        const Resp ref = measure (fs, set, 32768, 1.0);
        double worst = 0;
        for (double amp : { 0.001, 0.03, 4.0 })
        {
            const Resp r = measure (fs, set, 32768, amp);
            for (double f : logFreqs (20, 20000, 60))
                if (magL (ref, f) > -30.0)   // (en el fondo del notch la medicion es ruido de la propia medicion)
                {
                    const double d = std::abs (magL (r, f) - magL (ref, f));
                    worst = std::max (worst, d);
                }
        }
        GauxEqProcessor::allowSleep = true;
        report (juce::String ("Linealidad ") + m.name + ": la misma curva a -60, -30, 0 y +12 dBFS (desvio max " + juce::String (worst, 4) + " dB)", worst < 0.01);
    }

    //--------------------------------------------------------------------------------------------------------------
    // 2) THD+N del EQ limpio (Plugin Doctor, analisis armonico): con 9 bandas moviendo la curva, un seno de 997 Hz a
    //    -1 dBFS no debe salir con distorsion ni ruido. Referencia: un buen conversor da -110 dB.
    for (auto& m : modes())
    {
        const size_t n = (size_t) (fs * 1.5) + 140000;
        St in (n);
        for (size_t i = 0; i < n; ++i) in.l[i] = in.r[i] = 0.89 * std::sin (2 * kPi * 997.0 * (double) i / fs);
        int lat = 0;
        const St y = renderLayout (fs, in, 512, false, [&] (GauxEqProcessor& p) { busyStatic (p); m.set (p); }, {}, &lat);
        const double t = thdN (y.l, (size_t) lat + (size_t) (fs * 0.7), (size_t) (fs * 0.7), fs, 997.0);
        report (juce::String ("THD+N del EQ limpio, ") + m.name + ": " + juce::String (t, 1) + " dB (seno 997 Hz a -1 dBFS)", t < -110.0);
    }

    //--------------------------------------------------------------------------------------------------------------
    // 3) CADA MODO DE SATURACION HACE LO QUE PROMETE (huella armonica, como en Plugin Doctor). Banda Bell 100 Hz
    //    con saturacion (drive 18), tono de 100 Hz a -12 dBFS, HQ 4x (sin aliasing que ensucie la medicion).
    {
        const char* names[] = { "TUBE", "TAPE", "SOFT", "HARD", "FUZZ" };
        Harm h[5];
        for (int t = 0; t < 5; ++t)
            h[t] = harmonics (fs, 100.0, 0.25, [&] (GauxEqProcessor& p) { applyBand (p, 0, sat (bell (100, 0, 0.5), t, 18)); setP (p, quality, 2); });
        auto hs = [&] (int t) { return juce::String (names[t]) + " (2do " + juce::String (h[t].h[2], 1) + ", 3ro " + juce::String (h[t].h[3], 1) + ", 5to " + juce::String (h[t].h[5], 1) + ", 7mo " + juce::String (h[t].h[7], 1) + " dBc)"; };
        report ("Saturacion " + hs (0) + ": valvula asimetrica, domina el 2do armonico (calidez)", h[0].h[2] > h[0].h[3] && h[0].h[2] > -40.0);
        report ("Saturacion " + hs (1) + ": cinta simetrica, 3ro suave y casi sin pares", h[1].h[3] > h[1].h[2] + 15.0 && h[1].h[3] > -50.0);
        report ("Saturacion " + hs (2) + ": consola, impar y redonda (se apaga rapido despues del 3ro)", h[2].h[3] > h[2].h[2] + 15.0 && h[2].h[7] < h[2].h[3] - 15.0);
        report ("Saturacion " + hs (3) + ": clip duro, serie impar larga (5to y 7mo claramente mas fuertes que en SOFT)", h[3].h[3] > h[3].h[2] + 15.0 && h[3].h[7] > h[2].h[7] + 6.0 && h[3].h[5] > h[2].h[5] + 3.0);
        report ("Saturacion " + hs (4) + ": fuzz, pares e impares fuertes (octava arriba + mordisco)", h[4].h[2] > -30.0 && h[4].h[3] > -30.0);
        report ("Los 5 modos de saturacion suenan distintos entre si (la huella armonica de cada par difiere al menos 3 dB)", [&]
        {
            for (int a = 0; a < 5; ++a) for (int b = a + 1; b < 5; ++b)
            {
                double d = 0; for (int k = 2; k <= 7; ++k) d = std::max (d, std::abs (h[a].h[k] - h[b].h[k]));
                if (d < 3.0) return false;
            }
            return true;
        }());
        // Mas drive = mas armonicos, siempre (perilla que hace lo que dice en todo su recorrido)
        for (int t = 0; t < 5; ++t)
        {
            double prev = -300; bool mono = true; juce::String s;
            for (double d : { 1.0, 4.0, 8.0, 14.0, 20.0 })
            {
                const Harm x = harmonics (fs, 100.0, 0.25, [&] (GauxEqProcessor& p) { applyBand (p, 0, sat (bell (100, 0, 0.5), t, d)); setP (p, quality, 2); });
                mono = mono && x.thdDb > prev - 0.5; prev = x.thdDb; s << juce::String (x.thdDb, 0) << " ";
            }
            report (juce::String ("DRIVE de ") + names[t] + " sube la distorsion en todo su recorrido (THD con drive 1/4/8/14/20: " + s.trim() + " dB)", mono);
        }
        // Compensacion de nivel: al subir el drive la banda gana armonicos, no volumen (se puede comparar a oido)
        for (int t = 0; t < 5; ++t)
        {
            const Harm a = harmonics (fs, 100.0, 0.25, [&] (GauxEqProcessor& p) { applyBand (p, 0, bell (100, 0, 0.5)); });
            const Harm b = harmonics (fs, 100.0, 0.25, [&] (GauxEqProcessor& p) { applyBand (p, 0, sat (bell (100, 0, 0.5), t, 18)); setP (p, quality, 2); });
            const double d = dB (b.fund) - dB (a.fund);
            report (juce::String ("Compensacion de nivel ") + names[t] + " con drive 18: la fundamental cambia " + juce::String (d, 1) + " dB (gana armonicos, no volumen)", std::abs (d) < 3.0);
        }
        // MIX 0 % = la banda limpia; MIX 50 % = mitad de camino
        {
            const Harm clean = harmonics (fs, 100.0, 0.25, [&] (GauxEqProcessor& p) { applyBand (p, 0, bell (100, 3, 0.7)); setP (p, quality, 2); });
            const Harm m0 = harmonics (fs, 100.0, 0.25, [&] (GauxEqProcessor& p) { applyBand (p, 0, sat (bell (100, 3, 0.7), 3, 18, 0)); setP (p, quality, 2); });
            const Harm m50 = harmonics (fs, 100.0, 0.25, [&] (GauxEqProcessor& p) { applyBand (p, 0, sat (bell (100, 3, 0.7), 3, 18, 50)); setP (p, quality, 2); });
            const Harm m100 = harmonics (fs, 100.0, 0.25, [&] (GauxEqProcessor& p) { applyBand (p, 0, sat (bell (100, 3, 0.7), 3, 18, 100)); setP (p, quality, 2); });
            report ("MIX de saturacion 0 %: la banda queda limpia (THD " + juce::String (m0.thdDb, 0) + " dB, fundamental igual a la banda sin saturar: "
                    + juce::String (dB (m0.fund) - dB (clean.fund), 3) + " dB)", m0.thdDb < -100.0 && std::abs (dB (m0.fund) - dB (clean.fund)) < 0.01);
            report ("MIX de saturacion 50 %: distorsion entre 0 y 100 % (" + juce::String (m50.thdDb, 1) + " dB vs " + juce::String (m100.thdDb, 1) + " dB al 100 %)",
                    m50.thdDb < m100.thdDb - 3.0 && m50.thdDb > m100.thdDb - 15.0);
        }
        // La saturacion es POR BANDA: solo ensucia lo que la banda toca
        for (int t : { 0, 3 })
        {
            const size_t n = (size_t) (fs * 1.4);
            St in (n);
            for (size_t i = 0; i < n; ++i) in.l[i] = in.r[i] = 0.2 * std::sin (2 * kPi * 3000.0 * (double) i / fs) + 0.2 * std::sin (2 * kPi * 55.0 * (double) i / fs);
            const St y = renderLayout (fs, in, 512, false, [&] (GauxEqProcessor& p) { applyBand (p, 0, sat (bell (3000, 0, 1.5), t, 18)); setP (p, quality, 2); });
            const size_t st = (size_t) (fs * 0.6), len = (size_t) (fs * 0.6);   // (ventana con periodos enteros de 55 Hz)
            const double f55 = toneAmp (y.l, st, len, fs, 55.0);
            const double h2 = dB (toneAmp (y.l, st, len, fs, 110.0)) - dB (f55), h3 = dB (toneAmp (y.l, st, len, fs, 165.0)) - dB (f55);
            const double s2 = dB (toneAmp (y.l, st, len, fs, 6000.0)) - dB (toneAmp (y.l, st, len, fs, 3000.0)), s3 = dB (toneAmp (y.l, st, len, fs, 9000.0)) - dB (toneAmp (y.l, st, len, fs, 3000.0));
            report (juce::String ("Saturacion por banda (") + names[t] + " en 3 kHz): el bombo de 55 Hz queda limpio (2do " + juce::String (h2, 0) + ", 3ro " + juce::String (h3, 0)
                    + " dBc) y los 3 kHz si saturan (" + juce::String (std::max (s2, s3), 0) + " dBc)", h2 < -70.0 && h3 < -70.0 && std::max (s2, s3) > -45.0);
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 4) NULOS: subir y bajar lo mismo con dos bandas iguales deja la senal como estaba (el EQ es simetrico:
    //    +6 y -6 se cancelan). Lo usan los ingenieros para comprobar que un EQ "no colorea".
    {
        struct Pair { const char* name; Band a, b; };
        const Pair pairs[] = {
            { "Bell 1 kHz Q 1", bell (1000, 6, 1), bell (1000, -6, 1) },
            { "Bell 120 Hz Q 4", bell (120, 9, 4), bell (120, -9, 4) },
            { "Low shelf 200 Hz", lshelf (200, 6), lshelf (200, -6) },
            { "High shelf 6 kHz", hshelf (6000, 6), hshelf (6000, -6) },
            { "Tilt 1 kHz", tilt (1000, 4), tilt (1000, -4) },
        };
        for (auto& m : { modes()[0], modes()[3], modes()[4] })
            for (auto& pr : pairs)
            {
                const Resp r = measure (fs, [&] (GauxEqProcessor& p) { applyBand (p, 0, pr.a); applyBand (p, 1, pr.b); m.set (p); });
                double worst = 0; for (double f : logFreqs (20, 16000, 80)) worst = std::max (worst, std::abs (magL (r, f)));
                report (juce::String ("Nulo ") + m.name + ", " + pr.name + " +g y -g: queda plano (desvio max " + juce::String (worst, 3) + " dB de 20 Hz a 16 kHz)", worst < 0.1);
            }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 5) TEXTO DE LOS PARAMETROS (validador VST3 / pluginval): lo que el DAW muestra en la automatizacion se puede
    //    escribir de vuelta y da el mismo valor (Ableton, Cubase y Logic dejan escribir el valor a mano)
    {
        GauxEqProcessor p;
        std::map<juce::String, std::pair<int, int>> groups;   // sufijo -> (probados, fallidos)
        for (auto* prm : p.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm);
            if (rp == nullptr) continue;
            const juce::String id = rp->getParameterID();
            if (id.startsWith ("b") && id.contains ("_") && ! id.startsWith ("b1_")) continue;   // (las 24 bandas son iguales: se prueba la 1)
            const juce::String key = id.startsWith ("b1_") ? "banda: " + id.fromFirstOccurrenceOf ("_", false, false) : id.startsWith ("v_") ? "vintage: " + id.substring (2) : "general: " + id;
            auto& g = groups[key];
            for (int k = 0; k <= 40; ++k)
            {
                const float v = (float) k / 40.0f;
                const juce::String t = rp->getText (v, 64);
                const float v2 = rp->getValueForText (t);
                const juce::String t2 = rp->getText (v2, 64);
                ++g.first;
                if (t2 != t || ! std::isfinite (v2)) { ++g.second; if (g.second == 1) std::cout << "    (" << id << ": \"" << t << "\" -> \"" << t2 << "\")" << std::endl; }
            }
        }
        int nOk = 0; juce::StringArray bad;
        for (auto& [k, g] : groups) { if (g.second == 0) ++nOk; else bad.add (k); }
        // (un renglon por familia de controles: asi se ve cual falla)
        juce::StringArray fams { "banda: freq", "banda: gain", "banda: q", "banda: type", "banda: slope", "banda: drive", "banda: thr", "banda: atk",
                                 "banda: rel", "general: output", "general: gainScale", "vintage: lowBoost", "vintage: highFreq", "vintage: tube" };
        for (auto& f : fams)
        {
            auto it = groups.find (f);
            report ("Texto de parametro <-> valor (" + f + "): escribir lo que muestra el DAW devuelve lo mismo", it != groups.end() && it->second.second == 0);
        }
        report ("Texto <-> valor en TODOS los parametros (" + juce::String ((int) groups.size()) + " familias): " + (bad.isEmpty() ? juce::String ("todas bien") : "fallan " + bad.joinIntoString (", ")), bad.isEmpty());
    }

    //--------------------------------------------------------------------------------------------------------------
    // 6) RUIDO DE CREMALLERA AL AUTOMATIZAR (zipper noise): una curva de automatizacion de Ableton moviendo cada
    //    control 3 veces por segundo sobre un grave puro. Un control bien suavizado solo mueve el tono; uno con
    //    escalones genera agudos. Se mide lo que aparece arriba de 3 kHz (el tono y su EQ no tienen nada ahi).
    {
        struct Auto { const char* name; std::function<void (GauxEqProcessor&)> set; juce::String id; float lo, hi; };
        const std::vector<Auto> autos {
            { "Bell FREQ 60-400 Hz", [] (auto& p) { applyBand (p, 0, bell (110, 6, 1)); }, band (0, "freq"), 60, 400 },
            { "Bell GAIN -12/+12", [] (auto& p) { applyBand (p, 0, bell (110, 6, 1)); }, band (0, "gain"), -12, 12 },
            { "Bell Q 0.3-8", [] (auto& p) { applyBand (p, 0, bell (110, 6, 1)); }, band (0, "q"), 0.3f, 8 },
            { "Low shelf GAIN", [] (auto& p) { applyBand (p, 0, lshelf (200, 0)); }, band (0, "gain"), -12, 12 },
            { "High shelf FREQ (sobre el grave)", [] (auto& p) { applyBand (p, 0, hshelf (300, 6)); }, band (0, "freq"), 80, 1000 },
            { "Low cut FREQ 20-90 Hz (24 dB)", [] (auto& p) { applyBand (p, 0, lcut (40, 1)); }, band (0, "freq"), 20, 90 },
            { "High cut FREQ 150-2000 Hz", [] (auto& p) { applyBand (p, 0, hcut (1000, 1)); }, band (0, "freq"), 150, 2000 },
            { "Tilt GAIN", [] (auto& p) { applyBand (p, 0, tilt (500, 0)); }, band (0, "gain"), -6, 6 },
            { "Notch FREQ (barrido como un flanger)", [] (auto& p) { applyBand (p, 0, notch (300, 4)); }, band (0, "freq"), 70, 400 },
            { "Dinamica THRESHOLD", [] (auto& p) { applyBand (p, 0, dynb (bell (110, 0, 1), -30, -8, 5, 80)); }, band (0, "thr"), -40, 0 },
            { "Dinamica RANGE", [] (auto& p) { applyBand (p, 0, dynb (bell (110, 0, 1), -40, -8, 5, 80)); }, band (0, "range"), -12, 6 },
            { "OUTPUT -12/+6", [] (auto&) {}, output, -12, 6 },
            { "GAIN SCALE 0-200 %", [] (auto& p) { applyBand (p, 0, bell (110, 6, 1)); }, gainScale, 0, 200 },
            { "ZONE borde bajo 50-300 Hz", [] (auto& p) { applyBand (p, 0, bell (110, 6, 1)); setP (p, rangeOn, 1); setP (p, rangeHi, 8000); }, rangeLo, 50, 300 },
            { "Vintage LOW BOOST", [] (auto& p) { setP (p, mode, 1); }, vLowBoost, 0, 10 },
            { "Vintage LOW ATTEN", [] (auto& p) { setP (p, mode, 1); setP (p, vLowBoost, 4); }, vLowAtten, 0, 10 },
            { "Vintage MID LOW PEAK", [] (auto& p) { setP (p, mode, 1); setP (p, vLmFreq, 0); }, vLmBoost, 0, 10 },
            { "Vintage DIP", [] (auto& p) { setP (p, mode, 1); setP (p, vDipFreq, 0); }, vDip, 0, 10 },
            { "Vintage IRON", [] (auto& p) { setP (p, mode, 1); setP (p, vLowBoost, 3); }, vIron, 0, 100 },
            { "LIN: Bell GAIN -12/+12", [] (auto& p) { applyBand (p, 0, bell (110, 6, 1)); setP (p, phase, 1); }, band (0, "gain"), -12, 12 },
            { "LIN: Bell FREQ 60-400 Hz", [] (auto& p) { applyBand (p, 0, bell (110, 6, 1)); setP (p, phase, 1); }, band (0, "freq"), 60, 400 },
            { "NAT: Bell GAIN -12/+12", [] (auto& p) { applyBand (p, 0, bell (110, 6, 1)); setP (p, phase, 3); }, band (0, "gain"), -12, 12 },
            { "NAT: Low shelf GAIN", [] (auto& p) { applyBand (p, 0, lshelf (200, 0)); setP (p, phase, 3); }, band (0, "gain"), -12, 12 },
            { "HQ 4x: Bell FREQ 60-400 Hz", [] (auto& p) { applyBand (p, 0, bell (110, 6, 1)); setP (p, quality, 2); }, band (0, "freq"), 60, 400 },
        };
        const size_t n = (size_t) (fs * 3.0);
        St tone (n);
        for (size_t i = 0; i < n; ++i) tone.l[i] = tone.r[i] = 0.3 * std::sin (2 * kPi * 110.0 * (double) i / fs);
        for (auto& a : autos)
        {
            const bool exp = a.id == band (0, "freq") || a.id == band (0, "q") || a.id == rangeLo;
            const St y = renderLayout (fs, tone, 128, false, a.set, [&] (GauxEqProcessor& p, double t)
            {
                juce::MessageManager::getInstance()->runDispatchLoopUntil (0);
                const double u = 0.5 - 0.5 * std::cos (2 * kPi * 3.0 * t);
                setP (p, a.id, exp ? (float) (a.lo * std::pow (a.hi / a.lo, u)) : (float) (a.lo + (a.hi - a.lo) * u));
            });
            Bq h1 (makeHighPass (fs, 3000, 0.5412)), h2 (makeHighPass (fs, 3000, 1.3066)), h3 (makeHighPass (fs, 3000, 0.5412)), h4 (makeHighPass (fs, 3000, 1.3066));
            double pk = 0, outPk = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const double v = h4.p (h3.p (h2.p (h1.p (y.l[i]))));
                if (i > (size_t) (fs * 0.5)) { pk = std::max (pk, std::abs (v)); outPk = std::max (outPk, std::abs (y.l[i])); }
            }
            report (juce::String ("Automatizacion sin cremallera: ") + a.name + " moviendose 3 veces por segundo: agudos generados " + juce::String (dB (pk), 1)
                    + " dBFS (salida " + juce::String (dB (outPk), 1) + " dBFS)", allFinite (y) && dB (pk) < -80.0);
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 7) EXPORTAR = LO QUE SE OYE: Ableton al exportar (o congelar) procesa "offline", a veces mas rapido que el
    //    tiempo real. El resultado tiene que ser identico al de la reproduccion. Y dos copias del plugin con los
    //    mismos ajustes suenan exactamente igual.
    {
        const St in = noiseMusic (fs, 2.5, 21, false);
        auto busyDyn = [] (GauxEqProcessor& p) { busyStatic (p); applyBand (p, 9, dynb (bell (3000, 0, 1), -30, -6, 5, 90)); applyBand (p, 10, sat (bell (200, 2, 1), 0, 9)); };
        const std::vector<Mode> ms {
            { "MIN con dinamica y saturacion", [&] (auto& p) { busyDyn (p); } },
            { "LIN", [&] (auto& p) { busyDyn (p); setP (p, phase, 1); } },
            { "NAT", [&] (auto& p) { busyDyn (p); setP (p, phase, 3); } },
            { "ESPECTRAL", [&] (auto& p) { busyDyn (p); applyBand (p, 11, spectral (dynb (bell (5000, 0, 1), -35, -6))); } },
            { "VINTAGE con GLOW e IRON", [&] (auto& p) { setP (p, mode, 1); setP (p, vLowBoost, 4); setP (p, vHighBoost, 3); setP (p, vTube, 50); setP (p, vIron, 40); } },
            { "HQ 4x + AUTO GAIN", [&] (auto& p) { busyDyn (p); setP (p, quality, 2); setP (p, autoGain, 1); } },
        };
        for (auto& m : ms)
        {
            GauxEqProcessor off; off.setNonRealtime (true);
            const St a = renderLayout (fs, in, 1024, false, m.set, {}, nullptr, nullptr, &off);
            const St b = renderLayout (fs, in, 1024, false, m.set);
            double e = 0; for (size_t i = 0; i < in.size(); ++i) e = std::max ({ e, std::abs (a.l[i] - b.l[i]), std::abs (a.r[i] - b.r[i]) });
            report (juce::String ("Exportar (offline) = reproducir, ") + m.name + ": identico (" + juce::String (dB (e), 0) + " dB)", e < 1e-9);
        }
        for (auto& m : ms)
        {
            const St a = renderLayout (fs, in, 512, false, m.set), b = renderLayout (fs, in, 512, false, m.set);
            double e = 0; for (size_t i = 0; i < in.size(); ++i) e = std::max ({ e, std::abs (a.l[i] - b.l[i]), std::abs (a.r[i] - b.r[i]) });
            report (juce::String ("Dos copias del plugin, ") + m.name + ": suenan identico (" + juce::String (dB (e), 0) + " dB)", e < 1e-9);
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 8) LATENCIA EXACTA (compensacion de Ableton): lo que el plugin dice que retrasa es lo que retrasa, en cada modo
    //    y frecuencia de muestreo. Si no, la pista queda corrida contra las demas (fase con el bombo, procesos en
    //    paralelo). Se mide el retraso real en graves y medios (pendiente de la fase entre 100 Hz y 2 kHz); con
    //    oversampling IIR el retraso real no es un numero entero: lo correcto es estar a menos de media muestra.
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (auto& m : modes())
            {
                const Resp r = measure (sr, [&] (GauxEqProcessor& p) { applyBand (p, 0, bell (1000, 0.0001, 1)); m.set (p); }, 65536, 0.01);
                double worst = 0;
                for (double f : { 100.0, 300.0, 1000.0, 2000.0 })
                {
                    const double df = sr / 65536.0; const size_t k = (size_t) std::lround (f / df);
                    double d = std::arg (r.l[k + 1]) - std::arg (r.l[k]); while (d > kPi) d -= 2 * kPi; while (d < -kPi) d += 2 * kPi;
                    const double resid = -d / (2 * kPi * df) * sr;   // retraso de mas (en muestras) respecto de lo informado
                    if (std::abs (resid) > std::abs (worst)) worst = resid;
                }
                report (juce::String ("Latencia exacta ") + m.name + " a " + juce::String (sr / 1000.0, 1) + " kHz: informa " + juce::String (r.lat) + " muestras, el retraso real difiere "
                        + juce::String (worst, 2) + " muestras", std::abs (worst) <= 0.55);
            }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 9) VINTAGE CONTRA EL HARDWARE (Pultec EQP-1A y MEQ-5): cada posicion de los selectores cae donde dice
    {
        auto vint = [&] (std::function<void (GauxEqProcessor&)> s) { return measure (fs, [&] (GauxEqProcessor& p) { setP (p, mode, 1); s (p); }); };
        const double lowF[] = { 20, 30, 60, 100 };
        for (int i = 0; i < 4; ++i)
        {
            const Resp r = vint ([&] (auto& p) { setP (p, vLowFreq, (float) i); setP (p, vLowBoost, 10); });
            const double atF = magL (r, lowF[i]), far = magL (r, lowF[i] * 20);
            report ("Pultec LOW " + juce::String (lowF[i], 0) + " Hz, BOOST 10: +" + juce::String (atF, 1) + " dB en " + juce::String (lowF[i], 0) + " Hz y " + juce::String (far, 1)
                    + " dB en " + juce::String (lowF[i] * 20, 0) + " Hz (shelf de graves, como el EQP-1A: +10 a +15 dB)", atF > 9.0 && atF < 16.0 && far < 2.0);
        }
        for (int i = 0; i < 4; ++i)
        {
            const Resp r = vint ([&] (auto& p) { setP (p, vLowFreq, (float) i); setP (p, vLowBoost, 10); setP (p, vLowAtten, 10); });
            // (EQP-1A real: la atenuacion actua un poco mas abajo que el realce. Queda un "bulto" en la frecuencia elegida,
            //  un hueco 2-4 veces arriba y lo de mas abajo cae: la famosa curva del truco Pultec)
            const double dipF = peakFreq (r, lowF[i] * 1.2, lowF[i] * 15, true), dip = magL (r, dipF), bump = magL (r, lowF[i]), sub = magL (r, lowF[i] * 0.25);
            report ("Truco Pultec en " + juce::String (lowF[i], 0) + " Hz (BOOST 10 + ATTEN 10): bulto de " + juce::String (bump, 1) + " dB en " + juce::String (lowF[i], 0) + " Hz, hueco de "
                    + juce::String (dip, 1) + " dB en " + juce::String (dipF, 0) + " Hz y " + juce::String (sub, 1) + " dB dos octavas abajo",
                    bump > 1.0 && dip < -1.5 && dipF > lowF[i] * 1.8 && dipF < lowF[i] * 5 && sub < bump - 3.0);
        }
        const double hiF[] = { 3000, 4000, 5000, 8000, 10000, 12000, 16000 };
        for (int i = 0; i < 7; ++i)
        {
            const Resp r = vint ([&] (auto& p) { setP (p, vHighFreq, (float) i); setP (p, vHighBoost, 8); setP (p, vHighBw, 3); });
            const double pf = peakFreq (r, 1000, 22000);
            report ("Pultec HIGH FREQ " + juce::String (hiF[i] / 1000.0, 0) + "k: el realce cae en " + juce::String (pf / 1000.0, 2) + " kHz (" + juce::String (magL (r, pf), 1) + " dB)",
                    pf > hiF[i] * 0.75 && pf < hiF[i] * 1.33);
        }
        {
            const Resp sharp = vint ([&] (auto& p) { setP (p, vHighFreq, 2); setP (p, vHighBoost, 8); setP (p, vHighBw, 0); });
            const Resp broad = vint ([&] (auto& p) { setP (p, vHighFreq, 2); setP (p, vHighBoost, 8); setP (p, vHighBw, 10); });
            const double s = magL (sharp, 2500), b = magL (broad, 2500);
            report ("Pultec BANDWIDTH: SHARP es angosto (" + juce::String (s, 1) + " dB a una octava) y BROAD ancho (" + juce::String (b, 1) + " dB)", b > s + 1.5);
        }
        const double attF[] = { 5000, 10000, 20000 };
        for (int i = 0; i < 3; ++i)
        {
            const Resp r = vint ([&] (auto& p) { setP (p, vAttenFreq, (float) i); setP (p, vHighAtten, 10); });
            const double at = magL (r, std::min (attF[i], 20000.0)), low = magL (r, attF[i] / 8);
            report ("Pultec ATTEN SEL " + juce::String (attF[i] / 1000.0, 0) + "k, ATTEN 10: " + juce::String (at, 1) + " dB en " + juce::String (attF[i] / 1000.0, 0) + "k y "
                    + juce::String (low, 1) + " dB tres octavas abajo (shelf de corte de agudos)", at < -5.0 && low > -2.0);
        }
        const double lm[] = { 200, 300, 500, 700, 1000 };
        for (int i = 0; i < 5; ++i)
        {
            const Resp r = vint ([&] (auto& p) { setP (p, vLmFreq, (float) i); setP (p, vLmBoost, 8); });
            const double pf = peakFreq (r, 60, 5000);
            report ("MEQ-5 LOW PEAK " + juce::String (lm[i], 0) + " Hz: el pico cae en " + juce::String (pf, 0) + " Hz (" + juce::String (magL (r, pf), 1) + " dB)", pf > lm[i] * 0.8 && pf < lm[i] * 1.25);
        }
        const double dp[] = { 200, 500, 1000, 2000, 4000, 7000 }; const int dpi[] = { 0, 2, 4, 6, 8, 10 };
        for (int i = 0; i < 6; ++i)
        {
            const Resp r = vint ([&] (auto& p) { setP (p, vDipFreq, (float) dpi[i]); setP (p, vDip, 8); });
            const double pf = peakFreq (r, 60, 20000, true);
            report ("MEQ-5 DIP " + juce::String (dp[i], 0) + " Hz: el hueco cae en " + juce::String (pf, 0) + " Hz (" + juce::String (magL (r, pf), 1) + " dB)", pf > dp[i] * 0.8 && pf < dp[i] * 1.25);
        }
        const double hm[] = { 1500, 2000, 3000, 4000, 5000 };
        for (int i = 0; i < 5; ++i)
        {
            const Resp r = vint ([&] (auto& p) { setP (p, vHmFreq, (float) i); setP (p, vHmBoost, 8); });
            const double pf = peakFreq (r, 300, 20000);
            report ("MEQ-5 HIGH PEAK " + juce::String (hm[i] / 1000.0, 1) + " kHz: el pico cae en " + juce::String (pf / 1000.0, 2) + " kHz (" + juce::String (magL (r, pf), 1) + " dB)", pf > hm[i] * 0.8 && pf < hm[i] * 1.25);
        }
        const double lc[] = { 50, 100, 250, 1000 }; const int lci[] = { 1, 3, 5, 8 };
        for (int i = 0; i < 4; ++i)
        {
            const Resp r = vint ([&] (auto& p) { setP (p, vLowCut, (float) lci[i]); });
            const double at = magL (r, lc[i]), oct = magL (r, lc[i] * 4), below = magL (r, lc[i] / 4);
            report ("FILTER LOW CUT " + juce::String (lc[i], 0) + " Hz: " + juce::String (at, 1) + " dB en el corte, " + juce::String (below, 1) + " dB dos octavas abajo, "
                    + juce::String (oct, 1) + " dB dos octavas arriba", at < -1.0 && at > -9.0 && below < -15.0 && oct > -1.0);
        }
        const double hc[] = { 2000, 5000, 10000, 15000 }; const int hci[] = { 1, 4, 7, 9 };
        for (int i = 0; i < 4; ++i)
        {
            const Resp r = vint ([&] (auto& p) { setP (p, vHighCut, (float) hci[i]); });
            const double at = magL (r, hc[i]), below = magL (r, hc[i] / 4);
            report ("FILTER HIGH CUT " + juce::String (hc[i] / 1000.0, 1) + " kHz: " + juce::String (at, 1) + " dB en el corte y " + juce::String (below, 1) + " dB dos octavas abajo",
                    at < -1.0 && at > -9.0 && below > -1.0);
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 10) M/S Y CANALES EXACTOS en cada modo: una banda Mid no toca lo que es solo lados, Side no toca el centro,
    //     Left no toca el derecho y Right no toca el izquierdo (mezcla en mono que no cambia, por ejemplo)
    for (auto& m : { modes()[0], modes()[3], modes()[4] })
    {
        {
            const Resp r = measure (fs, [&] (GauxEqProcessor& p) { applyBand (p, 0, bell (800, 9, 1, 3)); applyBand (p, 1, hshelf (5000, 6, 0.707, 3)); m.set (p); }, 32768, 0.01, 2);
            double w = 0; for (double f : logFreqs (20, 20000, 50)) w = std::max (w, std::abs (magL (r, f)));
            report (juce::String ("M/S ") + m.name + ": bandas MID no tocan una senal que es solo lados (desvio " + juce::String (w, 4) + " dB)", w < 0.01);
        }
        {
            const Resp r = measure (fs, [&] (GauxEqProcessor& p) { applyBand (p, 0, bell (800, 9, 1, 4)); applyBand (p, 1, lcut (200, 2, 0.707, 4)); m.set (p); }, 32768, 0.01, 0);
            double w = 0; for (double f : logFreqs (20, 20000, 50)) w = std::max (w, std::abs (magL (r, f)));
            report (juce::String ("M/S ") + m.name + ": bandas SIDE no tocan una senal mono/centro (desvio " + juce::String (w, 4) + " dB)", w < 0.01);
        }
        {
            const Resp r = measure (fs, [&] (GauxEqProcessor& p) { applyBand (p, 0, bell (800, 9, 1, 1)); m.set (p); }, 32768, 0.01, 0);
            double wr = 0; for (double f : logFreqs (20, 20000, 50)) wr = std::max (wr, std::abs (magR (r, f)));
            report (juce::String ("Canal ") + m.name + ": banda LEFT +9 dB deja el derecho intacto (desvio " + juce::String (wr, 4) + " dB) y el izquierdo sube " + juce::String (magL (r, 800), 2) + " dB",
                    wr < 0.01 && std::abs (magL (r, 800) - 9.0) < 0.3);
        }
        {
            const Resp r = measure (fs, [&] (GauxEqProcessor& p) { applyBand (p, 0, bell (800, 9, 1, 2)); m.set (p); }, 32768, 0.01, 0);
            double wl = 0; for (double f : logFreqs (20, 20000, 50)) wl = std::max (wl, std::abs (magL (r, f)));
            report (juce::String ("Canal ") + m.name + ": banda RIGHT +9 dB deja el izquierdo intacto (desvio " + juce::String (wl, 4) + " dB) y el derecho sube " + juce::String (magR (r, 800), 2) + " dB",
                    wl < 0.01 && std::abs (magR (r, 800) - 9.0) < 0.3);
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 11) BALISTICA DE LA EQ DINAMICA: threshold, range, attack y release hacen lo que marcan (de-esser, control
    //     de resonancias, bajo que se aparta del bombo)
    {
        // tono de 1 kHz que salta de -40 a -6 dBFS (20 dB sobre el threshold de -26) y vuelve
        auto run = [&] (double thr, double range, double atk, double rel, double level, std::vector<double>* gainCurve)
        {
            GauxEqProcessor p;
            applyBand (p, 0, dynb (bell (1000, 0, 1), thr, range, atk, rel));
            p.prepareToPlay (fs, 48);
            juce::AudioBuffer<float> buf (2, 48); juce::MidiBuffer midi;
            double last = 0;
            for (int blk = 0; blk < (int) (fs * 4.5 / 48); ++blk)
            {
                const double t = blk * 48 / fs;
                const double a = (t >= 0.5 && t < 1.2) ? level : 0.01;
                for (int i = 0; i < 48; ++i) { const float v = (float) (a * std::sin (2 * kPi * 1000.0 * (blk * 48 + i) / fs)); buf.setSample (0, i, v); buf.setSample (1, i, v); }
                p.processBlock (buf, midi);
                last = p.dynGainDb[0].load();
                if (gainCurve) gainCurve->push_back (last);
            }
            return last;
        };
        // threshold: justo debajo no actua, bien arriba llega al range
        {
            std::vector<double> g; run (-20, -6, 5, 100, std::pow (10.0, -23.0 / 20.0) * 1.414, &g);
            double mx = 0; for (size_t i = (size_t) (0.9 * fs / 48); i < (size_t) (1.15 * fs / 48); ++i) mx = std::min (mx, g[i]);
            report ("Dinamica THRESHOLD -20: un tono 3 dB por debajo no la activa (" + juce::String (mx, 2) + " dB)", mx > -0.3);
        }
        for (double rg : { -3.0, -6.0, -12.0, 4.0 })
        {
            std::vector<double> g; run (-30, rg, 5, 100, 0.5, &g);
            const double steady = g[(size_t) (1.1 * fs / 48)];
            report ("Dinamica RANGE " + juce::String (rg, 0) + " dB: con la senal 24 dB arriba del threshold llega a " + juce::String (steady, 2) + " dB", std::abs (steady - rg) < 0.3);
        }
        auto t90 = [&] (const std::vector<double>& g, double from, double target, bool rising)
        {
            const size_t s0 = (size_t) (from * fs / 48);
            for (size_t i = s0; i < g.size(); ++i)
                if (rising ? g[i] <= 0.9 * target : g[i] >= 0.1 * target) return (double) (i - s0) * 48.0 / fs * 1000.0;
            return 1e9;
        };
        double prevA = 0;
        for (double atk : { 1.0, 10.0, 50.0 })
        {
            std::vector<double> g; run (-30, -6, atk, 100, 0.5, &g);
            const double ta = t90 (g, 0.5, -6.0, true);
            report ("Dinamica ATTACK " + juce::String (atk, 0) + " ms: llega al 90 % del recorte en " + juce::String (ta, 1) + " ms", ta < atk * 4.0 + 3.0 && ta > atk * 0.3 && ta > prevA);
            prevA = ta;
        }
        double prevR = 0;
        for (double rel : { 50.0, 200.0, 1000.0 })
        {
            std::vector<double> g; run (-30, -6, 2, rel, 0.5, &g);
            const double tr = t90 (g, 1.2, -6.0, false);
            report ("Dinamica RELEASE " + juce::String (rel, 0) + " ms: vuelve al 10 % del recorte en " + juce::String (tr, 0) + " ms", tr < rel * 4.0 + 20.0 && tr > rel * 0.3 && tr > prevR);
            prevR = tr;
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 12) MEDIDORES EXACTOS: lo que muestran el pico, el RMS y el true peak es lo que hay en la salida
    {
        GauxEqProcessor p; GauxEqProcessor::forceUiForTests = true;
        p.prepareToPlay (fs, 512);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        for (int blk = 0; blk < 200; ++blk)
        {
            for (int i = 0; i < 512; ++i) { const float v = (float) (0.5 * std::sin (2 * kPi * 1000.0 * (blk * 512 + i) / fs)); buf.setSample (0, i, v); buf.setSample (1, i, v); }
            p.processBlock (buf, midi);
        }
        const double pk = dB (p.outputPeak.load()), rms = dB (p.outputRms.load());
        report ("Medidor de PICO: un seno a -6.02 dBFS marca " + juce::String (pk, 2) + " dBFS", std::abs (pk + 6.02) < 0.1);
        report ("Medidor RMS: el mismo seno (RMS -9.03 dBFS) marca " + juce::String (rms, 2) + " dBFS", std::abs (rms + 9.03) < 0.3);
        // true peak: seno en fs/4 desfasado 45 grados (las muestras caen en 0.707 del pico real)
        GauxEqProcessor q; q.prepareToPlay (fs, 512);
        for (int blk = 0; blk < 200; ++blk)
        {
            for (int i = 0; i < 512; ++i) { const float v = (float) (0.5 * std::sin (2 * kPi * (fs / 4) * (blk * 512 + i) / fs + kPi / 4)); buf.setSample (0, i, v); buf.setSample (1, i, v); }
            q.processBlock (buf, midi);
        }
        const double tp = dB (q.truePeak.load()), sp = dB (q.outputPeak.load());
        report ("TRUE PEAK: seno con picos entre muestras: muestras en " + juce::String (sp, 2) + " dBFS, true peak " + juce::String (tp, 2) + " dBFS (el real es -6.02)", std::abs (tp + 6.02) < 0.6 && tp > sp + 2.0);
        GauxEqProcessor::forceUiForTests = false;
    }

    //--------------------------------------------------------------------------------------------------------------
    // 13) MISMO EQ A CUALQUIER FRECUENCIA DE MUESTREO: una sesion a 44.1 abierta a 96 kHz suena igual
    {
        auto set = [] (GauxEqProcessor& p) { applyBand (p, 0, bell (1000, 6, 2)); applyBand (p, 1, lshelf (100, 4)); applyBand (p, 2, hshelf (8000, -3)); applyBand (p, 3, lcut (40, 1)); };
        const Resp ref = measure (48000, set);
        for (double sr : { 44100.0, 88200.0, 96000.0, 192000.0 })
        {
            const Resp r = measure (sr, set, 131072);
            double w = 0; for (double f : logFreqs (20, 15000, 60)) w = std::max (w, std::abs (magL (r, f) - magL (ref, f)));
            report ("Misma curva a " + juce::String (sr / 1000.0, 1) + " kHz que a 48 kHz (desvio max " + juce::String (w, 3) + " dB hasta 15 kHz)", w < 0.25);
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 14) DC Y POLARIDAD: un DC en la senal (grabacion con offset) no se amplifica con un low shelf ni lo crea la
    //     saturacion asimetrica; y el EQ nunca invierte la polaridad
    {
        St in ((size_t) (fs * 3));
        for (size_t i = 0; i < in.size(); ++i) in.l[i] = in.r[i] = 0.1 + 0.2 * std::sin (2 * kPi * 440 * (double) i / fs);
        auto dcOf = [&] (const St& y) { double s = 0; const size_t a = (size_t) (fs * 2), b = (size_t) (fs * 3); for (size_t i = a; i < b; ++i) s += y.l[i]; return s / (double) (b - a); };
        const St a = renderLayout (fs, in, 512, false, [] (GauxEqProcessor& p) { applyBand (p, 0, lcut (25, 1)); });
        report ("Low cut 25 Hz quita un DC de 0.1 (queda " + juce::String (dB (std::abs (dcOf (a))), 0) + " dBFS)", std::abs (dcOf (a)) < 1e-4);
        St in2 ((size_t) (fs * 3));
        for (size_t i = 0; i < in2.size(); ++i) in2.l[i] = in2.r[i] = 0.4 * std::sin (2 * kPi * 440 * (double) i / fs);
        for (int t : { 0, 4 })
        {
            const St b = renderLayout (fs, in2, 512, false, [&] (GauxEqProcessor& p) { applyBand (p, 0, sat (bell (440, 0, 0.7), t, 24)); });
            report (juce::String ("Saturacion ") + (t == 0 ? "TUBE" : "FUZZ") + " (asimetrica) a tope no deja DC en la salida (" + juce::String (dB (std::abs (dcOf (b))), 0) + " dBFS)", std::abs (dcOf (b)) < 1e-3);
        }
        const Resp r = measure (fs, [] (GauxEqProcessor& p) { applyBand (p, 0, bell (1000, 12, 0.5)); applyBand (p, 1, lshelf (100, 6)); });
        report ("Polaridad: con realces fuertes la respuesta en graves sigue positiva (no se invierte la fase)", r.l[(size_t) std::lround (30.0 * (double) r.n / fs)].real() > 0.0);
    }

    //--------------------------------------------------------------------------------------------------------------
    // 15) COLAS QUE SE APAGAN SIN GASTAR CPU (denormales): un notch Q 30 en 20 Hz y un band pass resonante despues
    //     de un golpe fuerte, y despues 8 s de silencio. La cola llega a cero de verdad y el plugin se duerme.
    {
        GauxEqProcessor p;
        applyBand (p, 0, notch (20, 30)); applyBand (p, 1, bpass (60, 18, 3)); applyBand (p, 2, bell (40, 12, 30));
        p.prepareToPlay (fs, 512);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        double tFirst = 0, tLast = 0;
        for (int blk = 0; blk < (int) (fs * 10 / 512); ++blk)
        {
            for (int i = 0; i < 512; ++i) { const float v = blk == 2 && i == 0 ? 1.0f : 0.0f; buf.setSample (0, i, v); buf.setSample (1, i, v); }
            const auto t0 = std::chrono::high_resolution_clock::now();
            p.processBlock (buf, midi);
            const double dt = std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
            if (blk > 10 && blk < 110) tFirst += dt;
            if (blk > (int) (fs * 9 / 512) - 100 && blk <= (int) (fs * 9 / 512)) tLast += dt;
        }
        double last = 0; for (int i = 0; i < 512; ++i) last = std::max (last, (double) std::abs (buf.getSample (0, i)));
        report ("Cola resonante (Q 30 en 20-60 Hz) 8 s despues: salida " + juce::String (dB (last), 0) + " dBFS, sin denormales (CPU al final " + juce::String (tLast * 1000, 2)
                + " ms vs al principio " + juce::String (tFirst * 1000, 2) + " ms cada 100 bloques)", last < 1e-6 && tLast < tFirst * 1.5 + 0.0005);
    }
}
