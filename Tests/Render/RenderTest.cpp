#include <deque>
#include <functional>
#include <map>
// Prueba interna de Gaux Equalizer: procesa audio y guarda capturas de la interfaz.
#include "PluginProcessor.h"
#include "PluginEditor.h"

static void setP (GauxEqProcessor& p, const juce::String& id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

static bool processNoise (GauxEqProcessor& p, int blocks, double& peakOut, double& msPerBlock)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    juce::Random rnd (42);
    float b0 = 0, b1 = 0, b2 = 0;
    bool finite = true;
    peakOut = 0;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (int blk = 0; blk < blocks; ++blk)
    {
        for (int i = 0; i < 512; ++i)
        {
            // ruido rosa aproximado + bombo sinusoidal
            const float w = rnd.nextFloat() * 2 - 1;
            b0 = 0.99765f * b0 + w * 0.0990460f; b1 = 0.96300f * b1 + w * 0.2965164f; b2 = 0.57000f * b2 + w * 1.0526913f;
            const float pink = (b0 + b1 + b2 + w * 0.1848f) * 0.08f;
            const float kick = 0.4f * std::sin (2.0f * juce::MathConstants<float>::pi * 55.0f * (float) (blk * 512 + i) / 48000.0f);
            buf.setSample (0, i, pink + kick);
            buf.setSample (1, i, pink * 0.9f + kick);
        }
        p.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
            {
                const float s = buf.getSample (ch, i);
                finite = finite && std::isfinite (s);
                peakOut = std::max (peakOut, (double) std::abs (s));
            }
    }
    msPerBlock = (juce::Time::getMillisecondCounterHiRes() - t0) / blocks;
    return finite;
}


// Seno (con sidechain opcional): devuelve nivel RMS de salida en dB y error maximo contra la entrada retrasada
struct SineResult { double outDb = -200, maxErr = 0; };
static SineResult processSine (GauxEqProcessor& p, double freq, float amp, int blocks, float scAmp = 0.0f, double scFreq = 1000.0)
{
    const int nch = juce::jmax (2, p.getTotalNumInputChannels());
    juce::AudioBuffer<float> buf (nch, 512);
    juce::MidiBuffer midi;
    std::vector<float> hist;
    SineResult r;
    double sum = 0; int cnt = 0;
    const int lat = p.getLatencySamples();
    for (int blk = 0; blk < blocks; ++blk)
    {
        buf.clear();
        for (int i = 0; i < 512; ++i)
        {
            const double t = (double) (blk * 512 + i) / 48000.0;
            const float x = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * t);
            buf.setSample (0, i, x); buf.setSample (1, i, x);
            hist.push_back (x);
            if (nch >= 4)
            {
                const float sc = scAmp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * scFreq * t);
                buf.setSample (2, i, sc); buf.setSample (3, i, sc);
            }
        }
        p.processBlock (buf, midi);
        if (blk >= blocks / 2)
            for (int i = 0; i < 512; ++i)
            {
                const float y = buf.getSample (0, i);
                sum += (double) y * y; ++cnt;
                const int idx = blk * 512 + i - lat;
                if (idx >= 0) r.maxErr = std::max (r.maxErr, (double) std::abs (y - hist[(size_t) idx]));
            }
    }
    r.outDb = 10.0 * std::log10 (std::max (1.0e-20, sum / std::max (1, cnt)));
    return r;
}


// Diagnostico: estres aleatorio solo sobre los parametros cuyo ID empieza con 'prefix' (o "*")
static double stressExcess (double sr, int maxBlock, const juce::String& prefix, int seconds, juce::int64 seed, juce::String& worstInfo)
{
    using namespace gaux;
    GauxEqProcessor ps; ps.prepareToPlay (sr, maxBlock);
    juce::Random rnd (seed);
    auto& pp = ps.getParameters();
    juce::Array<juce::AudioProcessorParameter*> pool;
    for (auto* prm : pp)
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
        {
            const auto id = rp->getParameterID();
            if (id == ParamID::quality || id == ParamID::bypass) continue;
            bool take = prefix == "*" || id.startsWith (prefix) || (prefix.startsWith ("~") && id.endsWith (prefix.substring (1)));
            if (prefix.contains ("!"))
            {
                take = id.startsWith (prefix.upToFirstOccurrenceOf ("!", false, false));
                for (auto& ex : juce::StringArray::fromTokens (prefix.fromFirstOccurrenceOf ("!", false, false), ",", ""))
                    if (id.endsWith ("_" + ex)) take = false;
            }
            if (take) pool.add (prm);
        }
    juce::AudioBuffer<float> buf (2, maxBlock); juce::MidiBuffer midi;
    double phase = 0, worst = 0; long total = 0;
    std::deque<double> hist;
    juce::String lastChanges;
    juce::MemoryBlock worstSnap;
    auto maxGainNow = [&] ()
    {
        double best = 0; const double efs = ps.engineSampleRate.load();
        for (double f = 15; f < sr * 0.49; f *= 1.01)
        {
            double m = 1;
            for (int b = 0; b < kNumBands; ++b) { auto st = ps.params.band (b); if (st.dynOn && st.rangeDb > 0) st.gainDb += st.rangeDb; m *= magnitudeOfBand (st, f, efs); }
            best = std::max ({ best, m, magnitudeOfVintage (ps.params.vintage(), f, efs) });
        }
        return std::max (1.0, best) * juce::Decibels::decibelsToGain ((double) ps.params.output->load())
               * juce::Decibels::decibelsToGain ((double) std::max (0.0f, ps.autoGainDb.load())) * 2.0;
    };
    while (total < (long) (sr * seconds))
    {
        const int n = 1 + rnd.nextInt (maxBlock);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            phase += 2 * juce::MathConstants<double>::pi * 70.0 / sr;
            const float v = (float) (0.5 * std::sin (phase)) + (rnd.nextFloat() * 2 - 1) * 0.2f;
            buf.setSample (0, i, v); buf.setSample (1, i, -v * 0.8f);
        }
        juce::String changes;
        for (int k = 0; k < 4 && ! pool.isEmpty(); ++k)
        {
            auto* prm = pool[rnd.nextInt (pool.size())];
            prm->setValueNotifyingHost (rnd.nextFloat());
            changes << dynamic_cast<juce::RangedAudioParameter*> (prm)->getParameterID() << "=" << prm->getCurrentValueAsText() << " ";
        }
        juce::MemoryBlock snap; ps.getStateInformation (snap);
        ps.processBlock (buf, midi);
        double pk = 0;
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n; ++i) pk = std::max (pk, (double) std::abs (buf.getSample (ch, i)));
        hist.push_back (maxGainNow());
        while ((long) hist.size() * (maxBlock / 2) > (long) (sr * 0.25) && hist.size() > 1) hist.pop_front();
        const double ex = pk / std::max (1e-9, *std::max_element (hist.begin(), hist.end()) * 0.73);
        if (ex > worst) { worst = ex; worstInfo = "antes: " + lastChanges + " | ahora: " + changes; worstSnap = snap; }
        lastChanges = changes;
        total += n;
    }
    // Reproducir el peor momento con los ajustes quietos: si tambien se pasa, es la estimacion; si no, era un transitorio
    if (worstSnap.getSize() > 0)
    {
        GauxEqProcessor pr; pr.prepareToPlay (sr, maxBlock);
        pr.setStateInformation (worstSnap.getData(), (int) worstSnap.getSize());
        juce::AudioBuffer<float> b2 (2, maxBlock); double ph = 0, pk = 0;
        for (int blk = 0; blk < (int) (sr * 2 / maxBlock); ++blk)
        {
            for (int i = 0; i < maxBlock; ++i) { ph += 2 * juce::MathConstants<double>::pi * 70.0 / sr; const float v = (float) (0.5 * std::sin (ph)) + (rnd.nextFloat() * 2 - 1) * 0.2f; b2.setSample (0, i, v); b2.setSample (1, i, -v * 0.8f); }
            pr.processBlock (b2, midi);
            if (blk > (int) (sr / maxBlock)) for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < maxBlock; ++i) pk = std::max (pk, (double) std::abs (b2.getSample (ch, i)));
        }
        double best = 0; const double efs = pr.engineSampleRate.load();
        for (double f = 15; f < sr * 0.49; f *= 1.01)
        {
            double m = 1;
            for (int b = 0; b < gaux::kNumBands; ++b) { auto st = pr.params.band (b); if (st.dynOn && st.rangeDb > 0) st.gainDb += st.rangeDb; m *= gaux::magnitudeOfBand (st, f, efs); }
            best = std::max ({ best, m, gaux::magnitudeOfVintage (pr.params.vintage(), f, efs) });
        }
        const double allowed = std::max (1.0, best) * juce::Decibels::decibelsToGain ((double) pr.params.output->load())
                               * juce::Decibels::decibelsToGain ((double) std::max (0.0f, pr.autoGainDb.load())) * 2.0 * 0.73;
        worstInfo << "\n  mismo ajuste quieto: exceso " << juce::String (20 * std::log10 (std::max (1e-9, pk / allowed)), 1) << " dB";
        juce::String bands;
        for (int b = 0; b < gaux::kNumBands; ++b) { auto st = pr.params.band (b); if (st.on) bands << "b" << (b + 1) << "[t" << st.type << " " << juce::String (st.freq, 0) << "Hz " << juce::String (st.gainDb, 1) << "dB q" << juce::String (st.q, 2) << " sl" << st.slope << " st" << st.stereo << (st.dynOn ? " dyn" + juce::String (st.rangeDb, 1) : juce::String()) << (st.driveDb > 0.1 ? " drv" + juce::String (st.driveDb, 0) : juce::String()) << "] "; }
        // Quitar cada banda y ver cuanto baja el pico
        auto staticPeak = [&] (int skipBand)
        {
            GauxEqProcessor q; q.prepareToPlay (sr, maxBlock);
            q.setStateInformation (worstSnap.getData(), (int) worstSnap.getSize());
            if (skipBand >= 0) setP (q, gaux::ParamID::band (skipBand, "on"), 0);
            juce::AudioBuffer<float> b3 (2, maxBlock); double ph2 = 0, pk2 = 0; juce::Random r2 (5);
            for (int blk = 0; blk < (int) (sr * 2 / maxBlock); ++blk)
            {
                for (int i = 0; i < maxBlock; ++i) { ph2 += 2 * juce::MathConstants<double>::pi * 70.0 / sr; const float v = (float) (0.5 * std::sin (ph2)) + (r2.nextFloat() * 2 - 1) * 0.2f; b3.setSample (0, i, v); b3.setSample (1, i, -v * 0.8f); }
                q.processBlock (b3, midi);
                if (blk > (int) (sr / maxBlock)) for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < maxBlock; ++i) pk2 = std::max (pk2, (double) std::abs (b3.getSample (ch, i)));
            }
            return 20 * std::log10 (pk2);
        };
        worstInfo << "\n  pico quieto " << juce::String (staticPeak (-1), 1) << " dBFS; sin cada banda:";
        for (int b = 0; b < gaux::kNumBands; ++b) if (pr.params.band (b).on) worstInfo << " b" << (b + 1) << "->" << juce::String (staticPeak (b), 1);
        worstInfo << "\n  bandas: " << bands << " out " << juce::String (pr.params.output->load(), 1) << " modo " << (int) pr.params.mode->load() << " zone " << (int) pr.params.rangeOn->load();
    }
    return 20 * std::log10 (std::max (1e-9, worst));
}

// Sin monitor no hay "vblank": las pruebas avanzan los cuadros de animacion a mano
static void pumpFrames (juce::Component&, float dt = 1.0f / 30.0f) { gaux::FrameClock::pumpAllForTests (dt); }

static void snapshot (juce::Component& c, const juce::String& file)
{
    auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
    juce::File f (file);
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("captura: %s\n", file.toRawUTF8());
}

static juce::Component* findById (juce::Component* c, const juce::String& id)
{
    if (c->getComponentID() == id) return c;
    for (auto* ch : c->getChildren())
        if (auto* f = findById (ch, id)) return f;
    return nullptr;
}

static juce::MouseEvent mouseAt (juce::Component* c, juce::Point<float> pos, juce::Point<float> downPos, int clicks = 1,
                                 juce::ModifierKeys mods = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier))
{
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    return juce::MouseEvent (src, pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, c, c, juce::Time::getCurrentTime(),
                             downPos, juce::Time::getCurrentTime(), clicks, false);
}

int main (int argc, char** argv)
{
    GauxEqProcessor::forceUiForTests = true;   // las pruebas leen medidores y calavera sin abrir la ventana
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String outDir = argc > 1 ? argv[1] : ".";
    if (argc > 2 && juce::String (argv[2]) == "fps")
    {
        // Medicion de costo de dibujo por cuadro (pantalla Retina: escala 2x)
        GauxEqProcessor p; p.prepareToPlay (48000.0, 512);
        using namespace gaux;
        auto band = [&] (int b, int type, float f, float g, float q, float drive = 0)
        {
            setP (p, ParamID::band (b, "on"), 1); setP (p, ParamID::band (b, "type"), (float) type);
            setP (p, ParamID::band (b, "freq"), f); setP (p, ParamID::band (b, "gain"), g); setP (p, ParamID::band (b, "q"), q);
            setP (p, ParamID::band (b, "drive"), drive);
        };
        band (0, (int) BandType::LowCut, 30, 0, 0.707f);
        band (1, (int) BandType::Bell, 60, 4.5f, 1.2f, 20);
        band (2, (int) BandType::Bell, 320, -3.5f, 2.0f);
        band (3, (int) BandType::Bell, 900, 0.0f, 1.4f);
        setP (p, ParamID::band (3, "dyn"), 1); setP (p, ParamID::band (3, "thr"), -40); setP (p, ParamID::band (3, "range"), -8);
        band (4, (int) BandType::Bell, 2800, 2.5f, 0.9f, 12);
        band (6, (int) BandType::HighShelf, 9000, 3.0f, 0.7f);
        band (7, (int) BandType::HighCut, 18000, 0, 0.707f);
        double peak = 0, ms = 0;
        processNoise (p, 60, peak, ms);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* editor = dynamic_cast<GauxEqEditor*> (ed.get());
        editor->setUiScale (1.0f);
        editor->selectBand (3);
        std::function<juce::Component* (juce::Component*)> findDisplay = [&] (juce::Component* c) -> juce::Component*
        {
            if (dynamic_cast<SpectrumDisplay*> (c) != nullptr && c->isVisible()) return c;
            for (auto* ch : c->getChildren()) if (auto* r = findDisplay (ch)) return r;
            return nullptr;
        };
        auto* disp = findDisplay (ed.get());
        for (int i = 0; i < 20; ++i) { processNoise (p, 4, peak, ms); dynamic_cast<SpectrumDisplay*> (disp)->advanceFrame (1.0f / 60.0f); }
        float benchScale = std::getenv ("GAUX_SCALE") ? (float) std::atof (std::getenv ("GAUX_SCALE")) : 2.0f;
        auto timeIt = [&] (juce::Component& c, int frames, bool feed)
        {
            const float sc = benchScale;
            juce::Image img (juce::Image::ARGB, (int) (c.getWidth() * sc), (int) (c.getHeight() * sc), true);
            double tot = 0, worstF = 0;
            for (int f = 0; f < frames; ++f)
            {
                if (feed) { processNoise (p, 1, peak, ms); dynamic_cast<SpectrumDisplay*> (disp)->advanceFrame (1.0f / 120.0f); }
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                { juce::Graphics g (img); g.addTransform (juce::AffineTransform::scale (sc)); c.paintEntireComponent (g, true); }
                const double d = juce::Time::getMillisecondCounterHiRes() - t0;
                tot += d; worstF = std::max (worstF, d);
            }
            return std::make_pair (tot / frames, worstF);
        };
        if (std::getenv ("GAUX_MICRO"))
        {
            juce::Image img (juce::Image::ARGB, 2320, 884, true), grad (juce::Image::ARGB, 2320, 884, true), half (juce::Image::ARGB, 1160, 442, true);
            juce::Path fill, line;
            juce::Random r (1);
            line.startNewSubPath (0, 400);
            for (float x = 0; x <= 1160; x += 3) line.lineTo (x, 150 + 120 * std::sin (x * 0.01f) + r.nextFloat() * 30);
            fill = line; fill.lineTo (1163, 442); fill.lineTo (0, 442); fill.closeSubPath();
            auto bench = [&] (const char* name, std::function<void (juce::Graphics&)> fn, juce::Image& target, float sc)
            {
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < 50; ++i) { juce::Graphics g (target); g.addTransform (juce::AffineTransform::scale (sc)); fn (g); }
                std::printf ("  %-34s %.2f ms\n", name, (juce::Time::getMillisecondCounterHiRes() - t0) / 50);
            };
            bench ("relleno solido 2x", [&] (juce::Graphics& g) { g.setColour (juce::Colours::white.withAlpha (0.05f)); g.fillPath (fill); }, img, 2);
            bench ("relleno degradado 2x", [&] (juce::Graphics& g) { g.setGradientFill (juce::ColourGradient (juce::Colours::red.withAlpha (0.3f), 0, 100, juce::Colours::transparentBlack, 0, 442, false)); g.fillPath (fill); }, img, 2);
            bench ("recorte + imagen 2x", [&] (juce::Graphics& g) { g.reduceClipRegion (fill); g.drawImageTransformed (grad, juce::AffineTransform::scale (0.5f)); }, img, 2);
            bench ("trazo 1 px 2x", [&] (juce::Graphics& g) { g.setColour (juce::Colours::white.withAlpha (0.3f)); g.strokePath (line, juce::PathStrokeType (1.0f)); }, img, 2);
            bench ("relleno solido 1x", [&] (juce::Graphics& g) { g.setColour (juce::Colours::white.withAlpha (0.05f)); g.fillPath (fill); }, half, 1);
            bench ("imagen 1x -> 2x (bilineal)", [&] (juce::Graphics& g) { g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality); g.drawImageTransformed (half, juce::AffineTransform::scale (2.0f)); }, img, 1);
            bench ("imagen 1x -> 2x (sin filtrar)", [&] (juce::Graphics& g) { g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality); g.drawImageTransformed (half, juce::AffineTransform::scale (2.0f)); }, img, 1);
            bench ("copia imagen 2x (mezcla)", [&] (juce::Graphics& g) { g.drawImageAt (grad, 0, 0); }, img, 1);
            return 0;
        }
        const auto d = timeIt (*disp, 240, true);
        const auto e = timeIt (*ed, 60, true);
        std::printf ("Pantalla de espectro (%dx%d @2x): %.2f ms por cuadro (peor %.2f) -> %.0f FPS posibles\n", disp->getWidth(), disp->getHeight(), d.first, d.second, 1000.0 / d.first);
        std::printf ("Ventana completa (%dx%d @2x): %.2f ms por cuadro (peor %.2f)\n", ed->getWidth(), ed->getHeight(), e.first, e.second);
        {
            // En silencio (sin reproducir) la pantalla deja de dibujarse: 0 % de CPU
            auto* sd = dynamic_cast<SpectrumDisplay*> (disp);
            juce::AudioBuffer<float> sil (2, 512); juce::MidiBuffer mid;
            int drew = 0;
            for (int f = 0; f < 600; ++f)
            {
                if (f % 2 == 0) { sil.clear(); p.processBlock (sil, mid); }
                sd->advanceFrame (1.0f / 120.0f);
                if (f >= 480 && sd->lastFrameDrew()) ++drew;
            }
            std::printf ("En silencio, tras 4 s: %d de 120 cuadros dibujados %s\n", drew, drew == 0 ? "OK (pantalla en reposo)" : "FALLA");
            if (drew != 0) return 1;
            // y vuelve a la vida en cuanto suena
            processNoise (p, 2, peak, ms); sd->advanceFrame (1.0f / 120.0f);
            std::printf ("Vuelve a sonar: %s\n", sd->lastFrameDrew() ? "dibuja OK" : "FALLA");
            if (! sd->lastFrameDrew()) return 1;
        }
        benchScale = 1.0f;
        const auto d1 = timeIt (*disp, 240, true);
        std::printf ("Pantalla de espectro (%dx%d @1x, PC Full HD): %.2f ms por cuadro -> %.0f FPS posibles\n", disp->getWidth(), disp->getHeight(), d1.first, 1000.0 / d1.first);
        return 0;
    }
    if (argc > 2)
    {
        // Modo diagnostico: GauxRenderTest <dir> <prefijo> [segundos]
        double worst = -200; juce::String info;
        for (int seed = 1; seed <= 6; ++seed)
            for (int mb : { 64, 512 })
            {
                juce::String inf;
                const double e = stressExcess (48000.0, mb, argv[2], argc > 3 ? juce::String (argv[3]).getIntValue() : 6, seed, inf);
                if (e > worst) { worst = e; info = inf; }
            }
        std::printf ("prefijo '%s': exceso maximo %+.1f dB\n  %s\n", argv[2], worst, info.toRawUTF8());
        return 0;
    }

    {
        // Gobernador de FPS: que elige segun el monitor y lo que cuesta dibujar un cuadro
        using gaux::FrameClock;
        struct Case { double hz, ms; int expectFps; const char* what; };
        const Case cases[] = {
            { 120, 1.5, 120, "MacBook ProMotion, cuadro barato" },
            { 120, 3.7, 120, "MacBook ProMotion, cuadro de 3.7 ms (45 %)" },
            { 120, 4.5,  60, "MacBook ProMotion, cuadro de 4.5 ms -> 60 FPS" },
            { 120, 8.0,  60, "MacBook ProMotion, cuadro de 8 ms -> 60 FPS" },
            {  60, 3.0,  60, "monitor de 60 Hz" },
            { 144, 2.0, 144, "monitor gamer de 144 Hz" },
            { 144, 3.5,  72, "144 Hz con cuadro de 3.5 ms" },
            {  60, 9.5,  60, "PC lento (9.5 ms por cuadro): sostiene 60" },
            {  60, 12.0, 30, "PC muy lento (12 ms por cuadro): protege al DAW" } };
        bool govOk = true;
        for (auto& c : cases)
        {
            const int every = FrameClock::chooseEvery (c.ms * 0.001, 1.0 / c.hz, 0);
            const int fps = (int) std::lround (c.hz / every);
            const bool ok = fps == c.expectFps;
            govOk = govOk && ok;
            std::printf ("Gobernador FPS: %-48s -> %3d FPS %s\n", c.what, fps, ok ? "OK" : "FALLA");
        }
        if (! govOk) return 1;
    }

    GauxEqProcessor p;
    p.prepareToPlay (48000.0, 512);

    // Escena 1: modo parametrico con varias bandas, dos de ellas saturando
    using namespace gaux;
    auto band = [&] (int b, int type, float f, float g, float q, float drive = 0, int satType = 0)
    {
        setP (p, ParamID::band (b, "on"), 1); setP (p, ParamID::band (b, "type"), (float) type);
        setP (p, ParamID::band (b, "freq"), f); setP (p, ParamID::band (b, "gain"), g); setP (p, ParamID::band (b, "q"), q);
        setP (p, ParamID::band (b, "drive"), drive); setP (p, ParamID::band (b, "satType"), (float) satType);
    };
    band (0, (int) BandType::LowCut, 30, 0, 0.707f);
    band (1, (int) BandType::Bell, 60, 4.5f, 1.2f, 20, 0);
    band (2, (int) BandType::Bell, 320, -3.5f, 2.0f);
    band (4, (int) BandType::Bell, 2800, 2.5f, 0.9f, 12, 1);
    band (6, (int) BandType::HighShelf, 9000, 3.0f, 0.7f);
    band (7, (int) BandType::HighCut, 18000, 0, 0.707f);
    // Banda 4 dinamica (domando medios), en Mid
    band (3, (int) BandType::Bell, 900, 0.0f, 1.4f);
    setP (p, ParamID::band (3, "dyn"), 1); setP (p, ParamID::band (3, "thr"), -40); setP (p, ParamID::band (3, "range"), -8);
    setP (p, ParamID::band (3, "stereo"), 3);
    setP (p, ParamID::band (2, "stereo"), 4);   // Side
    setP (p, ParamID::band (4, "stereo"), 1);   // Left
    setP (p, ParamID::rangeOn, 1); setP (p, ParamID::rangeLo, 40); setP (p, ParamID::rangeHi, 15000);

    double peak = 0, ms = 0;
    bool ok = true;
    for (int q = 0; q < 3; ++q)
    {
        setP (p, ParamID::quality, (float) q);
        const bool f = processNoise (p, 200, peak, ms);
        ok = ok && f;
        std::printf ("Parametrico calidad %d: finito=%d pico=%.3f  %.3f ms/bloque (limite tiempo real 10.7 ms)  latencia=%d\n",
                     q, f, peak, ms, p.getLatencySamples());
    }
    setP (p, ParamID::quality, 0);
    processNoise (p, 60, peak, ms);

    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* editor = dynamic_cast<GauxEqEditor*> (ed.get());
        // dejar correr los timers para que el analizador se llene
        std::printf ("Escala por defecto al abrir: %.0f%%\n", editor->getUiScale() * 100.0f);
        editor->setUiScale (1.0f);
        editor->selectBand (3);
        for (int i = 0; i < 25; ++i) { processNoise (p, 4, peak, ms); juce::MessageManager::getInstance()->runDispatchLoopUntil (30); pumpFrames (*ed); }
        snapshot (*editor, outDir + "/gaux_parametrico.png");

        // Filtros nuevos: high cut 72 dB/oct seleccionado, band pass, tilt y bandas 17-24
        setP (p, ParamID::band (7, "slope"), 6);
        band (16, (int) BandType::BandPass, 1800, 0, 2.0f); setP (p, ParamID::band (16, "slope"), 1);
        band (19, (int) BandType::TiltShelf, 500, -3.0f, 0.7f);
        band (23, (int) BandType::AllPass, 250, 0, 1.0f);
        editor->selectBand (7);
        for (int i = 0; i < 10; ++i) { processNoise (p, 4, peak, ms); juce::MessageManager::getInstance()->runDispatchLoopUntil (30); pumpFrames (*ed); }
        snapshot (*editor, outDir + "/gaux_filtros.png");
        editor->selectBand (16);
        for (int i = 0; i < 4; ++i) { juce::MessageManager::getInstance()->runDispatchLoopUntil (40); pumpFrames (*ed); }
        snapshot (*editor, outDir + "/gaux_bandpass.png");
        for (int b : { 16, 19, 23 }) setP (p, ParamID::band (b, "on"), 0);
        setP (p, ParamID::band (7, "slope"), 0);

        // Dinamica espectral en la banda 4 (se ve la accion frecuencia por frecuencia)
        setP (p, ParamID::band (3, "spec"), 1); setP (p, ParamID::band (3, "thr"), -72); setP (p, ParamID::band (3, "range"), -12);
        setP (p, ParamID::band (3, "q"), 0.6f); setP (p, ParamID::band (3, "stereo"), 0);
        editor->selectBand (3);
        for (int i = 0; i < 30; ++i) { processNoise (p, 4, peak, ms); juce::MessageManager::getInstance()->runDispatchLoopUntil (20); pumpFrames (*ed); }
        std::printf ("Espectral: latencia %d muestras, accion maxima %.1f dB\n", p.getLatencySamples(), p.dynGainDb[3].load());
        snapshot (*editor, outDir + "/gaux_espectral.png");
        setP (p, ParamID::band (3, "spec"), 0); setP (p, ParamID::band (3, "thr"), -40); setP (p, ParamID::band (3, "range"), -8);
        setP (p, ParamID::band (3, "q"), 1.4f); setP (p, ParamID::band (3, "stereo"), 3);

        // ---- EQ MATCH por la interfaz: archivo de referencia + aprender esta pista + aplicar ----
        {
            const double fs = 48000.0;
            // referencia: ruido "rosado" con mas brillo y menos graves (un EQ conocido)
            juce::Random rnd (99);
            const int len = (int) (fs * 8);
            juce::AudioBuffer<float> refBuf (2, len);
            double b0 = 0, b1 = 0, b2 = 0;
            std::vector<float> pinkSrc ((size_t) len);
            for (int i = 0; i < len; ++i)
            {
                const double w = rnd.nextFloat() * 2.0 - 1.0;
                b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
                pinkSrc[(size_t) i] = (float) (0.05 * (b0 + b1 + b2 + w * 0.1848));
            }
            gaux::BandSettings hs; hs.on = true; hs.type = (int) BandType::HighShelf; hs.freq = 6000; hs.gainDb = 5; hs.q = 0.707;
            gaux::BandSettings ls; ls.on = true; ls.type = (int) BandType::LowShelf; ls.freq = 150; ls.gainDb = -4; ls.q = 0.707;
            {
                gaux::EqBand e1, e2; e1.prepare (fs); e2.prepare (fs); e1.setTarget (hs); e2.setTarget (ls);
                std::vector<double> l (pinkSrc.begin(), pinkSrc.end()), r = l;
                for (int i = 0; i < len; i += 16)
                {
                    const int n = std::min (16, len - i);
                    e1.controlTick(); e2.controlTick();
                    e1.processSpan (l.data() + i, r.data() + i, n, true, nullptr); e2.processSpan (l.data() + i, r.data() + i, n, true, nullptr);
                }
                for (int i = 0; i < len; ++i) { refBuf.setSample (0, i, (float) l[(size_t) i]); refBuf.setSample (1, i, (float) r[(size_t) i]); }
            }
            const auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("gaux_referencia.wav");
            wav.deleteFile();
            {
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::OutputStream> os (wav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.release(), fs, 2, 24, {}, 0));
                w->writeFromAudioSampleBuffer (refBuf, 0, len);
            }
            GauxEqProcessor pm; pm.prepareToPlay (fs, 512);
            std::unique_ptr<juce::AudioProcessorEditor> edm (pm.createEditor());
            auto* editorM = dynamic_cast<GauxEqEditor*> (edm.get());
            editorM->setUiScale (1.0f);
            auto panel = std::make_unique<gaux::MatchPanel> (pm);
            pm.match.loadReferenceFile (wav);
            pm.match.setLearnSource (true);
            juce::AudioBuffer<float> blk (2, 512); juce::MidiBuffer midi;
            for (int pos = 0; pos + 512 <= len; pos += 512)
            {
                for (int i = 0; i < 512; ++i) { blk.setSample (0, i, pinkSrc[(size_t) (pos + i)]); blk.setSample (1, i, pinkSrc[(size_t) (pos + i)]); }
                pm.processBlock (blk, midi);
                if ((pos / 512) % 8 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
            }
            for (int i = 0; i < 100 && ! pm.match.referenceReady(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
            const bool ready = pm.match.referenceReady() && pm.match.sourceReady();
            std::printf ("Match por la interfaz: referencia %s (%s, %.1f s), pista %.1f s aprendidos\n", ready ? "lista" : "NO LISTA",
                         pm.match.referenceName().toRawUTF8(), pm.match.referenceSeconds(), pm.match.sourceSeconds());
            // mostrar el panel junto a la ventana para la captura
            juce::Component holder; holder.setSize (edm->getWidth() + panel->getWidth() + 20, edm->getHeight());
            holder.addAndMakeVisible (*edm); holder.addAndMakeVisible (*panel); panel->setTopLeftPosition (edm->getWidth() + 20, 80);
            for (int i = 0; i < 10; ++i) { juce::MessageManager::getInstance()->runDispatchLoopUntil (40); gaux::FrameClock::pumpAllForTests (1.0f / 30.0f); }
            snapshot (holder, outDir + "/gaux_match_panel.png");
            // APLICAR
            std::function<juce::Button* (juce::Component*)> findApply = [&] (juce::Component* c) -> juce::Button*
            {
                if (auto* b = dynamic_cast<juce::Button*> (c)) if (b->getButtonText() == "APLICAR MATCH") return b;
                for (auto* ch : c->getChildren()) if (auto* r = findApply (ch)) return r;
                return nullptr;
            };
            auto* applyB = findApply (panel.get());
            if (applyB != nullptr) applyB->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
            int onBands = 0; double err = 0;
            for (int b = 0; b < gaux::kNumBands; ++b) if (pm.params.band (b).on) ++onBands;
            for (double f = 60; f <= 14000; f *= 1.05)
            {
                double m = 0; for (int b = 0; b < gaux::kNumBands; ++b) m += 20 * std::log10 (gaux::analogMagnitudeOfBand (pm.params.band (b), f));
                const double want = 20 * std::log10 (gaux::analogMagnitudeOfBand (hs, f) * gaux::analogMagnitudeOfBand (ls, f));
                err = std::max (err, std::abs (m - want));
            }
            // (el volumen general no cuenta: se compara la forma)
            double off = 0; int cnt = 0;
            for (double f = 60; f <= 12000; f *= 1.05)
            {
                double m = 0; for (int b = 0; b < gaux::kNumBands; ++b) m += 20 * std::log10 (gaux::analogMagnitudeOfBand (pm.params.band (b), f));
                off += 20 * std::log10 (gaux::analogMagnitudeOfBand (hs, f) * gaux::analogMagnitudeOfBand (ls, f)) - m; ++cnt;
            }
            off /= cnt; err = 0;
            for (double f = 60; f <= 12000; f *= 1.05)
            {
                double m = 0; for (int b = 0; b < gaux::kNumBands; ++b) m += 20 * std::log10 (gaux::analogMagnitudeOfBand (pm.params.band (b), f));
                err = std::max (err, std::abs (m + off - 20 * std::log10 (gaux::analogMagnitudeOfBand (hs, f) * gaux::analogMagnitudeOfBand (ls, f))));
            }
            const bool okM = ready && applyB != nullptr && onBands >= 1 && err < 1.0;
            std::printf ("Match aplicado por la interfaz: %d bandas, la curva coincide con el EQ de la referencia (error %.2f dB) %s\n", onBands, err, okM ? "OK" : "FALLA");
            for (int i = 0; i < 6; ++i) { juce::MessageManager::getInstance()->runDispatchLoopUntil (40); gaux::FrameClock::pumpAllForTests (1.0f / 30.0f); }
            snapshot (holder, outDir + "/gaux_match_aplicado.png");
            holder.removeAllChildren();
            panel.reset();
            if (! okM) return 1;
            wav.deleteFile();
        }
    }

    // ---- SKETCH por la interfaz: dibujar con el mouse -> bandas ----
    {
        GauxEqProcessor ps;
        ps.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> eds (ps.createEditor());
        eds->setSize (1160, 760);
        auto* d = dynamic_cast<gaux::SpectrumDisplay*> (findById (eds.get(), "display"));
        bool okS = d != nullptr;
        if (okS)
        {
            // una banda que ya existe (grave +3 dB): el dibujo es el total
            setP (ps, ParamID::band (0, "type"), 1); setP (ps, ParamID::band (0, "freq"), 90); setP (ps, ParamID::band (0, "gain"), 3); setP (ps, ParamID::band (0, "on"), 1);
            ps.history.commit();
            // 1) el boton activa el modo dibujo
            const auto bc = d->sketchButtonBounds().getCentre();
            d->mouseDown (mouseAt (d, bc, bc)); d->mouseUp (mouseAt (d, bc, bc));
            const bool modeOn = d->isSketchMode();
            // 2) trazo: campana de -5 dB en 2.5 kHz y un shelf de +4 dB desde 9 kHz, de 600 Hz a 20 kHz
            gaux::BandSettings bell; bell.on = true; bell.type = 0; bell.freq = 2500; bell.gainDb = -5; bell.q = 1.6;
            gaux::BandSettings hs; hs.on = true; hs.type = 2; hs.freq = 9000; hs.gainDb = 4; hs.q = 0.707;
            auto want = [&] (double f) { return 20 * std::log10 (gaux::analogMagnitudeOfBand (bell, f) * gaux::analogMagnitudeOfBand (hs, f))
                                                + 20 * std::log10 (gaux::analogMagnitudeOfBand (ps.params.band (0), f)); };
            const auto start = d->pointFor (600, want (600));
            d->mouseDown (mouseAt (d, start, start));
            bool ghostSeen = false;
            juce::Random rnd (3);
            int step = 0;
            for (double f = 600; f <= 20000; f *= std::pow (2.0, 1.0 / 14.0), ++step)
            {
                auto pt = d->pointFor (f, want (f)) + juce::Point<float> (0.0f, (float) (rnd.nextDouble() - 0.5) * 2.0f);   // pulso de la mano
                d->mouseDrag (mouseAt (d, pt, start));
                gaux::FrameClock::pumpAllForTests (1.0f / 120.0f);
                juce::Thread::sleep (2);
                ghostSeen = ghostSeen || d->sketchPreviewVisible();
                if (step == 50) snapshot (*eds, outDir + "/gaux_sketch_dibujando.png");
            }
            const auto endP = d->pointFor (20000, want (20000));
            d->mouseUp (mouseAt (d, endP, start));
            for (int i = 0; i < 4; ++i) { juce::MessageManager::getInstance()->runDispatchLoopUntil (20); gaux::FrameClock::pumpAllForTests (1.0f / 30.0f); }
            snapshot (*eds, outDir + "/gaux_sketch_aplicado.png");
            int onB = 0; for (int b = 0; b < gaux::kNumBands; ++b) if (ps.params.band (b).on) ++onB;
            double err = 0, low = 0;
            for (double f = 600; f <= 18000; f *= 1.03)
            {
                double m = 0; for (int b = 0; b < gaux::kNumBands; ++b) m += 20 * std::log10 (gaux::analogMagnitudeOfBand (ps.params.band (b), f));
                err = std::max (err, std::abs (m - want (f)));
            }
            for (double f = 20; f <= 150; f *= 1.05)
            {
                double m = 0; for (int b = 0; b < gaux::kNumBands; ++b) m += 20 * std::log10 (gaux::analogMagnitudeOfBand (ps.params.band (b), f));
                low = std::max (low, std::abs (m - 20 * std::log10 (gaux::analogMagnitudeOfBand (ps.params.band (0), f))));
            }
            // 3) deshacer quita lo dibujado de una vez
            ps.history.undo();
            for (int i = 0; i < 3; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            int afterUndo = 0; for (int b = 0; b < gaux::kNumBands; ++b) if (ps.params.band (b).on) ++afterUndo;
            // 4) sin modo dibujo, arrastrar en zona vacia no dibuja; Cmd/Ctrl + arrastrar si
            d->setSketchMode (false);
            const auto q0 = d->pointFor (300, 0), q1 = d->pointFor (1200, 0);
            d->mouseDown (mouseAt (d, q0, q0)); d->mouseDrag (mouseAt (d, q1, q0)); d->mouseUp (mouseAt (d, q1, q0));
            int plain = 0; for (int b = 0; b < gaux::kNumBands; ++b) if (ps.params.band (b).on) ++plain;
            const juce::ModifierKeys cmd (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier);
            d->mouseDown (mouseAt (d, q0, q0, 1, cmd));
            for (double f = 300; f <= 1200; f *= 1.06) d->mouseDrag (mouseAt (d, d->pointFor (f, 6.0 * std::sin ((f - 300) / 900 * 3.14159)), q0, 1, cmd));
            d->mouseUp (mouseAt (d, q1, q0, 1, cmd));
            int withCmd = 0; for (int b = 0; b < gaux::kNumBands; ++b) if (ps.params.band (b).on) ++withCmd;
            okS = modeOn && ghostSeen && onB >= 2 && onB <= 6 && err < 1.0 && low < 0.5 && afterUndo == 1 && plain == 1 && withCmd >= 2;
            std::printf ("SKETCH por la interfaz: boton %s, vista previa en vivo %s, %d bandas nuevas, error del trazo %.2f dB, graves intactos %.2f dB, "
                         "deshacer %s, sin modo no dibuja %s, Cmd+arrastrar dibuja %s -> %s\n",
                         modeOn ? "OK" : "FALLA", ghostSeen ? "OK" : "FALLA", onB - 1, err, low, afterUndo == 1 ? "OK" : "FALLA",
                         plain == 1 ? "OK" : "FALLA", withCmd >= 2 ? "OK" : "FALLA", okS ? "OK" : "FALLA");
        }
        if (! okS) return 1;
    }

    // ---- CAZA DE PICOS por la interfaz: la resonancia se marca; clic la doma, arrastrar la recorta ----
    {
        GauxEqProcessor pk;
        pk.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edk (pk.createEditor());
        edk->setSize (1160, 760);
        auto* d = dynamic_cast<gaux::SpectrumDisplay*> (findById (edk.get(), "display"));
        juce::Random rnd (11);
        double ph1 = 0, ph2 = 0, lp = 0;
        auto play = [&] (double seconds, bool tones)
        {
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer mid;
            for (int blk = 0; blk < (int) (seconds * 48000 / 512); ++blk)
            {
                for (int i = 0; i < 512; ++i)
                {
                    lp += 0.25 * ((rnd.nextDouble() * 2 - 1) - lp);   // ruido oscuro (como una mezcla)
                    double v = 0.25 * lp;
                    if (tones) { v += 0.03 * std::sin (ph1) + 0.05 * std::sin (ph2); ph1 += 2 * 3.14159265 * 2500 / 48000; ph2 += 2 * 3.14159265 * 180 / 48000; }
                    buf.setSample (0, i, (float) v); buf.setSample (1, i, (float) v);
                }
                pk.processBlock (buf, mid);
                d->advanceFrame (512.0f / 48000.0f);
            }
        };
        play (3.0, true);
        auto find = [&] (double f) -> const gaux::SpectrumDisplay::GrabPeak*
        {
            for (auto& p : d->grabPeaks()) if (p.alive && std::abs (std::log2 (p.f / f)) < 1.0 / 12.0) return &p;
            return nullptr;
        };
        const auto* p25 = find (2500); const auto* p180 = find (180);
        const int count = (int) d->grabPeaks().size();
        bool okP = p25 != nullptr && p180 != nullptr && count <= 8;
        std::printf ("Caza de picos: %d marcas; 2.5 kHz %s (+%.1f dB, Q %.1f), 180 Hz %s\n", count,
                     p25 ? "encontrado" : "NO", p25 ? p25->prom : 0.0f, p25 ? p25->q : 0.0, p180 ? "encontrado" : "NO");
        snapshot (*edk, outDir + "/gaux_picos.png");
        if (okP)
        {
            // 1) clic sin arrastrar sobre 2.5 kHz: campana de recorte sugerido, en su frecuencia
            const auto m = d->peakMarker (*p25);
            const float prom = p25->prom;
            d->mouseMove (mouseAt (d, m, m));
            snapshot (*edk, outDir + "/gaux_picos_hover.png");
            d->mouseDown (mouseAt (d, m, m)); d->mouseUp (mouseAt (d, m, m));
            const auto b0 = pk.params.band (0);
            const bool tame = b0.on && b0.type == 0 && std::abs (std::log2 (b0.freq / 2500.0)) < 1.0 / 24.0 && b0.gainDb < -3.0
                              && std::abs (b0.gainDb + std::min (12.0f, prom * 0.75f)) < 0.2 && b0.q >= 1.0;
            std::printf ("  clic = domar: banda en %.0f Hz, %+.1f dB, Q %.1f %s\n", b0.freq, b0.gainDb, b0.q, tame ? "OK" : "FALLA");
            // y el pico baja en el analizador
            play (2.5, true);
            const auto* after = find (2500);
            const bool lower = after == nullptr || after->prom < prom - 3.0f;
            std::printf ("  despues de domarlo el pico %s\n", lower ? (after ? "sobresale menos OK" : "ya no se marca OK") : "SIGUE IGUAL (FALLA)");
            // 2) arrastrar el de 180 Hz hacia abajo: recorta lo que se arrastra
            const auto* q180 = find (180);
            bool drag = false;
            if (q180 != nullptr)
            {
                const auto m2 = d->peakMarker (*q180);
                d->mouseDown (mouseAt (d, m2, m2));
                const auto to = m2 + juce::Point<float> (0.0f, d->pointFor (1000, -8).y - d->pointFor (1000, 0).y);
                for (int i = 1; i <= 10; ++i) d->mouseDrag (mouseAt (d, m2 + (to - m2) * (i / 10.0f), m2));
                d->mouseUp (mouseAt (d, to, m2));
                const auto b1 = pk.params.band (1);
                drag = b1.on && std::abs (std::log2 (b1.freq / 180.0)) < 1.0 / 12.0 && std::abs (b1.gainDb + 8.0) < 0.6;
                std::printf ("  arrastrar = recortar: banda en %.0f Hz, %+.1f dB %s\n", b1.freq, b1.gainDb, drag ? "OK" : "FALLA");
            }
            // 3) en silencio las marcas se van y la pantalla vuelve a reposo
            play (5.0, false);
            juce::AudioBuffer<float> sil (2, 512); juce::MidiBuffer mid;
            int drew = 0;
            for (int f = 0; f < 600; ++f) { if (f % 2 == 0) { sil.clear(); pk.processBlock (sil, mid); } d->advanceFrame (1.0f / 120.0f); if (f >= 480 && d->lastFrameDrew()) ++drew; }
            const bool rest = drew == 0 && d->grabPeaks().empty();
            std::printf ("  en silencio: marcas %d, cuadros dibujados %d %s\n", (int) d->grabPeaks().size(), drew, rest ? "OK" : "FALLA");
            okP = tame && lower && drag && rest;
        }
        std::printf ("CAZA DE PICOS %s\n", okP ? "OK" : "FALLA");
        if (! okP) return 1;
    }

    // ---- CHOQUES entre dos Gaux del mismo proyecto (kick vs bajo) ----
    {
        auto kick = std::make_unique<GauxEqProcessor>(); auto bass = std::make_unique<GauxEqProcessor>();
        kick->prepareToPlay (48000.0, 512); bass->prepareToPlay (48000.0, 512);
        juce::AudioProcessor::TrackProperties tk; tk.name = juce::String ("KICK"); tk.colourARGB = 0xffff5a3c; kick->updateTrackProperties (tk);
        juce::AudioProcessor::TrackProperties tb; tb.name = juce::String ("BAJO"); tb.colourARGB = 0xff4aa8ff; bass->updateTrackProperties (tb);
        std::unique_ptr<juce::AudioProcessorEditor> edc (kick->createEditor());
        edc->setSize (1160, 760);
        auto* d = dynamic_cast<gaux::SpectrumDisplay*> (findById (edc.get(), "display"));
        const bool idle = ! kick->clashHub->wanted();
        // la lista ve a la otra pista con su nombre
        bool listed = false; int bassId = -1;
        for (auto& i : kick->clashHub->list()) if (i.name == "BAJO") { listed = true; bassId = i.id; }
        d->setClashTarget (bassId);
        const bool wanted = kick->clashHub->wanted();
        juce::Random rnd (5);
        double t = 0;
        auto play = [&] (double secs)
        {
            juce::AudioBuffer<float> bk (2, 512), bb (2, 512); juce::MidiBuffer mid;
            for (int blk = 0; blk < (int) (secs * 48000 / 512); ++blk)
            {
                for (int i = 0; i < 512; ++i, t += 1.0 / 48000)
                {
                    const double beat = std::fmod (t, 0.5);
                    // kick: golpe de 60 Hz que baja de tono + click en 3 kHz
                    const double env = std::exp (-beat * 9.0), f = 55 + 90 * std::exp (-beat * 30.0);
                    const double k = 0.7 * env * std::sin (2 * 3.14159265 * f * beat) + 0.05 * std::exp (-beat * 200.0) * std::sin (2 * 3.14159265 * 3000 * beat);
                    // bajo: 55 Hz + armonicos (hasta ~500 Hz), sostenido
                    double bv = 0; for (int h = 1; h <= 8; ++h) bv += 0.25 / h * std::sin (2 * 3.14159265 * 55 * h * t);
                    bk.setSample (0, i, (float) k); bk.setSample (1, i, (float) k);
                    bb.setSample (0, i, (float) bv); bb.setSample (1, i, (float) bv);
                }
                kick->processBlock (bk, mid); bass->processBlock (bb, mid);
                if (blk % 3 == 0) { kick->clashHub->pumpForTests (3 * 512.0f / 48000.0f); d->advanceFrame (3 * 512.0f / 48000.0f); }
            }
        };
        play (4.0);
        {
            std::array<float, gaux::kClashPoints> sa, sl, oa, ol; bool ha, hb;
            kick->clashHub->read (kick->clashSource.id, sa, sl, ha); kick->clashHub->read (bassId, oa, ol, hb);
            std::array<float, gaux::kClashPoints> st; gaux::clashStrength (sl, ol, st);
            if (std::getenv ("CLASH_DBG")) for (int i = 0; i < gaux::kClashPoints; i += 8) std::printf ("   %6.0f Hz: kick %.1f bajo %.1f choque %.2f\n", gaux::clashFreq (i), sl[(size_t) i], ol[(size_t) i], st[(size_t) i]);
        }
        const auto zones = d->getClashZones();
        bool lowZone = false, highZone = false;
        for (auto& z : zones)
        {
            std::printf ("  zona de choque %.0f - %.0f Hz (centro %.0f, fuerza %.2f)\n", z.fLo, z.fHi, z.fCenter, z.strength);
            if (z.fLo < 120 && z.fHi > 50) lowZone = true;
            if (z.fLo > 1500) highZone = true;
        }
        bool okC = idle && listed && wanted && lowZone && ! highZone;
        std::printf ("Choques: en reposo no se copia audio %s, la otra pista aparece con su nombre %s, choque kick/bajo en graves %s, sin choques falsos en agudos %s\n",
                     idle ? "OK" : "FALLA", listed ? "OK" : "FALLA", lowZone ? "OK" : "FALLA", ! highZone ? "OK" : "FALLA");
        if (lowZone)
        {
            const gaux::ClashZone* zl = nullptr;
            for (auto& z : zones) if (z.fLo < 120 && z.fHi > 50) { zl = &z; break; }
            const auto h = d->clashZoneHandle (*zl);
            d->mouseMove (mouseAt (d, h, h));
            for (int i = 0; i < 3; ++i) d->advanceFrame (1.0f / 60.0f);
            snapshot (*edc, outDir + "/gaux_choques.png");
            d->mouseDown (mouseAt (d, h, h)); d->mouseUp (mouseAt (d, h, h));
            const auto b0 = kick->params.band (0);
            const bool made = b0.on && b0.type == 0 && b0.gainDb < -1.5 && b0.freq > zl->fLo * 0.9 && b0.freq < zl->fHi * 1.1;
            std::printf ("  clic en el choque: campana en %.0f Hz, %+.1f dB, Q %.2f %s\n", b0.freq, b0.gainDb, b0.q, made ? "OK" : "FALLA");
            okC = okC && made;
        }
        // la otra pista se borra del proyecto mientras se mira: no pasa nada, el choque se apaga solo
        bass.reset();
        kick->clashHub->pumpForTests (0.02f);
        for (int i = 0; i < 3; ++i) d->advanceFrame (1.0f / 60.0f);
        const bool gone = d->getClashTarget() < 0;
        edc.reset();   // al cerrar la ventana nadie mira: el audio deja de copiarse
        const bool released = ! kick->clashHub->wanted();
        std::printf ("  la otra pista se borra: %s; al cerrar la ventana deja de copiar audio: %s\n", gone ? "OK" : "FALLA", released ? "OK" : "FALLA");
        okC = okC && gone && released;
        {
            // nombre puesto a mano: manda sobre el del DAW y se guarda con la sesion
            kick->setUserTrackName ("KICK PRINCIPAL");
            juce::MemoryBlock mb; kick->getStateInformation (mb);
            GauxEqProcessor other; other.setStateInformation (mb.getData(), (int) mb.getSize());
            juce::AudioProcessor::TrackProperties t2; t2.name = juce::String ("Audio 3"); other.updateTrackProperties (t2);
            const bool named = other.clashSource.getName() == "KICK PRINCIPAL";
            std::printf ("  nombre de la pista a mano, guardado en la sesion: %s\n", named ? "OK" : "FALLA");
            okC = okC && named;
        }
        std::printf ("CHOQUES %s\n", okC ? "OK" : "FALLA");
        if (! okC) return 1;
    }

    // ---- Detalles de uso: escribir valores, copiar/pegar curvas, varias bandas a la vez, opciones de la pantalla ----
    {
        bool okU = true;
        // 1) escribir la frecuencia: numeros, "k" y NOTAS
        {
            GauxEqProcessor pw;
            auto* fp = pw.apvts.getParameter (ParamID::band (0, "freq"));
            struct T { const char* txt; double want; };
            const T ts[] = { { "A4", 440.0 }, { "C#2", 69.296 }, { "Eb1", 38.891 }, { "2k5", 2500.0 }, { "1.2 kHz", 1200.0 }, { "850", 850.0 }, { "g3", 196.0 } };
            bool ok = true; juce::String row;
            for (auto& t : ts)
            {
                const double got = fp->convertFrom0to1 (fp->getValueForText (t.txt));
                const bool g = std::abs (got - t.want) / t.want < 0.002;
                ok = ok && g; row << t.txt << " = " << juce::String (got, 1) << " Hz  ";
            }
            std::printf ("Escribir frecuencias: %s %s\n", row.toRawUTF8(), ok ? "OK" : "FALLA");
            okU = okU && ok;
        }
        // 2) copiar la curva de una pista y pegarla en otra (y deshacer)
        {
            GauxEqProcessor pa, pb;
            setP (pa, ParamID::band (0, "on"), 1); setP (pa, ParamID::band (0, "type"), 3); setP (pa, ParamID::band (0, "freq"), 35); setP (pa, ParamID::band (0, "slope"), 3);
            setP (pa, ParamID::band (4, "on"), 1); setP (pa, ParamID::band (4, "freq"), 2500); setP (pa, ParamID::band (4, "gain"), -3.5f); setP (pa, ParamID::band (4, "q"), 2.2f);
            setP (pa, ParamID::band (4, "stereo"), 4); setP (pa, ParamID::band (4, "dyn"), 1); setP (pa, ParamID::band (4, "thr"), -18);
            setP (pb, ParamID::band (7, "on"), 1);   // (lo que tenia la otra se reemplaza)
            pb.history.commit();
            const auto txt = pa.curveToText();
            const bool pasted = pb.curveFromText (txt);
            bool same = pasted;
            for (int b = 0; b < gaux::kNumBands; ++b)
            {
                const auto x = pa.params.band (b), y = pb.params.band (b);
                same = same && x.on == y.on;
                if (x.on) same = same && x.type == y.type && std::abs (x.freq - y.freq) < 0.01 && std::abs (x.gainDb - y.gainDb) < 0.01 && std::abs (x.q - y.q) < 0.001
                                   && x.slope == y.slope && x.stereo == y.stereo && x.dynOn == y.dynOn && std::abs (x.thresholdDb - y.thresholdDb) < 0.01;
            }
            pb.history.undo();
            const bool undone = pb.params.band (7).on && ! pb.params.band (4).on;
            const bool junk = ! pb.curveFromText ("hola, esto no es una curva");
            std::printf ("Copiar/pegar la curva: identica %s, deshacer %s, texto ajeno ignorado %s\n", same ? "OK" : "FALLA", undone ? "OK" : "FALLA", junk ? "OK" : "FALLA");
            okU = okU && same && undone && junk;
        }
        // 3) varias bandas a la vez: Shift + clic, arrastrar el grupo, recuadro, Supr
        {
            GauxEqProcessor pm;
            pm.prepareToPlay (48000.0, 512);
            for (auto [b, f, g] : { std::tuple<int, float, float> { 0, 200.0f, 3.0f }, { 1, 800.0f, -2.0f }, { 2, 5000.0f, 4.0f } })
            { setP (pm, ParamID::band (b, "on"), 1); setP (pm, ParamID::band (b, "type"), 0); setP (pm, ParamID::band (b, "freq"), f); setP (pm, ParamID::band (b, "gain"), g); }
            pm.history.commit();
            std::unique_ptr<juce::AudioProcessorEditor> edm (pm.createEditor());
            edm->setSize (1160, 760);
            auto* d = dynamic_cast<gaux::SpectrumDisplay*> (findById (edm.get(), "display"));
            for (int i = 0; i < 3; ++i) d->advanceFrame (1.0f / 60.0f);
            const auto n0 = d->nodePosition (0), n1 = d->nodePosition (1);
            d->mouseDown (mouseAt (d, n0, n0)); d->mouseUp (mouseAt (d, n0, n0));
            const juce::ModifierKeys sh (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
            d->mouseDown (mouseAt (d, n1, n1, 1, sh)); d->mouseUp (mouseAt (d, n1, n1, 1, sh));
            const bool two = d->selectionCount() == 2;
            // arrastrar la banda 1 una octava arriba y +2 dB: la 0 se mueve igual (y la 2 no)
            const auto to = d->pointFor (1600.0, 0.0);
            d->mouseDown (mouseAt (d, n1, n1));
            for (int i = 1; i <= 8; ++i) d->mouseDrag (mouseAt (d, n1 + (to - n1) * (i / 8.0f), n1));
            d->mouseUp (mouseAt (d, to, n1));
            const auto b0 = pm.params.band (0), b1 = pm.params.band (1), b2 = pm.params.band (2);
            const bool moved = std::abs (b1.freq - 1600) < 20 && std::abs (b0.freq - 400) < 6 && std::abs (b0.gainDb - (3.0 + (b1.gainDb + 2.0))) < 0.05 && std::abs (b2.freq - 5000) < 1 && std::abs (b2.gainDb - 4) < 0.01;
            std::printf ("Varias bandas: Shift+clic elige 2 %s; arrastrar el grupo (banda 1 -> %.0f Hz %+.1f dB): la 0 queda en %.0f Hz %+.1f dB, la 2 quieta %s\n",
                         two ? "OK" : "FALLA", b1.freq, b1.gainDb, b0.freq, b0.gainDb, moved ? "OK" : "FALLA");
            // recuadro alrededor de las bandas 1 y 2, y Supr
            const auto p1 = d->nodePosition (1), p2 = d->nodePosition (2);
            const juce::Point<float> c0 (juce::jmin (p1.x, p2.x) - 25.0f, juce::jmin (p1.y, p2.y) - 25.0f), c1 (juce::jmax (p1.x, p2.x) + 25.0f, juce::jmax (p1.y, p2.y) + 25.0f);
            const auto empty = c0;   // (zona vacia)
            d->mouseDown (mouseAt (d, empty, empty));
            for (int i = 1; i <= 6; ++i) d->mouseDrag (mouseAt (d, c0 + (c1 - c0) * (i / 6.0f), c0));
            snapshot (*edm, outDir + "/gaux_seleccion.png");
            d->mouseUp (mouseAt (d, c1, c0));
            const bool lassoOk = d->isBandInSelection (1) && d->isBandInSelection (2) && ! d->isBandInSelection (0);
            d->keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
            const bool del = pm.params.band (0).on && ! pm.params.band (1).on && ! pm.params.band (2).on;
            pm.history.undo();
            const bool back = pm.params.band (1).on && pm.params.band (2).on;
            std::printf ("Recuadro elige las bandas 1 y 2 %s, Supr las borra %s, Cmd+Z las devuelve %s\n", lassoOk ? "OK" : "FALLA", del ? "OK" : "FALLA", back ? "OK" : "FALLA");
            okU = okU && two && moved && lassoOk && del && back;

            // 4) opciones de la pantalla: rango, congelar, piano
            const float y12a = d->pointFor (1000, 12).y;
            d->setViewOption ("dispRange", 1);   // +-12 dB
            const float y12b = d->pointFor (1000, 12).y;
            const bool rangeOk = y12b < y12a - 50;
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer mid; juce::Random rr (2);
            auto feed = [&] (float amp, int blocks) { for (int k = 0; k < blocks; ++k) { for (int i = 0; i < 512; ++i) { const float v = amp * (rr.nextFloat() * 2 - 1); buf.setSample (0, i, v); buf.setSample (1, i, v); } pm.processBlock (buf, mid); d->advanceFrame (512.0f / 48000.0f); } };
            feed (0.3f, 60);
            d->setViewOption ("anFreeze", 1);
            const auto frozen = d->analyzerShapeBounds();
            feed (0.02f, 30);
            const int drew = d->analyzerShapeBounds() == frozen ? 0 : 1;
            for (int i = 0; i < 2; ++i) d->advanceFrame (1.0f / 60.0f);
            snapshot (*edm, outDir + "/gaux_piano.png");
            d->setViewOption ("anFreeze", 0);
            feed (0.02f, 30);
            const int drew2 = d->analyzerShapeBounds() == frozen ? 0 : 1;
            // se guardan con la sesion
            juce::MemoryBlock mb; pm.getStateInformation (mb);
            GauxEqProcessor pr; pr.setStateInformation (mb.getData(), (int) mb.getSize());
            const bool saved = (int) pr.apvts.state.getProperty ("dispRange", 2) == 1 && (bool) pr.apvts.state.getProperty ("anFreeze", true) == false;
            std::printf ("Pantalla: rango +-12 dB %s, congelar (el analizador queda quieto: %d cambios) %s, al descongelar vuelve %s, se guarda con la sesion %s\n",
                         rangeOk ? "OK" : "FALLA", drew, drew == 0 ? "OK" : "FALLA", drew2 > 0 ? "OK" : "FALLA", saved ? "OK" : "FALLA");
            okU = okU && rangeOk && drew == 0 && drew2 > 0 && saved;
        }
        // 5) teclado de piano abajo: alineado con la pantalla, clic = llevar la banda elegida a esa nota
        {
            GauxEqProcessor pq;
            pq.prepareToPlay (48000.0, 512);
            setP (pq, ParamID::band (0, "on"), 1); setP (pq, ParamID::band (0, "freq"), 452.0f);
            std::unique_ptr<juce::AudioProcessorEditor> edq (pq.createEditor());
            edq->setSize (1160, 760);
            auto* d = dynamic_cast<gaux::SpectrumDisplay*> (findById (edq.get(), "display"));
            auto* pk = dynamic_cast<gaux::PianoStrip*> (findById (edq.get(), "piano"));
            bool ok = d != nullptr && pk != nullptr && pk->isVisible();
            if (ok)
            {
                const auto n0 = d->nodePosition (0);
                d->mouseDown (mouseAt (d, n0, n0)); d->mouseUp (mouseAt (d, n0, n0));
                pk->timerCallback();
                const bool lit = pk->litBandNote() == 69;   // 452 Hz ~ A4
                // la tecla A4 queda justo debajo de 440 Hz en la pantalla
                const float xa = pk->keyCentre (69) + (float) (pk->getX() - d->getX());
                const bool aligned = std::abs (xa - d->xForFreq (440.0)) < 0.5f;
                const juce::Point<float> kp (pk->keyCentre (57), (float) pk->getHeight() - 3.0f);   // A3
                pk->mouseDown (mouseAt (pk, kp, kp));
                const bool tuned = std::abs (pq.params.band (0).freq - 220.0) < 0.05;
                for (int i = 0; i < 3; ++i) d->advanceFrame (1.0f / 60.0f);
                snapshot (*edq, outDir + "/gaux_teclado.png");
                ok = lit && aligned && tuned;
                std::printf ("Teclado: nota de la banda iluminada %s, alineado con la pantalla %s, clic en A3 lleva la banda a %.1f Hz %s\n",
                             lit ? "OK" : "FALLA", aligned ? "OK" : "FALLA", pq.params.band (0).freq, tuned ? "OK" : "FALLA");
            }
            okU = okU && ok;
        }
        std::printf ("DETALLES DE USO %s\n", okU ? "OK" : "FALLA");
        if (! okU) return 1;
    }

    // Escena 2: modo vintage
    setP (p, ParamID::mode, 1);
    setP (p, ParamID::vLowBoost, 6); setP (p, ParamID::vLowAtten, 4); setP (p, ParamID::vHighBoost, 5);
    setP (p, ParamID::vHighBw, 7); setP (p, ParamID::vAirGain, 6); setP (p, ParamID::vTube, 40);
    setP (p, ParamID::vLmBoost, 4); setP (p, ParamID::vDip, 3); setP (p, ParamID::vDipFreq, 2); setP (p, ParamID::vHmBoost, 5);
    setP (p, ParamID::vLowCut, 1); setP (p, ParamID::vHighCut, 9); setP (p, ParamID::vIron, 30);
    for (int q = 0; q < 3; ++q)
    {
        setP (p, ParamID::quality, (float) q);
        const bool f = processNoise (p, 200, peak, ms);
        ok = ok && f;
        std::printf ("Vintage calidad %d: finito=%d pico=%.3f  %.3f ms/bloque\n", q, f, peak, ms);
    }
    setP (p, ParamID::quality, 0);
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* editor = dynamic_cast<GauxEqEditor*> (ed.get());
        editor->setUiScale (1.0f);
        for (int i = 0; i < 25; ++i) { processNoise (p, 4, peak, ms); juce::MessageManager::getInstance()->runDispatchLoopUntil (30); pumpFrames (*ed); }
        snapshot (*ed, outDir + "/gaux_vintage.png");
        editor->setAdvanced (true);
        for (int i = 0; i < 25; ++i) { processNoise (p, 4, peak, ms); juce::MessageManager::getInstance()->runDispatchLoopUntil (30); pumpFrames (*ed); }
        snapshot (*ed, outDir + "/gaux_vintage_avanzado.png");
    }

    // ---- Motor v0.7: rango de proceso, sidechain externo, threshold automatico ----
    bool engOk = true;
    {
        // a) Rango activo con el EQ plano: la senal sale identica (en las 3 calidades)
        for (int q = 0; q < 3; ++q)
        {
            GauxEqProcessor pr; pr.prepareToPlay (48000.0, 512);
            setP (pr, ParamID::quality, (float) q);
            setP (pr, ParamID::rangeOn, 1); setP (pr, ParamID::rangeLo, 150); setP (pr, ParamID::rangeHi, 6000);
            processSine (pr, 1000, 0.3f, 4);                  // prepara la calidad
            GauxEqProcessor ref; ref.prepareToPlay (48000.0, 512);
            setP (ref, ParamID::quality, (float) q);
            processSine (ref, 1000, 0.3f, 4);
            double worst = 0;
            for (double f : { 60.0, 150.0, 1000.0, 6000.0, 12000.0 })
            {
                const auto a = processSine (pr, f, 0.3f, 40), b = processSine (ref, f, 0.3f, 40);
                worst = std::max (worst, std::abs (a.outDb - b.outDb));
            }
            const bool okq = worst < 0.1;
            std::printf ("Rango con EQ plano (calidad %d): diferencia maxima %.4f dB %s\n", q, worst, okq ? "OK" : "FALLO");
            engOk = engOk && okq;
        }

        // b) Bell +12 dB en 60 Hz con el rango 200 Hz-20 kHz: los 60 Hz no se tocan, 1 kHz si
        {
            GauxEqProcessor pr; pr.prepareToPlay (48000.0, 512);
            setP (pr, ParamID::band (0, "on"), 1); setP (pr, ParamID::band (0, "type"), (float) BandType::Bell);
            setP (pr, ParamID::band (0, "freq"), 60); setP (pr, ParamID::band (0, "gain"), 12); setP (pr, ParamID::band (0, "q"), 1);
            setP (pr, ParamID::band (1, "on"), 1); setP (pr, ParamID::band (1, "type"), (float) BandType::Bell);
            setP (pr, ParamID::band (1, "freq"), 1000); setP (pr, ParamID::band (1, "gain"), 6); setP (pr, ParamID::band (1, "q"), 1);
            const double in60 = 20 * std::log10 (0.3 / std::sqrt (2.0));
            const double off60 = processSine (pr, 60, 0.3f, 40).outDb - in60;
            setP (pr, ParamID::rangeOn, 1); setP (pr, ParamID::rangeLo, 200); setP (pr, ParamID::rangeHi, 20000);
            const double on60 = processSine (pr, 60, 0.3f, 60).outDb - in60;
            const double on1k = processSine (pr, 1000, 0.3f, 40).outDb - in60;
            const bool okb = off60 > 11 && std::abs (on60) < 0.6 && on1k > 5.5;
            std::printf ("Rango 200 Hz-20 kHz: 60 Hz sin rango %+.2f dB, con rango %+.2f dB, 1 kHz %+.2f dB %s\n",
                         off60, on60, on1k, okb ? "OK" : "FALLO");
            engOk = engOk && okb;
        }

        // c) Sidechain externo: la banda dinamica reacciona a la otra pista, no a la propia
        {
            GauxEqProcessor pr;
            pr.enableAllBuses();
            pr.prepareToPlay (48000.0, 512);
            setP (pr, ParamID::band (0, "on"), 1); setP (pr, ParamID::band (0, "type"), (float) BandType::Bell);
            setP (pr, ParamID::band (0, "freq"), 1000); setP (pr, ParamID::band (0, "gain"), 0); setP (pr, ParamID::band (0, "q"), 1);
            setP (pr, ParamID::band (0, "dyn"), 1); setP (pr, ParamID::band (0, "thr"), -30); setP (pr, ParamID::band (0, "range"), -10);
            setP (pr, ParamID::band (0, "ext"), 1);
            processSine (pr, 1000, 0.5f, 40, 0.0f);
            const float quiet = pr.dynGainDb[0].load();
            const bool scQuiet = ! pr.sidechainActive.load();
            processSine (pr, 1000, 0.5f, 60, 0.5f, 1000);
            const float loud = pr.dynGainDb[0].load();
            const bool okc = std::abs (quiet) < 0.5f && loud < -6.0f && scQuiet && pr.sidechainActive.load();
            std::printf ("Sidechain EXT: sin sidechain %+.1f dB, con sidechain %+.1f dB %s\n", quiet, loud, okc ? "OK" : "FALLO");
            engOk = engOk && okc;
        }

        // c2) Sidechain general: la curva se aplana cuando suena la otra pista (en los dos modos)
        {
            GauxEqProcessor pr;
            pr.enableAllBuses();
            pr.prepareToPlay (48000.0, 512);
            setP (pr, ParamID::band (0, "on"), 1); setP (pr, ParamID::band (0, "type"), (float) BandType::Bell);
            setP (pr, ParamID::band (0, "freq"), 1000); setP (pr, ParamID::band (0, "gain"), 12); setP (pr, ParamID::band (0, "q"), 1);
            setP (pr, ParamID::scOn, 1); setP (pr, ParamID::scDuck, 100); setP (pr, ParamID::scThr, -30);
            const double in = 20 * std::log10 (0.1 / std::sqrt (2.0));
            const double quiet = processSine (pr, 1000, 0.1f, 40, 0.0f).outDb - in;
            const double loud = processSine (pr, 1000, 0.1f, 60, 0.5f, 60).outDb - in;
            setP (pr, ParamID::mode, 1); setP (pr, ParamID::vHighFreq, 0); setP (pr, ParamID::vHighBoost, 10);
            const double vq = processSine (pr, 3000, 0.1f, 60, 0.0f).outDb - in;
            const double vl = processSine (pr, 3000, 0.1f, 60, 0.5f, 60).outDb - in;
            const bool okg = quiet > 11 && std::abs (loud) < 1.0 && vq > 8 && std::abs (vl) < 1.5;
            std::printf ("Sidechain general: parametrico %+.1f -> %+.1f dB, vintage %+.1f -> %+.1f dB al sonar el kick %s\n",
                         quiet, loud, vq, vl, okg ? "OK" : "FALLO");
            engOk = engOk && okg;
        }

        // d) Threshold automatico: se ubica justo sobre el nivel medio de la banda
        {
            GauxEqProcessor pr; pr.prepareToPlay (48000.0, 512);
            setP (pr, ParamID::band (0, "on"), 1); setP (pr, ParamID::band (0, "type"), (float) BandType::Bell);
            setP (pr, ParamID::band (0, "freq"), 1000); setP (pr, ParamID::band (0, "q"), 1);
            setP (pr, ParamID::band (0, "dyn"), 1); setP (pr, ParamID::band (0, "range"), -10);
            setP (pr, ParamID::band (0, "autoThr"), 1);
            processSine (pr, 1000, 0.2f, 600);
            const float thr = pr.thresholdDb[0].load(), det = pr.detectorDb[0].load();
            const bool okd = std::abs (thr - (det + 3.0f)) < 2.0f;
            std::printf ("Threshold auto: detector %.1f dB, threshold %.1f dB %s\n", det, thr, okd ? "OK" : "FALLO");
            engOk = engOk && okd;
        }
    }
    // ---- Deshacer / rehacer y presets ----
    {
        GauxEqProcessor pu; pu.prepareToPlay (48000.0, 512);
        auto* g1 = pu.apvts.getParameter (ParamID::band (0, "gain"));
        auto settle = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (20); };
        g1->setValueNotifyingHost (g1->convertTo0to1 (6.0f)); pu.history.commit();
        g1->setValueNotifyingHost (g1->convertTo0to1 (-3.0f)); pu.history.commit();
        pu.history.undo();
        const float afterUndo = pu.params.bands[0].gain->load();
        pu.history.redo();
        const float afterRedo = pu.params.bands[0].gain->load();
        const bool okU = std::abs (afterUndo - 6.0f) < 0.05f && std::abs (afterRedo + 3.0f) < 0.05f;
        std::printf ("Deshacer: %.1f dB, rehacer: %.1f dB %s\n", afterUndo, afterRedo, okU ? "OK" : "FALLO");
        engOk = engOk && okU;

        bool allOk = true, modeKept = true; double worst = 0; int count = 0;
        for (int mode : { 0, 1 })
        {
            setP (pu, ParamID::mode, (float) mode);
            const int n = (int) pu.presets.entries().size();
            for (int i = 0; i < n; ++i)
            {
                pu.presets.load (i); ++count;
                modeKept = modeKept && ((pu.params.mode->load() > 0.5f) == (mode == 1));
                double pk = 0, ms2 = 0;
                const bool fin = processNoise (pu, 30, pk, ms2);
                worst = std::max (worst, pk);
                if (! fin || pk > 4.0) { allOk = false; std::printf ("  preset %s: FALLO (pico %.2f)\n", pu.presets.entries()[(size_t) i].name.toRawUTF8(), pk); }
            }
            // Pasar con las flechas tampoco cambia de modo
            for (int k = 0; k < n + 2; ++k) { pu.presets.step (1); modeKept = modeKept && ((pu.params.mode->load() > 0.5f) == (mode == 1)); }
        }
        // Un preset vintage no toca las bandas del parametrico
        setP (pu, ParamID::mode, 0); pu.presets.load (0);
        const float bandBefore = pu.params.bands[1].gain->load();
        setP (pu, ParamID::mode, 1); pu.presets.load (2);
        const bool untouched = std::abs (pu.params.bands[1].gain->load() - bandBefore) < 0.01f;
        setP (pu, ParamID::mode, 0);
        std::printf ("Presets separados por modo: %d probados, el modo no cambia %s, el otro modo queda intacto %s\n",
                     count, modeKept ? "OK" : "FALLO", untouched ? "OK" : "FALLO");
        allOk = allOk && modeKept && untouched;
        pu.presets.load (0); settle();
        const bool nameOk = pu.presets.currentName() == pu.presets.entries()[0].name && pu.params.bands[0].on->load() > 0.5f;
        pu.presets.load (5);
        pu.history.undo();
        const bool presetUndo = std::abs (pu.params.bands[1].gain->load() - (-2.5f)) < 0.05f;   // vuelve al preset 0
        std::printf ("Presets de fabrica: %d cargados, pico maximo %.2f %s; nombre %s; deshacer preset %s\n",
                     count, worst, allOk ? "OK" : "FALLO", nameOk ? "OK" : "FALLO", presetUndo ? "OK" : "FALLO");
        engOk = engOk && allOk && nameOk && presetUndo;

        // Guardar y volver a cargar un preset del usuario
        const bool saved = pu.presets.saveUser ("Prueba GAUX");
        const int idx = pu.presets.currentIndex();
        pu.presets.load (3); settle();
        pu.presets.load (idx); settle();
        const bool userOk = saved && pu.presets.currentName() == "Prueba GAUX" && std::abs (pu.params.bands[1].gain->load() - (-2.5f)) < 0.05f;
        std::printf ("Preset del usuario guardado y cargado: %s\n", userOk ? "OK" : "FALLO");
        gaux::PresetManager::userFolder().getChildFile ("Prueba GAUX.gauxpreset").deleteFile();
        engOk = engOk && userOk;
    }

    std::printf ("MOTOR v0.7 %s\n", engOk ? "OK" : "FALLO");
    ok = ok && engOk;

    // ---- Pruebas de interaccion con el mouse ----
    bool interOk = true;
    {
        GauxEqProcessor pi;
        pi.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> ed (pi.createEditor());
        auto* d = dynamic_cast<gaux::SpectrumDisplay*> (findById (ed.get(), "display"));
        auto* be = findById (ed.get(), "bandEditor");
        auto freqOf = [&] (int b) { return pi.params.bands[(size_t) b].freq->load(); };
        auto gainOf = [&] (int b) { return pi.params.bands[(size_t) b].gain->load(); };

        // 1) Un clic simple en zona vacia NO crea banda; el doble clic si (y se puede arrastrar de una vez)
        const juce::Point<float> p0 (500.0f, 120.0f);
        d->mouseDown (mouseAt (d, p0, p0));
        d->mouseUp (mouseAt (d, p0, p0));
        const bool noAccident = pi.params.bands[0].on->load() < 0.5f;
        d->mouseDown (mouseAt (d, p0, p0, 2));
        const bool created = pi.params.bands[0].on->load() > 0.5f;
        const float f0 = freqOf (0), g0 = gainOf (0);
        std::printf ("Clic simple no crea: %s  /  doble clic crea banda: %s (%.0f Hz, %+.1f dB)\n",
                     noAccident ? "OK" : "FALLO", created ? "OK" : "FALLO", f0, g0);
        interOk = interOk && created && noAccident;

        // 2) Arrastre continuo: la frecuencia sube de forma monotona, sin saltos
        float prev = f0; bool mono = true; float maxStep = 0;
        for (int i = 1; i <= 40; ++i)
        {
            const juce::Point<float> pp (p0.x + i * 5.0f, p0.y + i * 1.0f);
            d->mouseDrag (mouseAt (d, pp, p0));
            const float f = freqOf (0);
            mono = mono && f >= prev;
            maxStep = juce::jmax (maxStep, std::abs (std::log2 (f / prev)));
            prev = f;
        }
        d->mouseUp (mouseAt (d, { p0.x + 200, p0.y + 40 }, p0));
        std::printf ("Arrastre monotono: %s, salto maximo %.3f octavas por paso de 5 px (gain %+.1f dB)\n",
                     mono && maxStep < 0.1f ? "OK" : "FALLO", maxStep, gainOf (0));
        interOk = interOk && mono && maxStep < 0.1f;

        // 3) Agarrar el punto con el mouse algo descentrado NO lo hace saltar
        const auto node = d->nodePosition (0);
        const float fBefore = freqOf (0), gBefore = gainOf (0);
        const auto grab = node.translated (6.0f, -5.0f);
        d->mouseDown (mouseAt (d, grab, grab));
        d->mouseDrag (mouseAt (d, grab.translated (0.5f, 0), grab));
        d->mouseUp (mouseAt (d, grab.translated (0.5f, 0), grab));
        const bool noJump = std::abs (std::log2 (freqOf (0) / fBefore)) < 0.02f && std::abs (gainOf (0) - gBefore) < 0.3f;
        std::printf ("Sin salto al agarrar descentrado: %s\n", noJump ? "OK" : "FALLO");
        interOk = interOk && noJump;

        // 4) Shift = ajuste fino (5x mas lento)
        const auto n2 = d->nodePosition (0);
        const float fA = freqOf (0);
        d->mouseDown (mouseAt (d, n2, n2));
        d->mouseDrag (mouseAt (d, n2.translated (50, 0), n2, 1, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier)));
        d->mouseUp (mouseAt (d, n2.translated (50, 0), n2));
        const float octFine = std::log2 (freqOf (0) / fA);
        std::printf ("Shift fino: 50 px = %.3f octavas (normal seria ~%.3f)\n", octFine, octFine * 5.0f);

        // 5) El panel de la banda queda fijo aunque la dinamica mueva el punto
        setP (pi, gaux::ParamID::band (0, "dyn"), 1); setP (pi, gaux::ParamID::band (0, "thr"), -40); setP (pi, gaux::ParamID::band (0, "range"), -12);
        const auto before = be->getBounds();
        bool fixedPanel = true;
        for (int i = 0; i < 30; ++i)
        {
            processNoise (pi, 4, peak, ms);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
            fixedPanel = fixedPanel && be->getBounds() == before;
        }
        std::printf ("Panel fijo con dinamica activa: %s (accion dinamica %.1f dB)\n", fixedPanel ? "OK" : "FALLO", pi.dynGainDb[0].load());
        interOk = interOk && fixedPanel;

        // 6) Doble clic sobre la banda recien creada no la borra; sobre una existente si
        const juce::Point<float> p3 (300.0f, 300.0f);
        d->mouseDown (mouseAt (d, p3, p3));
        d->mouseUp (mouseAt (d, p3, p3));
        d->mouseDown (mouseAt (d, p3, p3, 2));
        d->mouseUp (mouseAt (d, p3, p3, 2));
        d->mouseDoubleClick (mouseAt (d, p3, p3, 2));
        const bool keep = pi.params.bands[1].on->load() > 0.5f;
        d->mouseDown (mouseAt (d, d->nodePosition (1), d->nodePosition (1)));
        d->mouseUp (mouseAt (d, d->nodePosition (1), d->nodePosition (1)));
        d->mouseDoubleClick (mouseAt (d, d->nodePosition (1), d->nodePosition (1), 2));
        const bool removed = pi.params.bands[1].on->load() < 0.5f;
        std::printf ("Doble clic: conserva la nueva %s, borra la existente %s\n", keep ? "OK" : "FALLO", removed ? "OK" : "FALLO");
        interOk = interOk && keep && removed;

        // 7) La X junto al punto seleccionado borra la banda
        {
            const juce::Point<float> p4 (700.0f, 200.0f);
            d->mouseDown (mouseAt (d, p4, p4, 2));
            d->mouseUp (mouseAt (d, p4, p4, 2));
            int nb = -1;
            for (int b = 0; b < gaux::kNumBands; ++b)
                if (pi.params.bands[(size_t) b].on->load() > 0.5f && std::abs (d->nodePosition (b).x - p4.x) < 3.0f) nb = b;
            const auto xp = d->deleteBadgePosition (nb);
            d->mouseDown (mouseAt (d, xp, xp));
            d->mouseUp (mouseAt (d, xp, xp));
            const bool gone = nb >= 0 && pi.params.bands[(size_t) nb].on->load() < 0.5f;
            std::printf ("X borra la banda seleccionada: %s\n", gone ? "OK" : "FALLO");
            interOk = interOk && gone;
        }

        // 8) Tirador del Range dinamico (banda 1, dinamica)
        {
            dynamic_cast<GauxEqEditor*> (ed.get())->selectBand (0);
            setP (pi, gaux::ParamID::band (0, "range"), -6);
            const auto hp = d->rangeHandlePosition (0);
            d->mouseDown (mouseAt (d, hp, hp));
            d->mouseDrag (mouseAt (d, hp.translated (0, 30), hp));
            d->mouseUp (mouseAt (d, hp.translated (0, 30), hp));
            const float rg = pi.params.bands[0].range->load();
            const bool okr = rg < -8.0f && std::abs (freqOf (0) - fA) > -1.0f;
            std::printf ("Tirador de Range: -6 dB -> %.1f dB %s\n", rg, okr ? "OK" : "FALLO");
            interOk = interOk && okr;
        }

        // 9) Tiradores de la zona de proceso
        {
            setP (pi, gaux::ParamID::rangeOn, 1); setP (pi, gaux::ParamID::rangeLo, 100); setP (pi, gaux::ParamID::rangeHi, 10000);
            const float x = d->zoneX (false);
            const juce::Point<float> tp (x - 20.0f, 12.0f);
            d->mouseDown (mouseAt (d, tp, tp));
            d->mouseDrag (mouseAt (d, tp.translated (60, 0), tp));
            d->mouseUp (mouseAt (d, tp.translated (60, 0), tp));
            const float lo = pi.params.rangeLo->load();
            const bool okz = lo > 130.0f && lo < 10000.0f;
            std::printf ("Tirador de zona LO: 100 Hz -> %.0f Hz %s\n", lo, okz ? "OK" : "FALLO");
            interOk = interOk && okz;
        }

        // 10) Escala de la interfaz
        auto* ge = dynamic_cast<GauxEqEditor*> (ed.get());
        ge->setUiScale (0.75f);
        std::printf ("Escala 75%%: ventana %dx%d\n", ed->getWidth(), ed->getHeight());
        ge->setUiScale (1.0f);
        std::printf ("Escala 100%%: ventana %dx%d\n", ed->getWidth(), ed->getHeight());
    }
    std::printf ("INTERACCION %s\n", interOk ? "OK" : "FALLO");
    ok = ok && interOk;

    // Galeria de estados de la mascota
    {
        juce::Component strip;
        strip.setSize (5 * 120, 110);
        juce::OwnedArray<gaux::Mascot> ms;
        const gaux::Mascot::Mood moods[] = { gaux::Mascot::Mood::Sleep, gaux::Mascot::Mood::Chill, gaux::Mascot::Mood::Warm,
                                             gaux::Mascot::Mood::Angry, gaux::Mascot::Mood::Clip };
        for (int i = 0; i < 5; ++i)
        {
            auto* m = ms.add (new gaux::Mascot (p));
            m->setBounds (i * 120 + 10, 5, 100, 100);
            m->forceMood (moods[i], 0.6f);
            strip.addAndMakeVisible (m);
        }
        struct Bg : juce::Component { void paint (juce::Graphics& g) override { g.fillAll (gaux::Colours::inkHi); } } bg;
        bg.setBounds (strip.getLocalBounds());
        strip.addAndMakeVisible (bg);
        bg.toBack();
        snapshot (strip, outDir + "/gaux_mascota.png");
    }

    // ---- La curva de la pantalla = lo que realmente suena (los dos modos, las tres calidades) ----
    {
        struct Case { const char* name; std::function<void (GauxEqProcessor&)> setup; bool vintage; };
        std::vector<Case> cases = {
            { "Vintage: truco 60 Hz + HIGH 10k + SOFTEN 20k", [] (GauxEqProcessor& q) { setP (q, ParamID::mode, 1);
                setP (q, ParamID::vLowFreq, 2); setP (q, ParamID::vLowBoost, 6); setP (q, ParamID::vLowAtten, 4);
                setP (q, ParamID::vHighFreq, 4); setP (q, ParamID::vHighBoost, 6); setP (q, ParamID::vHighBw, 4);
                setP (q, ParamID::vAttenFreq, 2); setP (q, ParamID::vHighAtten, 3); }, true },
            { "Vintage: MID + FILTER + AIR", [] (GauxEqProcessor& q) { setP (q, ParamID::mode, 1);
                setP (q, ParamID::vLmFreq, 1); setP (q, ParamID::vLmBoost, 5); setP (q, ParamID::vDipFreq, 6); setP (q, ParamID::vDip, 6);
                setP (q, ParamID::vHmFreq, 3); setP (q, ParamID::vHmBoost, 4); setP (q, ParamID::vLowCut, 2); setP (q, ParamID::vHighCut, 8);
                setP (q, ParamID::vAirFreq, 2); setP (q, ParamID::vAirGain, 5); }, true },
            { "Vintage: HIGH 16k BROAD a tope", [] (GauxEqProcessor& q) { setP (q, ParamID::mode, 1);
                setP (q, ParamID::vHighFreq, 6); setP (q, ParamID::vHighBoost, 10); setP (q, ParamID::vHighBw, 10); }, true },
            { "Vintage: PROGRAM IN apagado", [] (GauxEqProcessor& q) { setP (q, ParamID::mode, 1);
                setP (q, ParamID::vLowBoost, 8); setP (q, ParamID::vHighBoost, 8); setP (q, ParamID::vProgramIn, 0); }, true },
            { "Parametrico: 5 bandas", [] (GauxEqProcessor& q) {
                auto B = [&] (int b, int t, float f, float g, float qq) { setP (q, ParamID::band (b, "on"), 1); setP (q, ParamID::band (b, "type"), (float) t);
                    setP (q, ParamID::band (b, "freq"), f); setP (q, ParamID::band (b, "gain"), g); setP (q, ParamID::band (b, "q"), qq); };
                B (0, 3, 40, 0, 0.707f); B (1, 0, 120, 4, 1.0f); B (2, 0, 900, -6, 2.0f); B (3, 0, 6000, 5, 0.8f); B (4, 2, 14000, 4, 0.7f); }, false },
        };
        bool okC = true;
        for (auto& c : cases)
            for (int qual = 0; qual < 3; ++qual)
            {
                GauxEqProcessor pq; pq.prepareToPlay (48000.0, 512);
                c.setup (pq);
                setP (pq, ParamID::quality, (float) qual);
                processSine (pq, 1000, 0.05f, 4);
                double worst = 0, worstF = 0;
                for (double f : { 25.0, 40.0, 60.0, 90.0, 140.0, 220.0, 350.0, 600.0, 1000.0, 1700.0, 2800.0, 4500.0, 7000.0, 10000.0, 13000.0, 16000.0, 19000.0 })
                {
                    const double inDb = 20 * std::log10 (0.05 / std::sqrt (2.0));
                    const double meas = processSine (pq, f, 0.05f, 24).outDb - inDb;
                    double curve;
                    const double efs = pq.engineSampleRate.load();
                    if (c.vintage) curve = 20 * std::log10 (gaux::magnitudeOfVintage (pq.params.vintage(), f, efs));
                    else { double m = 1; for (int b = 0; b < gaux::kNumBands; ++b) m *= gaux::magnitudeOfBand (pq.params.band (b), f, efs); curve = 20 * std::log10 (m); }
                    const double tol = f > 17000 ? 1.0 : 0.35;
                    const double err = std::abs (meas - curve);
                    if (err - tol > worst - (worstF > 17000 ? 1.0 : 0.35)) { worst = err; worstF = f; }
                    okC = okC && err < tol;
                }
                std::printf ("Pantalla vs audio [%s] calidad %dx: diferencia maxima %.2f dB (en %.0f Hz) %s\n", c.name, 1 << qual, worst, worstF,
                             worst < (worstF > 17000 ? 1.0 : 0.35) ? "OK" : "FALLO");
            }
        ok = ok && okC;
    }

    // ---- TODOS los presets, uno por uno ----
    {
        // Material de prueba: ruido rosa + bombo + bajo, alrededor de -14 dBFS RMS
        auto program = [] (int i, double fsr, juce::Random& rnd, float* st)
        {
            const float w = rnd.nextFloat() * 2 - 1;
            st[0] = 0.99765f * st[0] + w * 0.0990460f; st[1] = 0.96300f * st[1] + w * 0.2965164f; st[2] = 0.57000f * st[2] + w * 1.0526913f;
            const float pink = (st[0] + st[1] + st[2] + w * 0.1848f) * 0.06f;
            const double t = i / fsr, beat = 60.0 / 124.0, tb = std::fmod (t, beat);
            const double kick = 0.5 * std::exp (-tb * 9.0) * std::min (1.0, tb / 0.0015) * std::min (1.0, (beat - tb) / 0.03)
                              * std::sin (2 * juce::MathConstants<double>::pi * (48.0 * tb + 140.0 / 32.0 * (1.0 - std::exp (-tb * 32.0))));
            return (float) (pink + kick);
        };
        bool allOk = true; int tested = 0;
        std::printf ("\nPRESETS (modo | nombre | valores exactos | pico salida | cambio de nivel | realce max | estado guardado)\n");
        for (int mode : { 0, 1 })
        {
            GauxEqProcessor pp; pp.prepareToPlay (48000.0, 512);
            setP (pp, ParamID::mode, (float) mode);
            const auto list = pp.presets.entries();
            for (int idx = 0; idx < (int) list.size(); ++idx)
            {
                if (list[(size_t) idx].user) continue;
                pp.presets.load (idx);
                ++tested;
                // 1) Cada valor del preset quedo exactamente como se diseno
                const auto& fpv = gaux::factoryPresets()[(size_t) list[(size_t) idx].factoryIndex];
                double worstVal = 0;
                for (auto& [id, val] : fpv.values)
                {
                    auto* prm = pp.apvts.getParameter (id);
                    if (prm == nullptr) { worstVal = 1e9; continue; }
                    const float actual = prm->convertFrom0to1 (prm->getValue());
                    const float want = prm->convertFrom0to1 (prm->convertTo0to1 (val));   // lo representable
                    worstVal = std::max (worstVal, (double) std::abs (actual - want));
                }
                const bool modeOk = (pp.params.mode->load() > 0.5f) == (mode == 1);
                // 2) Audio real en las tres calidades
                double peak = 0, inSq = 0, outSq = 0; long n = 0; bool fin = true;
                for (int qual : { 0, 2 })
                {
                    setP (pp, ParamID::quality, (float) qual);
                    juce::Random rnd (7); float st[3] = { 0, 0, 0 };
                    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
                    for (int blk = 0; blk < 280; ++blk)
                    {
                        for (int i = 0; i < 512; ++i) { const float v = program (blk * 512 + i, 48000.0, rnd, st); buf.setSample (0, i, v); buf.setSample (1, i, v * 0.97f); }
                        double s1 = 0; for (int i = 0; i < 512; ++i) s1 += buf.getSample (0, i) * buf.getSample (0, i);
                        pp.processBlock (buf, midi);
                        if (blk > 60)
                        {
                            inSq += s1;
                            for (int i = 0; i < 512; ++i) { const float y = buf.getSample (0, i); fin = fin && std::isfinite (y); peak = std::max (peak, (double) std::abs (y)); outSq += y * y; }
                            n += 512;
                        }
                    }
                }
                setP (pp, ParamID::quality, 0);
                const double lvl = 10 * std::log10 (outSq / std::max (1e-12, inSq));
                // 3) Curva razonable (nada extremo que pueda lastimar oidos o monitores)
                double cmax = 0;
                for (double f = 20; f < 20000; f *= 1.05)
                {
                    double m;
                    if (mode == 1) m = gaux::magnitudeOfVintage (pp.params.vintage(), f, 48000.0);
                    else { m = 1; for (int b = 0; b < gaux::kNumBands; ++b) m *= gaux::magnitudeOfBand (pp.params.band (b), f, 48000.0); }
                    cmax = std::max (cmax, 20 * std::log10 (std::max (1e-9, m)));   // realce maximo
                }
                // 4) Se guarda y se restaura igual (como al reabrir el proyecto)
                juce::MemoryBlock mb; pp.getStateInformation (mb);
                GauxEqProcessor p2; p2.setStateInformation (mb.getData(), (int) mb.getSize());
                double stDiff = 0;
                for (int k = 0; k < pp.getParameters().size(); ++k)
                    stDiff = std::max (stDiff, (double) std::abs (pp.getParameters()[k]->getValue() - p2.getParameters()[k]->getValue()));
                const bool lofi = list[(size_t) idx].name.contains ("Lo-Fi") || list[(size_t) idx].name.contains ("Telefono");
                const bool pOk = worstVal < 1e-3 && modeOk && fin && peak < 1.0 && lvl < 4.0 && lvl > (lofi ? -16.0 : -10.0) && cmax < 15.0 && stDiff < 1e-6;
                std::printf ("  %-10s | %-28s | %s | %.2f | %+5.1f dB | %4.1f dB | %s %s\n", mode ? "Vintage" : "Parametrico",
                             list[(size_t) idx].name.toRawUTF8(), worstVal < 1e-3 ? "exactos" : "DIFIEREN", peak, lvl, cmax,
                             stDiff < 1e-6 ? "igual" : "DIFIERE", pOk ? "OK" : "FALLO");
                allOk = allOk && pOk;
            }
        }
        std::printf ("PRESETS: %d probados %s\n", tested, allOk ? "OK" : "FALLO");
        ok = ok && allOk;
    }

    // ---- ESTRES: automatizacion aleatoria, frecuencias de muestreo, bloques irregulares, extremos ----
    {
        bool sOk = true;
        auto& allParams = p.getParameters();
        juce::ignoreUnused (allParams);
        for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
            for (int maxBlock : { 64, 512, 2048 })
            {
                GauxEqProcessor ps; ps.prepareToPlay (sr, maxBlock);
                juce::Random rnd ((juce::int64) (sr + maxBlock));
                auto& ps_params = ps.getParameters();
                juce::AudioBuffer<float> buf (2, maxBlock); juce::MidiBuffer midi;
                bool fin = true; double peak = 0, ms = 0, worstExcess = 0; long total = 0; int changes = 0;
                const int seconds = sr > 100000 ? 2 : 3;
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                double phase = 0;
                std::deque<double> gainHist;
                // Ganancia maxima que permiten los ajustes actuales (peor caso, incluyendo dinamica, salida y auto gain)
                auto maxGainNow = [&] ()
                {
                    double best = 0;
                    const double efs = ps.engineSampleRate.load();
                    for (double f = 15; f < sr * 0.49; f *= 1.03)
                    {
                        double m = 1;
                        for (int b = 0; b < gaux::kNumBands; ++b)
                        {
                            auto st = ps.params.band (b);
                            if (st.dynOn && st.rangeDb > 0) st.gainDb += st.rangeDb;
                            m *= gaux::magnitudeOfBand (st, f, efs);
                        }
                        best = std::max ({ best, m, gaux::magnitudeOfVintage (ps.params.vintage(), f, efs) });
                    }
                    const double outG = juce::Decibels::decibelsToGain ((double) ps.params.output->load());
                    const double ag = juce::Decibels::decibelsToGain ((double) std::max (0.0f, ps.autoGainDb.load()));
                    return std::max (1.0, best) * outG * ag * 2.0;   // x2: saturacion/armonicos
                };
                while (total < (long) (sr * seconds))
                {
                    const int n = 1 + rnd.nextInt (maxBlock);
                    buf.setSize (2, n, false, false, true);
                    double inPk = 0;
                    for (int i = 0; i < n; ++i)
                    {
                        phase += 2 * juce::MathConstants<double>::pi * 70.0 / sr;
                        const float v = (float) (0.5 * std::sin (phase)) + (rnd.nextFloat() * 2 - 1) * 0.2f;
                        buf.setSample (0, i, v); buf.setSample (1, i, -v * 0.8f);
                        inPk = std::max (inPk, (double) std::abs (v));
                    }
                    for (int k = 0; k < 4; ++k)
                    {
                        auto* prm = ps_params[rnd.nextInt (ps_params.size())];
                        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                            if (rp->getParameterID() == ParamID::quality && rnd.nextInt (20) != 0) continue;
                        prm->setValueNotifyingHost (rnd.nextFloat());
                        ++changes;
                    }
                    ps.processBlock (buf, midi);
                    double blkPk = 0;
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < n; ++i) { const float y = buf.getSample (ch, i); fin = fin && std::isfinite (y); blkPk = std::max (blkPk, (double) std::abs (y)); }
                    peak = std::max (peak, blkPk);
                    // Ventana de ~250 ms: los ajustes se suavizan, asi que vale la ganancia maxima reciente
                    gainHist.push_back (maxGainNow());
                    while ((long) gainHist.size() * (maxBlock / 2) > (long) (sr * 0.25) && gainHist.size() > 1) gainHist.pop_front();
                    const double allowed = *std::max_element (gainHist.begin(), gainHist.end()) * 0.73;
                    worstExcess = std::max (worstExcess, blkPk / std::max (1e-9, allowed));
                    total += n;
                }
                ms = juce::Time::getMillisecondCounterHiRes() - t0;
                const double rt = ms / (1000.0 * seconds);       // fraccion de tiempo real usada
                // Con automatizacion aleatoria a tope hay ganancias de hasta +24 dB x 16 bandas: se exige que no explote ni de NaN
                // Nunca debe salir mas de lo que la curva permite (+12 dB de margen para transitorios del suavizado)
                const bool okR = fin && worstExcess < 8.0;    // < +18 dB con automatizacion absurda (barridos de resonancias)
                std::printf ("Estres %6.0f Hz bloques 1-%4d: %6d cambios aleatorios, finito %s, exceso maximo sobre la curva %+.1f dB, CPU %.1f%% %s\n",
                             sr, maxBlock, changes, fin ? "si" : "NO", 20 * std::log10 (std::max (1e-9, worstExcess)), rt * 100.0, okR ? "OK" : "FALLO");
                sOk = sOk && okR;
            }

        // Automatizacion realista (como la de un usuario o un DAW): movimientos suaves de todos los controles
        {
            GauxEqProcessor pa; pa.prepareToPlay (48000.0, 256);
            auto B = [&] (int b, int t, float f, float g, float q) { setP (pa, ParamID::band (b, "on"), 1); setP (pa, ParamID::band (b, "type"), (float) t);
                setP (pa, ParamID::band (b, "freq"), f); setP (pa, ParamID::band (b, "gain"), g); setP (pa, ParamID::band (b, "q"), q); };
            B (0, 0, 100, 6, 2); B (1, 0, 1000, -8, 4); B (2, 2, 8000, 6, 0.7f); B (3, 3, 30, 0, 0.7f); B (4, 0, 3000, 4, 1);
            setP (pa, ParamID::band (4, "dyn"), 1); setP (pa, ParamID::band (4, "range"), -10); setP (pa, ParamID::band (1, "drive"), 12);
            juce::AudioBuffer<float> b2 (2, 256); juce::MidiBuffer m2; juce::Random r2 (9);
            double worst = 0, ph = 0; bool fin = true; std::deque<double> hist;
            for (int blk = 0; blk < 48000 * 12 / 256; ++blk)
            {
                const double t = blk * 256 / 48000.0;
                // LFOs lentos (0.1 - 0.7 Hz) sobre frecuencia, ganancia, Q, drive y salida
                setP (pa, ParamID::band (0, "freq"), (float) (100 * std::pow (8.0, 0.5 + 0.5 * std::sin (t * 0.9))));
                setP (pa, ParamID::band (1, "gain"), (float) (12 * std::sin (t * 1.7)));
                setP (pa, ParamID::band (1, "q"), (float) (0.5 + 5 * (0.5 + 0.5 * std::sin (t * 0.6))));
                setP (pa, ParamID::band (2, "freq"), (float) (3000 * std::pow (4.0, 0.5 + 0.5 * std::sin (t * 0.4))));
                setP (pa, ParamID::band (4, "freq"), (float) (800 * std::pow (8.0, 0.5 + 0.5 * std::sin (t * 1.1))));
                setP (pa, ParamID::output, (float) (-3 + 3 * std::sin (t * 0.3)));
                double inPk = 0;
                for (int i = 0; i < 256; ++i) { ph += 2 * juce::MathConstants<double>::pi * 110.0 / 48000.0; const float v = (float) (0.3 * std::sin (ph)) + (r2.nextFloat() * 2 - 1) * 0.15f; b2.setSample (0, i, v); b2.setSample (1, i, v); inPk = std::max (inPk, (double) std::abs (v)); }
                pa.processBlock (b2, m2);
                double pk = 0; for (int i = 0; i < 256; ++i) { fin = fin && std::isfinite (b2.getSample (0, i)); pk = std::max (pk, (double) std::abs (b2.getSample (0, i))); }
                double best = 0; const double efs = pa.engineSampleRate.load();
                for (double f = 15; f < 23000; f *= 1.01) { double m = 1; for (int b = 0; b < gaux::kNumBands; ++b) m *= gaux::magnitudeOfBand (pa.params.band (b), f, efs); best = std::max (best, m); }
                hist.push_back (best * juce::Decibels::decibelsToGain ((double) pa.params.output->load()) * 1.25 * inPk);
                if (hist.size() > 20) hist.pop_front();
                if (blk > 50) worst = std::max (worst, pk / *std::max_element (hist.begin(), hist.end()));
            }
            const bool okA = fin && worst < 1.42;   // +3 dB
            std::printf ("Automatizacion realista (12 s, frecuencia/ganancia/Q/salida en movimiento): exceso maximo %+.1f dB %s\n", 20 * std::log10 (worst), okA ? "OK" : "FALLO");
            sOk = sOk && okA;
        }

        // Extremos: silencio largo despues de mucho volumen (denormales), DC, impulsos, +12 dBFS, mono
        {
            GauxEqProcessor px; px.prepareToPlay (48000.0, 512);
            setP (px, ParamID::mode, 1); setP (px, ParamID::vLowBoost, 10); setP (px, ParamID::vHighBoost, 10); setP (px, ParamID::vTube, 100); setP (px, ParamID::vIron, 100);
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
            bool fin = true; double peak = 0;
            auto runBlocks = [&] (int blocks, std::function<float (int)> gen)
            {
                double t = 0;
                for (int b = 0; b < blocks; ++b)
                {
                    for (int i = 0; i < 512; ++i) { const float v = gen (b * 512 + i); buf.setSample (0, i, v); buf.setSample (1, i, v); }
                    const auto t0 = juce::Time::getMillisecondCounterHiRes();
                    px.processBlock (buf, midi);
                    t += juce::Time::getMillisecondCounterHiRes() - t0;
                    for (int i = 0; i < 512; ++i) { fin = fin && std::isfinite (buf.getSample (0, i)); peak = std::max (peak, (double) std::abs (buf.getSample (0, i))); }
                }
                return t / blocks;
            };
            runBlocks (200, [] (int i) { return 4.0f * (float) std::sin (i * 0.01); });                 // +12 dBFS
            const double msLoud = runBlocks (100, [] (int i) { return (float) std::sin (i * 0.05) * 0.5f; });
            const double msSilent = runBlocks (400, [] (int) { return 0.0f; });                         // cola hacia denormales
            runBlocks (100, [] (int) { return 0.5f; });                                                   // DC
            runBlocks (100, [] (int i) { return (i % 9000) == 0 ? 1.0f : 0.0f; });                        // impulsos
            const bool denormOk = msSilent < msLoud * 3.0 + 0.05;
            std::printf ("Extremos (+12 dBFS, DC, impulsos, silencio largo): finito %s, pico %.1f, CPU en silencio %.3f ms vs con audio %.3f ms (sin denormales %s)\n",
                         fin ? "si" : "NO", peak, msSilent, msLoud, denormOk ? "OK" : "FALLO");
            sOk = sOk && fin && denormOk;

            // Mono
            GauxEqProcessor pmn;
            juce::AudioProcessor::BusesLayout lay; lay.inputBuses.add (juce::AudioChannelSet::mono()); lay.outputBuses.add (juce::AudioChannelSet::mono());
            lay.inputBuses.add (juce::AudioChannelSet::disabled());
            const bool layOk = pmn.setBusesLayout (lay);
            pmn.prepareToPlay (48000.0, 256);
            setP (pmn, ParamID::band (0, "on"), 1); setP (pmn, ParamID::band (0, "type"), 0); setP (pmn, ParamID::band (0, "stereo"), 4);
            juce::AudioBuffer<float> mb (1, 256);
            bool mfin = true;
            for (int b = 0; b < 100; ++b)
            {
                for (int i = 0; i < 256; ++i) mb.setSample (0, i, (float) std::sin ((b * 256 + i) * 0.03));
                pmn.processBlock (mb, midi);
                for (int i = 0; i < 256; ++i) mfin = mfin && std::isfinite (mb.getSample (0, i));
            }
            std::printf ("Pista mono (incluso con una banda en modo Side): diseno aceptado %s, finito %s\n", layOk ? "si" : "NO", mfin ? "si" : "NO");
            sOk = sOk && layOk && mfin;
        }
        // CPU real en el peor caso
        for (int mode : { 0, 1 })
            for (double sr : { 48000.0, 96000.0 })
            {
                GauxEqProcessor pc; pc.enableAllBuses(); pc.prepareToPlay (sr, 512);
                setP (pc, ParamID::quality, 2); setP (pc, ParamID::rangeOn, 1); setP (pc, ParamID::rangeLo, 60); setP (pc, ParamID::scOn, 1);
                setP (pc, ParamID::mode, (float) mode);
                for (int b = 0; b < gaux::kNumBands; ++b)
                {
                    setP (pc, ParamID::band (b, "on"), 1); setP (pc, ParamID::band (b, "type"), (float) (b % 3));
                    setP (pc, ParamID::band (b, "freq"), (float) (40 * std::pow (1.45, b))); setP (pc, ParamID::band (b, "gain"), 2);
                    setP (pc, ParamID::band (b, "drive"), 15); setP (pc, ParamID::band (b, "satType"), (float) (b % 5));
                    setP (pc, ParamID::band (b, "dyn"), 1); setP (pc, ParamID::band (b, "stereo"), (float) (b % 5));
                }
                setP (pc, ParamID::vTube, 60); setP (pc, ParamID::vIron, 60); setP (pc, ParamID::vLowBoost, 5); setP (pc, ParamID::vHighBoost, 5); setP (pc, ParamID::vLmBoost, 3);
                juce::AudioBuffer<float> bb (4, 512); juce::MidiBuffer mm; juce::Random r3 (1);
                const int blocks = (int) (sr * 6 / 512);
                double t = 0;
                for (int blk = 0; blk < blocks; ++blk)
                {
                    for (int ch = 0; ch < 4; ++ch) for (int i = 0; i < 512; ++i) bb.setSample (ch, i, (r3.nextFloat() * 2 - 1) * 0.3f);
                    const auto t0 = juce::Time::getMillisecondCounterHiRes();
                    pc.processBlock (bb, mm);
                    t += juce::Time::getMillisecondCounterHiRes() - t0;
                }
                const double pct = t / (blocks * 512.0 / sr * 1000.0) * 100.0;
                std::printf ("CPU peor caso (%s, %.0f kHz, HQ 4x, ZONE + SC%s): %.1f%% de un nucleo\n", mode ? "Vintage completo" : "16 bandas saturando + dinamicas",
                             sr / 1000.0, mode ? "" : " + M/S", pct);
            }
        std::printf ("ESTRES %s\n", sOk ? "OK" : "FALLO");
        ok = ok && sOk;
    }

    // ---- Calavera afinada: golpes reales, saturacion real y animo estable ----
    {
        auto loopSample = [] (int i, double fsr)
        {
            const double t = i / fsr, beat = 60.0 / 124.0, tb = std::fmod (t, beat);
            const double kEnv = std::exp (-tb * 9.0) * std::min (1.0, tb / 0.0015) * std::min (1.0, (beat - tb) / 0.03);
            const double kick = 0.75 * kEnv * std::sin (2 * juce::MathConstants<double>::pi * (48.0 * tb + 140.0 / 32.0 * (1.0 - std::exp (-tb * 32.0))));
            const double tl = tb - beat * 0.5;
            const double f = 55.0 * std::pow (2.0, ((int) (t / (beat * 2)) % 4) * 3.0 / 12.0);
            double saw = 0; for (int h = 1; h <= 8; ++h) saw += std::sin (2 * juce::MathConstants<double>::pi * f * h * t) / h;
            const double bEnv = tl > 0 ? std::min (1.0, tl / 0.006) * std::min (1.0, (beat * 0.5 - tl) / 0.025) * std::exp (-tl * 3.0) : 0.0;
            return (float) (kick + 0.28 * saw * bEnv);
        };
        struct Scene { const char* name; std::function<void (GauxEqProcessor&)> setup; float gain; gaux::Mascot::Mood expectLo, expectHi; };
        using Md = gaux::Mascot::Mood;
        std::vector<Scene> scenes = {
            { "limpio", [] (GauxEqProcessor&) {}, 0.5f, Md::Chill, Md::Chill },
            { "TUBE drive 14 en 140 Hz", [] (GauxEqProcessor& q) {
                setP (q, ParamID::band (0, "on"), 1); setP (q, ParamID::band (0, "type"), 0); setP (q, ParamID::band (0, "freq"), 140); setP (q, ParamID::band (0, "q"), 0.7f);
                setP (q, ParamID::band (0, "drive"), 14); }, 0.5f, Md::Warm, Md::Angry },
            { "HARD drive 36 en 140 Hz", [] (GauxEqProcessor& q) {
                setP (q, ParamID::band (0, "on"), 1); setP (q, ParamID::band (0, "type"), 0); setP (q, ParamID::band (0, "freq"), 140); setP (q, ParamID::band (0, "q"), 0.7f);
                setP (q, ParamID::band (0, "satType"), 3); setP (q, ParamID::band (0, "drive"), 36); }, 0.5f, Md::Angry, Md::Clip },
            { "drive 36 en 9 kHz (no hay nada ahi)", [] (GauxEqProcessor& q) {
                setP (q, ParamID::band (0, "on"), 1); setP (q, ParamID::band (0, "type"), 0); setP (q, ParamID::band (0, "freq"), 9000);
                setP (q, ParamID::band (0, "drive"), 36); }, 0.5f, Md::Chill, Md::Chill },
            { "salida clipeando (+18 dB)", [] (GauxEqProcessor& q) { setP (q, ParamID::output, 18); }, 0.5f, Md::Clip, Md::Clip },
            { "silencio", [] (GauxEqProcessor&) {}, 0.0f, Md::Sleep, Md::Sleep },
            { "vintage GLOW 30 %", [] (GauxEqProcessor& q) { setP (q, ParamID::mode, 1); setP (q, ParamID::vTube, 30); }, 0.5f, Md::Chill, Md::Warm },
            { "vintage GLOW 100 % + IRON 100 %", [] (GauxEqProcessor& q) { setP (q, ParamID::mode, 1); setP (q, ParamID::vTube, 100); setP (q, ParamID::vIron, 100); }, 0.5f, Md::Warm, Md::Clip },
        };
        bool mOk = true;
        for (auto& sc : scenes)
        {
            GauxEqProcessor pm; pm.prepareToPlay (48000.0, 480);
            sc.setup (pm);
            gaux::Mascot m (pm);
            juce::AudioBuffer<float> buf (2, 480); juce::MidiBuffer midi;
            std::map<int, int> moods; int changes = 0; auto last = m.getMood();
            const int seconds = 9;
            for (int blk = 0; blk < seconds * 100; ++blk)
            {
                for (int i = 0; i < 480; ++i) { const float v = sc.gain * loopSample (blk * 480 + i, 48000.0); buf.setSample (0, i, v); buf.setSample (1, i, v); }
                pm.processBlock (buf, midi);
                if (blk % 3 == 0 && blk >= 300)          // 33 tics por segundo, despues de 3 s de arranque
                {
                    m.tick();
                    moods[(int) m.getMood()]++;
                    if (m.getMood() != last) { ++changes; last = m.getMood(); }
                }
                else if (blk % 3 == 0) m.tick();
            }
            int dominant = 0, best = -1;
            for (auto& [k, v] : moods) if (v > best) { best = v; dominant = k; }
            const bool inRange = dominant >= (int) sc.expectLo && dominant <= (int) sc.expectHi;
            const char* names[] = { "dormida", "tranquila", "calida", "enojada", "clip" };
            const int beats = m.getBeatsSeen();
            const bool beatsOk = sc.gain == 0.0f ? beats == 0 : (beats >= 16 && beats <= 20);   // 124 bpm x 9 s = 18.6
            const bool stable = changes <= 2;
            std::printf ("Calavera [%s]: %s (saturacion real %.1f dB), golpes %d de ~19, cambios de animo %d %s\n", sc.name, names[dominant], pm.satDb.load(), beats, changes,
                         inRange && beatsOk && stable ? "OK" : "FALLO");
            mOk = mOk && inRange && beatsOk && stable;
        }
        ok = ok && mOk;
    }

    // Panel del sidechain general
    {
        GauxEqProcessor ps; ps.prepareToPlay (48000.0, 512);
        setP (ps, ParamID::scOn, 1);
        gaux::SidechainPanel panel (ps, false);
        gaux::GauxLookAndFeel lf;
        panel.setLookAndFeel (&lf);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
        snapshot (panel, outDir + "/gaux_sidechain.png");
        panel.setLookAndFeel (nullptr);
    }

    // Logo del modo parametrico en grande (y comparacion de tipografias si estan disponibles)
    {
        auto loadTf = [] (const char* path) -> juce::Typeface::Ptr
        {
            juce::MemoryBlock mb;
            if (! juce::File (path).loadFileAsData (mb)) return nullptr;
            return juce::Typeface::createSystemTypefaceFor (mb.getData(), mb.getSize());
        };
        struct L : juce::Component
        {
            std::vector<std::pair<juce::String, juce::Typeface::Ptr>> fonts;
            void paint (juce::Graphics& g) override
            {
                g.fillAll (gaux::Colours::ink);
                float y = 30.0f;
                for (auto& [name, tf] : fonts)
                {
                    gaux::Logo::drawRave (g, { 40.0f, y, gaux::Logo::raveWidth (120.0f, tf), 120.0f }, 1.0, tf);
                    gaux::Logo::drawRave (g, { 760.0f, y + 40.0f, gaux::Logo::raveWidth (30.0f, tf), 30.0f }, 1.0, tf);
                    g.setColour (gaux::Colours::textDim);
                    g.setFont (gaux::monoFont (14.0f));
                    g.drawText (name, juce::Rectangle<float> (760.0f, y + 90.0f, 300.0f, 20.0f), juce::Justification::centredLeft);
                    y += 170.0f;
                }
            }
        } l;
        l.fonts.push_back ({ "A - Syne (incluida)", nullptr });
        for (auto [n, p] : { std::pair<const char*, const char*> { "B - Grenze Gotisch", "/tmp/claude-0/fonts/GrenzeGotisch-Black.ttf" },
                             { "C - Unbounded", "/tmp/claude-0/fonts/Unbounded-Black.ttf" } })
            if (auto tf = loadTf (p)) l.fonts.push_back ({ n, tf });
        l.setSize (1080, 30 + 170 * (int) l.fonts.size());
        snapshot (l, outDir + "/gaux_logo.png");
    }

    // Guardar y restaurar el estado (como al reabrir un proyecto de Ableton)
    juce::MemoryBlock state;
    p.getStateInformation (state);
    GauxEqProcessor p2;
    p2.setStateInformation (state.getData(), (int) state.getSize());
    const bool restored = std::abs (p2.params.bands[1].drive->load() - 20.0f) < 0.01f && p2.params.mode->load() > 0.5f;
    std::printf ("Estado guardado/restaurado: %s\n", restored ? "OK" : "FALLO");

    // Cambio de modo durante la reproduccion: sin saltos bruscos
    setP (p, ParamID::mode, 0);
    processNoise (p, 50, peak, ms);
    std::printf ("Cambio de modo en vivo: pico=%.3f\n", peak);

    std::printf ("%s\n", ok && restored ? "RENDER TEST OK" : "RENDER TEST FALLO");
    return ok && restored ? 0 : 1;
}
