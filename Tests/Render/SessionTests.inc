// (Incluido desde MixLab.cpp, despues de LabTests.inc)
//==============================================================================
// SESIONES REALES v0.25: lo que pasa de verdad cuando el plugin se usa en un proyecto
//  - los 40 presets de fabrica, uno por uno, sobre una cancion: suenan, no saltan de volumen, AUTO GAIN los
//    iguala, se guardan y se abren igual, lo que se ve es lo que se oye, y suenan igual a 96 kHz
//  - Ableton y otros DAW: cada tamano de buffer, bloques que cambian en cada llamada, cada frecuencia de
//    muestreo, parar y volver a darle play, loops, bypass rapido
//  - trabajos tipicos de ingenieria medidos: low cut de un pad, de-esser, graves en mono, kick vs bajo
//==============================================================================

namespace sess
{
using namespace ParamID;

// Una "cancion" sintetica con todo lo que tiene una produccion: bombo, bajo, hats, pad estereo y una voz con eses
struct Song { St mix, kick, snare, hats, bass, pad, lead, voice; };
static Song song (double fs, double secs, unsigned seed = 7)
{
    Song s; s.mix = St ((size_t) (fs * secs));
    for (St* st : { &s.kick, &s.snare, &s.hats, &s.bass, &s.pad, &s.lead, &s.voice }) *st = St (s.mix.size());
    double leadPh = 0, snLp = 0;
    Rng rr (seed);
    double bassPh = 0, padPh[3] = { 0, 0, 0 }, vPh = 0, hpL = 0, hpR = 0, f1s = 0, f1c = 0, f2s = 0, f2c = 0;
    const double bpm = 124, beat = 60.0 / bpm;
    for (size_t i = 0; i < s.mix.size(); ++i)
    {
        const double t = (double) i / fs, pb = std::fmod (t, beat);
        // bombo
        const double k = pb < 0.25 ? 0.75 * std::sin (2 * kPi * (45 + 120 * std::exp (-pb * 35)) * pb) * std::exp (-pb * 11) : 0.0;
        // bajo (sierra suave en La 55 Hz, con sidechain del bombo)
        bassPh += 55.0 / fs; if (bassPh > 1) bassPh -= 1;
        const double duck = 1.0 - 0.7 * std::exp (-pb * 9);
        const double bass = 0.22 * duck * (std::sin (2 * kPi * bassPh) + 0.35 * std::sin (4 * kPi * bassPh) + 0.15 * std::sin (6 * kPi * bassPh));
        // hats (ruido agudo en las corcheas a contratiempo)
        const double ph8 = std::fmod (t + beat / 2, beat);
        const double nL = rr.uni(), nR = rr.uni();
        hpL = 0.6 * (hpL + nL - (i ? 0 : 0)); hpR = 0.6 * (hpR + nR);
        const double hat = ph8 < 0.05 ? std::exp (-ph8 * 80) : 0.0;
        // pad (acorde La menor, abierto en estereo)
        const double fr[3] = { 220.0, 261.63, 329.63 };
        double padL = 0, padR = 0;
        for (int j = 0; j < 3; ++j) { padPh[j] += fr[j] / fs; padL += std::sin (2 * kPi * padPh[j]); padR += std::sin (2 * kPi * padPh[j] * 1.003); }
        // voz: tren de pulsos 180 Hz por dos formantes (700 y 1200 Hz) + eses (6-9 kHz) cada 2 compases
        vPh += 180.0 / fs; double pulse = 0; if (vPh > 1) { vPh -= 1; pulse = 1; }
        const double w1 = 2 * kPi * 700 / fs, w2 = 2 * kPi * 1200 / fs;
        f1s += w1 * f1c + pulse * 0.6; f1c -= w1 * f1s; f1s *= 0.995; f1c *= 0.995;
        f2s += w2 * f2c + pulse * 0.4; f2c -= w2 * f2s; f2s *= 0.993; f2c *= 0.993;
        const double phrase = std::fmod (t, beat * 8);
        const double sib = (phrase > beat * 3 && phrase < beat * 3.4) ? 1.0 : 0.0;
        // (una "s" de verdad es fuerte: unos -20 dBFS en la zona de 6-9 kHz)
        const double voice = 0.05 * (f1s + f2s) * (phrase < beat * 6 ? 1.0 : 0.0) + 0.6 * sib * (nL - 0.5 * nR);
        // snare/clap en los tiempos 2 y 4
        const double p2 = std::fmod (t + beat, 2 * beat);
        snLp += 0.5 * (nR - snLp);
        const double snare = p2 < 0.2 ? (0.35 * snLp + 0.25 * std::sin (2 * kPi * 190 * p2)) * std::exp (-p2 * 22) : 0.0;
        // lead: sierra en arpegio de semicorcheas (La menor), filtrada por su envolvente
        const double nfs[4] = { 440.0, 523.25, 659.25, 880.0 };
        const double p16 = std::fmod (t, beat / 4);
        leadPh += nfs[(int) (t / (beat / 4)) % 4] / fs; if (leadPh > 1) leadPh -= 1;
        const double lead = 0.12 * (2 * leadPh - 1) * std::exp (-p16 * 18);
        const double hl = 0.06 * hat * hpL, hr = 0.06 * hat * hpR;
        const double l = k + bass + hl + 0.035 * padL + voice + snare + lead;
        const double r = k + bass + hr + 0.035 * padR + voice + snare + lead;
        s.mix.l[i] = 0.8 * l; s.mix.r[i] = 0.8 * r;
        s.kick.l[i] = s.kick.r[i] = k; s.bass.l[i] = s.bass.r[i] = bass; s.hats.l[i] = hl; s.hats.r[i] = hr;
        s.pad.l[i] = 0.035 * padL; s.pad.r[i] = 0.035 * padR; s.voice.l[i] = s.voice.r[i] = voice; s.snare.l[i] = s.snare.r[i] = snare;
        s.lead.l[i] = s.lead.r[i] = lead;
    }
    return s;
}

static double peakOf (const St& s, size_t from = 0) { double m = 0; for (size_t i = from; i < s.size(); ++i) m = std::max ({ m, std::abs (s.l[i]), std::abs (s.r[i]) }); return m; }
static double maxDiffDb (const St& a, const St& b, size_t from, size_t lagB = 0)
{
    double e = 0, m = 0;
    for (size_t i = from; i + lagB < a.size() && i + lagB < b.size(); ++i)
    { e = std::max ({ e, std::abs (a.l[i] - b.l[i + lagB]), std::abs (a.r[i] - b.r[i + lagB]) }); m = std::max (m, std::abs (a.l[i])); }
    return dB (e) - dB (std::max (1e-12, m));
}
// Energia por octava (125 Hz .. 8 kHz) de la salida relativa a la entrada
static std::vector<double> octaveGains (const St& in, const St& out, double fs, size_t from)
{
    std::vector<double> g;
    for (double fc = 125; fc <= 8000; fc *= 2)
    {
        Bq a1 (makeHighPass (fs, fc / 1.414, 0.7071)), a2 (makeLowPass (fs, fc * 1.414, 0.7071)), b1 (makeHighPass (fs, fc / 1.414, 0.7071)), b2 (makeLowPass (fs, fc * 1.414, 0.7071));
        double ei = 0, eo = 0;
        for (size_t i = 0; i < in.size(); ++i)
        {
            const double x = a2.p (a1.p (in.l[i])), y = b2.p (b1.p (out.l[i]));
            if (i >= from) { ei += x * x; eo += y * y; }
        }
        g.push_back (10.0 * std::log10 (std::max (1e-30, eo) / std::max (1e-30, ei)));
    }
    return g;
}

// Mezcla de pistas (para buses)
static St sum (std::initializer_list<const St*> parts) { St o (parts.begin()[0]->size()); for (auto* p : parts) for (size_t i = 0; i < o.size(); ++i) { o.l[i] += p->l[i]; o.r[i] += p->r[i]; } return o; }
// La pista para la que esta pensado cada preset (un preset de hats se prueba sobre hats, uno de voz sobre una voz)
static St stemFor (const Song& s, const juce::String& n)
{
    auto has = [&] (const char* k) { return n.containsIgnoreCase (k); };
    if (has ("Drum Bus")) return sum ({ &s.kick, &s.snare, &s.hats });
    if (has ("Kick")) return s.kick;
    if (has ("Snare") || has ("Clap")) return s.snare;
    if (has ("Hats")) return s.hats;
    if (has ("Percusion")) return sum ({ &s.snare, &s.hats });
    if (has ("Truco de Graves")) return sum ({ &s.kick, &s.bass });
    if (has ("Bajo")) return s.bass;
    if (has ("Pad")) return s.pad;
    if (has ("Lead") || has ("Pluck") || has ("Synth")) return s.lead;
    if (has ("Voz") || has ("Coros") || has ("De-Esser") || has ("Bus de Voces")) return s.voice;
    if (has ("Instrumental")) return sum ({ &s.kick, &s.snare, &s.hats, &s.bass, &s.pad, &s.lead });
    return s.mix;   // buses, masters, graves en mono, lo-fi radio
}
static St pink (double fs, double secs, unsigned seed)
{
    St s ((size_t) (fs * secs)); Rng rr (seed); double b[7] {};
    for (size_t i = 0; i < s.size(); ++i)
    {
        const double w = rr.uni();
        b[0] = 0.99886 * b[0] + w * 0.0555179; b[1] = 0.99332 * b[1] + w * 0.0750759; b[2] = 0.96900 * b[2] + w * 0.1538520;
        b[3] = 0.86650 * b[3] + w * 0.3104856; b[4] = 0.55000 * b[4] + w * 0.5329522; b[5] = -0.7616 * b[5] - w * 0.0168980;
        const double v = 0.05 * (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362); b[6] = w * 0.115926;
        s.l[i] = s.r[i] = v;   // (centro)
    }
    // lados: otro ruido rosa con el 20 % de la energia (como una mezcla real)
    const St sd = seed == 999 ? St (s.size()) : pink (fs, secs, 999);
    for (size_t i = 0; i < s.size(); ++i) { const double side = 0.5 * sd.l[i]; s.l[i] += side; s.r[i] -= side; }
    return s;
}

struct PresetRef { int factory; juce::String name, category; bool vintage; };
static std::vector<PresetRef> allPresets()
{
    std::vector<PresetRef> v;
    const auto& fp = gaux::factoryPresets();
    for (int i = 0; i < (int) fp.size(); ++i) v.push_back ({ i, fp[(size_t) i].name, fp[(size_t) i].category, gaux::PresetManager::isVintagePreset (fp[(size_t) i]) });
    return v;
}
static void loadPreset (GauxEqProcessor& p, const PresetRef& pr)
{
    setP (p, mode, pr.vintage ? 1.0f : 0.0f);
    auto list = p.presets.entries();
    for (int i = 0; i < (int) list.size(); ++i) if (! list[(size_t) i].user && list[(size_t) i].factoryIndex == pr.factory) { p.presets.load (i); return; }
}
} // namespace sess

static void sessionTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("sesiones sesion v025").contains (gFilter)) return;
    using namespace sess;
    std::cout << "\n== SESIONES REALES v0.25: presets sobre una cancion, DAWs, trabajos de ingenieria ==" << std::endl;
    const double fs = 48000;
    const Song sg = song (fs, 6.0);
    const size_t from = (size_t) (fs * 2.0);
    const double inLufs = lufs (sg.mix, fs, from);

    //--------------------------------------------------------------------------------------------------------------
    // 1) LOS 40 PRESETS DE FABRICA, UNO POR UNO
    for (auto& pr : allPresets())
    {
        const juce::String nm = "Preset \"" + pr.name + "\" (" + pr.category + ")";
        int lat = 0, res = 0;
        const St y = renderLayout (fs, sg.mix, 512, false, [&] (GauxEqProcessor& p) { loadPreset (p, pr); }, {}, &lat, &res);
        const St ya = [&] { St a (y.size()); for (size_t i = 0; i + (size_t) lat < y.size(); ++i) { a.l[i] = y.l[i + (size_t) lat]; a.r[i] = y.r[i + (size_t) lat]; } return a; }();
        const double pk = peakOf (y, from);
        report (nm + ": carga y suena (finito, sin reinicios, pico " + juce::String (dB (pk), 1) + " dBFS)", allFinite (y) && res == 0 && pk < 4.0 && pk > 0.01);

        juce::ignoreUnused (ya, inLufs);
        // volumen sobre la pista para la que esta pensado (un preset de hats sobre hats, uno de voz sobre una voz)
        const St stem = stemFor (sg, pr.name);
        const St ys = renderLayout (fs, stem, 512, false, [&] (GauxEqProcessor& p) { loadPreset (p, pr); }, {}, &lat);
        St ysA (ys.size()); for (size_t i = 0; i + (size_t) lat < ys.size(); ++i) { ysA.l[i] = ys.l[i + (size_t) lat]; ysA.r[i] = ys.r[i + (size_t) lat]; }
        const double dl = lufs (ysA, fs, from) - lufs (stem, fs, from);
        const bool special = pr.name.containsIgnoreCase ("Lo-Fi") || pr.name.containsIgnoreCase ("Telefono") || pr.name.containsIgnoreCase ("Radio");
        report (nm + ": sobre su pista, sin sorpresas de volumen (" + juce::String (dl, 1) + " LU" + (special ? ", efecto lo-fi" : "") + ")", std::abs (dl) <= (special ? 14.0 : 6.0));

        // AUTO GAIN compensa la curva (como Pro-Q: referencia de ruido rosa, el espectro promedio de la musica)
        const St pk_ = pink (fs, 6.0, 9);
        const St yg = renderLayout (fs, pk_, 512, false, [&] (GauxEqProcessor& p) { loadPreset (p, pr); setP (p, autoGain, 1); }, {}, &lat);
        St ygA (yg.size()); for (size_t i = 0; i + (size_t) lat < yg.size(); ++i) { ygA.l[i] = yg.l[i + (size_t) lat]; ygA.r[i] = yg.r[i + (size_t) lat]; }
        const double dg = lufs (ygA, fs, (size_t) (fs * 3.0)) - lufs (pk_, fs, (size_t) (fs * 3.0));
        report (nm + " + AUTO GAIN: mismo volumen que sin el plugin (" + juce::String (dg, 2) + " LU sobre ruido rosa)", std::abs (dg) <= (special ? 3.0 : 1.0));

        {
            GauxEqProcessor a; loadPreset (a, pr);
            juce::MemoryBlock mb; a.getStateInformation (mb);
            const St y2 = renderLayout (fs, sg.mix, 512, false, [&] (GauxEqProcessor& p) { p.setStateInformation (mb.getData(), (int) mb.getSize()); });
            report (nm + ": guardar el proyecto y abrirlo suena identico (" + juce::String (maxDiffDb (y, y2, 0), 0) + " dB)", maxDiffDb (y, y2, 0) < -120.0);
        }
        {
            // lo que muestra la pantalla = lo que suena (impulso chico: la dinamica y la saturacion no actuan)
            // (sin AUTO GAIN ni saturacion: la pantalla dibuja la curva del EQ; la saturacion agrega densidad y armonicos
            //  segun el nivel, eso no es una curva. Se mide con un impulso chico: la dinamica tampoco actua)
            auto prep = [&] (GauxEqProcessor& p) { loadPreset (p, pr); setP (p, autoGain, 0); for (int b = 0; b < kNumBands; ++b) setP (p, band (b, "drive"), 0); };
            GauxEqProcessor q; prep (q); q.prepareToPlay (fs, 512);
            RunResult rr; rr.efs = q.engineSampleRate.load();
            for (int b = 0; b < kNumBands; ++b) rr.bs[(size_t) b] = q.params.band (b);
            rr.vs = q.params.vintage(); rr.vintage = pr.vintage;
            const float ph = q.apvts.getRawParameterValue (phase)->load();
            rr.linear = ph > 0.5f && ! pr.vintage; rr.natural = std::lround (ph) == 3 && ! pr.vintage;
            rr.brickRes = (std::lround (ph) == 1 || std::lround (ph) == 2) && ! pr.vintage ? fs / linearPhaseLength (std::lround (ph), fs) : 0.0;
            rr.outGain = std::pow (10.0, q.params.output->load() / 20.0);
            const lab::Resp r = lab::measure (fs, prep, 65536, 0.003);
            double worst = 0, at = 0;
            for (double f : lab::logFreqs (25, 16000, 70))
            {
                const double ex = displayCurve (rr, f, 'M');   // (impulso igual en L y R = centro: entran las bandas Stereo y Mid)
                if (ex < 0.06) continue;   // (fondo de un corte)
                const double d = lab::magL (r, f) - dB (ex);
                if (std::abs (d) > std::abs (worst)) { worst = d; at = f; }
            }
            report (nm + ": lo que se ve es lo que suena (curva vs audio, max " + juce::String (worst, 2) + " dB en " + juce::String (at, 0) + " Hz)", std::abs (worst) < 0.5);
        }
        {
            auto prep = [&] (GauxEqProcessor& p) { loadPreset (p, pr); setP (p, autoGain, 0); for (int b = 0; b < kNumBands; ++b) setP (p, band (b, "drive"), 0); };
            const lab::Resp a = lab::measure (fs, prep, 65536, 0.003);
            const lab::Resp b = lab::measure (96000, prep, 131072, 0.003);
            double w = 0, at = 0;
            for (double f : lab::logFreqs (25, 12000, 60))
            {
                if (lab::magL (a, f) < -24.0) continue;   // (fondo de un corte)
                const double d = std::abs (lab::magL (a, f) - lab::magL (b, f)); if (d > w) { w = d; at = f; }
            }
            report (nm + ": la misma curva a 96 kHz (diferencia max " + juce::String (w, 2) + " dB en " + juce::String (at, 0) + " Hz)", w < 0.5);
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 2) ABLETON: cada tamano de buffer de las preferencias de audio da exactamente el mismo resultado
    {
        auto busy = [] (GauxEqProcessor& p)
        {
            applyBand (p, 0, lcut (30, 1)); applyBand (p, 1, bell (65, 3, 1.2)); applyBand (p, 2, bell (300, -3, 2)); applyBand (p, 3, hshelf (9000, 2.5));
            applyBand (p, 4, notch (3150, 8)); applyBand (p, 5, tilt (1000, 1)); applyBand (p, 6, bell (5000, 2, 1, 4));
        };
        const std::vector<lab::Mode> ms {
            { "MIN", [&] (auto& p) { busy (p); } },
            { "NAT", [&] (auto& p) { busy (p); setP (p, phase, 3); } },
            { "LIN", [&] (auto& p) { busy (p); setP (p, phase, 1); } },
            { "HQ 2x", [&] (auto& p) { busy (p); setP (p, quality, 1); } },
            { "VINTAGE", [&] (auto& p) { setP (p, mode, 1); setP (p, vLowBoost, 4); setP (p, vHighBoost, 3); setP (p, vHighAtten, 2); setP (p, vLmBoost, 2); } },
        };
        const St in = sg.mix;
        for (auto& m : ms)
        {
            const St ref = renderLayout (fs, in, 512, false, m.set);
            for (int bs : { 32, 64, 128, 256, 1024, 2048 })
            {
                const St y = renderLayout (fs, in, bs, false, m.set);
                const double d = maxDiffDb (ref, y, 0);
                report (juce::String ("Buffer de ") + juce::String (bs) + " muestras, " + m.name + ": identico al de 512 (" + juce::String (d, 0) + " dB)", d < -110.0);
            }
            // bloques que cambian en cada llamada (Ableton parte el buffer en cada punto de automatizacion)
            {
                auto p = std::make_unique<GauxEqProcessor>(); m.set (*p); p->prepareToPlay (fs, 1024);
                St y (in.size()); Rng rr (5); juce::MidiBuffer midi;
                for (size_t off = 0; off < in.size();)
                {
                    const int n = (int) std::min<size_t> ((size_t) (1 + (int) (rr.u01() * 1023)), in.size() - off);
                    juce::AudioBuffer<float> b (2, n);
                    for (int i = 0; i < n; ++i) { b.setSample (0, i, (float) in.l[off + i]); b.setSample (1, i, (float) in.r[off + i]); }
                    p->processBlock (b, midi);
                    for (int i = 0; i < n; ++i) { y.l[off + i] = b.getSample (0, i); y.r[off + i] = b.getSample (1, i); }
                    off += (size_t) n;
                }
                const double d = maxDiffDb (ref, y, 0);
                report (juce::String ("Bloques de tamano cambiante en cada llamada (1-1024), ") + m.name + ": identico (" + juce::String (d, 0) + " dB)", d < -110.0);
            }
            // loop de 2 compases repetido 3 veces: cada vuelta suena igual (nada se acumula)
            {
                const size_t L = (size_t) (fs * 60.0 / 124.0 * 8);
                St loop (L * 3); for (size_t i = 0; i < loop.size(); ++i) { loop.l[i] = in.l[i % L]; loop.r[i] = in.r[i % L]; }
                int lat = 0;
                const St y = renderLayout (fs, loop, 512, false, m.set, {}, &lat);
                double e = 0, mx = 0;
                for (size_t i = (size_t) lat + L + (size_t) (fs * 0.5); i + L < y.size(); ++i) { e = std::max (e, std::abs (y.l[i] - y.l[i + L])); mx = std::max (mx, std::abs (y.l[i])); }
                report (juce::String ("Loop repetido (3 vueltas), ") + m.name + ": la 2da y la 3ra vuelta son identicas (" + juce::String (dB (e) - dB (mx), 0) + " dB)", dB (e) - dB (mx) < -100.0);
            }
            // parar y volver a darle play: 2 s de silencio (el plugin se duerme) y el arranque es igual que sin dormir
            {
                St st ((size_t) (fs * 5.5));
                for (size_t i = 0; i < (size_t) (fs * 1.5); ++i) { st.l[i] = in.l[i]; st.r[i] = in.r[i]; }
                for (size_t i = (size_t) (fs * 3.5); i < st.size(); ++i) { st.l[i] = in.l[i - (size_t) (fs * 3.5) + (size_t) fs]; st.r[i] = in.r[i - (size_t) (fs * 3.5) + (size_t) fs]; }
                const St a = renderLayout (fs, st, 512, false, m.set);
                GauxEqProcessor::allowSleep = false;
                const St b = renderLayout (fs, st, 512, false, m.set);
                GauxEqProcessor::allowSleep = true;
                const double d = maxDiffDb (b, a, (size_t) (fs * 3.0));
                report (juce::String ("Stop y play (el plugin duerme en el silencio), ") + m.name + ": al volver suena exacto, sin clic (" + juce::String (d, 0) + " dB)", d < -100.0);
            }
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 3) CADA FRECUENCIA DE MUESTREO DE UN PROYECTO: la misma sesion suena igual (volumen y tono)
    {
        auto setSess = [] (GauxEqProcessor& p)
        {
            applyBand (p, 0, lcut (30, 1)); applyBand (p, 1, bell (65, 3, 1.2)); applyBand (p, 2, bell (300, -3, 2)); applyBand (p, 3, hshelf (9000, 2.5));
            applyBand (p, 4, dynb (bell (3000, 0, 1), -30, -4, 5, 120)); applyBand (p, 5, sat (bell (120, 1, 1), 0, 6));
        };
        auto setLin = [&] (GauxEqProcessor& p) { setSess (p); setP (p, band (5, "drive"), 0); };
        const lab::Resp r48 = lab::measure (fs, setLin, 65536, 0.003);
        for (double sr : { 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        {
            const Song s = song (sr, 6.0);
            int res = 0;
            const St y = renderLayout (sr, s.mix, 512, false, setSess, {}, nullptr, &res);
            report ("Proyecto a " + juce::String (sr / 1000.0, 1) + " kHz: finito y sin reinicios", allFinite (y) && res == 0);
            const lab::Resp r = lab::measure (sr, setLin, sr > 100000 ? 262144 : 131072, 0.003);
            double w = 0; for (double f : lab::logFreqs (25, 12000, 60)) w = std::max (w, std::abs (lab::magL (r, f) - lab::magL (r48, f)));
            report ("Proyecto a " + juce::String (sr / 1000.0, 1) + " kHz: la misma curva que a 48 kHz (max " + juce::String (w, 2) + " dB hasta 12 kHz)", w < 0.5);
        }
    }

    //--------------------------------------------------------------------------------------------------------------
    // 4) BYPASS A LO LOCO (alguien que compara A/B apretando el boton muchas veces): nunca se rompe y termina limpio
    for (auto& m : std::vector<lab::Mode> { { "MIN", [] (auto& p) { applyBand (p, 0, bell (100, 6, 1)); } },
                                            { "LIN", [] (auto& p) { applyBand (p, 0, bell (100, 6, 1)); setP (p, phase, 1); } },
                                            { "VINTAGE", [] (auto& p) { setP (p, mode, 1); setP (p, vLowBoost, 6); } } })
    {
        int lat = 0;
        const St y = renderLayout (fs, sg.mix, 256, false, m.set, [&] (GauxEqProcessor& p, double t) { if (t < 3.0) setP (p, bypass, std::fmod (t, 0.1) < 0.05 ? 1.0f : 0.0f); else setP (p, bypass, 1); }, &lat);
        double e = 0; for (size_t i = (size_t) (fs * 4.0); i < y.size(); ++i) e = std::max (e, std::abs (y.l[i] - sg.mix.l[i - (size_t) lat]));
        report (juce::String ("Bypass apretado 60 veces en 3 s, ") + m.name + ": sin romperse y al final sale la senal original exacta (" + juce::String (dB (e), 0) + " dBFS)", allFinite (y) && e < 1e-6);
    }

    //--------------------------------------------------------------------------------------------------------------
    // 5) TRABAJOS DE INGENIERIA MEDIDOS SOBRE LA CANCION
    {
        auto energy = [&] (const std::vector<double>& x, double f1, double f2, size_t a, size_t b)
        {
            Bq h1 (makeHighPass (fs, f1, 0.7071)), h2 (makeHighPass (fs, f1, 0.7071)), l1 (makeLowPass (fs, f2, 0.7071)), l2 (makeLowPass (fs, f2, 0.7071));
            double e = 0; for (size_t i = 0; i < x.size(); ++i) { const double v = l2.p (l1.p (h2.p (h1.p (x[i])))); if (i >= a && i < b) e += v * v; }
            return 10 * std::log10 (std::max (1e-30, e));
        };
        // low cut de 48 dB en 120 Hz: abajo de 50 Hz casi no queda nada, arriba de 250 Hz no se toca
        for (auto& m : std::vector<lab::Mode> { { "MIN", [] (auto& p) { applyBand (p, 0, lcut (120, 3)); } },
                                                { "NAT", [] (auto& p) { applyBand (p, 0, lcut (120, 3)); setP (p, phase, 3); } },
                                                { "LIN", [] (auto& p) { applyBand (p, 0, lcut (120, 3)); setP (p, phase, 1); } } })
        {
            int lat = 0;
            const St y = renderLayout (fs, sg.mix, 512, false, m.set, {}, &lat);
            const size_t a = from, b = sg.mix.size() - 20000;
            std::vector<double> ya (y.size()); for (size_t i = 0; i + (size_t) lat < y.size(); ++i) ya[i] = y.l[i + (size_t) lat];
            const double sub = energy (ya, 25, 50, a, b) - energy (sg.mix.l, 25, 50, a, b), mid = energy (ya, 400, 3000, a, b) - energy (sg.mix.l, 400, 3000, a, b);
            report (juce::String ("Low cut 48 dB en 120 Hz (") + m.name + "): sub de 25-50 Hz baja " + juce::String (-sub, 0) + " dB y los medios cambian " + juce::String (mid, 2) + " dB",
                    sub < -30.0 && std::abs (mid) < 0.3);
        }
        // de-esser (preset "De-Esser Dinamico"): baja las eses y deja el cuerpo de la voz
        {
            sess::PresetRef de {}; for (auto& pr : allPresets()) if (pr.name == "De-Esser Dinamico") de = pr;
            int lat = 0;
            const St y = renderLayout (fs, sg.mix, 512, false, [&] (GauxEqProcessor& p) { loadPreset (p, de); }, {}, &lat);
            std::vector<double> ya (y.size()); for (size_t i = 0; i + (size_t) lat < y.size(); ++i) ya[i] = y.l[i + (size_t) lat];
            const double beat = 60.0 / 124.0;
            const size_t s0 = (size_t) (fs * (beat * 8 + beat * 3.05)), s1 = (size_t) (fs * (beat * 8 + beat * 3.35));
            const double sib = energy (ya, 5500, 9500, s0, s1) - energy (sg.mix.l, 5500, 9500, s0, s1);
            const size_t b0 = (size_t) (fs * (beat * 8 + beat * 1.0)), b1 = (size_t) (fs * (beat * 8 + beat * 2.5));
            const double body = energy (ya, 500, 1500, b0, b1) - energy (sg.mix.l, 500, 1500, b0, b1);
            report ("De-esser: las eses bajan " + juce::String (-sib, 1) + " dB mientras suenan", sib < -2.0);
            report ("De-esser: el cuerpo de la voz (500-1500 Hz) casi no cambia (" + juce::String (body, 2) + " dB)", std::abs (body) < 1.0);
        }
        // graves en mono (preset "Graves en Mono"): lo estereo debajo de 100 Hz desaparece, el centro queda igual
        {
            sess::PresetRef gm {}; for (auto& pr : allPresets()) if (pr.name == "Graves en Mono") gm = pr;
            St wide (sg.mix.size());
            for (size_t i = 0; i < wide.size(); ++i) { const double t = (double) i / fs, sv = 0.2 * std::sin (2 * kPi * 50 * t); wide.l[i] = sg.mix.l[i] + sv; wide.r[i] = sg.mix.r[i] - sv; }
            int lat = 0;
            const St y = renderLayout (fs, wide, 512, false, [&] (GauxEqProcessor& p) { loadPreset (p, gm); }, {}, &lat);
            std::vector<double> sIn (wide.size()), sOut (wide.size()), mIn (wide.size()), mOut (wide.size());
            for (size_t i = 0; i + (size_t) lat < y.size(); ++i)
            { sIn[i] = 0.5 * (wide.l[i] - wide.r[i]); sOut[i] = 0.5 * (y.l[i + (size_t) lat] - y.r[i + (size_t) lat]); mIn[i] = 0.5 * (wide.l[i] + wide.r[i]); mOut[i] = 0.5 * (y.l[i + (size_t) lat] + y.r[i + (size_t) lat]); }
            const size_t a = from, b = wide.size() - 20000;
            const double sd = energy (sOut, 35, 70, a, b) - energy (sIn, 35, 70, a, b), md = energy (mOut, 35, 70, a, b) - energy (mIn, 35, 70, a, b);
            report ("Graves en mono: lo estereo en 50 Hz baja " + juce::String (-sd, 0) + " dB", sd < -15.0);
            report ("Graves en mono: el bajo del centro queda igual (" + juce::String (md, 2) + " dB)", std::abs (md) < 1.0);
        }
        // kick contra bajo: banda dinamica EXT en 55 Hz del bajo, con el bombo por sidechain
        {
            GauxEqProcessor p; p.enableAllBuses();
            applyBand (p, 0, dynb (bell (55, 0, 1.5), -26, -8, 1, 80, true));
            p.prepareToPlay (fs, 512);
            juce::AudioBuffer<float> buf (4, 512); juce::MidiBuffer midi;
            St y (sg.mix.size());
            for (size_t off = 0; off + 512 <= sg.mix.size(); off += 512)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const double t = (double) (off + (size_t) i) / fs;
                    const double bass = 0.3 * std::sin (2 * kPi * 55 * t);
                    buf.setSample (0, i, (float) bass); buf.setSample (1, i, (float) bass);
                    buf.setSample (2, i, (float) sg.kick.l[off + (size_t) i]); buf.setSample (3, i, (float) sg.kick.l[off + (size_t) i]);
                }
                p.processBlock (buf, midi);
                for (int i = 0; i < 512; ++i) { y.l[off + (size_t) i] = buf.getSample (0, i); y.r[off + (size_t) i] = buf.getSample (1, i); }
            }
            const double beat = 60.0 / 124.0;
            double onKick = 0, between = 0; int nk = 0;
            for (int b = 4; b < 10; ++b)
            {
                const size_t k0 = (size_t) (fs * (b * beat + 0.01)), k1 = (size_t) (fs * (b * beat + 0.06));
                const size_t m0 = (size_t) (fs * (b * beat + 0.35)), m1 = (size_t) (fs * (b * beat + 0.45));
                double ek = 0, em = 0; for (size_t i = k0; i < k1; ++i) ek = std::max (ek, std::abs (y.l[i])); for (size_t i = m0; i < m1; ++i) em = std::max (em, std::abs (y.l[i]));
                onKick += dB (ek / 0.3); between += dB (em / 0.3); ++nk;
            }
            onKick /= nk; between /= nk;
            report ("Kick vs bajo (banda EXT en 55 Hz): con cada golpe el bajo baja " + juce::String (-onKick, 1) + " dB", onKick < -4.0);
            report ("Kick vs bajo: entre golpes el bajo vuelve entero (" + juce::String (between, 2) + " dB)", between > -1.0);
        }
        // notch para una resonancia: la quita y no toca lo de al lado
        {
            St in (sg.mix.size());
            for (size_t i = 0; i < in.size(); ++i) { const double t = (double) i / fs; in.l[i] = in.r[i] = 0.1 * std::sin (2 * kPi * 3150 * t) + 0.1 * std::sin (2 * kPi * 1000 * t) + 0.1 * std::sin (2 * kPi * 8000 * t); }
            const St y = renderLayout (fs, in, 512, false, [] (GauxEqProcessor& p) { applyBand (p, 0, notch (3150, 8)); });
            const size_t st = (size_t) (fs * 2), len = (size_t) (fs * 2);
            const double res = dB (lab::toneAmp (y.l, st, len, fs, 3150) / 0.1), a1 = dB (lab::toneAmp (y.l, st, len, fs, 1000) / 0.1), a8 = dB (lab::toneAmp (y.l, st, len, fs, 8000) / 0.1);
            report ("Notch Q 8 sobre una resonancia en 3150 Hz: la baja " + juce::String (-res, 0) + " dB; 1 kHz cambia " + juce::String (a1, 2) + " y 8 kHz " + juce::String (a8, 2) + " dB",
                    res < -40.0 && std::abs (a1) < 0.3 && std::abs (a8) < 0.3);
        }
        // tilt +3: graves abajo, agudos arriba, simetrico alrededor del pivote
        {
            const lab::Resp r = lab::measure (fs, [] (GauxEqProcessor& p) { applyBand (p, 0, tilt (1000, 3)); });
            const double lo = lab::magL (r, 50), hi = lab::magL (r, 18000), mid = lab::magL (r, 1000);
            // (GAIN = diferencia total entre graves y agudos: +3 dB es -1.5 abajo y +1.5 arriba, 0 dB en el pivote)
            report ("Tilt +3 dB en 1 kHz: " + juce::String (lo, 2) + " dB en 50 Hz, " + juce::String (mid, 2) + " en 1 kHz y " + juce::String (hi, 2) + " en 18 kHz (balanza simetrica)",
                    std::abs (lo + 1.5) < 0.3 && std::abs (hi - 1.5) < 0.3 && std::abs (mid) < 0.1);
        }
        // SC general: con el bombo sonando la curva se aplana (el realce de 100 Hz desaparece en cada golpe)
        {
            GauxEqProcessor p; p.enableAllBuses();
            applyBand (p, 0, bell (100, 6, 1)); setP (p, scOn, 1); setP (p, scThr, -30); setP (p, scDuck, 100);
            p.prepareToPlay (fs, 512);
            juce::AudioBuffer<float> buf (4, 512); juce::MidiBuffer midi;
            double minG = 0;
            for (size_t off = 0; off + 512 <= sg.mix.size(); off += 512)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const double t = (double) (off + (size_t) i) / fs;
                    const double v = 0.2 * std::sin (2 * kPi * 100 * t);
                    buf.setSample (0, i, (float) v); buf.setSample (1, i, (float) v);
                    buf.setSample (2, i, (float) sg.kick.l[off + (size_t) i]); buf.setSample (3, i, (float) sg.kick.l[off + (size_t) i]);
                }
                p.processBlock (buf, midi);
                double pk = 0; for (int i = 0; i < 512; ++i) pk = std::max (pk, (double) std::abs (buf.getSample (0, i)));
                if (off > (size_t) (fs * 2)) minG = std::min (minG, dB (pk / 0.2));
            }
            report ("SC general al 100 %: con cada golpe del bombo el realce de +6 dB se aplana (minimo " + juce::String (minG, 1) + " dB)", minG < 1.0);
        }
    }
}
