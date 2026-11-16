// (Incluido desde MixLab.cpp, despues de SessionTests.inc)
//==============================================================================
// ESTRES MAXIMO v0.26: lo que hacen los validadores de la industria (pluginval, el validador de Steinberg,
// auval) llevado mas lejos, y lo que hacen los usuarios y los DAW de verdad:
//  - 200 configuraciones totalmente al azar (todas las perillas del plugin): suena finito, en silencio no
//    inventa ruido, se guarda y se abre identico, es determinista y el bypass es exacto
//  - cada parametro: el texto que muestra el DAW se puede escribir de vuelta y da lo mismo
//  - senales hostiles (DC, clip a 0 dBFS, Nyquist, +12 dBFS, NaN/Inf, denormales) en cada modo
//  - proyectos danados: estados truncados, corruptos o con valores imposibles
//  - DAWs raros: bloques de 0 muestras, bloques mas grandes de lo anunciado, cambio de frecuencia en
//    plena reproduccion, mono <-> estereo, sidechain conectado, hilos que leen el estado mientras suena
//  - CPU: el peor caso de cada modo en tiempo real
//==============================================================================

namespace est
{
using namespace ParamID;
static Band est_sat (int type) { return sat (bell (58, 4, 1.1), type, 20); }

// Todas las perillas del plugin, como las ve el DAW (normalizadas 0..1)
using State = std::vector<float>;

static State randomState (unsigned seed)
{
    GauxEqProcessor tmp;
    std::mt19937 g (seed * 7919u + 13u);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    State s;
    for (auto* prm : tmp.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm);
        const auto id = rp != nullptr ? rp->getParameterID() : juce::String();
        float v = u (g);
        if (id == bypass) v = 0.0f;                            // (el bypass se prueba aparte)
        else if (id.endsWith ("_on")) v = u (g) < 0.55f ? 1.0f : 0.0f;  // ~la mitad de las bandas encendidas
        s.push_back (v);
    }
    return s;
}

static void applyState (GauxEqProcessor& p, const State& s)
{
    auto& ps = p.getParameters();
    for (int i = 0; i < ps.size() && i < (int) s.size(); ++i) ps[i]->setValueNotifyingHost (s[(size_t) i]);
}

static float valueOf (GauxEqProcessor& p, const juce::String& id) { return p.apvts.getParameter (id)->getValue(); }

// Procesa con bloques de tamano variable (como Ableton al automatizar o FL Studio)
static St run (GauxEqProcessor& p, const St& in, double fs, int maxBlock, unsigned seed, bool prepare = true, int* lat = nullptr)
{
    if (prepare) p.prepareToPlay (fs, maxBlock);
    std::mt19937 g (seed);
    juce::AudioBuffer<float> buf (2, maxBlock); juce::MidiBuffer midi;
    St out (in.size());
    size_t off = 0;
    while (off < in.size())
    {
        const int n = (int) std::min<size_t> ((size_t) (1 + g() % (unsigned) maxBlock), in.size() - off);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i) { buf.setSample (0, i, (float) in.l[off + (size_t) i]); buf.setSample (1, i, (float) in.r[off + (size_t) i]); }
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i) { out.l[off + (size_t) i] = buf.getSample (0, i); out.r[off + (size_t) i] = buf.getSample (1, i); }
        off += (size_t) n;
    }
    if (lat) *lat = p.getLatencySamples();
    return out;
}

static double maxAbs (const St& s, size_t from = 0)
{
    double m = 0; for (size_t i = from; i < s.size(); ++i) m = std::max ({ m, std::abs (s.l[i]), std::abs (s.r[i]) }); return m;
}

static bool hasDenormals (const St& s)
{
    for (size_t i = 0; i < s.size(); ++i)
        for (double v : { s.l[i], s.r[i] }) { const float f = (float) v; if (f != 0.0f && std::abs (f) < std::numeric_limits<float>::min()) return true; }
    return false;
}

static juce::String describe (GauxEqProcessor& p)
{
    int on = 0; for (int b = 0; b < kNumBands; ++b) on += valueOf (p, band (b, "on")) > 0.5f;
    const auto* m = p.apvts.getRawParameterValue (mode); const auto* ph = p.apvts.getRawParameterValue (phase); const auto* q = p.apvts.getRawParameterValue (quality);
    return juce::String (m->load() > 0.5f ? "VINTAGE" : "PARAM") + " fase " + juce::String ((int) ph->load()) + " HQ " + juce::String ((int) q->load())
         + ", " + juce::String (on) + " bandas";
}

//------------------------------------------------------------------------------
// 1) 200 configuraciones al azar x 5 comprobaciones
static void fuzz()
{
    std::cout << "\n-- 200 configuraciones al azar (todas las perillas) --" << std::endl;
    const double fs = 48000;
    const St music = noiseMusic (fs, 0.6, 77, false);
    const St silence (music.size());
    for (unsigned seed = 1; seed <= 200; ++seed)
    {
        const State s = randomState (seed);
        const int maxBlock = std::array<int, 6> { 32, 64, 128, 441, 512, 1024 } [seed % 6];

        GauxEqProcessor a; applyState (a, s);
        const auto name = "#" + juce::String (seed) + " (" + describe (a) + ", bloques hasta " + juce::String (maxBlock) + ")";
        int lat = 0;
        const St ya = run (a, music, fs, maxBlock, seed, true, &lat);
        const double pk = maxAbs (ya);
        report ("Azar " + name + ": con musica sale finito, sin denormales y acotado (pico " + juce::String (dB (pk), 1) + " dBFS)",
                allFinite (ya) && ! hasDenormals (ya) && pk < 1000.0);

        // en silencio no inventa nada (tras 0.3 s de colas y sin el sidechain)
        GauxEqProcessor z; applyState (z, s);
        const St yz = run (z, silence, fs, maxBlock, seed + 1000);
        const double ns = maxAbs (yz, (size_t) (0.3 * fs));
        report ("Azar " + name + ": silencio entra, silencio sale (" + juce::String (dB (ns), 0) + " dBFS)", allFinite (yz) && ns < 1e-5);

        // guardar y abrir el proyecto: cada perilla vuelve exacta
        juce::MemoryBlock mb; a.getStateInformation (mb);
        GauxEqProcessor b; b.setStateInformation (mb.getData(), (int) mb.getSize());
        double worst = 0; juce::String which;
        auto& pa = a.getParameters(); auto& pb = b.getParameters();
        for (int i = 0; i < pa.size(); ++i)
        {
            const double d = std::abs (pa[i]->getValue() - pb[i]->getValue());
            if (d > worst) { worst = d; which = pa[i]->getName (32); }
        }
        report ("Azar " + name + ": guardar y abrir deja todo igual" + (worst > 1e-5 ? " (difiere " + which + ")" : juce::String()), worst <= 1e-5);

        // determinista: otra instancia con el estado guardado suena identica (misma secuencia de bloques)
        const St yb = run (b, music, fs, maxBlock, seed);
        double dif = 0; for (size_t i = 0; i < music.size(); ++i) dif = std::max ({ dif, std::abs (ya.l[i] - yb.l[i]), std::abs (ya.r[i] - yb.r[i]) });
        report ("Azar " + name + ": el proyecto abierto suena identico (" + (dif == 0 ? juce::String ("exacto") : juce::String (dB (dif) - dB (pk), 0) + " dB") + ")",
                dif <= 1e-5 * std::max (1.0, pk));   // (-100 dB: los valores se guardan en texto con decimales finitos)

        // bypass exacto: sale la entrada, con la latencia que reporta el plugin
        GauxEqProcessor c; applyState (c, s); c.apvts.getParameter (bypass)->setValueNotifyingHost (1.0f);
        int latC = 0;
        const St yc = run (c, music, fs, maxBlock, seed, true, &latC);
        double e = 0;
        for (size_t i = (size_t) latC + 64; i < music.size(); ++i)
            e = std::max ({ e, std::abs (yc.l[i] - music.l[i - (size_t) latC]), std::abs (yc.r[i] - music.r[i - (size_t) latC]) });
        report ("Azar " + name + ": bypass = la entrada exacta con " + juce::String (latC) + " muestras de latencia", e < 1e-6 && latC == lat);
    }
}

//------------------------------------------------------------------------------
// 2) Cada parametro: el texto que muestra el DAW, escrito de vuelta, da el mismo valor
static void textRoundTrip()
{
    std::cout << "\n-- Cada parametro: texto del DAW <-> valor --" << std::endl;
    GauxEqProcessor p;
    for (auto* prm : p.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm);
        if (rp == nullptr) continue;
        bool ok = true; juce::String bad;
        for (float v : { 0.0f, 0.13f, 0.37f, 0.5f, 0.71f, 0.94f, 1.0f })
        {
            const auto t = rp->getText (v, 64);
            const float back = rp->getValueForText (t);
            const auto t2 = rp->getText (back, 64);
            if (t != t2 || ! std::isfinite (back) || back < 0.0f || back > 1.0f || t.isEmpty()) { ok = false; bad = "'" + t + "' -> '" + t2 + "'"; break; }
        }
        report ("Parametro " + rp->getParameterID() + " (" + rp->getName (40) + "): el texto se puede escribir de vuelta" + (ok ? juce::String() : " " + bad), ok);
    }
}

//------------------------------------------------------------------------------
// 3) Senales hostiles en cada modo
static void hostileSignals()
{
    std::cout << "\n-- Senales hostiles en cada modo --" << std::endl;
    const double fs = 48000; const size_t n = (size_t) (fs * 1.2);
    struct Sig { const char* name; std::function<double (size_t, int)> f; bool poisoned; };
    const std::vector<Sig> sigs {
        { "DC +0.5", [] (size_t, int) { return 0.5; }, false },
        { "cuadrada a 0 dBFS (clip)", [&] (size_t i, int) { return (i / 40) % 2 ? 1.0 : -1.0; }, false },
        { "Nyquist (+1,-1,+1...)", [] (size_t i, int) { return i % 2 ? 0.9 : -0.9; }, false },
        { "seno a +12 dBFS", [&] (size_t i, int c) { return 4.0 * std::sin (2 * kPi * 100 * i / fs + c); }, false },
        { "un solo impulso", [&] (size_t i, int) { return i == (size_t) (0.1 * fs) ? 1.0 : 0.0; }, false },
        { "ruido a nivel denormal", [] (size_t i, int c) { return 1e-39 * ((i * 2654435761u + (unsigned) c) % 3 - 1.0); }, false },
        { "rafaga de NaN", [&] (size_t i, int c) { return i > (size_t) (0.2 * fs) && i < (size_t) (0.25 * fs) ? std::nan ("") : 0.3 * std::sin (2 * kPi * 440 * i / fs + c); }, true },
        { "rafaga de +Inf", [&] (size_t i, int c) { return i > (size_t) (0.2 * fs) && i < (size_t) (0.21 * fs) ? HUGE_VAL : 0.3 * std::sin (2 * kPi * 440 * i / fs + c); }, true },
        { "saltos de -inf a 0 dBFS", [&] (size_t i, int) { return (i / 4800) % 2 ? std::sin (2 * kPi * 60 * i / fs) : 0.0; }, false },
    };
    auto busy = [] (GauxEqProcessor& p)
    {
        applyBand (p, 0, lcut (30, 3)); applyBand (p, 1, bell (100, 6, 1)); applyBand (p, 2, sat (bell (800, 3, 1), 4, 18));
        applyBand (p, 3, dynb (bell (3000, 0, 1), -30, -9, 1, 50)); applyBand (p, 4, hshelf (10000, 4)); applyBand (p, 5, hcut (19000, 8));
        applyBand (p, 6, notch (50, 10)); applyBand (p, 7, bpass (1000, 0.5)); applyBand (p, 8, apass (200, 2));
    };
    struct Cfg { const char* name; std::function<void (GauxEqProcessor&)> set; };
    const std::vector<Cfg> cfgs {
        { "MIN", [&] (auto& p) { busy (p); } },
        { "MIN 4x", [&] (auto& p) { busy (p); setP (p, quality, 2); } },
        { "NAT", [&] (auto& p) { busy (p); setP (p, phase, 3); } },
        { "LIN", [&] (auto& p) { busy (p); setP (p, phase, 1); } },
        { "LIN HQ", [&] (auto& p) { busy (p); setP (p, phase, 2); } },
        { "ESPECTRAL", [&] (auto& p) { busy (p); applyBand (p, 9, spectral (dynb (bell (5000, 0, 0.8), -40, -12))); } },
        { "AUTO GAIN", [&] (auto& p) { busy (p); setP (p, autoGain, 1); } },
        { "VINTAGE", [&] (auto& p) { setP (p, mode, 1); setP (p, vLowBoost, 10); setP (p, vHighBoost, 10); setP (p, vTube, 100); setP (p, vIron, 100); } },
    };
    for (auto& c : cfgs)
        for (auto& sg : sigs)
        {
            St in (n); for (size_t i = 0; i < n; ++i) { in.l[i] = sg.f (i, 0); in.r[i] = sg.f (i, 1); }
            GauxEqProcessor p; c.set (p);
            const St y = run (p, in, fs, 512, 5);
            bool ok;
            juce::String extra;
            if (sg.poisoned)
            {
                // tras el veneno, en menos de 0.3 s vuelve a sonar normal y nunca saca NaN/Inf
                ok = allFinite (y);
                const double tail = maxAbs (y, (size_t) (0.6 * fs));
                ok = ok && tail > 0.05 && tail < 5.0;
                extra = " (vuelve a sonar: pico " + juce::String (dB (tail), 1) + " dBFS)";
            }
            else
            {
                const double pk = maxAbs (y);
                ok = allFinite (y) && ! hasDenormals (y) && pk < 200.0;
                extra = " (pico " + juce::String (dB (pk), 1) + " dBFS)";
            }
            report (juce::String ("Senal hostil, ") + c.name + ", " + sg.name + ": finito y se recupera" + extra, ok);
        }
}

//------------------------------------------------------------------------------
// 4) Proyectos danados
static void brokenStates()
{
    std::cout << "\n-- Proyectos danados (estados truncados, corruptos, valores imposibles) --" << std::endl;
    const double fs = 48000; const St music = noiseMusic (fs, 0.3, 3, false);
    auto sane = [] (GauxEqProcessor& p)
    {
        for (auto* prm : p.getParameters()) { const float v = prm->getValue(); if (! std::isfinite (v) || v < 0.0f || v > 1.0f) return false; }
        return true;
    };
    std::mt19937 g (2026);
    for (int k = 0; k < 30; ++k)
    {
        GauxEqProcessor a; applyState (a, randomState (500 + (unsigned) k));
        juce::MemoryBlock mb; a.getStateInformation (mb);
        juce::MemoryBlock bad (mb);
        juce::String kind;
        switch (k % 5)
        {
            case 0: bad.setSize (1 + g() % std::max<size_t> (1, mb.getSize() - 1)); kind = "truncado"; break;
            case 1: for (int j = 0; j < 40; ++j) static_cast<char*> (bad.getData())[g() % bad.getSize()] ^= (char) (1 << (g() % 8)); kind = "con bytes cambiados"; break;
            case 2: { juce::MemoryBlock r (64 + g() % 4000); for (size_t j = 0; j < r.getSize(); ++j) static_cast<char*> (r.getData())[j] = (char) g(); bad = r; kind = "basura al azar"; break; }
            case 3: bad.setSize (0); kind = "vacio"; break;
            default:
            {
                // XML valido pero con valores imposibles
                auto xml = a.apvts.copyState().createXml();
                int j = 0;
                for (auto* e : xml->getChildIterator())
                    if (e->hasAttribute ("value")) e->setAttribute ("value", std::array<const char*, 5> { "1e30", "-1e30", "nan", "inf", "abc" } [(size_t) (j++ % 5)]);
                juce::MemoryBlock m2; juce::AudioProcessor::copyXmlToBinary (*xml, m2); bad = m2; kind = "con valores imposibles (1e30, nan, inf, texto)";
            }
        }
        GauxEqProcessor b; b.setStateInformation (bad.getData(), (int) bad.getSize());
        const St y = run (b, music, fs, 256, 9);
        report ("Proyecto danado #" + juce::String (k + 1) + " " + kind + ": abre sin colgarse, perillas en rango y suena finito", sane (b) && allFinite (y));
    }
}

//------------------------------------------------------------------------------
// 5) Lo que hacen los DAW raros
static void hostQuirks()
{
    std::cout << "\n-- DAWs raros --" << std::endl;
    const double fs = 48000; const St music = noiseMusic (fs, 1.0, 21, false);
    for (unsigned seed = 1; seed <= 12; ++seed)
    {
        const State s = randomState (900 + seed);
        const juce::String tag = "estado al azar #" + juce::String (seed);

        { // bloques de 0 muestras entre bloques normales
            GauxEqProcessor p; applyState (p, s); p.prepareToPlay (fs, 256);
            juce::AudioBuffer<float> buf (2, 256), empty (2, 0); juce::MidiBuffer midi; bool fin = true;
            for (int k = 0; k < 40; ++k)
            {
                p.processBlock (empty, midi);
                for (int i = 0; i < 256; ++i) { buf.setSample (0, i, (float) music.l[(size_t) (k * 256 + i)]); buf.setSample (1, i, (float) music.r[(size_t) (k * 256 + i)]); }
                p.processBlock (buf, midi);
                for (int i = 0; i < 256; ++i) fin = fin && std::isfinite (buf.getSample (0, i)) && std::isfinite (buf.getSample (1, i));
            }
            report ("DAW raro, " + tag + ": bloques de 0 muestras entre medio", fin);
        }
        { // bloques MAS grandes de lo anunciado en prepareToPlay (pasa en FL Studio y al exportar)
            GauxEqProcessor p; applyState (p, s); p.prepareToPlay (fs, 128);
            St y = run (p, music, fs, 4096, seed, false);
            report ("DAW raro, " + tag + ": bloques de hasta 4096 cuando anuncio 128", allFinite (y));
        }
        { // cambio de frecuencia de muestreo en plena reproduccion, sin releaseResources
            GauxEqProcessor p; applyState (p, s);
            bool fin = true;
            for (double sr : { 44100.0, 96000.0, 48000.0, 192000.0, 44100.0 })
            {
                const St in = noiseMusic (sr, 0.25, seed, false);
                const St y = run (p, in, sr, 512, seed);
                fin = fin && allFinite (y);
            }
            report ("DAW raro, " + tag + ": cambia 44.1 > 96 > 48 > 192 > 44.1 kHz sin parar", fin);
        }
        { // mono <-> estereo entre reproducciones
            GauxEqProcessor p; applyState (p, s);
            bool fin = true;
            for (bool mono : { false, true, false, true })
            {
                juce::AudioProcessor::BusesLayout lay;
                lay.inputBuses.add (mono ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
                lay.outputBuses.add (mono ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
                lay.inputBuses.add (juce::AudioChannelSet::disabled());
                fin = fin && p.setBusesLayout (lay);
                p.prepareToPlay (fs, 512);
                const int nch = mono ? 1 : 2;
                juce::AudioBuffer<float> buf (nch, 512); juce::MidiBuffer midi;
                for (int k = 0; k < 30; ++k)
                {
                    for (int c = 0; c < nch; ++c) for (int i = 0; i < 512; ++i) buf.setSample (c, i, (float) music.l[(size_t) (k * 512 + i)]);
                    p.processBlock (buf, midi);
                    for (int c = 0; c < nch; ++c) for (int i = 0; i < 512; ++i) fin = fin && std::isfinite (buf.getSample (c, i));
                }
                p.releaseResources();
            }
            report ("DAW raro, " + tag + ": pista mono > estereo > mono > estereo", fin);
        }
        { // sidechain conectado (4 canales) con bombo fuerte
            GauxEqProcessor p; applyState (p, s);
            juce::AudioProcessor::BusesLayout lay;
            lay.inputBuses.add (juce::AudioChannelSet::stereo()); lay.outputBuses.add (juce::AudioChannelSet::stereo());
            lay.inputBuses.add (juce::AudioChannelSet::stereo());
            bool fin = p.setBusesLayout (lay);
            p.prepareToPlay (fs, 512);
            juce::AudioBuffer<float> buf (4, 512); juce::MidiBuffer midi;
            for (int k = 0; k < 60; ++k)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const size_t j = (size_t) (k * 512 + i) % music.size();
                    buf.setSample (0, i, (float) music.l[j]); buf.setSample (1, i, (float) music.r[j]);
                    const double t = (double) (k * 512 + i) / fs, ph = std::fmod (t, 0.5);
                    const float kick = (float) (ph < 0.1 ? 0.95 * std::sin (2 * kPi * 55 * ph) : 0.0);
                    buf.setSample (2, i, kick); buf.setSample (3, i, kick);
                }
                p.processBlock (buf, midi);
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) fin = fin && std::isfinite (buf.getSample (c, i));
            }
            report ("DAW raro, " + tag + ": sidechain conectado con bombo a -0.5 dBFS", fin);
        }
        { // el DAW guarda el proyecto (otro hilo) y mueve perillas mientras suena
            GauxEqProcessor p; applyState (p, s); p.prepareToPlay (fs, 256);
            std::atomic<bool> stop { false };
            std::atomic<int> saves { 0 };
            std::thread ui ([&]
            {
                std::mt19937 gg (seed);
                while (! stop.load())
                {
                    juce::MemoryBlock mb; p.getStateInformation (mb); ++saves;
                    auto& ps = p.getParameters();
                    for (int k = 0; k < 8; ++k)
                    {
                        auto* prm = ps[(int) (gg() % (unsigned) ps.size())];
                        if (dynamic_cast<juce::RangedAudioParameter*> (prm)->getParameterID() != bypass) prm->setValueNotifyingHost ((float) (gg() % 1000) / 999.0f);
                    }
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
                }
            });
            bool fin = true;
            juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer midi;
            for (int k = 0; k < 180; ++k)
            {
                for (int i = 0; i < 256; ++i) { const size_t j = (size_t) (k * 256 + i) % music.size(); buf.setSample (0, i, (float) music.l[j]); buf.setSample (1, i, (float) music.r[j]); }
                p.processBlock (buf, midi);
                for (int i = 0; i < 256; ++i) fin = fin && std::isfinite (buf.getSample (0, i)) && std::isfinite (buf.getSample (1, i));
            }
            stop = true; ui.join();
            report ("DAW raro, " + tag + ": guarda el proyecto y mueve perillas desde otro hilo mientras suena (" + juce::String (saves.load()) + " guardados)", fin && saves.load() > 0);
        }
    }
}

//------------------------------------------------------------------------------
// 6) CPU: el peor caso de cada modo, en tiempo real
static void cpuWorst()
{
    std::cout << "\n-- CPU: peor caso de cada modo --" << std::endl;
    const double fs = 48000; const St music = noiseMusic (fs, 3.0, 31, false);
    auto all = [] (GauxEqProcessor& p, bool dyn)
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            Band x = bell (40.0 * std::pow (400.0, b / 23.0), (b % 2 ? 3 : -3), 2);
            if (b == 0) x = lcut (30, 8);
            if (b == kNumBands - 1) x = hcut (18000, 8);
            if (dyn && b > 0 && b < kNumBands - 1) x = dynb (x, -30, -6, 5, 80);
            if (b % 6 == 3) x = sat (x, b % 5, 8);
            applyBand (p, b, x);
        }
    };
    struct Cfg { const char* name; std::function<void (GauxEqProcessor&)> set; double limit; };
    const std::vector<Cfg> cfgs {
        { "24 bandas MIN", [&] (auto& p) { all (p, false); }, 60 },
        { "24 bandas dinamicas MIN", [&] (auto& p) { all (p, true); }, 80 },
        { "24 bandas dinamicas MIN 4x", [&] (auto& p) { all (p, true); setP (p, quality, 2); }, 100 },
        { "24 bandas NAT", [&] (auto& p) { all (p, false); setP (p, phase, 3); }, 80 },
        { "24 bandas LIN", [&] (auto& p) { all (p, false); setP (p, phase, 1); }, 80 },
        { "24 bandas dinamicas LIN HQ", [&] (auto& p) { all (p, true); setP (p, phase, 2); }, 100 },
        { "VINTAGE completo", [&] (auto& p) { setP (p, mode, 1); setP (p, vLowBoost, 5); setP (p, vHighBoost, 5); setP (p, vTube, 60); setP (p, vIron, 60); setP (p, vLmBoost, 3); }, 40 },
    };
    for (auto& c : cfgs)
        for (int block : { 32, 128, 1024 })
        {
            GauxEqProcessor p; c.set (p);
            run (p, noiseMusic (fs, 0.3, 1, false), fs, block, 1);   // calentar
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            p.prepareToPlay (fs, block);
            juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi;
            for (size_t off = 0; off + (size_t) block <= music.size(); off += (size_t) block)
            {
                for (int i = 0; i < block; ++i) { buf.setSample (0, i, (float) music.l[off + (size_t) i]); buf.setSample (1, i, (float) music.r[off + (size_t) i]); }
                p.processBlock (buf, midi);
            }
            const double pct = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0 / 3.0 * 100.0;
            // (limite: % de UN nucleo de esta maquina de pruebas, que es mas lenta que un Mac M)
            report (juce::String ("CPU ") + c.name + ", bloque " + juce::String (block) + ": " + juce::String (pct, 1) + " % de un nucleo", pct < c.limit);
        }
}


//------------------------------------------------------------------------------
// 7) Aviso de version nueva (beta -> final): logica, archivo danado, sin internet, omitir version
static void updateTests()
{
    std::cout << "\n-- Aviso de version nueva (beta -> final) --" << std::endl;
    struct C { const char* a; const char* b; int r; };
    for (auto c : std::vector<C> { { "1.0.0", "0.26.0", 1 }, { "0.26.0", "0.26.0", 0 }, { "0.26.1", "0.26.0", 1 }, { "0.27", "0.26.9", 1 },
                                   { "1.0", "1.0.0", 0 }, { "0.26.0-beta", "0.26.0", -1 }, { "0.26.0 beta", "0.26.0", -1 }, { "1.0.0", "1.0.0-rc1", 1 },
                                   { "0.9.0", "0.26.0", -1 }, { "2.0.0", "10.0.0", -1 }, { "", "0.26.0", -1 }, { "0.26.0", "", 1 } })
        report (juce::String ("Version '") + c.a + "' frente a '" + c.b + "': " + (c.r > 0 ? "mas nueva" : c.r < 0 ? "mas vieja" : "igual"),
                UpdateLogic::compare (c.a, c.b) == c.r);

    const juce::String good = R"J({"version":"1.0.0","channel":"final","title":"Primera version final","url":"https://aleksandergaux.com","url_mac":"https://example.com/mac.zip","url_win":"https://example.com/win.zip"})J";
    for (auto [json, wantValid, label] : std::vector<std::tuple<juce::String, bool, const char*>> {
            { good, true, "anuncio correcto" },
            { "", false, "archivo vacio (sin internet)" },
            { "<html>404 Not Found</html>", false, "pagina de error 404" },
            { R"J({"version":"1.0.0"})J", false, "sin link de descarga" },
            { R"J({"version":"1.0.0","url":"http://inseguro.com"})J", false, "link sin https" },
            { R"J({"version":"1.0.0","url":"javascript:alert(1)"})J", false, "link peligroso" },
            { R"J({"version":"<script>","url":"https://a.com"})J", false, "version con texto raro" },
            { R"J([1,2,3])J", false, "JSON que no es un objeto" },
            { juce::String::repeatedString ("{", 5000), false, "JSON danado y enorme" } })
    {
        const auto u = UpdateLogic::parse (json);
        report (juce::String ("Anuncio de version: ") + label + (wantValid ? " se acepta" : " se ignora sin fallar"), u.valid() == wantValid);
    }
    const auto u = UpdateLogic::parse (good);
    report ("Anuncio: avisa de 1.0.0 a quien tiene la beta 0.26.0", UpdateLogic::shouldNotify (u, "0.26.0", ""));
    report ("Anuncio: no avisa si ya tiene 1.0.0", ! UpdateLogic::shouldNotify (u, "1.0.0", ""));
    report ("Anuncio: no avisa si el usuario omitio la 1.0.0", ! UpdateLogic::shouldNotify (u, "0.26.0", "1.0.0"));
    report ("Anuncio: si omitio la 1.0.0, si avisa de la 1.0.1", UpdateLogic::shouldNotify (UpdateLogic::parse (good.replace ("1.0.0", "1.0.1")), "0.26.0", "1.0.0"));
   #if JUCE_LINUX
    report ("Anuncio: en este sistema usa el link general", u.url == "https://aleksandergaux.com");
   #endif

    // De punta a punta con un archivo local (sin red) y ajustes temporales
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("gaux_update_test");
    dir.deleteRecursively(); dir.createDirectory();
    juce::PropertiesFile::Options o; o.applicationName = "t"; o.folderName = dir.getFullPathName(); o.filenameSuffix = ".settings"; o.millisecondsBeforeSaving = 0;
    juce::PropertiesFile props (dir.getChildFile ("t.settings"), o);
    const auto manifest = dir.getChildFile ("latest.json");
    manifest.replaceWithText (good);
    UpdateInfo info;
    report ("Punta a punta: el plugin beta lee el anuncio publicado y avisa", UpdateChecker::fetchAndDecide (juce::URL (manifest).toString (false), "0.26.0", props, info) && info.version == "1.0.0");
    report ("Punta a punta: guarda el anuncio para no volver a leerlo en 24 h", props.getValue ("lastJson") == good);
    props.setValue ("skipVersion", "1.0.0");
    report ("Punta a punta: 'omitir' silencia esa version", ! UpdateChecker::fetchAndDecide (juce::URL (manifest).toString (false), "0.26.0", props, info));
    props.setValue ("skipVersion", "");
    props.setValue ("updatesOff", true);
    report ("Punta a punta: con los avisos apagados no lee nada", ! UpdateChecker::fetchAndDecide (juce::URL (manifest).toString (false), "0.26.0", props, info));
    props.setValue ("updatesOff", false);
    report ("Punta a punta: archivo inexistente (sin internet) no avisa ni falla", ! UpdateChecker::fetchAndDecide ("file:///no/existe/latest.json", "0.26.0", props, info));
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    report ("Punta a punta: un link que no es https no se abre", ! UpdateChecker::fetchAndDecide ("ftp://example.com/x.json", "0.26.0", props, info) && juce::Time::getMillisecondCounterHiRes() - t0 < 500);

    // El anuncio que esta publicado hoy en el repositorio: valido y sin avisar a la beta actual
    const auto repoManifest = juce::File (__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile ("update/latest.json");
    const auto pub = UpdateLogic::parse (repoManifest.loadFileAsString());
    report ("update/latest.json del repositorio: es valido (version " + pub.version + ")", pub.valid());
    report ("update/latest.json: no avisa a quien ya tiene esta version (" JucePlugin_VersionString ")", ! UpdateLogic::shouldNotify (pub, JucePlugin_VersionString, ""));
    report ("update/latest.json: anuncia la misma version que compila el proyecto", UpdateLogic::compare (pub.version, JucePlugin_VersionString) == 0);
    dir.deleteRecursively();
}


//------------------------------------------------------------------------------
// 8) Lo que prometen los videos y la guia, medido en el plugin
static void promiseTests()
{
    std::cout << "\n-- Promesas de los videos y la guia --" << std::endl;
    const double fs = 48000; const St music = noiseMusic (fs, 3.0, 41, false);
    auto bandE = [&] (const St& x, double f1, double f2)
    {
        std::vector<double> m (x.size()); for (size_t i = 0; i < x.size(); ++i) m[i] = 0.5 * (x.l[i] + x.r[i]);
        const auto sp = welch (m, m, fs, (size_t) (0.5 * fs));
        double e = 0; for (size_t k = 0; k < sp.sxx.size(); ++k) { const double f = k * fs / (double) sp.nfft; if (f >= f1 && f < f2) e += sp.sxx[k]; }
        return 10 * std::log10 (e + 1e-30);
    };
    auto mixBands = [] (GauxEqProcessor& p)
    {
        applyBand (p, 0, lcut (28, 1)); applyBand (p, 1, bell (58, 4, 1.1)); applyBand (p, 2, bell (320, -3, 1.6));
        applyBand (p, 3, bell (2800, 2, 0.9)); applyBand (p, 4, hshelf (10000, 3.5));
    };
    const St clean = renderLayout (fs, music, 512, false, mixBands);
    const char* names[] = { "TUBE", "TAPE", "SOFT", "HARD", "FUZZ" };
    for (int type = 0; type < 5; ++type)
    {
        const St sat = renderLayout (fs, music, 512, false, [&] (GauxEqProcessor& p) { mixBands (p); applyBand (p, 1, est_sat (type)); });
        double e = 0, r = 0; for (size_t i = (size_t) (0.5 * fs); i < music.size(); ++i) { e += std::pow (sat.l[i] - clean.l[i], 2); r += std::pow (clean.l[i], 2); }
        const double diff = 10 * std::log10 (e / r);
        St dif (music.size()); for (size_t i = 0; i < music.size(); ++i) { dif.l[i] = sat.l[i] - clean.l[i]; dif.r[i] = sat.r[i] - clean.r[i]; }
        const double harm = bandE (dif, 150, 2000) - bandE (clean, 150, 2000);   // armonicos nuevos frente a lo que ya habia ahi
        const double lvl = 10 * std::log10 (r / std::max (1e-30, [&] { double q = 0; for (size_t i = (size_t) (0.5 * fs); i < music.size(); ++i) q += sat.l[i] * sat.l[i]; return q; }()));
        report (juce::String ("Video \"Graves saturados: ") + names[type] + "\" (58 Hz, DRIVE 20): se oye (cambio " + juce::String (diff, 1) + " dB), agrega armonicos nuevos en 150 Hz - 2 kHz ("
                + juce::String (harm, 1) + " dB frente a lo que ya sonaba ahi) y no salta de volumen (" + juce::String (-lvl, 1) + " dB)", diff > -30.0 && harm > -40.0 && std::abs (lvl) < 3.0);
    }
}

} // namespace est

static void stressMaxTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("estres stress maximo promesas aviso").contains (gFilter)) return;
    std::cout << "\n== ESTRES MAXIMO (v0.26) ==" << std::endl;
    if (gFilter == "promesas") { est::promiseTests(); return; }
    if (gFilter == "aviso") { est::updateTests(); return; }
    est::fuzz();
    est::textRoundTrip();
    est::hostileSignals();
    est::brokenStates();
    est::hostQuirks();
    est::cpuWorst();
    est::updateTests();
    est::promiseTests();
}

static int debugBypass (unsigned seed)
{
    using namespace est;
    const double fs = 48000; const St music = noiseMusic (fs, 0.6, 77, false);
    const State s = randomState (seed);
    GauxEqProcessor c; applyState (c, s); c.apvts.getParameter (ParamID::bypass)->setValueNotifyingHost (1.0f);
    c.prepareToPlay (fs, 1024);
    std::printf ("lat tras prepare %d\n", c.getLatencySamples());
    std::mt19937 g (seed);
    juce::AudioBuffer<float> buf (2, 1024); juce::MidiBuffer midi; size_t off = 0;
    while (off < music.size())
    {
        const int n = (int) std::min<size_t> ((size_t) (1 + g() % 1024u), music.size() - off);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i) { buf.setSample (0, i, (float) music.l[off + i]); buf.setSample (1, i, (float) music.r[off + i]); }
        const int latBefore = c.getLatencySamples();
        c.processBlock (buf, midi);
        double e = 0; for (int i = 0; i < n; ++i) { const long j = (long) (off + i) - c.getLatencySamples(); if (j >= 0) e = std::max (e, (double) std::abs (buf.getSample (0, i) - (float) music.l[(size_t) j])); }
        if (e > 1e-6 || latBefore != c.getLatencySamples()) std::printf ("t=%.3f n=%d lat %d->%d err %g\n", off / fs, n, latBefore, c.getLatencySamples(), e);
        off += (size_t) n;
    }
    return 0;
}
static int debugState (unsigned seed)
{
    using namespace est;
    GauxEqProcessor a; applyState (a, randomState (seed));
    juce::MemoryBlock mb; a.getStateInformation (mb);
    GauxEqProcessor b; b.setStateInformation (mb.getData(), (int) mb.getSize());
    auto& pa = a.getParameters(); auto& pb = b.getParameters();
    for (int i = 0; i < pa.size(); ++i)
        if (std::abs (pa[i]->getValue() - pb[i]->getValue()) > 1e-6)
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (pa[i]);
            std::printf ("%s: norm %.9f -> %.9f   valor %.9f -> %.9f\n", rp->getParameterID().toRawUTF8(), pa[i]->getValue(), pb[i]->getValue(), rp->convertFrom0to1 (pa[i]->getValue()), rp->convertFrom0to1 (pb[i]->getValue()));
        }
    if (auto x = a.apvts.copyState().createXml()) { auto s = x->toString(); int k = s.indexOf ("rangeLo"); std::printf ("%s\n", s.substring (k - 20, k + 60).toRawUTF8()); }
    return 0;
}
static int debugGolden (const char* path)
{
    juce::StringArray lines; lines.addLines (juce::File (path).loadFileAsString());
    GauxEqProcessor p;
    for (int i = 2; i < lines.size(); ++i)
        if (lines[i].contains ("="))
        {
            const auto id = lines[i].upToFirstOccurrenceOf ("=", false, false); const float v = lines[i].fromFirstOccurrenceOf ("=", false, false).getFloatValue();
            if (auto* rp = p.apvts.getParameter (id)) { rp->setValueNotifyingHost (rp->convertTo0to1 (v)); const float got = rp->convertFrom0to1 (rp->getValue());
                if (std::abs (got - v) > 1e-3f * std::max (1.0f, std::abs (v))) std::printf ("%s: quiere %g obtiene %g\n", id.toRawUTF8(), v, got); }
        }
    return 0;
}
