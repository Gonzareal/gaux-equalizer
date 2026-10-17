// Gaux Equalizer - Pantalla de espectro
#include "SpectrumDisplay.h"

namespace gaux
{
static constexpr double kMinF = 20.0, kMaxF = 20000.0;
static constexpr float kAnMin = -84.0f, kAnMax = 6.0f;

static juce::String fmtFreq (double f)
{
    return f >= 1000.0 ? juce::String (f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz";
}

// Polilinea simple (las curvas del EQ tienen un punto cada 2 px: se ven igual de suaves y cuestan menos)
#ifdef JucePlugin_VersionString
 #define GAUX_VERSION_STR JucePlugin_VersionString
#else
 #define GAUX_VERSION_STR "0.21.0"
#endif

static juce::Path linePath (const std::vector<juce::Point<float>>& pts)
{
    juce::Path p;
    if (pts.size() < 2) return p;
    p.preallocateSpace ((int) pts.size() * 3 + 8);
    p.startNewSubPath (pts[0]);
    for (size_t i = 1; i < pts.size(); ++i) p.lineTo (pts[i]);
    return p;
}

// Suaviza una polilinea con curvas cuadraticas entre puntos medios
static juce::Path smoothPath (const std::vector<juce::Point<float>>& pts)
{
    juce::Path p;
    if (pts.size() < 2) return p;
    p.startNewSubPath (pts[0]);
    for (size_t i = 1; i + 1 < pts.size(); ++i)
    {
        const auto mid = (pts[i] + pts[i + 1]) * 0.5f;
        p.quadraticTo (pts[i], mid);
    }
    p.lineTo (pts.back());
    return p;
}

SpectrumDisplay::SpectrumDisplay (GauxEqProcessor& p) : proc (p)
{
    for (auto* an : { &post, &pre })
    {
        an->ring.assign ((size_t) kFftSize, 0.0f);
        an->smoothed.assign ((size_t) kFftSize / 2 + 1, -120.0f);
    }
    fftData.assign ((size_t) kFftSize * 2, 0.0f);
    pullBuffer.assign ((size_t) AnalyzerFifo::kSize, 0.0f);
    setOpaque (true);
    setWantsKeyboardFocus (true);   // Supr borra las bandas elegidas, Cmd+A / Cmd+C / Cmd+V
}

void SpectrumDisplay::visibilityChanged() { repaint(); }   // el reloj de la pantalla se detiene solo si no esta a la vista

void SpectrumDisplay::parentHierarchyChanged() { visibilityChanged(); }

SpectrumDisplay::~SpectrumDisplay()
{
    if (clashSubscribed) proc.clashHub->unsubscribe();
}

juce::Rectangle<float> SpectrumDisplay::plotArea() const
{
    return getLocalBounds().toFloat().reduced (0.0f, 2.0f);
}

float SpectrumDisplay::freqToX (double f) const
{
    auto a = plotArea();
    return a.getX() + a.getWidth() * (float) (std::log (f / kMinF) / std::log (kMaxF / kMinF));
}

double SpectrumDisplay::xToFreq (float x) const
{
    auto a = plotArea();
    const double t = juce::jlimit (0.0, 1.0, (double) ((x - a.getX()) / a.getWidth()));
    return kMinF * std::pow (kMaxF / kMinF, t);
}

float SpectrumDisplay::dbToY (double db) const
{
    auto a = plotArea();
    return a.getCentreY() - (float) (db / dbRange) * a.getHeight() * 0.5f;
}

double SpectrumDisplay::yToDb (float y) const
{
    auto a = plotArea();
    return (a.getCentreY() - y) / (a.getHeight() * 0.5f) * dbRange;
}

float SpectrumDisplay::analyzerDbToY (float db) const
{
    auto a = plotArea();
    const float t = (juce::jlimit (kAnMin, kAnMax, db) - kAnMin) / (kAnMax - kAnMin);
    return a.getBottom() - t * a.getHeight();
}


//==============================================================================
SpectrumDisplay::ViewOpts SpectrumDisplay::viewOptions() const
{
    const auto& st = proc.apvts.state;
    ViewOpts o;
    o.speed = juce::jlimit (0, 2, (int) st.getProperty ("anSpeed", 1));
    o.tilt = juce::jlimit (0, 3, (int) st.getProperty ("anTilt", 2));
    o.range = juce::jlimit (0, 3, (int) st.getProperty ("dispRange", 2));
    o.freeze = (bool) st.getProperty ("anFreeze", false);
    o.pre = (bool) st.getProperty ("anPre", true);
    o.piano = (bool) st.getProperty ("piano", true);   // (teclado de abajo, en la ventana)
    o.bandCurves = (bool) st.getProperty ("bandCurves", true);
    return o;
}

void SpectrumDisplay::setViewOption (const char* key, int value)
{
    proc.apvts.state.setProperty (key, value, nullptr);
    advanceFrame (0.0f);
    repaint();
}

void SpectrumDisplay::advanceFrame (float dt)
{
    {
        // opciones de la pantalla (pueden cambiar al cargar una sesion)
        const auto o = viewOptions();
        const bool changed = o.speed != opts.speed || o.tilt != opts.tilt || o.range != opts.range || o.freeze != opts.freeze || o.pre != opts.pre || o.piano != opts.piano || o.bandCurves != opts.bandCurves;
        opts = o;
        if (! vintageMode && dbRange != kRanges[opts.range]) dbRange = kRanges[opts.range];
        if (optsTilt != kTilts[opts.tilt]) { optsTilt = kTilts[opts.tilt]; tiltFs = 0; }
        if (! opts.pre && ! pre.fill.isEmpty()) { pre.fill.clear(); pre.outline.clear(); pre.visible = false; }
        if (changed) repaint();
    }
    // Resplandor que late con la musica (tiempos en segundos reales: igual a 60, 120 o 144 FPS)
    const float lvl = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (proc.outputRms.load(), -100.0f) + 36.0f) / 30.0f);
    const float oldBloom = bloom;
    const float kb = 1.0f - std::pow (lvl > bloom ? 0.45f : 0.88f, dt * 40.0f);
    bloom += (lvl - bloom) * kb;
    if (bloom < 1.0e-3f && lvl <= 0.0f) bloom = 0.0f;

    bool dirty = updateAnalyzer (post, proc.analyzerFifo, dt);
    if (! vintageMode && opts.pre) dirty = updateAnalyzer (pre, proc.preFifo, dt) || dirty;
    else proc.preFifo.pull (pullBuffer.data(), (int) pullBuffer.size());   // vaciar

    dirty = refreshCurves() || dirty;
    const bool audio = bloom > 0.0f || post.fresh;
    if (audio) animTime += dt;
    dirty = dirty || std::abs (bloom - oldBloom) > 1.0e-3f;

    // Animaciones de saturacion (brasas, armonicos): solo mientras suena
    if (audio && ! vintageMode)
        for (int b = 0; b < kNumBands && ! dirty; ++b)
        {
            const auto& s = liveCurve[(size_t) b].s;
            dirty = s.on && typeCanSaturate (s.type) && s.driveDb > 0.01;
        }
    // Con el medidor de FPS visible se sigue dibujando para poder medir
    {
        const bool mv = proc.match.previewVisible.load() && proc.match.hasTarget();
        const int ver = mv ? proc.match.getTargetVersion() : -1;
        if (ver != lastMatchVersion) { lastMatchVersion = ver; dirty = true; }
    }
    if (showFps) { fpsAcc += dt; if (fpsAcc > 0.25f) { fpsAcc = 0; dirty = true; } }
    if (! vintageMode && ! opts.freeze) dirty = detectPeaks (dt) || dirty;
    if (clashTarget >= 0) dirty = updateClash() || dirty;
    // SKETCH: ajuste en vivo pendiente, tinta que se desvanece, anillos de las bandas nuevas
    if (inking && sketchFitStale && juce::Time::getMillisecondCounterHiRes() - lastSketchFitMs > 33.0) { refitSketch(); dirty = true; }
    if (inkFade > 0.0f)
    {
        inkFade = juce::jmax (0.0f, inkFade - dt / 0.55f);
        if (inkFade <= 0.0f && ! inking) sketch.clear();
        dirty = true;
    }
    if (! bursts.empty())
    {
        for (auto& b : bursts) b.age += dt / 0.7f;
        bursts.erase (std::remove_if (bursts.begin(), bursts.end(), [] (const Burst& b) { return b.age >= 1.0f; }), bursts.end());
        dirty = true;
    }
    lastDirty = dirty;
    if (dirty) repaint();
}

bool SpectrumDisplay::updateAnalyzer (Analyzer& an, AnalyzerFifo& fifo, float dt)
{
    const int n = fifo.pull (pullBuffer.data(), (int) pullBuffer.size());
    for (int i = 0; i < n; ++i)
    {
        an.ring[(size_t) an.pos] = pullBuffer[(size_t) i];
        an.pos = (an.pos + 1) % kFftSize;
    }
    const bool newData = n > 0;
    if (newData) an.fresh = true;
    if (opts.freeze && an.visible) { an.fresh = false; return false; }   // CONGELADO: se queda la ultima foto

    if (proc.params.analyzer->load() < 0.5f)
    {
        const bool was = an.visible;
        an.fill.clear(); an.outline.clear(); an.visible = false;
        std::fill (an.smoothed.begin(), an.smoothed.end(), -140.0f);
        return was;
    }
    if (! newData && ! an.visible) return false;   // nada que mover: no se redibuja

    // Balistica en tiempo real: subida rapida, caida de ~44 dB/s (independiente de los FPS)
    static constexpr float kAtk[3] = { 0.06f, 0.025f, 0.01f }, kFall[3] = { 20.0f, 44.0f, 90.0f };   // lenta / media / rapida
    const float attack = 1.0f - std::pow (0.4f, dt / kAtk[opts.speed]);
    const float fall = kFall[opts.speed] * dt;
    float top = -140.0f;
    if (an.fresh)
    {
        an.fresh = false;
        for (int i = 0; i < kFftSize; ++i)
            fftData[(size_t) i] = an.ring[(size_t) ((an.pos + i) % kFftSize)];
        std::fill (fftData.begin() + kFftSize, fftData.end(), 0.0f);
        window.multiplyWithWindowingTable (fftData.data(), (size_t) kFftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const float norm = 4.0f / (float) kFftSize;
        const double fs = proc.currentSampleRate.load();
        if (tiltFs != fs)
        {
            // Inclinacion de +4.5 dB/oct para que el espectro se lea "plano" con musica (tabla fija)
            tiltFs = fs; tilt.resize ((size_t) kFftSize / 2 + 1);
            for (int k = 1; k <= kFftSize / 2; ++k)
                tilt[(size_t) k] = (float) (kTilts[opts.tilt] * std::log2 (juce::jmax (k * fs / kFftSize, 20.0) / 1000.0));
        }
        const bool isPost = &an == &post;
        if (isPost && longDb.size() != an.smoothed.size()) longDb.assign (an.smoothed.size(), -140.0f);
        const float kl = 1.0f - std::exp (-dt / 1.2f);   // promedio de ~1.2 s (marcas estables)
        for (int k = 1; k <= kFftSize / 2; ++k)
        {
            const float db = juce::Decibels::gainToDecibels (fftData[(size_t) k] * norm, -140.0f) + tilt[(size_t) k];
            if (isPost) longDb[(size_t) k] += (db - longDb[(size_t) k]) * kl;
            float& sm = an.smoothed[(size_t) k];
            sm = db > sm ? sm + attack * (db - sm) : juce::jmax (db, sm - fall);
            top = juce::jmax (top, sm);
        }
    }
    else
    {
        for (auto& sm : an.smoothed) { sm = juce::jmax (-140.0f, sm - fall); top = juce::jmax (top, sm); }
        if (&an == &post) for (auto& v : longDb) v = juce::jmax (-140.0f, v - fall);
    }
    const bool wasVisible = an.visible;
    an.visible = top > kAnMin;
    if (! an.visible && ! wasVisible)
    {
        // solo llega silencio: nada visible que mover (no se redibuja)
        if (! an.fill.isEmpty()) { an.fill.clear(); an.outline.clear(); return true; }
        return false;
    }

    auto a = plotArea();
    const double fs = proc.currentSampleRate.load();
    std::vector<juce::Point<float>> pts;
    pts.reserve ((size_t) (a.getWidth() / 3.0f) + 4);
    for (float x = a.getX(); x <= a.getRight() + 3.0f; x += 3.0f)
    {
        const double f0 = xToFreq (x), f1 = xToFreq (x + 3.0f);
        int k0 = (int) std::floor (f0 * kFftSize / fs), k1 = (int) std::ceil (f1 * kFftSize / fs);
        k0 = juce::jlimit (1, kFftSize / 2, k0);
        k1 = juce::jlimit (k0, kFftSize / 2, k1);
        float db = -140.0f;
        if (k1 - k0 <= 1)
        {
            const double kf = f0 * kFftSize / fs;
            const int ka = juce::jlimit (1, kFftSize / 2 - 1, (int) kf);
            const float tt = (float) juce::jlimit (0.0, 1.0, kf - ka);
            db = an.smoothed[(size_t) ka] + tt * (an.smoothed[(size_t) ka + 1] - an.smoothed[(size_t) ka]);
        }
        else
        {
            for (int k = k0; k <= k1; ++k) db = juce::jmax (db, an.smoothed[(size_t) k]);
        }
        pts.push_back ({ x, analyzerDbToY (db) });
    }
    // Polilinea (a 3 px se ve igual que una curva y tiene muchos menos bordes que rasterizar)
    an.outline.clear();
    an.outline.preallocateSpace ((int) pts.size() * 3 + 12);
    an.outline.startNewSubPath (pts.front());
    for (size_t i = 1; i < pts.size(); ++i) an.outline.lineTo (pts[i]);
    an.fill = an.outline;
    an.fill.lineTo (a.getRight() + 3.0f, a.getBottom());
    an.fill.lineTo (a.getX(), a.getBottom());
    an.fill.closeSubPath();
    return true;
}

//==============================================================================
// Curvas en cache
bool SpectrumDisplay::updateCurve (Curve& c, const BandSettings& s)
{
    if (c.valid && sameCurve (c.s, s) && c.db.size() == gridFreqs.size()) { c.s = s; return false; }
    c.s = s; c.valid = true; c.version = ++curveSerial;
    c.db.resize (gridFreqs.size());
    responseOfBandDb (s, gridFreqs.data(), (int) gridFreqs.size(), gridFs, gridLin, c.db.data(), ! gridNat, gridBrickRes);
    return true;
}

juce::int64 SpectrumDisplay::stateSignature() const
{
    // Todo lo que cambia el dibujo y no es una curva (colores, saturacion, dinamica, solo, zona...)
    juce::uint64 h = 1469598103934665603ull;
    auto mix = [&h] (double v) { juce::uint64 u; std::memcpy (&u, &v, sizeof u); h = (h ^ u) * 1099511628211ull; };
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& s = liveCurve[(size_t) b].s;
        mix (s.stereo); mix (s.driveDb); mix (s.mix); mix (s.dynOn); mix (s.rangeDb); mix (s.satType);
    }
    mix (proc.soloBand.load()); mix (proc.params.rangeOn->load()); mix (proc.params.rangeLo->load()); mix (proc.params.rangeHi->load());
    mix (proc.params.analyzer->load()); mix (selectedBand); mix (vintageMode);
    return (juce::int64) h;
}

bool SpectrumDisplay::refreshCurves()
{
    auto a = plotArea();
    const double fs = proc.engineSampleRate.load();
    // fase lineal o natural: suena (y se dibuja) la curva analogica exacta
    const bool lin = proc.linearActiveNow.load() || proc.naturalActiveNow.load();
    bool changed = false;
    const bool nat = proc.naturalActiveNow.load();   // (natural: el BRICK es el de 144 dB/oct, no la pared ideal)
    double brickRes = 0.0;   // (fase lineal: la pared BRICK que el FIR puede hacer)
    brickRes = proc.linearBrickResolution();
    if (a != gridArea || fs != gridFs || lin != gridLin || nat != gridNat || brickRes != gridBrickRes || dbRange != curvesDbRange)
    {
        gridArea = a; gridFs = fs; gridLin = lin; gridNat = nat; gridBrickRes = brickRes; curvesDbRange = dbRange;
        gridFreqs.clear(); gridX.clear();
        for (float x = a.getX(); x <= a.getRight() + 1.0f; x += 2.0f) { gridX.push_back (x); gridFreqs.push_back (xToFreq (x)); }
        gridExIdx.clear();
        for (double f : gridFreqs)
            gridExIdx.push_back ((float) juce::jlimit (0.0, (double) kSpectralExportPoints - 1.0, std::log (f / 20.0) / std::log (1000.0) * kSpectralExportPoints - 0.5));
        for (auto* arr : { &liveCurve, &statCurve, &fullCurve }) for (auto& c : *arr) c.valid = false;
        vintageValid = false;
        changed = true;
    }

    if (vintageMode)
    {
        const auto v = proc.params.vintage();
        if (! vintageValid || ! sameVintage (v, vintageCached) || vintageDb.size() != gridFreqs.size())
        {
            vintageCached = v; vintageValid = true;
            vintageDb.resize (gridFreqs.size());
            responseOfVintageDb (v, gridFreqs.data(), (int) gridFreqs.size(), fs, vintageDb.data());
            changed = true;
        }
    }
    else
    {
        bool totalChanged = changed || totalDb.size() != gridFreqs.size();
        for (int b = 0; b < kNumBands; ++b)
        {
            auto live = proc.params.band (b);
            const float dg = dynamicGain (b);
            const bool dyn = live.on && live.dynOn && typeCanBeDynamic (live.type);
            auto stat = live;
            live.gainDb += dg;   // la curva muestra la EQ dinamica en vivo
            totalChanged = updateCurve (liveCurve[(size_t) b], live) || totalChanged;
            if (dyn)
            {
                auto full = stat; full.gainDb += stat.rangeDb;
                changed = updateCurve (statCurve[(size_t) b], stat) || changed;
                changed = updateCurve (fullCurve[(size_t) b], full) || changed;
            }
            // Dinamica espectral: estatica + lo que el motor esta haciendo en cada frecuencia
            const bool spec = dyn && isSpectral (b);
            if (spec)
            {
                const auto& ex = proc.getSpectral().exportDb[(size_t) b];
                auto& last = specLast[(size_t) b];
                bool moved = ! specOn[(size_t) b] || specDb[(size_t) b].size() != gridFreqs.size() || liveCurve[(size_t) b].version != specBase[(size_t) b];
                for (int i = 0; i < kSpectralExportPoints; ++i)
                {
                    const float v = ex[(size_t) i].load (std::memory_order_relaxed);
                    if (std::abs (v - last[(size_t) i]) > 0.02f) moved = true;
                    last[(size_t) i] = v;
                }
                if (moved)
                {
                    auto& d = specDb[(size_t) b];
                    d.resize (gridFreqs.size());
                    const auto& base = liveCurve[(size_t) b].db;
                    for (size_t i = 0; i < d.size(); ++i)
                    {
                        const float t = gridExIdx[i];
                        const int i0 = (int) t; const int i1 = std::min (kSpectralExportPoints - 1, i0 + 1);
                        const float fr = t - (float) i0;
                        d[i] = base[i] + last[(size_t) i0] + fr * (last[(size_t) i1] - last[(size_t) i0]);
                    }
                    specVersion[(size_t) b] = ++curveSerial;
                    specBase[(size_t) b] = liveCurve[(size_t) b].version;
                    totalChanged = true;
                }
            }
            if (spec != specOn[(size_t) b]) { specOn[(size_t) b] = spec; totalChanged = true; }
        }
        if (totalChanged)
        {
            totalDb.assign (gridFreqs.size(), 0.0f);
            for (int b = 0; b < kNumBands; ++b)
                if (liveCurve[(size_t) b].s.on) juce::FloatVectorOperations::add (totalDb.data(), liveDb (b).data(), (int) totalDb.size());
            totalVersion = ++curveSerial;
            changed = true;
        }
    }
    const auto sig = stateSignature();
    if (sig != lastSignature) { lastSignature = sig; changed = true; }
    return changed;
}

void SpectrumDisplay::pathFromDb (const std::vector<float>& db, std::vector<juce::Point<float>>& pts) const
{
    pts.resize (db.size());
    for (size_t i = 0; i < db.size(); ++i) pts[i] = { gridX[i], dbToY (juce::jmax (-60.0f, db[i])) };
}

//==============================================================================
void SpectrumDisplay::paintGrid (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour lineCol, juce::Colour labelCol)
{
    const double minor[] = { 30, 40, 60, 70, 80, 90, 300, 400, 600, 700, 800, 900, 3000, 4000, 6000, 7000, 8000, 9000 };
    g.setColour (lineCol.withMultipliedAlpha (0.45f));
    for (double f : minor) g.fillRect (juce::Rectangle<float> (freqToX (f), a.getY(), 1.0f, a.getHeight()));

    const double major[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    const char* majorLbl[] = { "50", "100", "200", "500", "1k", "2k", "5k", "10k" };
    g.setFont (monoFont (9.5f));
    for (int i = 0; i < 8; ++i)
    {
        const float x = freqToX (major[i]);
        g.setColour (lineCol);
        g.fillRect (juce::Rectangle<float> (x, a.getY(), 1.0f, a.getHeight()));
        g.setColour (labelCol);
        g.drawText (majorLbl[i], juce::Rectangle<float> (x + 4, a.getBottom() - 16 - (vintageMode ? 0.0f : bottomInset()), 40, 12), juce::Justification::centredLeft);
    }

    const int step = vintageMode ? 5 : 6;
    for (int db = -(int) dbRange + step; db < (int) dbRange; db += step)
    {
        const float y = dbToY (db);
        g.setColour (db == 0 ? lineCol.brighter (0.35f) : lineCol.withMultipliedAlpha (0.8f));
        g.fillRect (juce::Rectangle<float> (a.getX(), y, a.getWidth(), 1.0f));
        g.setColour (labelCol);
        g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (a.getRight() - 34, y - 13, 30, 12),
                    juce::Justification::centredRight);
    }
}

void SpectrumDisplay::paint (juce::Graphics& g)
{
    clock.paintBegin();
    struct EndMeasure { FrameClock& c; ~EndMeasure() { c.paintEnd(); } } endMeasure { clock };
    refreshCurves();   // por si se dibuja sin pasar por un cuadro (cambio de tamano, captura)
    auto a = plotArea();
    const auto bounds = getLocalBounds();
    const double t = animTime;
    const juce::int64 layoutKey = (juce::int64) (dbRange * 100) + (vintageMode ? 1000000 : 0) + (opts.piano ? 10000000 : 0);
    std::vector<juce::Point<float>> pts;

    if (vintageMode)
    {
        // Pantalla del cajon "Avanzado": cristal oscuro calido (fondo y grilla en cache)
        bgLayer.draw (g, bounds, layoutKey, [&] (juce::Graphics& gi)
        {
            gi.setGradientFill (juce::ColourGradient (juce::Colour (0xff12100c), 0, 0, juce::Colour (0xff0a0907), 0, (float) getHeight(), false));
            gi.fillAll();
            paintGrid (gi, a, juce::Colour (0xff2a241b), Colours::cream.withAlpha (0.35f));
        });

        if (! post.fill.isEmpty())
        {
            g.setGradientFill (juce::ColourGradient (Colours::cream.withAlpha (0.14f), 0, a.getY(), Colours::cream.withAlpha (0.0f), 0, a.getBottom(), false));
            g.fillPath (post.fill);
            g.setColour (Colours::cream.withAlpha (0.25f));
            g.strokePath (post.outline, juce::PathStrokeType (1.0f));
        }

        // Misma curva que el audio: mismo diseno, misma frecuencia interna (con oversampling) e interruptores IN
        pathFromDb (vintageDb, pts);
        auto curve = smoothPath (pts);
        juce::Path fill (curve);
        fill.lineTo (a.getRight(), dbToY (0)); fill.lineTo (a.getX(), dbToY (0)); fill.closeSubPath();
        g.setColour (Colours::lamp.withAlpha (0.10f));
        g.fillPath (fill);
        g.setColour (Colours::lamp.withAlpha (0.18f));
        g.strokePath (curve, juce::PathStrokeType (7.0f));
        g.setColour (Colours::lamp);
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        paintZone (g, a);

        // Reflejo del cristal
        glassLayer.draw (g, bounds, layoutKey, [&] (juce::Graphics& gi)
        {
            gi.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.05f), 0, 0, juce::Colours::white.withAlpha (0.0f), 0, (float) getHeight() * 0.4f, false));
            gi.fillRect (getLocalBounds().toFloat().removeFromTop ((float) getHeight() * 0.4f));
        });
        return;
    }

    // ---- Modo parametrico ----
    // Fondo (degradado + luz naranja) y resplandor rojo en cache; el resplandor late con la musica
    // Fondo, luz naranja, lineas de barrido y grilla: una sola imagen opaca en cache (se copia, no se mezcla)
    bgLayer.opaque = true;
    bgLayer.draw (g, bounds, layoutKey, [&] (juce::Graphics& gi)
    {
        gi.setGradientFill (juce::ColourGradient (Colours::inkHi, 0, 0, Colours::ink, 0, (float) getHeight(), false));
        gi.fillAll();
        juce::ColourGradient top (Colours::orange.withAlpha (0.06f), a.getRight(), a.getY(), Colours::orange.withAlpha (0.0f), a.getRight() - a.getWidth() * 0.5f, a.getY() + 200, true);
        gi.setGradientFill (top);
        gi.fillRect (a);
        gi.setColour (juce::Colours::black.withAlpha (0.18f));
        for (float y = a.getY(); y < a.getBottom(); y += 3.0f)
            gi.fillRect (a.getX(), y, a.getWidth(), 1.0f);
        paintGrid (gi, a, Colours::line, Colours::textDim.withAlpha (0.75f));
    });
    {
        // Resplandor rojo que late con la musica (como luz de club): solo el circulo donde se ve
        const float pulse = juce::jlimit (0.0f, 1.0f, bloom);
        const juce::Point<float> gc (a.getCentreX(), a.getBottom() + 60.0f);
        const float R = gc.y - (a.getY() + a.getHeight() * 0.45f);
        const auto glowArea = juce::Rectangle<float> (gc.x - R, gc.y - R, 2 * R, R).getIntersection (a).getSmallestIntegerContainer();
        glowLayer.draw (g, glowArea, layoutKey, [&] (juce::Graphics& gi)
        {
            juce::ColourGradient glow (Colours::accent, gc.x, gc.y, Colours::accent.withAlpha (0.0f), gc.x, gc.y - R, true);
            gi.setGradientFill (glow);
            gi.fillRect (glowArea);
        }, 0.10f + 0.28f * pulse);
    }

    // Analizador: entrada (antes del EQ) tenue y salida (despues) con degradado
    if (! pre.outline.isEmpty())
    {
        // (solo el contorno: un relleno casi transparente costaba ~12 % del cuadro y casi no se veia)
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.strokePath (pre.outline, juce::PathStrokeType (1.0f));
    }
    if (! post.fill.isEmpty())
    {
        juce::ColourGradient grad (Colours::accent.withAlpha (0.30f), 0, a.getY() + a.getHeight() * 0.25f,
                                   juce::Colour (0xff3a0610).withAlpha (0.0f), 0, a.getBottom(), false);
        g.setGradientFill (grad);
        g.fillPath (post.fill);
        g.setColour (juce::Colour (0xffff6a5a).withAlpha (0.38f));
        g.strokePath (post.outline, juce::PathStrokeType (1.0f));
    }

    std::array<BandSettings, kNumBands> bs;
    for (int b = 0; b < kNumBands; ++b) bs[(size_t) b] = liveCurve[(size_t) b].s;   // con la EQ dinamica en vivo
    const int soloing = proc.soloBand.load();

    g.saveState();
    g.reduceClipRegion (a.toNearestInt());

    // Marcadores de armonicos de las bandas que saturan
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& s = bs[(size_t) b];
        if (! s.on || ! typeCanSaturate (s.type) || s.driveDb < 0.01) continue;
        const bool focus = b == selectedBand || b == hoverBand || b == dragBand;
        const float amt = (float) (s.driveDb / 36.0) * (float) s.mix;
        for (int k = 2; k <= 7; ++k)
        {
            const double hf = s.freq * k;
            if (hf > kMaxF) break;
            const float x = freqToX (hf);
            const float h = a.getHeight() * (0.10f + 0.32f * amt) / (float) (k - 1);
            const float shimmer = 0.75f + 0.25f * (float) std::sin (t * 3.0 + k * 1.3 + b);
            auto bar = juce::Rectangle<float> (x - 1.0f, a.getBottom() - h, 2.0f, h);
            g.setGradientFill (juce::ColourGradient (Colours::ember.withAlpha ((focus ? 0.75f : 0.35f) * shimmer), x, bar.getY(),
                                                     Colours::ember.withAlpha (0.0f), x, a.getBottom(), false));
            g.fillRect (bar);
            g.setColour (Colours::emberHot.withAlpha ((focus ? 0.9f : 0.45f) * shimmer));
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ x, bar.getY() }));
            if (focus)
            {
                g.setColour (Colours::ember.withAlpha (0.7f));
                g.setFont (monoFont (9.0f));
                g.drawText (juce::String (k) + "H", juce::Rectangle<float> (x - 12, bar.getY() - 15, 24, 11), juce::Justification::centred);
            }
        }
    }

    // EQ dinamica (estilo "lo que se esta comprimiendo"): para cada banda dinamica se dibuja
    //  - la curva estatica (sin dinamica) en linea fina,
    //  - el rango maximo (estatica + Range) en linea punteada,
    //  - y RELLENO entre la estatica y la curva actual: es exactamente lo que se esta quitando o sumando ahora.
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& live = bs[(size_t) b];
        if (! live.on || ! live.dynOn || ! typeCanBeDynamic (live.type)) continue;
        const auto col = colourOf (b);
        const auto dyn = Colours::dynamic;
        const auto& stat = statCurve[(size_t) b].s;
        // Los trazos se generan solo cuando cambia alguna de las tres curvas (estatica, en vivo, rango)
        const auto vs = statCurve[(size_t) b].version, vl = liveVersion (b), vf = fullCurve[(size_t) b].version;
        auto& sc = dynStrokes[(size_t) b];
        std::vector<juce::Point<float>> ps, pl, pf;
        auto need = [&] (std::vector<juce::Point<float>>& v, const std::vector<float>& db) { if (v.empty()) pathFromDb (db, v); return std::cref (v); };
        // Zona de rango posible (muy tenue)
        g.setColour (dyn.withAlpha (0.06f));
        g.fillPath (sc[0].get ((vs << 32) ^ vf, [&] (juce::Path& range)
        {
            range = linePath (need (ps, statCurve[(size_t) b].db));
            const auto& f = need (pf, fullCurve[(size_t) b].db).get();
            for (auto it = f.rbegin(); it != f.rend(); ++it) range.lineTo (*it);
            range.closeSubPath();
        }));
        // Accion en vivo
        {
            const float act = isSpectral (b) ? proc.dynGainDb[(size_t) b].load() : dynamicGain (b);
            const float amount = juce::jlimit (0.0f, 1.0f, std::abs (act) / juce::jmax (1.0f, (float) std::abs (stat.rangeDb)));
            g.setColour (dyn.withAlpha (0.22f + 0.45f * amount));
            g.fillPath (sc[1].get ((vs << 32) ^ vl, [&] (juce::Path& act)
            {
                act = linePath (need (ps, statCurve[(size_t) b].db));
                const auto& l = need (pl, liveDb (b)).get();
                for (auto it = l.rbegin(); it != l.rend(); ++it) act.lineTo (*it);
                act.closeSubPath();
            }));
            g.setColour (dyn.withAlpha (0.5f + 0.4f * amount));
            g.fillPath (sc[2].get (vl, [&] (juce::Path& out)
            {
                juce::PathStrokeType (1.2f).createStrokedPath (out, linePath (need (pl, liveDb (b))));
            }));
        }
        g.setColour (dyn.withAlpha (b == selectedBand ? 0.85f : 0.5f));
        g.fillPath (sc[3].get (vf, [&] (juce::Path& dashed)
        {
            const float dashes[] = { 4.0f, 4.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, linePath (need (pf, fullCurve[(size_t) b].db)), dashes, 2);
        }));
        g.setColour (col.withAlpha (0.45f));
        g.fillPath (sc[4].get (vs, [&] (juce::Path& out)
        {
            juce::PathStrokeType (1.0f).createStrokedPath (out, linePath (need (ps, statCurve[(size_t) b].db)));
        }));
    }

    // Area de la banda enfocada
    const int focus = dragBand >= 0 ? dragBand : (hoverBand >= 0 ? hoverBand : selectedBand);

    // Cada banda deja su huella en su color (aunque no este elegida): relleno muy suave hasta 0 dB y una linea fina
    if (opts.bandCurves && ! vintageMode)
    {
        const float y0 = dbToY (0);
        for (int b = 0; b < kNumBands; ++b)
        {
            if (b == focus || ! bs[(size_t) b].on) continue;
            if (soloing >= 0 && soloing != b) continue;
            const auto col = colourOf (b);
            const auto v = liveVersion (b) ^ ((juce::int64) juce::roundToInt (a.getWidth() * 8 + a.getHeight()) << 40);
            std::vector<juce::Point<float>> bp;
            auto build = [&] { if (bp.empty()) pathFromDb (liveDb (b), bp); };
            const auto np = nodePosition (b);
            g.setGradientFill (juce::ColourGradient (col.withAlpha (0.20f), np.x, np.y, col.withAlpha (0.03f), np.x, y0, false));
            g.fillPath (bandFill[(size_t) b].get (v, [&] (juce::Path& fill)
            {
                build();
                fill = linePath (bp);
                fill.lineTo (a.getRight(), y0); fill.lineTo (a.getX(), y0); fill.closeSubPath();
            }));
            g.setColour (col.brighter (0.2f).withAlpha (0.80f));
            g.fillPath (bandLine[(size_t) b].get (v, [&] (juce::Path& out) { build(); juce::PathStrokeType (1.4f).createStrokedPath (out, linePath (bp)); }));
        }
    }
    if (focus >= 0 && bs[(size_t) focus].on)
    {
        const auto col = colourOf (focus);
        const auto v = liveVersion (focus);
        auto buildPts = [&] { if (pts.empty()) pathFromDb (liveDb (focus), pts); };
        const float y0 = dbToY (0);
        const auto pos = nodePosition (focus);
        g.setGradientFill (juce::ColourGradient (col.withAlpha (0.30f), pos.x, pos.y, col.withAlpha (0.02f), pos.x, y0, false));
        g.fillPath (focusFill.get (v, [&] (juce::Path& fill)
        {
            buildPts();
            fill = linePath (pts);
            fill.lineTo (a.getRight(), y0); fill.lineTo (a.getX(), y0); fill.closeSubPath();
        }));
        g.setColour (col.withAlpha (0.55f));
        g.fillPath (focusLine.get (v, [&] (juce::Path& out) { buildPts(); juce::PathStrokeType (1.2f).createStrokedPath (out, linePath (pts)); }));
        pts.clear();
    }

    // Curva total con resplandor
    {
        // Los tres trazos (resplandor ancho, medio y linea) se generan solo cuando cambia la curva
        juce::Path total;
        auto buildTotal = [&] { if (total.isEmpty()) { pathFromDb (totalDb, pts); total = linePath (pts); } };
        auto stroke = [&] (float w) { return [&, w] (juce::Path& out)
        {
            buildTotal();
            juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (out, total);
        }; };
        g.setColour (Colours::accent.withAlpha (0.10f));
        g.fillPath (totalGlowWide.get (totalVersion, stroke (12.0f)));
        g.setColour (Colours::accent.withAlpha (0.28f));
        g.fillPath (totalGlow.get (totalVersion, stroke (5.0f)));
        g.setColour (juce::Colour (0xfffff3f0));
        g.fillPath (totalLine.get (totalVersion, stroke (2.0f)));
    }
    // EQ MATCH: curva objetivo (turquesa punteada) mientras el panel esta abierto
    if (proc.match.previewVisible.load() && proc.match.hasTarget())
    {
        const auto& gf = proc.match.targetGrid(); const auto& td = proc.match.targetDb();
        juce::Path tp;
        for (size_t i = 0; i < gf.size(); ++i)
        {
            const juce::Point<float> pt (freqToX (gf[i]), dbToY (juce::jlimit (-dbRange, dbRange, td[i])));
            if (i == 0) tp.startNewSubPath (pt); else tp.lineTo (pt);
        }
        juce::Path dashed;
        const float dashes[] = { 6.0f, 4.0f };
        juce::PathStrokeType (2.0f).createDashedStroke (dashed, tp, dashes, 2);
        g.setColour (Colours::match.withAlpha (0.18f));
        g.strokePath (tp, juce::PathStrokeType (7.0f));
        g.setColour (Colours::match);
        g.fillPath (dashed);
        g.setFont (labelFont (9.5f));
        g.drawText ("MATCH", juce::Rectangle<float> (60, 14).withPosition (a.getX() + 12, a.getY() + 44), juce::Justification::centredLeft);
    }
    g.restoreState();
    paintZone (g, a);

    // Puntos de las bandas
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& s = bs[(size_t) b];
        if (! s.on) continue;
        const auto pos = nodePosition (b);
        const auto col = colourOf (b);
        const bool sel = b == selectedBand;
        const bool hot = sel || b == hoverBand || b == dragBand;

        // EQ dinamica: punto fantasma en la posicion estatica
        if (s.dynOn && typeCanBeDynamic (s.type))
        {
            const float yStatic = dbToY (s.gainDb - dynamicGain (b));
            g.setColour (Colours::dynamic.withAlpha (0.6f));
            g.drawEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ pos.x, yStatic }), 1.2f);
        }
        g.setOpacity (soloing >= 0 && soloing != b ? 0.3f : 1.0f);

        // Anillo de brasa si la banda satura (late suavemente)
        if (typeCanSaturate (s.type) && s.driveDb > 0.01)
        {
            const float amt = (float) (s.driveDb / 36.0) * (float) s.mix;
            const float pulse = 0.5f + 0.5f * (float) std::sin (t * 4.0 + b);
            const float rr = 14.0f + 5.0f * amt + 1.5f * pulse;
            juce::ColourGradient eg (Colours::ember.withAlpha (0.0f), pos.x, pos.y, Colours::ember.withAlpha (0.0f), pos.x + rr + 6, pos.y, true);
            eg.addColour (0.55, Colours::ember.withAlpha (0.18f + 0.3f * amt));
            g.setGradientFill (eg);
            g.fillEllipse (juce::Rectangle<float> ((rr + 6) * 2, (rr + 6) * 2).withCentre (pos));
            g.setColour (Colours::emberHot.withAlpha (0.55f + 0.35f * pulse * amt));
            g.drawEllipse (juce::Rectangle<float> (rr * 2 - 6, rr * 2 - 6).withCentre (pos), 1.3f);
        }

        // Halo
        const float glowR = hot ? 18.0f : 13.0f;
        g.setGradientFill (juce::ColourGradient (col.withAlpha (hot ? 0.45f : 0.25f), pos.x, pos.y, col.withAlpha (0.0f), pos.x + glowR, pos.y, true));
        g.fillEllipse (juce::Rectangle<float> (glowR * 2, glowR * 2).withCentre (pos));

        // Esfera
        const float r = hot ? 8.5f : 7.0f;
        auto orb = juce::Rectangle<float> (r * 2, r * 2).withCentre (pos);
        g.setGradientFill (juce::ColourGradient (col.brighter (0.6f), orb.getX() + r * 0.6f, orb.getY() + r * 0.5f,
                                                 col.darker (0.35f), orb.getRight(), orb.getBottom(), true));
        g.fillEllipse (orb);
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.fillEllipse (orb.reduced (r * 0.55f).translated (-r * 0.25f, -r * 0.3f));
        if (sel)
        {
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawEllipse (orb.expanded (3.5f), 1.4f);
        }
        if (multiSel[(size_t) b] && selectionCount() > 1)
        {
            // en el grupo seleccionado: anillo punteado que gira lento
            juce::Path ring; ring.addEllipse (orb.expanded (6.0f));
            juce::Path dashed; const float dl[] = { 3.0f, 3.0f };
            juce::PathStrokeType (1.3f).createDashedStroke (dashed, ring, dl, 2, juce::AffineTransform::rotation ((float) (t * 0.8), pos.x, pos.y));
            g.setColour (col.brighter (0.4f));
            g.fillPath (dashed);
        }
        g.setColour (juce::Colour (0xff0a0c10).withAlpha (0.85f));
        g.setFont (monoFont (hot ? 10.5f : 9.5f));
        g.drawText (juce::String (b + 1), orb.translated (0, 0.5f), juce::Justification::centred);

        // Letra del canal (L / R / M / S) bajo el punto
        if ((int) s.stereo != 0)
        {
            auto tag = juce::Rectangle<float> (14.0f, 12.0f).withCentre ({ pos.x, orb.getBottom() + 10.0f });
            g.setColour (Colours::ink.withAlpha (0.85f));
            g.fillRoundedRectangle (tag, 3.0f);
            g.setColour (col);
            g.drawRoundedRectangle (tag, 3.0f, 0.8f);
            g.setFont (monoFont (9.0f));
            g.drawText (Colours::stereoLetter ((int) s.stereo), tag, juce::Justification::centred);
        }
        // Indicador de dinamica: pequeno anillo ambar
        if (s.dynOn && typeCanBeDynamic (s.type))
        {
            g.setColour (Colours::dynamic.withAlpha (0.9f));
            g.drawEllipse (orb.expanded (sel ? 6.5f : 2.5f), 1.0f);
        }
    }
    g.setOpacity (1.0f);

    // Controles sobre la banda seleccionada: X para borrar y tirador del Range dinamico
    if (selectedBand >= 0 && bs[(size_t) selectedBand].on && dragMode != Drag::Node)
    {
        const auto xp = deleteBadgePosition (selectedBand);
        auto xr = juce::Rectangle<float> (16.0f, 16.0f).withCentre (xp);
        g.setColour (hoverDelete ? Colours::accent : Colours::glass.withAlpha (0.95f));
        g.fillEllipse (xr);
        g.setColour (hoverDelete ? juce::Colours::white : Colours::textDim);
        g.drawEllipse (xr, 1.0f);
        const auto xi = xr.reduced (5.0f);
        g.drawLine ({ xi.getTopLeft(), xi.getBottomRight() }, 1.6f);
        g.drawLine ({ xi.getTopRight(), xi.getBottomLeft() }, 1.6f);
    }
    if (selectedBand >= 0 && hasRangeHandle (selectedBand))
    {
        const auto rp = rangeHandlePosition (selectedBand);
        const auto st = nodePosition (selectedBand);
        const float yStatic = dbToY (proc.params.band (selectedBand).gainDb);
        g.setColour (Colours::dynamic.withAlpha (0.35f));
        g.drawLine (rp.x, yStatic, rp.x, rp.y, 1.0f);
        g.drawLine (rp.x, yStatic, st.x, yStatic, 1.0f);
        auto hr = juce::Rectangle<float> (22.0f, 10.0f).withCentre (rp);
        g.setColour (Colours::dynamic.withAlpha (hoverRange || dragMode == Drag::DynRange ? 1.0f : 0.8f));
        g.fillRoundedRectangle (hr, 5.0f);
        g.setColour (Colours::ink);
        g.drawLine (hr.getX() + 6, hr.getCentreY(), hr.getRight() - 6, hr.getCentreY(), 1.2f);
        if (hoverRange || dragMode == Drag::DynRange)
        {
            g.setFont (monoFont (10.5f));
            g.setColour (Colours::dynamic);
            g.drawText ("RANGE " + formatDb ((float) proc.params.band (selectedBand).rangeDb), juce::Rectangle<float> (110.0f, 14.0f).withPosition (rp.x - 122.0f, rp.y - 7.0f),
                        juce::Justification::centredRight);
        }
    }

    if (soloing >= 0)
    {
        auto badge = juce::Rectangle<float> (150, 26).withPosition (a.getX() + 16, a.getBottom() - 46);
        g.setColour (colourOf (soloing).withAlpha (0.2f));
        g.fillRoundedRectangle (badge, 13.0f);
        g.setColour (colourOf (soloing));
        g.drawRoundedRectangle (badge, 13.0f, 1.2f);
        g.setFont (displayFont (13.0f, 0.12f));
        g.drawText ("SOLO  BAND " + juce::String (soloing + 1), badge, juce::Justification::centred);
    }

    // Lectura de valores al pasar el mouse o arrastrar
    const int info = dragBand >= 0 ? dragBand : hoverBand;
    if (info >= 0 && bs[(size_t) info].on)
    {
        const auto& s = bs[(size_t) info];
        const auto pos = nodePosition (info);
        juce::String txt = formatFreq ((float) s.freq) + "  " + noteName (s.freq);
        if ((int) s.stereo != 0) txt = juce::String (kStereoNames[(int) s.stereo]).toUpperCase() + "  " + txt;
        if (typeHasGain (s.type)) txt << "  " << formatDb ((float) s.gainDb);
        txt << "  Q " << juce::String (s.q, 2);

        g.setFont (monoFont (11.0f));
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), txt) + 18.0f;
        auto box = juce::Rectangle<float> (w, 22.0f).withCentre ({ pos.x, pos.y - 40.0f }).constrainedWithin (a);
        g.setColour (Colours::glass.withAlpha (0.95f));
        g.fillRoundedRectangle (box, 11.0f);
        g.setColour (colourOf (info).withAlpha (0.6f));
        g.drawRoundedRectangle (box, 11.0f, 1.0f);
        g.setColour (Colours::text);
        g.drawText (txt, box, juce::Justification::centred);
    }

    if (! vintageMode) { paintClash (g, a); paintPeaks (g, a); paintSketch (g, a); }
    if (! lasso.isEmpty())
    {
        g.setColour (Colours::accent.withAlpha (0.07f));
        g.fillRoundedRectangle (lasso, 4.0f);
        juce::Path rp; rp.addRoundedRectangle (lasso, 4.0f);
        juce::Path dashed; const float dl[] = { 5.0f, 4.0f };
        juce::PathStrokeType (1.2f).createDashedStroke (dashed, rp, dl, 2);
        g.setColour (Colours::accent.withAlpha (0.9f));
        g.fillPath (dashed);
    }
    if (opts.freeze && ! vintageMode)
    {
        auto r = juce::Rectangle<float> (70.0f, 18.0f).withPosition (a.getRight() - 120.0f, a.getY() + 14.0f);
        g.setColour (juce::Colour (0xff9fd8ff).withAlpha (0.18f)); g.fillRoundedRectangle (r, 9.0f);
        g.setColour (juce::Colour (0xff9fd8ff)); g.setFont (labelFont (9.0f));
        g.drawText ("CONGELADO", r, juce::Justification::centred);
    }

    if (showFps)
    {
        const auto txt = juce::String (juce::roundToInt (clock.framesPerSecond())) + " FPS  |  " + juce::String (clock.averagePaintMs(), 1)
                         + " ms por cuadro  |  monitor " + juce::String (juce::roundToInt (clock.refreshHz())) + " Hz";
        g.setFont (monoFont (10.5f));
        auto box = juce::Rectangle<float> (300.0f, 20.0f).withPosition (a.getX() + 12.0f, a.getBottom() - 46.0f);
        g.setColour (Colours::glass.withAlpha (0.9f));
        g.fillRoundedRectangle (box, 10.0f);
        g.setColour (Colours::text);
        g.drawText (txt, box, juce::Justification::centred);
    }

    bool anyOn = false;
    for (auto& s : bs) anyOn = anyOn || s.on;
    if (! anyOn)
    {
        g.setColour (Colours::textDim);
        g.setFont (uiFont (14.0f));
        g.drawText ("Doble clic en cualquier punto para crear una banda", a.withTrimmedBottom (a.getHeight() * 0.25f), juce::Justification::centred);
    }
}

//==============================================================================
bool SpectrumDisplay::isSpectral (int band) const
{
    return proc.spectralActiveNow.load() && proc.params.bands[(size_t) band].spec->load() > 0.5f;
}

float SpectrumDisplay::dynamicGain (int band) const
{
    const auto s = proc.params.band (band);
    if (isSpectral (band)) return 0.0f;   // espectral: la ganancia de la banda no se mueve entera (se ve frecuencia por frecuencia)
    return (s.dynOn && typeCanBeDynamic (s.type)) ? proc.dynGainDb[(size_t) band].load() : 0.0f;
}

juce::Point<float> SpectrumDisplay::nodePosition (int band) const
{
    const auto s = proc.params.band (band);
    const float x = freqToX (s.freq);
    const float y = typeHasGain (s.type) ? dbToY (s.gainDb + dynamicGain (band)) : dbToY (0.0);
    return { x, y };
}

juce::Colour SpectrumDisplay::colourOf (int band) const
{
    return Colours::bandFor (band, (int) proc.params.bands[(size_t) band].stereo->load());
}

juce::Point<float> SpectrumDisplay::deleteBadgePosition (int band) const
{
    auto p = nodePosition (band).translated (17.0f, -17.0f);
    auto a = plotArea().reduced (10.0f);
    if (p.x > a.getRight()) p.x -= 34.0f;
    if (p.y < a.getY()) p.y += 34.0f;
    return p;
}

bool SpectrumDisplay::hasRangeHandle (int band) const
{
    if (vintageMode || band < 0) return false;
    const auto s = proc.params.band (band);
    return s.on && s.dynOn && typeCanBeDynamic (s.type) && typeHasGain (s.type);
}

juce::Point<float> SpectrumDisplay::rangeHandlePosition (int band) const
{
    const auto s = proc.params.band (band);
    const float y = juce::jlimit (dbToY (dbRange) + 6.0f, dbToY (-dbRange) - 6.0f, dbToY (s.gainDb + s.rangeDb));
    // Al costado izquierdo del punto, para no tapar nunca el punto ni la X
    float x = freqToX (s.freq) - 26.0f;
    if (x < plotArea().getX() + 14.0f) x += 52.0f;
    return { x, y };
}

float SpectrumDisplay::zoneX (bool high) const
{
    return freqToX (high ? proc.params.rangeHi->load() : proc.params.rangeLo->load());
}

int SpectrumDisplay::hitTestZone (juce::Point<float> p) const
{
    if (proc.params.rangeOn->load() < 0.5f) return 0;
    auto a = plotArea();
    const bool inTab = p.y < a.getY() + 24.0f;
    for (int h : { 1, 2 })
    {
        const float x = zoneX (h == 2);
        const float tabX = h == 1 ? x - 46.0f : x;   // la pestana queda del lado de afuera
        if (inTab && p.x >= tabX && p.x <= tabX + 46.0f) return h;
        if (std::abs (p.x - x) < 5.0f) return h;
    }
    return 0;
}

// Zona de proceso: lo de afuera se ve apagado y rayado, con un tirador en cada borde
void SpectrumDisplay::paintZone (juce::Graphics& g, juce::Rectangle<float> a)
{
    if (proc.params.rangeOn->load() < 0.5f) return;
    const float xl = zoneX (false), xh = zoneX (true);
    const auto tint = vintageMode ? Colours::cream : Colours::text;
    const auto edge = vintageMode ? Colours::lamp : Colours::text;
    for (auto r : { a.withRight (xl), a.withLeft (xh) })
    {
        if (r.getWidth() <= 0.5f) continue;
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (r);
        g.saveState();
        g.reduceClipRegion (r.toNearestInt());
        g.setColour (tint.withAlpha (0.05f));
        for (float x = r.getX() - a.getHeight(); x < r.getRight(); x += 10.0f)
            g.drawLine (x, a.getBottom(), x + a.getHeight(), a.getY(), 1.0f);
        g.restoreState();
    }
    for (int h : { 1, 2 })
    {
        const float x = h == 1 ? xl : xh;
        const bool hot = hoverZone == h || (dragMode == (h == 1 ? Drag::ZoneLo : Drag::ZoneHi));
        g.setColour (edge.withAlpha (hot ? 0.9f : 0.5f));
        g.fillRect (juce::Rectangle<float> (x - 0.75f, a.getY(), 1.5f, a.getHeight()));
        auto tab = juce::Rectangle<float> (46.0f, 18.0f).withPosition (h == 1 ? x - 46.0f : x, a.getY() + 3.0f);
        g.setColour ((vintageMode ? juce::Colour (0xff1a1611) : Colours::glass).withAlpha (0.95f));
        g.fillRoundedRectangle (tab, 4.0f);
        g.setColour (edge.withAlpha (hot ? 1.0f : 0.6f));
        g.drawRoundedRectangle (tab, 4.0f, 1.0f);
        g.setFont (monoFont (9.5f));
        g.drawText (formatFreq (h == 1 ? proc.params.rangeLo->load() : proc.params.rangeHi->load()), tab, juce::Justification::centred);
    }
}

int SpectrumDisplay::hitTestNode (juce::Point<float> p) const
{
    if (vintageMode) return -1;
    int best = -1;
    float bestDist = 14.0f;
    for (int b = kNumBands - 1; b >= 0; --b)
    {
        if (proc.params.bands[(size_t) b].on->load() < 0.5f) continue;
        const float d = nodePosition (b).getDistanceFrom (p);
        if (d < bestDist) { bestDist = d; best = b; }
    }
    return best;
}

juce::RangedAudioParameter* SpectrumDisplay::param (int band, const char* name) const
{
    return proc.apvts.getParameter (ParamID::band (band, name));
}

void SpectrumDisplay::setParam (juce::RangedAudioParameter* p, float value)
{
    if (p != nullptr)
        p->setValueNotifyingHost (p->convertTo0to1 (p->getNormalisableRange().snapToLegalValue (value)));
}

void SpectrumDisplay::beginGesture (int band)
{
    param (band, "freq")->beginChangeGesture();
    param (band, "gain")->beginChangeGesture();
}

void SpectrumDisplay::endGesture()
{
    if (dragBand < 0) return;
    param (dragBand, "freq")->endChangeGesture();
    param (dragBand, "gain")->endChangeGesture();
}

//==============================================================================
void SpectrumDisplay::mouseMove (const juce::MouseEvent& e)
{
    const bool sb = ! vintageMode && sketchButtonBounds().contains (e.position);
    if (sb != hoverSketchButton) { hoverSketchButton = sb; repaint (sketchButtonBounds().expanded (4).toNearestInt()); }
    if (sb) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    const bool cb = ! vintageMode && clashButtonBounds().contains (e.position);
    if (cb != hoverClashButton) { hoverClashButton = cb; repaint (clashButtonBounds().expanded (4).toNearestInt()); }
    if (cb) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    {
        const int hc = vintageMode ? -1 : hitTestClash (e.position);
        if (hc != hoverClash) { hoverClash = hc; repaint(); }
        if (hc >= 0) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    }
    const int h = hitTestNode (e.position);
    {
        const int hp = h < 0 && ! sketchMode ? hitTestPeak (e.position) : -1;
        if (hp != hoverPeak) { hoverPeak = hp; repaint(); }
        if (hp >= 0) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    }
    const bool del = ! vintageMode && selectedBand >= 0 && proc.params.bands[(size_t) selectedBand].on->load() > 0.5f
                     && deleteBadgePosition (selectedBand).getDistanceFrom (e.position) < 10.0f;
    const bool rng = hasRangeHandle (selectedBand) && rangeHandlePosition (selectedBand).getDistanceFrom (e.position) < 12.0f;
    const int z = (h < 0 && ! del && ! rng) ? hitTestZone (e.position) : 0;
    if (h != hoverBand || del != hoverDelete || rng != hoverRange || z != hoverZone)
    {
        hoverBand = h; hoverDelete = del; hoverRange = rng; hoverZone = z;
        repaint();
    }
    if (del) setMouseCursor (juce::MouseCursor::PointingHandCursor);
    else if (rng) setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else if (z != 0) setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (h >= 0) setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else setMouseCursor (sketchMode && ! vintageMode ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
}

void SpectrumDisplay::mouseExit (const juce::MouseEvent&)
{
    hoverBand = -1;
    hoverSketchButton = false;
    hoverPeak = -1;
    hoverClashButton = false;
    hoverClash = -1;
    repaint();
}

int SpectrumDisplay::createBandAt (juce::Point<float> p)
{
    for (int nb = 0; nb < kNumBands; ++nb)
    {
        if (proc.params.bands[(size_t) nb].on->load() > 0.5f) continue;
        const double f = xToFreq (p.x);
        int type = (int) BandType::Bell;
        if (f < 30.0) type = (int) BandType::LowCut;
        else if (f > 15000.0) type = (int) BandType::HighCut;

        const float scale = juce::jmax (0.01f, proc.params.gainScale->load() / 100.0f);
        auto set = [this] (juce::RangedAudioParameter* prm, float v) { prm->beginChangeGesture(); setParam (prm, v); prm->endChangeGesture(); };
        set (param (nb, "type"), (float) type);
        set (param (nb, "freq"), (float) f);
        set (param (nb, "gain"), typeHasGain (type) ? (float) yToDb (p.y) / scale : 0.0f);
        set (param (nb, "q"), type == (int) BandType::Bell ? 1.0f : 0.707f);
        set (param (nb, "dyn"), 0.0f);
        set (param (nb, "drive"), 0.0f);
        set (param (nb, "on"), 1.0f);
        return nb;
    }
    return -1;
}

void SpectrumDisplay::mouseDown (const juce::MouseEvent& e)
{
    justCreated = -1;
    dragMode = Drag::None;

    // 1) X de borrar sobre la banda seleccionada
    if (! vintageMode && selectedBand >= 0 && proc.params.bands[(size_t) selectedBand].on->load() > 0.5f
        && deleteBadgePosition (selectedBand).getDistanceFrom (e.position) < 10.0f)
    {
        auto* on = param (selectedBand, "on");
        on->beginChangeGesture(); setParam (on, 0.0f); on->endChangeGesture();
        if (proc.soloBand.load() == selectedBand) proc.soloBand.store (-1);
        hoverDelete = false;
        if (onBandSelected) onBandSelected (-1);
        return;
    }

    // 2) Tirador del Range dinamico
    if (hasRangeHandle (selectedBand) && rangeHandlePosition (selectedBand).getDistanceFrom (e.position) < 12.0f)
    {
        dragMode = Drag::DynRange;
        dragBand = selectedBand;
        dragNode = rangeHandlePosition (selectedBand);
        lastMouse = e.position;
        param (selectedBand, "range")->beginChangeGesture();
        return;
    }

    // SKETCH: boton (arriba a la izquierda de la pantalla)
    if (! vintageMode && ! e.mods.isPopupMenu() && sketchButtonBounds().contains (e.position))
    {
        setSketchMode (! sketchMode);
        return;
    }
    // CHOQUES: boton (elegir con que pista comparar) y zonas de choque (clic = abrir espacio en esta pista)
    if (! vintageMode && clashButtonBounds().contains (e.position)) { showClashMenu(); return; }
    if (! vintageMode && ! e.mods.isPopupMenu())
        if (const int z = hitTestClash (e.position); z >= 0) { openClashSpace (clashZonesNow[(size_t) z]); return; }

    int b = hitTestNode (e.position);

    // SKETCH: en modo dibujo (o con Cmd/Ctrl) arrastrar en la pantalla vacia dibuja la curva
    if (b < 0 && ! vintageMode && ! e.mods.isPopupMenu() && (sketchMode || e.mods.isCommandDown()))
    {
        dragMode = Drag::Ink;
        startInk (e.position);
        return;
    }

    // CAZA DE PICOS: agarrar una resonancia marcada crea ahi una campana con el ancho del pico.
    // Arrastrar hacia abajo la recorta; un clic sin arrastrar la "doma" (recorte sugerido)
    if (b < 0 && ! vintageMode && ! e.mods.isPopupMenu())
        if (const int pk = hitTestPeak (e.position); pk >= 0)
        {
            const auto peak = peaks[(size_t) pk];
            const int nb = createBandAt (e.position);
            if (nb >= 0)
            {
                auto set = [this] (juce::RangedAudioParameter* prm, float v) { prm->beginChangeGesture(); setParam (prm, v); prm->endChangeGesture(); };
                set (param (nb, "type"), (float) BandType::Bell);
                set (param (nb, "freq"), (float) peak.f);
                set (param (nb, "gain"), 0.0f);
                set (param (nb, "q"), (float) peak.q);
                justCreated = nb;
                grabbedPeak = pk; grabMoved = false;
                if (onBandSelected) onBandSelected (nb);
                dragMode = Drag::Node;
                dragBand = nb;
                dragNode = { freqToX (peak.f), dbToY (0.0) };
                lastMouse = e.position;
                beginGesture (nb);
                return;
            }
        }

    // 3) Tiradores de la zona de proceso (tambien en el cajon vintage)
    if (b < 0 && ! e.mods.isPopupMenu())
    {
        if (const int z = hitTestZone (e.position))
        {
            dragMode = z == 1 ? Drag::ZoneLo : Drag::ZoneHi;
            dragNode = { zoneX (z == 2), e.position.y };
            lastMouse = e.position;
            proc.apvts.getParameter (z == 1 ? ParamID::rangeLo : ParamID::rangeHi)->beginChangeGesture();
            return;
        }
    }
    if (vintageMode) return;

    if (b < 0)
    {
        if (e.mods.isPopupMenu())
        {
            // Clic derecho en la pantalla vacia: opciones de la pantalla
            juce::PopupMenu m, an, rg;
            // Para saber con certeza que version esta cargada en el DAW
            m.addSectionHeader (juce::String ("Gaux EQ ") + GAUX_VERSION_STR + "  -  compilado " + __DATE__ + " " + juce::String (__TIME__).substring (0, 5) + " UTC");
            const auto o = viewOptions();
            an.addSectionHeader ("Velocidad");
            an.addItem (10, "Lenta (promedia mas)", true, o.speed == 0);
            an.addItem (11, "Media", true, o.speed == 1);
            an.addItem (12, "Rapida", true, o.speed == 2);
            an.addSectionHeader ("Inclinacion (para leer la musica plana)");
            for (int i = 0; i < 4; ++i) an.addItem (20 + i, juce::String (kTilts[i], 1) + " dB/oct", true, o.tilt == i);
            an.addSeparator();
            an.addItem (30, "Mostrar tambien antes del EQ", true, o.pre);
            an.addItem (31, "Congelar (FREEZE)", true, o.freeze);
            for (int i = 0; i < 4; ++i) rg.addItem (40 + i, "+-" + juce::String ((int) kRanges[i]) + " dB", true, o.range == i);
            m.addSubMenu ("Analizador", an);
            m.addSubMenu ("Rango de la pantalla", rg);
            m.addItem (50, "Teclado de piano abajo (notas)", true, o.piano);
            m.addItem (51, "Curva de cada banda en su color", true, o.bandCurves);
            m.addSeparator();
            m.addItem (2, "SKETCH: dibujar la curva con el mouse", true, sketchMode);
            m.addItem (60, "Copiar la curva (todas las bandas)");
            const bool canPaste = juce::SystemClipboard::getTextFromClipboard().startsWith ("GAUX-CURVA");
            m.addItem (61, "Pegar la curva", canPaste);
            m.addItem (62, "Seleccionar todas las bandas  (Cmd+A)");
            m.addSeparator();
            m.addItem (1, "Mostrar FPS (fluidez de la pantalla)", true, showFps);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                             [safe = juce::Component::SafePointer<SpectrumDisplay> (this)] (int r)
                             {
                                 if (safe == nullptr) return;
                                 const auto o2 = safe->viewOptions();
                                 if (r == 1) { showFps = ! showFps; safe->repaint(); }
                                 if (r == 2) safe->setSketchMode (! safe->sketchMode);
                                 if (r >= 10 && r <= 12) safe->setViewOption ("anSpeed", r - 10);
                                 if (r >= 20 && r <= 23) safe->setViewOption ("anTilt", r - 20);
                                 if (r == 30) safe->setViewOption ("anPre", o2.pre ? 0 : 1);
                                 if (r == 31) safe->setViewOption ("anFreeze", o2.freeze ? 0 : 1);
                                 if (r >= 40 && r <= 43) safe->setViewOption ("dispRange", r - 40);
                                 if (r == 50) safe->setViewOption ("piano", o2.piano ? 0 : 1);
                                 if (r == 51) safe->setViewOption ("bandCurves", o2.bandCurves ? 0 : 1);
                                 if (r == 60) juce::SystemClipboard::copyTextToClipboard (safe->proc.curveToText());
                                 if (r == 61) { safe->proc.curveFromText (juce::SystemClipboard::getTextFromClipboard()); safe->repaint(); }
                                 if (r == 62) safe->selectAll();
                             });
            return;
        }
        // Clic simple en zona vacia: deselecciona (y si se arrastra, recuadro para elegir varias bandas).
        // Doble clic: crea una banda (y se puede arrastrar)
        if (e.getNumberOfClicks() < 2)
        {
            if (! e.mods.isShiftDown()) { multiSel.fill (false); if (onBandSelected) onBandSelected (-1); }
            dragMode = Drag::Lasso;
            lassoStart = e.position; lasso = {};
            grabKeyboardFocus();
            return;
        }
        b = createBandAt (e.position);
        if (b < 0) return;
        justCreated = b;
    }

    // Seleccion de varias: Shift + clic suma o quita; un clic en una banda fuera del grupo deja solo esa
    bool keepFocus = true;
    if (e.mods.isShiftDown() && justCreated < 0 && ! e.mods.isPopupMenu())
    {
        if (selectedBand >= 0 && selectedBand != b && proc.params.bands[(size_t) selectedBand].on->load() > 0.5f) multiSel[(size_t) selectedBand] = true;
        multiSel[(size_t) b] = ! multiSel[(size_t) b];
        keepFocus = multiSel[(size_t) b];
    }
    else if (! multiSel[(size_t) b]) { multiSel.fill (false); multiSel[(size_t) b] = true; }
    if (keepFocus && onBandSelected) onBandSelected (b);
    repaint();

    if (e.mods.isPopupMenu())
    {
        showBandMenu (b);
        return;
    }
    grabKeyboardFocus();
    if (! keepFocus) return;   // (Shift + clic que saco la banda del grupo: no se arrastra)
    // el grupo se mueve junto: se guarda donde empezo cada una
    for (int k = 0; k < kNumBands; ++k)
        if (multiSel[(size_t) k])
        {
            groupStart[(size_t) k] = { std::log (proc.params.bands[(size_t) k].freq->load()), proc.params.band (k).gainDb };
            if (k != b) { param (k, "freq")->beginChangeGesture(); param (k, "gain")->beginChangeGesture(); }
        }
    if (e.mods.isAltDown())
    {
        // Alt + clic sostenido: escuchar solo esta banda mientras se arrastra
        proc.soloBand.store (b);
        altSolo = true;
    }
    dragMode = Drag::Node;
    dragBand = b;
    // El punto no salta al cursor: se mueve con el desplazamiento del mouse
    dragNode = justCreated >= 0 ? e.position : nodePosition (b);
    // Para bandas dinamicas se arrastra la posicion estatica (la que controla el usuario)
    if (justCreated < 0)
    {
        const auto raw = proc.params.band (b);
        if (typeHasGain (raw.type)) dragNode.y = dbToY (raw.gainDb);
    }
    lastMouse = e.position;
    beginGesture (b);
}

void SpectrumDisplay::applyDragPosition()
{
    auto a = plotArea();
    dragNode.x = juce::jlimit (a.getX(), a.getRight(), dragNode.x);
    dragNode.y = juce::jlimit (dbToY (dbRange), dbToY (-dbRange), dragNode.y);

    setParam (param (dragBand, "freq"), (float) xToFreq (dragNode.x));
    const int type = (int) proc.params.bands[(size_t) dragBand].type->load();
    const float scale = proc.params.gainScale->load() / 100.0f;
    if (typeHasGain (type) && scale > 0.01f)
        setParam (param (dragBand, "gain"), (float) yToDb (dragNode.y) / scale);
}

void SpectrumDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == Drag::None) return;
    if (dragMode == Drag::Ink) { addInk (e.position); return; }
    if (dragMode == Drag::Lasso)
    {
        lasso = juce::Rectangle<float> (lassoStart, e.position);
        // las bandas dentro del recuadro quedan seleccionadas (mientras se arrastra se ve)
        if (lasso.getWidth() > 4 || lasso.getHeight() > 4)
            for (int k = 0; k < kNumBands; ++k)
                if (proc.params.bands[(size_t) k].on->load() > 0.5f && lasso.contains (nodePosition (k))) multiSel[(size_t) k] = true;
        repaint();
        return;
    }
    if (grabbedPeak >= 0 && e.getDistanceFromDragStart() > 3) grabMoved = true;
    if (grabbedPeak >= 0 && ! grabMoved) return;   // (el temblor del clic no mueve la banda)
    // Shift = ajuste fino (5 veces mas lento)
    const float k = e.mods.isShiftDown() ? 0.2f : 1.0f;
    dragNode += (e.position - lastMouse) * k;
    lastMouse = e.position;

    if (dragMode == Drag::DynRange)
    {
        const auto s = proc.params.band (dragBand);
        const float scale = proc.params.gainScale->load() / 100.0f;
        if (scale > 0.01f)
            setParam (param (dragBand, "range"), (float) (yToDb (dragNode.y) - s.gainDb) / scale);
    }
    else if (dragMode == Drag::ZoneLo || dragMode == Drag::ZoneHi)
    {
        auto a = plotArea();
        dragNode.x = juce::jlimit (a.getX(), a.getRight(), dragNode.x);
        double f = xToFreq (dragNode.x);
        if (dragMode == Drag::ZoneLo) f = juce::jmin (f, (double) proc.params.rangeHi->load() / 1.25);
        else                          f = juce::jmax (f, (double) proc.params.rangeLo->load() * 1.25);
        setParam (proc.apvts.getParameter (dragMode == Drag::ZoneLo ? ParamID::rangeLo : ParamID::rangeHi), (float) f);
    }
    else if (dragBand >= 0)
    {
        applyDragPosition();
        applyGroupDrag();
    }
    repaint();
}

void SpectrumDisplay::mouseUp (const juce::MouseEvent&)
{
    if (altSolo) { proc.soloBand.store (-1); altSolo = false; }
    if (dragMode == Drag::Ink) finishInk();
    if (dragMode == Drag::Lasso)
    {
        lasso = {};
        int last = -1, n = 0;
        for (int k = 0; k < kNumBands; ++k) if (multiSel[(size_t) k]) { last = k; ++n; }
        if (n > 0 && onBandSelected) onBandSelected (last);
    }
    if (dragMode == Drag::Node)
        for (int k = 0; k < kNumBands; ++k)
            if (multiSel[(size_t) k] && k != dragBand) { param (k, "freq")->endChangeGesture(); param (k, "gain")->endChangeGesture(); }
    if (dragMode == Drag::Node && grabbedPeak >= 0 && ! grabMoved && dragBand >= 0)
    {
        // clic sin arrastrar sobre un pico: recorte sugerido (3/4 de lo que sobresale, hasta -12 dB)
        const float scale = juce::jmax (0.01f, proc.params.gainScale->load() / 100.0f);
        const float prom = grabbedPeak < (int) peaks.size() ? peaks[(size_t) grabbedPeak].prom : 6.0f;
        setParam (param (dragBand, "gain"), -juce::jmin (12.0f, prom * 0.75f) / scale);
    }
    grabbedPeak = -1;
    if (dragMode == Drag::Node) endGesture();
    else if (dragMode == Drag::DynRange && dragBand >= 0) param (dragBand, "range")->endChangeGesture();
    else if (dragMode == Drag::ZoneLo) proc.apvts.getParameter (ParamID::rangeLo)->endChangeGesture();
    else if (dragMode == Drag::ZoneHi) proc.apvts.getParameter (ParamID::rangeHi)->endChangeGesture();
    dragMode = Drag::None;
    dragBand = -1;
    repaint();
}

void SpectrumDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (vintageMode) return;
    const int b = hitTestNode (e.position);
    // Doble clic en un punto existente: borrar la banda (no la que se acaba de crear con este doble clic)
    if (b >= 0 && b != justCreated)
    {
        auto* on = param (b, "on");
        on->beginChangeGesture(); setParam (on, 0.0f); on->endChangeGesture();
        if (onBandSelected) onBandSelected (-1);
    }
}

void SpectrumDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int b = hitTestNode (e.position);
    if (b < 0) return;
    auto* q = param (b, "q");
    const float current = proc.params.bands[(size_t) b].q->load();
    const float delta = (std::abs (w.deltaY) > 1.0e-6f ? w.deltaY : w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
    q->beginChangeGesture();
    setParam (q, current * std::exp (delta * 1.2f));
    q->endChangeGesture();
    if (onBandSelected) onBandSelected (b);
}

void SpectrumDisplay::showBandMenu (int band)
{
    juce::PopupMenu m;
    m.addSectionHeader ("Banda " + juce::String (band + 1));
    const int currentType = (int) proc.params.bands[(size_t) band].type->load();
    for (int t = 0; t < kNumBandTypes; ++t)
        m.addItem (100 + t, kBandTypeNames[t], true, t == currentType);
    m.addSeparator();
    m.addItem (1, "Ganancia a 0 dB");
    m.addItem (2, "Quitar saturacion");
    m.addItem (3, "Borrar banda");

    juce::Component::SafePointer<SpectrumDisplay> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe, band] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto set = [&] (const char* name, float v)
        {
            auto* p = safe->param (band, name);
            p->beginChangeGesture(); safe->setParam (p, v); p->endChangeGesture();
        };
        if (r >= 100) set ("type", (float) (r - 100));
        else if (r == 1) set ("gain", 0.0f);
        else if (r == 2) set ("drive", 0.0f);
        else if (r == 3) set ("on", 0.0f);
    });
}

//==============================================================================
// SKETCH: dibujar la curva
juce::Rectangle<float> SpectrumDisplay::sketchButtonBounds() const
{
    const auto a = plotArea();
    return { a.getX() + 12.0f, a.getY() + 12.0f, 78.0f, 22.0f };
}

void SpectrumDisplay::setSketchMode (bool on)
{
    sketchMode = on;
    setMouseCursor (on ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void SpectrumDisplay::startInk (juce::Point<float> p)
{
    inking = true;
    inkFade = 0.0f;
    sketch.clear();
    // lo que ya suena (bandas encendidas) en la grilla del trazo: el dibujo es el TOTAL
    const auto existing = proc.currentStaticBands();
    const auto& f = sketch.freqs();
    sketchFixed.assign (f.size(), 0.0);
    for (auto& b : existing)
        for (size_t i = 0; i < f.size(); ++i) sketchFixed[i] += 20.0 * std::log10 (std::max (1.0e-6, analogMagnitudeOfBand (b, f[i])));
    sketchFree = kNumBands - (int) existing.size();
    sketchFit = {};
    if (onBandSelected) onBandSelected (-1);
    addInk (p);
}

void SpectrumDisplay::addInk (juce::Point<float> p)
{
    const auto a = plotArea();
    const float x = juce::jlimit (a.getX(), a.getRight(), p.x);
    const double db = juce::jlimit (-dbRange, dbRange, yToDb (p.y));
    sketch.add (xToFreq (x), db);
    sketchFitStale = true;
    // el ajuste en vivo cuesta pocos ms: se recalcula como mucho ~30 veces por segundo (lo pide el cuadro)
    if (juce::Time::getMillisecondCounterHiRes() - lastSketchFitMs > 33.0) refitSketch();
    repaint();
}

void SpectrumDisplay::refitSketch()
{
    sketchFitStale = false;
    lastSketchFitMs = juce::Time::getMillisecondCounterHiRes();
    sketchFit = sketch.fit (sketchFixed, sketchFree, 24.0);
}

void SpectrumDisplay::finishInk()
{
    sketch.endStroke();
    inking = false;
    refitSketch();
    sketchPlaced = 0;
    const float scale = proc.params.gainScale->load() / 100.0f;
    if (! sketchFit.bands.empty() && scale > 0.05f)
    {
        // las bandas nuevas nacen con un anillo de su color, donde aparecen
        std::array<bool, kNumBands> wasOn {};
        for (int b = 0; b < kNumBands; ++b) wasOn[(size_t) b] = proc.params.bands[(size_t) b].on->load() > 0.5f;
        sketchPlaced = proc.applyFittedBands (sketchFit.bands);
        int last = -1;
        for (int b = 0; b < kNumBands; ++b)
            if (! wasOn[(size_t) b] && proc.params.bands[(size_t) b].on->load() > 0.5f)
            {
                bursts.push_back ({ nodePosition (b), colourOf (b), 0.0f });
                last = b;
            }
        if (last >= 0 && onBandSelected) onBandSelected (last);
    }
    sketchFit = {};
    inkFade = 1.0f;   // la tinta se desvanece mientras aparecen las bandas
    repaint();
}

void SpectrumDisplay::paintSketch (juce::Graphics& g, juce::Rectangle<float> a)
{
    // Boton SKETCH (pastilla con un pincel)
    {
        const auto r = sketchButtonBounds();
        const auto c = Colours::sketch;
        g.setColour (sketchMode ? c.withAlpha (0.92f) : Colours::glass.withAlpha (hoverSketchButton ? 0.95f : 0.75f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (sketchMode ? c : c.withAlpha (hoverSketchButton ? 0.8f : 0.35f));
        g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
        // pincel: trazo ondulado
        juce::Path wave;
        const float x0 = r.getX() + 10.0f, cy = r.getCentreY();
        wave.startNewSubPath (x0, cy + 2.5f);
        wave.cubicTo (x0 + 3.0f, cy - 5.0f, x0 + 6.0f, cy + 6.0f, x0 + 11.0f, cy - 3.0f);
        const auto ink = sketchMode ? Colours::ink : c.withAlpha (hoverSketchButton ? 1.0f : 0.75f);
        g.setColour (ink);
        g.strokePath (wave, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillEllipse (x0 + 9.5f, cy - 4.5f, 3.0f, 3.0f);
        g.setFont (labelFont (9.5f));
        g.drawText ("SKETCH", r.withTrimmedLeft (25.0f).withTrimmedRight (6.0f), juce::Justification::centred);
    }

    const auto& f = sketch.freqs();
    const float inkAlpha = inking ? 1.0f : inkFade;
    if (inkAlpha > 0.0f && sketch.count() > 0)
    {
        // Ajuste en vivo: la curva que van a dar las bandas (fantasma) y donde naceran (rombos huecos)
        if (inking && ! sketchFit.bands.empty())
        {
            juce::Path ghost;
            for (size_t i = 0; i < f.size(); ++i)
            {
                double db = sketchFixed[i];
                for (auto& b : sketchFit.bands) db += 20.0 * std::log10 (std::max (1.0e-6, analogMagnitudeOfBand (b, f[i])));
                const juce::Point<float> pt (freqToX (f[i]), dbToY (juce::jlimit (-dbRange, dbRange, db)));
                if (i == 0) ghost.startNewSubPath (pt); else ghost.lineTo (pt);
            }
            juce::Path dashed;
            const float dashes[] = { 2.0f, 4.0f };
            juce::PathStrokeType (1.6f).createDashedStroke (dashed, ghost, dashes, 2);
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillPath (dashed);
            for (auto& b : sketchFit.bands)
            {
                const double db = typeHasGain (b.type) ? b.gainDb : 0.0;
                const double fb = juce::jlimit (kMinF, kMaxF, b.freq);
                const juce::Point<float> c (freqToX (fb), dbToY (juce::jlimit (-dbRange, dbRange, db)));
                juce::Path d;
                d.startNewSubPath (c.x, c.y - 7.0f); d.lineTo (c.x + 7.0f, c.y); d.lineTo (c.x, c.y + 7.0f); d.lineTo (c.x - 7.0f, c.y); d.closeSubPath();
                g.setColour (Colours::sketch.withAlpha (0.9f));
                g.strokePath (d, juce::PathStrokeType (1.5f));
            }
            g.setFont (monoFont (10.0f));
            g.setColour (Colours::sketch);
            const auto txt = juce::String ((int) sketchFit.bands.size()) + (sketchFit.bands.size() == 1 ? " banda" : " bandas")
                             + (sketchFree < 8 ? "  (" + juce::String (sketchFree) + " libres)" : juce::String());
            g.drawText (txt, juce::Rectangle<float> (200.0f, 16.0f).withPosition (sketchButtonBounds().getX() + 4.0f, sketchButtonBounds().getBottom() + 4.0f),
                        juce::Justification::centredLeft);
        }
        // La tinta: tramos continuos del trazo, nucleo blanco con resplandor lima
        juce::Path inkPath;
        bool open = false;
        for (size_t i = 0; i < f.size(); ++i)
        {
            if (! sketch.touched (i)) { open = false; continue; }
            const juce::Point<float> pt (freqToX (f[i]), dbToY (sketch.inkDb (i)));
            if (! open) { inkPath.startNewSubPath (pt); open = true; } else inkPath.lineTo (pt);
        }
        const auto rise = inking ? 0.0f : (1.0f - inkFade) * 6.0f;   // al soltar la tinta "se evapora" hacia arriba
        if (rise > 0.0f) inkPath.applyTransform (juce::AffineTransform::translation (0.0f, -rise));
        g.setColour (Colours::sketch.withAlpha (0.16f * inkAlpha));
        g.strokePath (inkPath, juce::PathStrokeType (11.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Colours::sketch.withAlpha (0.45f * inkAlpha));
        g.strokePath (inkPath, juce::PathStrokeType (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (juce::Colours::white.withAlpha (0.95f * inkAlpha));
        g.strokePath (inkPath, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    // Anillos donde nacieron las bandas
    for (auto& b : bursts)
    {
        const float t = b.age, rad = 9.0f + 26.0f * (1.0f - (1.0f - t) * (1.0f - t));
        g.setColour (b.c.withAlpha (0.8f * (1.0f - t)));
        g.drawEllipse (b.p.x - rad, b.p.y - rad, rad * 2.0f, rad * 2.0f, 2.0f * (1.0f - t) + 0.5f);
        g.setColour (Colours::sketch.withAlpha (0.5f * (1.0f - t)));
        const float r2 = rad * 0.6f;
        g.drawEllipse (b.p.x - r2, b.p.y - r2, r2 * 2.0f, r2 * 2.0f, 1.0f);
    }
    // Indicacion en modo dibujo
    if (sketchMode && ! inking && inkFade <= 0.0f)
    {
        g.setFont (uiFont (11.0f));
        g.setColour (Colours::sketch.withAlpha (0.8f));
        g.drawText ("Dibuja la curva que quieres: el Gaux la convierte en bandas  -  Cmd/Ctrl + arrastrar tambien dibuja  -  Cmd+Z deshace",
                    juce::Rectangle<float> (a.getX(), a.getBottom() - 30.0f, a.getWidth(), 18.0f), juce::Justification::centred);
    }
}


//==============================================================================
// CAZA DE PICOS
bool SpectrumDisplay::detectPeaks (float dt)
{
    // true si cambio algo visible (si no, no se redibuja por esto)
    float sig0 = 0; for (auto& p : peaks) sig0 += p.alpha + (float) p.f * 1.0e-3f + p.db * 0.01f;
    const size_t n0 = peaks.size();
    auto changed = [&]
    {
        float sig1 = 0; for (auto& p : peaks) sig1 += p.alpha + (float) p.f * 1.0e-3f + p.db * 0.01f;
        return n0 != peaks.size() || std::abs (sig1 - sig0) > 1.0e-4f;
    };
    const bool on = proc.params.analyzer->load() > 0.5f && post.visible && ! longDb.empty() && ! sketchMode;
    if (! on)
    {
        if (! peaks.empty()) { for (auto& p : peaks) p.alpha = juce::jmax (0.0f, p.alpha - dt / 0.25f); }
        peaks.erase (std::remove_if (peaks.begin(), peaks.end(), [] (const GrabPeak& p) { return p.alpha <= 0.0f; }), peaks.end());
        if (peaks.empty()) hoverPeak = -1;
        return changed();
    }
    // aparecer / desaparecer suave
    for (auto& p : peaks) p.alpha = juce::jlimit (0.0f, 1.0f, p.alpha + (p.alive ? dt / 0.3f : -dt / 0.4f));
    peakTimer += dt;
    if (peakTimer < 0.1f) return changed();
    peakTimer = 0.0f;

    const double fs = proc.currentSampleRate.load();
    if (fs <= 0) return changed();
    // grilla de 1/48 de octava (40 Hz - 16 kHz): el mayor valor de las frecuencias de cada punto
    constexpr int perOct = 48;
    const double f0 = 40.0, f1 = juce::jmin (16000.0, fs * 0.45);
    const int G = (int) std::floor (std::log2 (f1 / f0) * perOct) + 1;
    std::vector<float> v ((size_t) G);
    const int K = (int) longDb.size() - 1;
    for (int i = 0; i < G; ++i)
    {
        const double fa = f0 * std::pow (2.0, (i - 0.5) / perOct), fb = f0 * std::pow (2.0, (i + 0.5) / perOct);
        int k0 = juce::jlimit (1, K, (int) std::floor (fa * kFftSize / fs)), k1 = juce::jlimit (k0, K, (int) std::ceil (fb * kFftSize / fs));
        float m = -140.0f;
        for (int k = k0; k <= k1; ++k) m = juce::jmax (m, longDb[(size_t) k]);
        v[(size_t) i] = m;
    }
    // un pico: maximo local (+-1/12 oct) que sobresale >= 6 dB del promedio de su entorno (+-1/3 oct, sin el centro)
    std::vector<GrabPeak> found;
    const int near = perOct / 12, wide = perOct / 3;
    for (int i = near; i < G - near; ++i)
    {
        const float x = v[(size_t) i];
        if (x < kAnMin + 12.0f) continue;
        bool isMax = true;
        for (int j = i - near; j <= i + near && isMax; ++j) if (j != i && v[(size_t) j] > x) isMax = false;
        if (! isMax) continue;
        double s = 0; int n = 0;
        for (int j = juce::jmax (0, i - wide); j <= juce::jmin (G - 1, i + wide); ++j)
            if (std::abs (j - i) > near) { s += v[(size_t) j]; ++n; }
        if (n < 4) continue;
        const float prom = x - (float) (s / n);
        if (prom < 6.0f) continue;
        // ancho del pico a -3 dB (para la Q de la campana)
        int lo = i, hi = i;
        while (lo > 0 && v[(size_t) lo - 1] > x - 3.0f && i - lo < wide) --lo;
        while (hi < G - 1 && v[(size_t) hi + 1] > x - 3.0f && hi - i < wide) ++hi;
        const double bwOct = juce::jmax (1.0 / perOct, (hi - lo + 1) / (double) perOct);
        GrabPeak p;
        p.f = f0 * std::pow (2.0, i / (double) perOct);
        {
            // frecuencia exacta: la FFT mas fuerte del punto, con interpolacion parabolica (ventana de Hann)
            const double fa = p.f * std::pow (2.0, -1.0 / perOct), fb = p.f * std::pow (2.0, 1.0 / perOct);
            const int ka = juce::jlimit (2, K - 1, (int) std::floor (fa * kFftSize / fs)), kb = juce::jlimit (ka, K - 1, (int) std::ceil (fb * kFftSize / fs));
            int km = ka;
            for (int k = ka; k <= kb; ++k) if (longDb[(size_t) k] > longDb[(size_t) km]) km = k;
            const double y0 = longDb[(size_t) km - 1], y1 = longDb[(size_t) km], y2 = longDb[(size_t) km + 1];
            const double den = y0 - 2.0 * y1 + y2;
            const double delta = std::abs (den) > 1.0e-9 ? juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / den) : 0.0;
            p.f = (km + delta) * fs / kFftSize;
        }
        p.q = juce::jlimit (1.0, 12.0, 1.0 / (2.0 * std::sinh (std::log (2.0) / 2.0 * bwOct)));
        p.db = x; p.prom = prom; p.alive = true;
        found.push_back (p);
    }
    // los 8 que mas sobresalen
    std::sort (found.begin(), found.end(), [] (const GrabPeak& a, const GrabPeak& b) { return a.prom > b.prom; });
    if (found.size() > 8) found.resize (8);
    // seguir a los que ya estaban (misma frecuencia +-1/12 oct): se mueven suave, no parpadean
    for (auto& p : peaks) p.alive = false;
    for (auto& nf : found)
    {
        GrabPeak* match = nullptr;
        for (auto& p : peaks) if (std::abs (std::log2 (p.f / nf.f)) < 1.0 / 12.0 && (match == nullptr || std::abs (p.f - nf.f) < std::abs (match->f - nf.f))) match = &p;
        if (match != nullptr && ! match->alive)
        {
            match->f = std::exp (std::log (match->f) * 0.5 + std::log (nf.f) * 0.5);
            match->q = nf.q; match->db = nf.db; match->prom = nf.prom; match->alive = true;
        }
        else if (match == nullptr) { nf.alpha = 0.0f; peaks.push_back (nf); }
    }
    peaks.erase (std::remove_if (peaks.begin(), peaks.end(), [] (const GrabPeak& p) { return ! p.alive && p.alpha <= 0.0f; }), peaks.end());
    if (hoverPeak >= (int) peaks.size()) hoverPeak = -1;
    return changed();
}

juce::Point<float> SpectrumDisplay::peakMarker (const GrabPeak& p) const
{
    // sobre lo que se ve del analizador en ese punto (no sobre el promedio lento, que queda mas abajo)
    float db = p.db;
    const double fs = proc.currentSampleRate.load();
    if (fs > 0 && ! post.smoothed.empty())
    {
        const int K = (int) post.smoothed.size() - 1;
        const int ka = juce::jlimit (1, K, (int) std::floor (p.f * std::pow (2.0, -1.0 / 24.0) * kFftSize / fs));
        const int kb = juce::jlimit (ka, K, (int) std::ceil (p.f * std::pow (2.0, 1.0 / 24.0) * kFftSize / fs));
        for (int k = ka; k <= kb; ++k) db = juce::jmax (db, post.smoothed[(size_t) k]);
    }
    return { freqToX (p.f), analyzerDbToY (db) - 12.0f };
}

int SpectrumDisplay::hitTestPeak (juce::Point<float> pos) const
{
    int best = -1; float bd = 13.0f;
    for (size_t i = 0; i < peaks.size(); ++i)
    {
        if (peaks[i].alpha < 0.3f) continue;
        const auto m = peakMarker (peaks[i]);
        // la marca y el pico mismo (debajo) se pueden agarrar
        const float d = juce::jmin (m.getDistanceFrom (pos), juce::Point<float> (m.x, m.y + 12.0f).getDistanceFrom (pos));
        if (d < bd) { bd = d; best = (int) i; }
    }
    return best;
}

void SpectrumDisplay::paintPeaks (juce::Graphics& g, juce::Rectangle<float> a)
{
    for (size_t i = 0; i < peaks.size(); ++i)
    {
        const auto& p = peaks[i];
        if (p.alpha <= 0.0f) continue;
        const bool hov = (int) i == hoverPeak;
        const auto m = peakMarker (p);
        if (! a.contains (m)) continue;
        // "colmillo": un triangulo que apunta al pico, mas intenso cuanto mas sobresale
        const float strength = juce::jlimit (0.35f, 1.0f, (p.prom - 4.0f) / 10.0f);
        const float s = hov ? 6.5f : 4.5f;
        juce::Path t;
        t.addTriangle (m.x - s, m.y - s * 0.9f, m.x + s, m.y - s * 0.9f, m.x, m.y + s * 0.9f);
        const auto c = Colours::accent.interpolatedWith (juce::Colours::white, hov ? 0.35f : 0.0f);
        g.setColour (c.withAlpha (p.alpha * strength * (hov ? 1.0f : 0.75f)));
        g.fillPath (t);
        if (hov)
        {
            // guia vertical hasta el pico y etiqueta con la nota
            g.setColour (c.withAlpha (0.5f * p.alpha));
            g.drawEllipse (m.x - 4.0f, m.y + 8.0f, 8.0f, 8.0f, 1.2f);
            const auto txt = fmtFreq (p.f) + "  " + noteName (p.f) + "  +" + juce::String (p.prom, 1) + " dB";
            g.setFont (monoFont (10.0f));
            const float w = 150.0f;
            auto box = juce::Rectangle<float> (w, 34.0f).withCentre ({ juce::jlimit (a.getX() + w * 0.5f + 4, a.getRight() - w * 0.5f - 4, m.x), m.y - 30.0f });
            g.setColour (Colours::glass.withAlpha (0.95f));
            g.fillRoundedRectangle (box, 8.0f);
            g.setColour (c.withAlpha (0.7f));
            g.drawRoundedRectangle (box, 8.0f, 1.0f);
            g.setColour (Colours::text);
            g.drawText (txt, box.removeFromTop (19.0f).translated (0, 2), juce::Justification::centred);
            g.setFont (uiFont (9.5f));
            g.setColour (Colours::textDim);
            g.drawText ("clic: domar  -  arrastrar: recortar", box.translated (0, -2), juce::Justification::centred);
        }
    }
}


//==============================================================================
// CHOQUES
juce::Rectangle<float> SpectrumDisplay::clashButtonBounds() const
{
    const auto s = sketchButtonBounds();
    return { s.getRight() + 8.0f, s.getY(), clashTarget >= 0 ? 150.0f : 84.0f, s.getHeight() };
}

void SpectrumDisplay::setClashTarget (int id)
{
    clashTarget = id;
    if (id >= 0 && ! clashSubscribed) { proc.clashHub->subscribe(); clashSubscribed = true; }
    if (id < 0 && clashSubscribed) { proc.clashHub->unsubscribe(); clashSubscribed = false; }
    clashZonesNow.clear(); clashSt.fill (0.0f); clashOther.fill (-140.0f); lastClashTick = -1; hoverClash = -1;
    clashName = {};
    for (auto& i : proc.clashHub->list()) if (i.id == id) { clashName = i.name; clashColour = i.colour; }
    repaint();
}

void SpectrumDisplay::showClashMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("CHOQUES: comparar con otra pista");
    const auto all = proc.clashHub->list();
    std::vector<int> ids;
    for (auto& i : all)
    {
        if (i.id == proc.clashSource.id) continue;
        ids.push_back (i.id);
        m.addColouredItem ((int) ids.size(), i.name + (i.hasAudio ? "" : "  (sin sonar)"), i.colour, true, i.id == clashTarget);
    }
    if (ids.empty()) m.addItem (-2, "No hay otro Gaux Equalizer en el proyecto (ponlo en el kick, el bajo...)", false);
    m.addSeparator();
    m.addItem (1001, "Nombre de esta pista: " + proc.clashSource.getName() + "  (cambiar...)");
    m.addItem (1000, "Ninguna (apagar)", clashTarget >= 0, false);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (clashButtonBounds().toNearestInt())),
                     [safe = juce::Component::SafePointer<SpectrumDisplay> (this), ids] (int r)
                     {
                         if (safe == nullptr || r <= 0) return;
                         if (r == 1000) safe->setClashTarget (-1);
                         else if (r == 1001)
                         {
                             auto* w = new juce::AlertWindow ("Nombre de esta pista", "Como la ven los otros Gaux del proyecto (KICK, BAJO, VOZ...)", juce::MessageBoxIconType::NoIcon);
                             w->addTextEditor ("n", safe->proc.clashSource.getName());
                             w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                             w->addButton ("Cancelar", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                             w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int res)
                             {
                                 if (res == 1 && safe != nullptr) safe->proc.setUserTrackName (w->getTextEditorContents ("n").trim().substring (0, 24));
                             }), true);
                         }
                         else if (r <= (int) ids.size()) safe->setClashTarget (ids[(size_t) r - 1]);
                     });
}

bool SpectrumDisplay::updateClash()
{
    auto& hub = *proc.clashHub;
    const int t = hub.getTick();
    if (t == lastClashTick) return false;
    lastClashTick = t;
    bool hasA = false, hasB = false;
    if (! hub.read (clashTarget, clashOther, clashOtherSlow, hasB)) { setClashTarget (-1); return true; }   // la otra pista se cerro
    hub.read (proc.clashSource.id, clashMine, clashMineSlow, hasA);
    for (auto& i : hub.list()) if (i.id == clashTarget) { clashName = i.name; clashColour = i.colour; }
    clashStrength (clashMineSlow, clashOtherSlow, clashSt);
    clashZonesNow = clashZones (clashSt);
    if (hoverClash >= (int) clashZonesNow.size()) hoverClash = -1;
    return true;
}

juce::Point<float> SpectrumDisplay::clashZoneHandle (const ClashZone& z) const
{
    return { freqToX (z.fCenter), plotArea().getBottom() - 9.0f - bottomInset() };
}

int SpectrumDisplay::hitTestClash (juce::Point<float> p) const
{
    if (clashTarget < 0) return -1;
    const auto a = plotArea();
    if (p.y < a.getBottom() - 22.0f - bottomInset() || p.y > a.getBottom() - bottomInset()) return -1;   // (solo la franja de abajo: arriba se crean y mueven bandas)
    for (size_t i = 0; i < clashZonesNow.size(); ++i)
    {
        const auto& z = clashZonesNow[i];
        if (p.x >= freqToX (z.fLo) - 3.0f && p.x <= freqToX (z.fHi) + 3.0f) return (int) i;
    }
    return -1;
}

void SpectrumDisplay::openClashSpace (const ClashZone& z)
{
    // una campana suave en ESTA pista, del ancho del choque, para hacerle lugar a la otra
    const double bw = std::max (0.2, std::log2 (z.fHi / z.fLo));
    const double q = juce::jlimit (0.7, 6.0, 1.0 / (2.0 * std::sinh (std::log (2.0) / 2.0 * bw)));
    const float scale = juce::jmax (0.01f, proc.params.gainScale->load() / 100.0f);
    for (int nb = 0; nb < kNumBands; ++nb)
    {
        if (proc.params.bands[(size_t) nb].on->load() > 0.5f) continue;
        auto set = [this] (juce::RangedAudioParameter* prm, float v) { prm->beginChangeGesture(); setParam (prm, v); prm->endChangeGesture(); };
        set (param (nb, "type"), (float) BandType::Bell);
        set (param (nb, "freq"), (float) z.fCenter);
        set (param (nb, "gain"), -(2.0f + 2.0f * z.strength) / scale);
        set (param (nb, "q"), (float) q);
        set (param (nb, "dyn"), 0.0f); set (param (nb, "drive"), 0.0f); set (param (nb, "stereo"), 0.0f);
        set (param (nb, "on"), 1.0f);
        proc.history.commit();
        bursts.push_back ({ nodePosition (nb), clashColour, 0.0f });
        if (onBandSelected) onBandSelected (nb);
        return;
    }
}

void SpectrumDisplay::paintClash (juce::Graphics& g, juce::Rectangle<float> a)
{
    // Boton CHOQUE (dos ondas que se cruzan) y la pista elegida
    {
        const auto r = clashButtonBounds();
        const bool on = clashTarget >= 0;
        const auto c = on ? clashColour : Colours::accent;
        g.setColour (on ? Colours::glass.withAlpha (0.95f) : Colours::glass.withAlpha (hoverClashButton ? 0.95f : 0.75f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (c.withAlpha (on ? 0.9f : (hoverClashButton ? 0.8f : 0.35f)));
        g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
        juce::Path w1, w2;
        const float x0 = r.getX() + 9.0f, cy = r.getCentreY();
        w1.startNewSubPath (x0, cy + 3.0f); w1.cubicTo (x0 + 4.0f, cy - 6.0f, x0 + 8.0f, cy - 6.0f, x0 + 12.0f, cy + 3.0f);
        w2.startNewSubPath (x0, cy - 3.0f); w2.cubicTo (x0 + 4.0f, cy + 6.0f, x0 + 8.0f, cy + 6.0f, x0 + 12.0f, cy - 3.0f);
        g.setColour (Colours::accent.withAlpha (on || hoverClashButton ? 1.0f : 0.7f));
        g.strokePath (w1, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour ((on ? clashColour : Colours::text).withAlpha (on || hoverClashButton ? 1.0f : 0.6f));
        g.strokePath (w2, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setFont (labelFont (9.5f));
        g.setColour (on ? Colours::text : Colours::accent.withAlpha (hoverClashButton ? 1.0f : 0.8f));
        g.drawFittedText (on ? "VS " + clashName.toUpperCase() : juce::String ("CHOQUE"), r.withTrimmedLeft (26.0f).withTrimmedRight (8.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }
    if (clashTarget < 0) return;

    // Zonas de choque: columnas rayadas (como cinta de peligro) y una franja abajo que se puede tocar
    for (size_t i = 0; i < clashZonesNow.size(); ++i)
    {
        const auto& z = clashZonesNow[i];
        const float x0 = freqToX (z.fLo), x1 = freqToX (z.fHi);
        const bool hov = (int) i == hoverClash;
        const auto col = Colours::accent.interpolatedWith (clashColour, 0.35f);
        juce::ColourGradient gr (col.withAlpha (0.0f), 0.0f, a.getY() + a.getHeight() * 0.25f, col.withAlpha ((hov ? 0.28f : 0.16f) * z.strength), 0.0f, a.getBottom(), false);
        g.setGradientFill (gr);
        g.fillRect (juce::Rectangle<float> (x0, a.getY(), x1 - x0, a.getHeight()));
        {
            juce::Graphics::ScopedSaveState ss (g);
            const float yb = a.getBottom() - bottomInset();
            g.reduceClipRegion (juce::Rectangle<float> (x0, yb - 18.0f, x1 - x0, 14.0f).toNearestInt());
            g.setColour (col.withAlpha (hov ? 0.95f : 0.35f + 0.5f * z.strength));
            for (float x = x0 - 20.0f; x < x1 + 20.0f; x += 7.0f) g.drawLine (x, yb - 4.0f, x + 12.0f, yb - 18.0f, 2.2f);
        }
        if (hov)
        {
            const auto txt = "CHOQUE con " + clashName + "  " + fmtFreq (z.fLo) + " - " + fmtFreq (z.fHi);
            g.setFont (monoFont (10.0f));
            const float w = juce::jmax (210.0f, juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), txt) + 24.0f);
            auto box = juce::Rectangle<float> (w, 34.0f).withCentre ({ juce::jlimit (a.getX() + w * 0.5f + 4, a.getRight() - w * 0.5f - 4, (x0 + x1) * 0.5f), a.getBottom() - 44.0f - bottomInset() });
            g.setColour (Colours::glass.withAlpha (0.95f)); g.fillRoundedRectangle (box, 8.0f);
            g.setColour (col.withAlpha (0.8f)); g.drawRoundedRectangle (box, 8.0f, 1.0f);
            g.setColour (Colours::text); g.drawText (txt, box.removeFromTop (19.0f).translated (0, 2), juce::Justification::centred);
            g.setFont (uiFont (9.5f)); g.setColour (Colours::textDim);
            g.drawText ("clic: abrir espacio en esta pista (campana suave)", box.translated (0, -2), juce::Justification::centred);
        }
    }
    // Espectro de la otra pista: linea de su color
    {
        juce::Path p;
        bool started = false;
        for (int i = 0; i < kClashPoints; ++i)
        {
            // (suavizado 1-2-1 y sin dibujar lo que queda debajo del piso del analizador)
            const float v = 0.25f * clashOther[(size_t) juce::jmax (0, i - 1)] + 0.5f * clashOther[(size_t) i] + 0.25f * clashOther[(size_t) juce::jmin (kClashPoints - 1, i + 1)];
            if (v < kAnMin + 1.0f) { started = false; continue; }
            const juce::Point<float> pt (freqToX (clashFreq (i)), analyzerDbToY (v));
            if (! started) { p.startNewSubPath (pt); started = true; } else p.lineTo (pt);
        }
        g.setColour (clashColour.withAlpha (0.18f));
        g.strokePath (p, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (clashColour.withAlpha (0.85f));
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}


//==============================================================================
// Seleccion de varias bandas, teclado y piano
void SpectrumDisplay::applyGroupDrag()
{
    if (dragBand < 0 || selectionCount() < 2 || ! multiSel[(size_t) dragBand]) return;
    // todo el grupo se mueve lo mismo: la misma distancia en octavas y los mismos dB que la banda que se arrastra
    const double dLog = std::log (proc.params.bands[(size_t) dragBand].freq->load()) - groupStart[(size_t) dragBand].logF;
    const double dG = proc.params.band (dragBand).gainDb - groupStart[(size_t) dragBand].gain;
    const float scale = juce::jmax (0.01f, proc.params.gainScale->load() / 100.0f);
    for (int k = 0; k < kNumBands; ++k)
    {
        if (k == dragBand || ! multiSel[(size_t) k] || proc.params.bands[(size_t) k].on->load() < 0.5f) continue;
        setParam (param (k, "freq"), (float) juce::jlimit (kMinF, kMaxF, std::exp (groupStart[(size_t) k].logF + dLog)));
        if (typeHasGain ((int) proc.params.bands[(size_t) k].type->load()) && typeHasGain ((int) proc.params.bands[(size_t) dragBand].type->load()))
            setParam (param (k, "gain"), (float) juce::jlimit (-24.0, 24.0, (groupStart[(size_t) k].gain + dG) / scale));
    }
}

void SpectrumDisplay::selectAll()
{
    int last = -1;
    for (int k = 0; k < kNumBands; ++k) { multiSel[(size_t) k] = proc.params.bands[(size_t) k].on->load() > 0.5f; if (multiSel[(size_t) k]) last = k; }
    if (last >= 0 && onBandSelected) onBandSelected (last);
    repaint();
}

void SpectrumDisplay::deleteSelection()
{
    bool any = false;
    for (int k = 0; k < kNumBands; ++k)
        if ((multiSel[(size_t) k] || k == selectedBand) && proc.params.bands[(size_t) k].on->load() > 0.5f)
        {
            auto* on = param (k, "on");
            on->beginChangeGesture(); setParam (on, 0.0f); on->endChangeGesture();
            any = true;
        }
    multiSel.fill (false);
    if (any) { proc.history.commit(); if (onBandSelected) onBandSelected (-1); }
    repaint();
}

bool SpectrumDisplay::keyPressed (const juce::KeyPress& k)
{
    if (vintageMode) return false;
    if (k.getKeyCode() == juce::KeyPress::deleteKey || k.getKeyCode() == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
    if (k.getModifiers().isCommandDown() && (k.getKeyCode() == 'A' || k.getKeyCode() == 'a')) { selectAll(); return true; }
    if (k.getModifiers().isCommandDown() && (k.getKeyCode() == 'C' || k.getKeyCode() == 'c')) { juce::SystemClipboard::copyTextToClipboard (proc.curveToText()); return true; }
    if (k.getModifiers().isCommandDown() && (k.getKeyCode() == 'V' || k.getKeyCode() == 'v'))
    {
        const bool ok = proc.curveFromText (juce::SystemClipboard::getTextFromClipboard());
        repaint();
        return ok;
    }
    if (k.getKeyCode() == juce::KeyPress::escapeKey) { multiSel.fill (false); if (sketchMode) setSketchMode (false); repaint(); return true; }
    return false;
}

void SpectrumDisplay::paintPiano (juce::Graphics& g, juce::Rectangle<float> a)
{
    // Teclado de piano abajo: cada tecla en su frecuencia. Se iluminan las notas de los picos marcados y la de
    // la banda elegida (para afinar el EQ con la tonalidad del tema)
    const float y0 = a.getBottom() - 12.0f, h = 12.0f;
    g.setColour (juce::Colour (0xffd9ccd0).withAlpha (0.20f));   // teclas blancas (una franja continua)
    g.fillRect (juce::Rectangle<float> (a.getX(), y0, a.getWidth(), h));
    std::vector<int> lit;
    for (auto& p : peaks) if (p.alive && p.alpha > 0.5f) lit.push_back ((int) std::lround (69.0 + 12.0 * std::log2 (p.f / 440.0)));
    const int selNote = selectedBand >= 0 && proc.params.bands[(size_t) selectedBand].on->load() > 0.5f
                          ? (int) std::lround (69.0 + 12.0 * std::log2 (proc.params.bands[(size_t) selectedBand].freq->load() / 440.0)) : -1000;
    for (int n = 16; n <= 135; ++n)
    {
        const double f = 440.0 * std::pow (2.0, (n - 69) / 12.0);
        const float x0 = freqToX (f * std::pow (2.0, -1.0 / 24.0)), x1 = freqToX (f * std::pow (2.0, 1.0 / 24.0));
        if (x1 < a.getX() || x0 > a.getRight()) continue;
        const int pc = n % 12;
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        const bool isLit = std::find (lit.begin(), lit.end(), n) != lit.end();
        if (n == selNote || isLit)
        {
            g.setColour (n == selNote ? colourOf (selectedBand) : Colours::accent);
            g.fillRect (juce::Rectangle<float> (x0, y0, x1 - x0, black ? h * 0.62f : h));
        }
        if (black)
        {
            if (n != selNote && ! isLit) { g.setColour (juce::Colour (0xff120d10)); g.fillRect (juce::Rectangle<float> (x0, y0, x1 - x0, h * 0.62f)); }
            // el borde entre las dos blancas queda debajo de la negra
            g.setColour (juce::Colour (0xff120d10).withAlpha (0.8f));
            g.drawVerticalLine (juce::roundToInt ((x0 + x1) * 0.5f), y0 + h * 0.62f, y0 + h);
        }
        else if (pc == 4 || pc == 11)
        {
            // E-F y B-C: dos blancas juntas
            g.setColour (juce::Colour (0xff120d10).withAlpha (0.8f));
            g.drawVerticalLine (juce::roundToInt (x1), y0, y0 + h);
        }
        if (pc == 0 && x1 - x0 > 3.0f)
        {
            g.setColour (Colours::text.withAlpha (0.75f));
            g.setFont (monoFont (7.0f));
            g.drawText ("C" + juce::String (n / 12 - 1), juce::Rectangle<float> (x0 + 1.0f, y0 + h * 0.5f, 22.0f, h * 0.5f), juce::Justification::centredLeft);
        }
    }
}

} // namespace gaux
