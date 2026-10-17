// Gaux Equalizer - Procesador de audio
#include "PluginProcessor.h"
#include <map>
#include "PluginEditor.h"

GauxEqProcessor::GauxEqProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "GauxEQ", gaux::createParameterLayout())
{
    params.init (apvts);
    presets.history = &history;
    history.reset();
    clashHub->add (&clashSource);
    startTimerHz (10);
}

GauxEqProcessor::~GauxEqProcessor()
{
    clashHub->remove (&clashSource);
    stopTimer();
    linEngine.store (nullptr);
}

// True peak: interpolacion 4x con un FIR polifasico (sinc con ventana de Kaiser, 48 coeficientes)
void GauxEqProcessor::prepareTruePeak()
{
    const int L = 4, N = L * kTpTaps;
    auto bessel = [] (double x) { double sum = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); sum += t; } return sum; };
    const double beta = 7.0;
    for (int n = 0; n < N; ++n)
    {
        const double m = n - (N - 1) * 0.5;
        const double x = m / L;
        const double sinc = std::abs (x) < 1.0e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
        const double r = 2.0 * n / (N - 1) - 1.0;
        const double w = bessel (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / bessel (beta);
        tpCoeffs[(size_t) (n % L)][(size_t) (kTpTaps - 1 - n / L)] = (float) (sinc * w);   // invertido (ver measureTruePeak)
    }
    for (auto& h : tpHist) h.fill (0.0f);
    tpPos = 0; tpHold = 0.0f;
}

float GauxEqProcessor::measureTruePeak (const float* const* ch, int numChannels, int n)
{
    // coeficientes guardados al reves para que el producto sea una suma contigua
    float peak = 0.0f;
    for (int c = 0; c < juce::jmin (numChannels, 2); ++c)
    {
        auto& h = tpHist[(size_t) c];
        int pos = tpPos;
        for (int i = 0; i < n; ++i)
        {
            pos = pos + 1 == kTpTaps ? 0 : pos + 1;
            const float x = ch[c][i];
            h[(size_t) pos] = x; h[(size_t) (pos + kTpTaps)] = x;
            const float* w = h.data() + pos + 1;   // las ultimas kTpTaps muestras, de la mas vieja a la mas nueva
            for (int ph = 0; ph < 4; ++ph)
            {
                const float* k = tpCoeffs[(size_t) ph].data();
                float acc = 0.0f;
                for (int t = 0; t < kTpTaps; ++t) acc += k[t] * w[t];
                peak = juce::jmax (peak, std::abs (acc));
            }
        }
        if (c == juce::jmin (numChannels, 2) - 1) tpPos = pos;
    }
    return peak;
}

static_assert (gaux::kLinearBands == gaux::kNumBands, "la fase lineal debe cubrir todas las bandas");
static_assert (gaux::kNaturalBands == gaux::kNumBands, "la fase natural debe cubrir todas las bandas");

gaux::LinearSpec GauxEqProcessor::makeLinearSpec (int resolution, bool mono) const
{
    gaux::LinearSpec sp;
    for (int b = 0; b < gaux::kNumBands; ++b) sp.bands[(size_t) b] = params.band (b);
    sp.zoneOn = params.rangeOn->load() > 0.5f;
    sp.zoneLo = params.rangeLo->load(); sp.zoneHi = params.rangeHi->load();
    sp.fs = currentSampleRate.load();
    sp.natural = resolution >= 3;
    sp.osFactor = sp.natural ? 1 << juce::jlimit (0, 2, resolution - 3) : 1;
    sp.length = sp.natural ? gaux::naturalSizes (sp.fs, sp.osFactor).length : gaux::linearPhaseLength (resolution, sp.fs);
    sp.mono = mono;
    return sp;
}

// Arma un motor de fase lineal nuevo (fuera del hilo de audio) y lo publica
void GauxEqProcessor::buildLinearEngine (int resolution)
{
    auto e = std::make_unique<gaux::LinearPhaseEngine>();
    e->resolution = resolution;
    e->prepare (makeLinearSpec (resolution, getMainBusNumInputChannels() == 1), juce::jmax (64, maxBlockSize));
    auto* old = linEngine.exchange (e.get());
    linOwned.push_back (std::move (e));
    for (auto it = linOwned.begin(); it != linOwned.end(); ++it)
        if (it->get() == old) { linRetired.push_back ({ std::move (*it), juce::Time::getMillisecondCounter() }); linOwned.erase (it); break; }
}

void GauxEqProcessor::timerCallback()
{
    if (const int w = linWanted.exchange (0); w > 0 && preparedOnce)
    {
        auto* e = linEngine.load();
        if (e == nullptr || e->resolution != w || e->getSampleRate() != currentSampleRate.load() || e->getMaxBlock() < maxBlockSize)
            buildLinearEngine (w);
    }
    // los motores viejos se liberan cuando el audio ya no los puede estar usando
    const auto now = juce::Time::getMillisecondCounter();
    linRetired.erase (std::remove_if (linRetired.begin(), linRetired.end(), [now] (const auto& r) { return now - r.second > 2000; }), linRetired.end());
}

bool GauxEqProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != out)
        return false;
    // Sidechain: apagado, mono o estereo
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void GauxEqProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    clashSource.sampleRate.store (sampleRate);
    maxBlockSize = samplesPerBlock;
    monoBuffer.allocate ((size_t) samplesPerBlock, true);
    duckBuf.allocate ((size_t) samplesPerBlock, true);

    for (int i = 1; i < 3; ++i)
    {
        oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
            2, (size_t) i, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversamplers[i]->initProcessing ((size_t) samplesPerBlock);
        scOversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
            1, (size_t) i, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        scOversamplers[i]->initProcessing ((size_t) samplesPerBlock);
    }
    scBuffer.setSize (1, samplesPerBlock, false, true, true);
    outOfRange.setSize (2, samplesPerBlock * 4, false, true, true);

    dryBuffer.setSize (2, samplesPerBlock, false, true, true);
    // Linea de retardo de la senal original: en bypass sale con la misma latencia que reporta el plugin
    {
        int size = 1;
        while (size < samplesPerBlock + 65536 + 8192 + 256) size <<= 1;   // hasta la latencia de la fase lineal HQ (mitad del filtro + particion)
        dryDelay.setSize (2, size, false, true, true);
        dryDelayMask = size - 1;
        dryDelayWrite = 0;
    }
    bypassMix.reset (sampleRate, 0.03);
    bypassMix.setCurrentAndTargetValue (params.bypass->load() > 0.5f ? 1.0f : 0.0f);
    autoGainSmooth.reset (sampleRate, 0.25);
    autoGainSmooth.setCurrentAndTargetValue (1.0f);
    for (auto& st : listenStates) st.reset();
    outputGain.reset (sampleRate, 0.05);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (params.output->load()));

    currentQuality = -1; // fuerza la preparacion de los motores
    qualityFade = 1.0f; qualityFadeDir = 0; pendingQuality = -1;
    monoScratch.setSize (1, samplesPerBlock, false, true, true);
    prepareTruePeak();
    preparedOnce = true;
    // Fase lineal: si esta elegida, el motor se arma ahora (el audio no esta corriendo)
    int q = juce::jlimit (0, 2, (int) params.quality->load());
    while (q > 0 && sampleRate * (1 << q) > 200000.0) --q;
    int linRes = params.mode->load() > 0.5f ? 0 : juce::jlimit (0, 3, (int) params.phase->load());
    if (linRes == 3) linRes = 3 + q;   // fase natural: el motor sabe que oversampling corregir
    linEngine.store (nullptr);
    linOwned.clear(); linRetired.clear();
    prepareEngines (isLinearRes (linRes) ? 0 : q);
    if (linRes > 0) buildLinearEngine (linRes);
    currentLinRes = pendingLinRes = linRes;
    spectral.prepare (sampleRate);
    matchScratch.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);
    spectralOn = pendingSpectral = ! isLinearRes (linRes) && wantsSpectral();
    applyLatency();
    linearActiveNow.store (isLinearRes (linRes));
    naturalActiveNow.store (linRes >= 3);
}

// Bandas estaticas que hay ahora (para descontarlas al calcular un match)
std::vector<gaux::BandSettings> GauxEqProcessor::currentStaticBands() const
{
    std::vector<gaux::BandSettings> v;
    for (int b = 0; b < gaux::kNumBands; ++b) { auto s = params.band (b); if (s.on) v.push_back (s); }
    return v;
}

// Pone las bandas en los lugares libres (apagados) como un solo paso de deshacer
int GauxEqProcessor::applyFittedBands (const std::vector<gaux::BandSettings>& bands)
{
    using namespace gaux;
    auto set = [this] (const juce::String& id, float v)
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 (v)); p->endChangeGesture();
        }
    };
    int placed = 0;
    size_t next = 0;
    for (int b = 0; b < kNumBands && next < bands.size(); ++b)
    {
        if (params.bands[(size_t) b].on->load() > 0.5f) continue;
        const auto& s = bands[next++];
        set (ParamID::band (b, "type"), (float) s.type);
        set (ParamID::band (b, "freq"), (float) s.freq);
        set (ParamID::band (b, "gain"), (float) juce::jlimit (-24.0, 24.0, s.gainDb / juce::jmax (0.01, params.gainScale->load() / 100.0)));
        set (ParamID::band (b, "q"), (float) s.q);
        set (ParamID::band (b, "slope"), 1.0f);
        set (ParamID::band (b, "stereo"), 0.0f);
        set (ParamID::band (b, "drive"), 0.0f);
        set (ParamID::band (b, "dyn"), 0.0f);
        set (ParamID::band (b, "spec"), 0.0f);
        set (gaux::ParamID::band (b, "on"), 1.0f);
        ++placed;
    }
    history.commit();
    return placed;
}

// Alguna banda pide dinamica espectral (solo Parametrico y fuera de la fase lineal)
bool GauxEqProcessor::wantsSpectral() const
{
    if (params.mode->load() > 0.5f) return false;
    for (int b = 0; b < gaux::kNumBands; ++b)
    {
        const auto& p = params.bands[(size_t) b];
        if (p.on->load() > 0.5f && p.dyn->load() > 0.5f && p.spec->load() > 0.5f && gaux::typeCanBeDynamic ((int) p.type->load()))
            return true;
    }
    return false;
}

// Latencia que se informa al DAW: fase lineal, o oversampling + dinamica espectral
void GauxEqProcessor::applyLatency()
{
    auto* e = linEngine.load();
    if (isLinearRes (currentLinRes) && e != nullptr) setLatencySamples (e->getLatency());
    // fase natural: el FIR corrige tambien el retraso del oversampling (la latencia queda fija en ~1.3 ms)
    else if (currentLinRes >= 3 && e != nullptr && e->isNatural()) setLatencySamples (e->getLatency() + (spectralOn ? spectral.getLatency() : 0));
    else setLatencySamples (osLatency + (spectralOn ? spectral.getLatency() : 0));
    spectralActiveNow.store (spectralOn && ! isLinearRes (currentLinRes));
}

void GauxEqProcessor::prepareEngines (int qualityIndex)
{
    qualityIndex = juce::jlimit (0, 2, qualityIndex);
    currentQuality = qualityIndex;
    const double fs = currentSampleRate * (1 << qualityIndex);
    engineSampleRate.store (fs);

    for (auto& b : bands) b.prepare (fs);
    vintage.prepare (fs);
    rangeMix.reset (fs, 0.04);
    rangeMix.setCurrentAndTargetValue (params.rangeOn->load() > 0.5f ? 1.0f : 0.0f);
    for (auto& ch : rangeStates) for (auto& st : ch) st.reset();
    modeMix.setTime (fs / gaux::EqBand::kTickSize, 0.015);
    modeMix.snap (params.mode->load() > 0.5f ? 1.0 : 0.0);
    tickCounter = 0;

    if (oversamplers[qualityIndex] != nullptr)
    {
        oversamplers[qualityIndex]->reset();
        if (scOversamplers[qualityIndex] != nullptr) scOversamplers[qualityIndex]->reset();
        osLatency = (int) std::round (oversamplers[qualityIndex]->getLatencyInSamples());
    }
    else
    {
        osLatency = 0;
    }
    applyLatency();
}

void GauxEqProcessor::processChain (juce::dsp::AudioBlock<float>& block, int numChannels, const float* sc, const float* duck, int osFactor)
{
    // Se procesa por tramos de un "tic" (16 muestras): en cada tic se actualizan los parametros y se arma la
    // lista de bandas encendidas; luego cada banda procesa el tramo entero (solo las que estan encendidas).
    // Todo el tramo va en doble precision entre banda y banda.
    constexpr int T = gaux::EqBand::kTickSize;
    const int n = (int) block.getNumSamples();
    const bool stereo = numChannels > 1;
    float* d0 = block.getChannelPointer (0);
    float* d1 = stereo ? block.getChannelPointer (1) : nullptr;
    double pl[T], pr[T], vl[T], vr[T];

    for (int i = 0; i < n;)
    {
        if (tickCounter == 0)
        {
            const bool wasVintageSilent = modeMix.current <= 0.0;
            const bool wasParamSilent = modeMix.current >= 1.0;
            modeMix.tick();
            // Al empezar un cambio de modo, limpiar la memoria del motor que entra
            if (wasVintageSilent && modeMix.current > 0.0) vintage.reset();
            if (wasParamSilent && modeMix.current < 1.0) for (auto& b : bands) b.reset();

            // Sidechain general: cuanto se aplana la curva en este instante
            const double dk = duck != nullptr ? (double) duck[juce::jmin (n - 1, i) / juce::jmax (1, osFactor)] : 0.0;
            numActiveBands = 0;
            if (modeMix.current < 1.0)
                for (int b = 0; b < gaux::kNumBands; ++b)
                {
                    auto& band = bands[(size_t) b];
                    if (band.isAsleep()) continue;
                    band.setDuck (dk);
                    band.controlTick();
                    if (band.isActive()) activeBands[(size_t) numActiveBands++] = b;
                }
            if (modeMix.current > 0.0) { vintage.setDuck (dk); vintage.controlTick(); }
        }
        const int seg = juce::jmin (n - i, T - tickCounter);
        tickCounter = (tickCounter + seg) % T;

        const double vMix = modeMix.current;
        for (int j = 0; j < seg; ++j) { pl[j] = d0[i + j]; pr[j] = stereo ? (double) d1[i + j] : 0.0; }
        if (vMix > 0.0)
        {
            for (int j = 0; j < seg; ++j) { vl[j] = pl[j]; vr[j] = pr[j]; }
            vintage.processSpan (vl, vr, seg, stereo);
        }
        if (vMix < 1.0)
            for (int k = 0; k < numActiveBands; ++k)
                bands[(size_t) activeBands[(size_t) k]].processSpan (pl, pr, seg, stereo, sc != nullptr ? sc + i : nullptr);

        if (vMix <= 0.0)
        {
            for (int j = 0; j < seg; ++j) d0[i + j] = (float) pl[j];
            if (stereo) for (int j = 0; j < seg; ++j) d1[i + j] = (float) pr[j];
        }
        else if (vMix >= 1.0)
        {
            for (int j = 0; j < seg; ++j) d0[i + j] = (float) vl[j];
            if (stereo) for (int j = 0; j < seg; ++j) d1[i + j] = (float) vr[j];
        }
        else
        {
            for (int j = 0; j < seg; ++j) d0[i + j] = (float) (pl[j] + vMix * (vl[j] - pl[j]));
            if (stereo) for (int j = 0; j < seg; ++j) d1[i + j] = (float) (pr[j] + vMix * (vr[j] - pr[j]));
        }
        i += seg;
    }
}

float GauxEqProcessor::estimateCurveLoudnessDb (bool vintageMode) const
{
    // Promedio de energia de la curva entre 40 Hz y 16 kHz (peso igual por octava)
    const double fs = currentSampleRate.load();
    constexpr int points = 32;
    std::array<gaux::BandSettings, gaux::kNumBands> bs;
    for (int b = 0; b < gaux::kNumBands; ++b) bs[(size_t) b] = params.band (b);
    const auto v = params.vintage();
    double sum = 0.0;
    for (int i = 0; i < points; ++i)
    {
        const double f = 40.0 * std::pow (16000.0 / 40.0, i / (double) (points - 1));
        double m = 1.0;
        if (vintageMode) m = gaux::magnitudeOfVintage (v, f, fs);
        else for (auto& s : bs) m *= gaux::magnitudeOfBand (s, f, fs);
        sum += m * m;
    }
    return (float) (10.0 * std::log10 (std::max (1.0e-9, sum / points)));
}

// Separa la senal en 3 partes con cruces Linkwitz-Riley (24 dB/oct): graves bajo "Low",
// el rango de proceso, y agudos sobre "High". Solo el rango pasa por el EQ; las otras dos
// se devuelven intactas. La suma de las 3 es plana (sin cambio de volumen en ninguna frecuencia).
// Se hace dentro del dominio con oversampling: todas las partes pasan por el mismo filtro.
void GauxEqProcessor::splitProcessRange (juce::dsp::AudioBlock<float>& blk, int numChannels, double fs)
{
    const int n = (int) blk.getNumSamples();
    const double loT = juce::jlimit (20.0, 20000.0, (double) params.rangeLo->load());
    const double hiT = juce::jlimit (loT * 1.05, 20000.0, (double) params.rangeHi->load());
    // Los bordes de la zona se deslizan (~25 ms) en vez de saltar: sin transitorios al moverlos o automatizarlos
    if (zoneLoLog < 0.0 || zoneFs != fs) { zoneLoLog = std::log (loT); zoneHiLog = std::log (hiT); zoneFs = fs; }
    const double sc = std::exp (-16.0 / (fs * 0.025));
    gaux::SvfCoeffs lpLo, hpLo, lpHi, hpHi, apHi;
    bool useLo = false, useHi = false;
    auto design = [&]
    {
        const double lo = std::exp (zoneLoLog), hi = std::exp (zoneHiLog);
        const double hiF = juce::jmin (hi, fs * 0.45);
        using S = gaux::SvfCoeffs;
        lpLo = S::fromBiquad (gaux::makeLowPass (fs, lo, 0.7071)); hpLo = S::fromBiquad (gaux::makeHighPass (fs, lo, 0.7071));
        lpHi = S::fromBiquad (gaux::makeLowPass (fs, hiF, 0.7071)); hpHi = S::fromBiquad (gaux::makeHighPass (fs, hiF, 0.7071));
        apHi = S::fromBiquad (gaux::makeAllPass (fs, hiF, 0.7071));
        // Siempre se separa (aunque el borde este en 20 Hz / 20 kHz): la suma es plana igual y
        // no hay cambios de estructura mientras se mueven los bordes
        useLo = true; useHi = true; juce::ignoreUnused (hi);
    };
    design();

    for (int i = 0; i < n; ++i)
    {
        if ((i & 15) == 0)
        {
            const double tl = std::log (loT), th = std::log (hiT);
            if (std::abs (zoneLoLog - tl) > 1.0e-4 || std::abs (zoneHiLog - th) > 1.0e-4)
            {
                zoneLoLog = tl + (zoneLoLog - tl) * sc;
                zoneHiLog = th + (zoneHiLog - th) * sc;
                if (zoneHiLog < zoneLoLog + 0.05) zoneHiLog = zoneLoLog + 0.05;
                design();
            }
        }
        const float m = rangeMix.getNextValue();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* d = blk.getChannelPointer ((size_t) ch);
            auto& st = rangeStates[(size_t) ch];
            const double x = d[i];
            double low = 0.0, rest = x;
            if (useLo)
            {
                low  = st[1].process (st[0].process (x, lpLo), lpLo);
                rest = st[3].process (st[2].process (x, hpLo), hpLo);
            }
            double mid = rest, high = 0.0;
            if (useHi)
            {
                mid  = st[5].process (st[4].process (rest, lpHi), lpHi);
                high = st[7].process (st[6].process (rest, hpHi), hpHi);
                if (useLo) low = st[8].process (low, apHi);   // alinea la fase de los graves
            }
            outOfRange.setSample (ch, i, (float) (m * (low + high)));
            d[i] = (float) (m * mid + (1.0 - m) * x);
        }
    }
}

// Devuelve intacta la parte de afuera
void GauxEqProcessor::addBackOutOfRange (juce::dsp::AudioBlock<float>& blk, int numChannels)
{
    const int n = (int) blk.getNumSamples();
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* d = blk.getChannelPointer ((size_t) ch);
        const float* o = outOfRange.getReadPointer (ch);
        for (int i = 0; i < n; ++i) d[i] += o[i];
    }
}

void GauxEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    // Algunos DAWs (al exportar) mandan bloques mas grandes que los anunciados: se procesan por partes
    if (maxBlockSize > 0 && buffer.getNumSamples() > maxBlockSize)
    {
        for (int off = 0; off < buffer.getNumSamples(); off += maxBlockSize)
        {
            juce::AudioBuffer<float> part (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), off,
                                           juce::jmin (maxBlockSize, buffer.getNumSamples() - off));
            processBlock (part, midi);
        }
        return;
    }
    auto mainBuffer = getBusBuffer (buffer, true, 0);
    const int numChannels = juce::jmin (2, mainBuffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();

    for (int ch = getMainBusNumOutputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);
    if (numChannels == 0 || numSamples == 0) return;

    // Sidechain externo (si el DAW lo conecta): se mezcla a mono
    const float* scData = nullptr;
    {
        auto* scBus = getBus (true, 1);
        const bool scOn = scBus != nullptr && scBus->isEnabled() && getChannelCountOfBus (true, 1) > 0 && numSamples <= scBuffer.getNumSamples();
        bool signal = false;
        if (scOn)
        {
            auto scIn = getBusBuffer (buffer, true, 1);
            const int nc = scIn.getNumChannels();
            float* dst = scBuffer.getWritePointer (0);
            for (int i = 0; i < numSamples; ++i)
            {
                float v = 0.0f;
                for (int ch = 0; ch < nc; ++ch) v += scIn.getSample (ch, i);
                dst[i] = v / (float) juce::jmax (1, nc);
                signal = signal || std::abs (dst[i]) > 1.0e-6f;
            }
            scData = dst;
        }
        sidechainActive.store (signal);
    }

    auto& buf = mainBuffer;   // a partir de aqui se trabaja solo con el bus principal

    // EQ MATCH aprendiendo: copiar esta pista (antes del EQ) y/o el sidechain a sus colas (nada mas)
    if (match.learnSrc.load (std::memory_order_relaxed) && numSamples <= (int) matchScratch.size())
    {
        const float* l = buf.getReadPointer (0);
        const float* r = numChannels > 1 ? buf.getReadPointer (1) : l;
        for (int i = 0; i < numSamples; ++i) matchScratch[(size_t) i] = 0.5f * (l[i] + r[i]);
        matchSrcFifo.push (matchScratch.data(), numSamples);
    }
    if (match.learnRef.load (std::memory_order_relaxed) && scData != nullptr) matchRefFifo.push (scData, numSamples);

    // La interfaz esta abierta? Medidores, analizador y calavera solo se calculan si alguien los mira
    const bool ui = uiActive.load (std::memory_order_relaxed) > 0 || forceUiForTests.load (std::memory_order_relaxed);
    const int lat = juce::jlimit (0, juce::jmax (0, dryDelayMask - maxBlockSize), getLatencySamples());
    const bool needDry = lat > 0 || bypassMix.isSmoothing() || params.bypass->load() > 0.5f;

    // Medidor de entrada y copia de la senal original (para el bypass)
    float inPeakNow = 0.0f;
    {
        float sumSq = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* d = buf.getReadPointer (ch);
            const auto mm = juce::FloatVectorOperations::findMinAndMax (d, numSamples);
            inPeakNow = juce::jmax (inPeakNow, -mm.getStart(), mm.getEnd());
            if (ui) for (int i = 0; i < numSamples; ++i) sumSq += d[i] * d[i];
            if (needDry && dryDelayMask > 0)
            {
                float* dl = dryDelay.getWritePointer (ch);
                const int first = juce::jmin (numSamples, dryDelayMask + 1 - dryDelayWrite);
                std::copy (d, d + first, dl + dryDelayWrite);
                if (first < numSamples) std::copy (d + first, d + numSamples, dl);
            }
        }
        dryBlockStart = dryDelayWrite;
        dryDelayWrite = (dryDelayWrite + numSamples) & dryDelayMask;
        if (ui) inputRms.store (std::sqrt (sumSq / (float) (numSamples * numChannels)));
    }

    // Pista en silencio: si no entra nada (menos de -140 dBFS) desde hace 100 ms y la cola de los filtros ya
    // se apago del todo (-160 dBFS),
    // el plugin "duerme" y no gasta CPU (en una sesion grande la mayoria de las pistas esta en silencio
    // buena parte del tiempo). Se despierta al instante con la primera muestra que llega.
    {
        const bool silentIn = inPeakNow < 1.0e-7f;   // -140 dBFS: silencio digital
        silentInSamples = silentIn ? juce::jmin (silentInSamples + numSamples, 1 << 30) : 0;
        const bool canSleep = allowSleep.load (std::memory_order_relaxed) && silentIn && silentInSamples > (int) (0.1 * currentSampleRate.load()) + getLatencySamples() && lastOutPeak < 1.0e-8f
                           && ! bypassMix.isSmoothing() && qualityFadeDir == 0 && ! rangeMix.isSmoothing();
        if (canSleep)
        {
            for (int ch = 0; ch < numChannels; ++ch) buf.clear (ch, 0, numSamples);
            if (ui)
            {
                outputRms.store (0.0f); inputPeak.store (inputPeak.load() * 0.9f); outputPeak.store (outputPeak.load() * 0.9f);
                satDb.store (-100.0f);
                clipHold *= std::exp (-(float) numSamples / (float) (currentSampleRate.load() * 0.8)); clipLevel.store (clipHold);
                if (params.analyzer->load() > 0.5f && numSamples <= maxBlockSize)
                {
                    std::fill (monoBuffer.get(), monoBuffer.get() + numSamples, 0.0f);
                    preFifo.push (monoBuffer.get(), numSamples);
                    analyzerFifo.push (monoBuffer.get(), numSamples);
                }
            }
            sleepingNow.store (true);
            return;
        }
        sleepingNow.store (false);
    }

    if (ui)
    {
        // Detector de golpes del kick en la musica que entra (asi la calavera sigue el groove
        // aunque el plugin este saturando fuerte): graves < 150 Hz, envolvente rapida vs media
        {
            const double fsr = currentSampleRate.load();
            const double lpC = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 150.0 / fsr);
            const double fa = 1.0 - std::exp (-1.0 / (fsr * 0.0008)), fr = 1.0 - std::exp (-1.0 / (fsr * 0.030));
            const double ma = 1.0 - std::exp (-1.0 / (fsr * 0.040)), mr = 1.0 - std::exp (-1.0 / (fsr * 0.160));
            const int refractory = (int) (fsr * 0.14);
            for (int i = 0; i < numSamples; ++i)
            {
                double x = buf.getReadPointer (0)[i];
                if (numChannels > 1) x = 0.5 * (x + buf.getReadPointer (1)[i]);
                beatLp += lpC * (x - beatLp);
                const double a = std::abs (beatLp);
                beatFast += (a > beatFast ? fa : fr) * (a - beatFast);
                beatSlow += (beatFast > beatSlow ? ma : mr) * (beatFast - beatSlow);
                const double r = std::min (4.0, beatFast / std::max (1.0e-6, beatSlow));
                if (beatHold > 0) --beatHold;
                else if (beatFast > 0.002 && r > 1.8)
                {
                    beatHold = refractory;
                    beatStrength.store ((float) juce::jlimit (0.0, 1.0, beatFast / 0.25));
                    beatCount.fetch_add (1);
                }
            }
        }
        inputPeak.store (juce::jmax (inPeakNow, inputPeak.load() * 0.9f));

        // Espectro de entrada (antes del EQ)
        if (params.analyzer->load() > 0.5f && numSamples <= maxBlockSize)
        {
            const float sc = numChannels > 1 ? 0.5f : 1.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                float v = buf.getReadPointer (0)[i];
                if (numChannels > 1) v += buf.getReadPointer (1)[i];
                monoBuffer[i] = v * sc;
            }
            preFifo.push (monoBuffer.get(), numSamples);
        }
    }

    // Cambio de calidad (oversampling): re-preparar motores
    // Oversampling efectivo: nunca mas de ~192 kHz internos (a 96 kHz, HQ 4x trabaja a 2x;
    // a 192 kHz no hace falta). Mismo resultado sonoro, mucho menos CPU.
    int quality = juce::jlimit (0, 2, (int) params.quality->load());
    while (quality > 0 && currentSampleRate.load() * (1 << quality) > 200000.0) --quality;
    // Cambiar la calidad reinicia el oversampling y cambia la latencia: para que no suene un clic, la salida
    // baja a cero en 6 ms, se cambia el motor y vuelve a subir en 6 ms (un "dip" cortisimo, sin chasquido)
    // Fase lineal (solo Parametrico). Si el motor todavia no esta listo, se pide y se sigue como estaba.
    // (3 = fase natural: los IIR siguen trabajando y un FIR corto corrige la fase)
    int wantLin = params.mode->load() > 0.5f ? 0 : juce::jlimit (0, 3, (int) params.phase->load());
    if (wantLin == 3) wantLin = 3 + quality;   // fase natural: un motor para cada oversampling (3/4/5 = 1x/2x/4x)
    auto* linE = linEngine.load();
    const bool linOk = wantLin == 0 || (linE != nullptr && linE->resolution == wantLin && linE->getSampleRate() == currentSampleRate.load()
                                        && linE->getMaxBlock() >= maxBlockSize);
    if (! linOk) linWanted.store (wantLin);
    // mientras se arma el motor de fase natural nuevo, el oversampling sigue como estaba (cambian juntos)
    if (! linOk && wantLin >= 3) quality = currentLinRes >= 3 ? currentLinRes - 3 : currentQuality;
    const int targetLin = linOk ? wantLin : currentLinRes;
    if (isLinearRes (targetLin)) quality = 0;   // la fase lineal no necesita oversampling
    // Dinamica espectral: agrega latencia, asi que prenderla o apagarla tambien pasa por el fundido corto
    const bool wantSpec = ! isLinearRes (targetLin) && wantsSpectral();
    if (qualityFadeDir == 0 && (quality != currentQuality || targetLin != currentLinRes || wantSpec != spectralOn))
    { pendingQuality = quality; pendingLinRes = targetLin; pendingSpectral = wantSpec; qualityFadeDir = -1; }
    if (qualityFadeDir == -1 && qualityFade <= 0.0f)
    {
        currentLinRes = (pendingLinRes > 0 && linE != nullptr) ? pendingLinRes : 0;
        if (spectralOn != pendingSpectral) spectral.reset();
        spectralOn = pendingSpectral && ! isLinearRes (currentLinRes);
        prepareEngines (pendingQuality);   // (tambien informa la latencia nueva)
        if (currentLinRes > 0) { linE->reset(); if (currentLinRes >= 3) linE->setSpec (makeLinearSpec (currentLinRes, numChannels == 1)); applyLatency(); }
        // Los motores arrancan vacios (oversampling, fase lineal, espectral): la salida vuelve a subir cuando
        // ya trae audio, no antes (si no, la senal retrasada apareceria de golpe al terminar el fundido)
        qualityFadeHold = getLatencySamples();
        qualityFadeDir = 1;
    }
    linearActiveNow.store (isLinearRes (currentLinRes));
    naturalActiveNow.store (currentLinRes >= 3 && linE != nullptr);

    // Sidechain general: detector de nivel de la otra pista. El nivel se sigue rapido (pico con 25 ms de
    // salida, para no perder los golpes) y el "duck" baja al instante cuando el golpe supera el umbral y vuelve
    // con el RELEASE elegido, como un compresor de sidechain. Rodilla de 8 dB: con el umbral unos 8 dB debajo
    // del pico del kick la curva se aplana del todo en cada golpe y vuelve entre golpes.
    const bool scGlobal = params.scOn->load() > 0.5f;
    const bool duckActive = scGlobal || scDuckSmooth > 1.0e-6;
    if (duckActive || (ui && scData != nullptr))
    {
        const double fs = currentSampleRate.load();
        const double att = 1.0 - std::exp (-1.0 / (fs * 0.0005)), envRel = 1.0 - std::exp (-1.0 / (fs * 0.025));
        const double duckRel = 1.0 - std::exp (-1.0 / (fs * 0.001 * juce::jlimit (10.0, 1000.0, (double) params.scRel->load())));
        const double thr = params.scThr->load(), amt = params.scDuck->load() / 100.0;
        const double thrLin = std::pow (10.0, thr / 20.0);
        float* dk = duckBuf.get();
        for (int i = 0; i < numSamples; ++i)
        {
            if (scData != nullptr)
            {
                const double a = std::abs ((double) scData[i]);
                scEnv += (a > scEnv ? att : envRel) * (a - scEnv);
            }
            else scEnv -= envRel * scEnv;
            double duck = 0.0;
            if (scGlobal && scEnv > thrLin)
                duck = juce::jlimit (0.0, 1.0, (20.0 * std::log10 (scEnv) - thr) / 8.0) * amt;   // rodilla de 8 dB
            // la curva vuelve suave (como el release de un compresor) aunque se apague el SC
            scDuckSmooth = duck > scDuckSmooth ? duck : scDuckSmooth + (duck - scDuckSmooth) * duckRel;
            if (scDuckSmooth < 1.0e-7) scDuckSmooth = 0.0;
            if (dk != nullptr) dk[i] = (float) scDuckSmooth;
        }
        sidechainDb.store ((float) juce::Decibels::gainToDecibels (scEnv, -100.0));
        scDuckNow.store ((float) scDuckSmooth);
    }
    else
    {
        scEnv = 0.0; scDuckSmooth = 0.0;
        if (ui) { sidechainDb.store (-100.0f); scDuckNow.store (0.0f); }
    }
    float* const duckPtr = duckActive ? duckBuf.get() : nullptr;

    // Parametros actuales
    for (int b = 0; b < gaux::kNumBands; ++b)
    {
        // banda apagada y dormida: basta con mirar su interruptor (no se leen sus 17 parametros)
        if (bands[(size_t) b].isAsleep() && params.bands[(size_t) b].on->load() < 0.5f) continue;
        auto s = params.band (b);
        s.extSc = s.extSc || scGlobal;      // con SC general, todas las bandas dinamicas escuchan al sidechain
        bands[(size_t) b].setTarget (s);
    }
    vintage.setTarget (params.vintage());
    modeMix.target = params.mode->load() > 0.5f ? 1.0 : 0.0;

    // Rango de proceso (separar lo de afuera)
    rangeMix.setTargetValue (params.rangeOn->load() > 0.5f ? 1.0f : 0.0f);
    const bool rangeActive = rangeMix.isSmoothing() || rangeMix.getTargetValue() > 0.5f;
    if (! rangeActive) for (auto& ch : rangeStates) for (auto& st : ch) st.reset();
    const double fsUp = currentSampleRate.load() * (1 << currentQuality);
    auto runChain = [&] (juce::dsp::AudioBlock<float>& b, const float* scPtr, const float* duckPtr)
    {
        const bool split = rangeActive && (int) b.getNumSamples() <= outOfRange.getNumSamples();
        if (split) splitProcessRange (b, numChannels, fsUp);
        processChain (b, numChannels, scPtr, duckPtr, 1 << currentQuality);
        if (split) addBackOutOfRange (b, numChannels);
    };

    juce::dsp::AudioBlock<float> block (buf.getArrayOfWritePointers(), (size_t) numChannels, (size_t) numSamples);

    // Por seguridad se procesa en trozos del tamano preparado
    const int chunk = juce::jmax (1, maxBlockSize);
    if (isLinearRes (currentLinRes) && linE != nullptr)
    {
        // FASE LINEAL: toda la curva en un filtro FIR (la dinamica, la saturacion y el SC general se pausan)
        linE->setSpec (makeLinearSpec (currentLinRes, numChannels == 1));
        for (int offset = 0; offset < numSamples; offset += chunk)
        {
            const int len = juce::jmin (chunk, numSamples - offset);
            float* l = buf.getWritePointer (0) + offset;
            float* r = numChannels > 1 ? buf.getWritePointer (1) + offset : monoScratch.getWritePointer (0);
            if (numChannels == 1) std::copy (l, l + len, r);
            linE->process (l, r, len);
        }
    }
    else for (int offset = 0; offset < numSamples; offset += chunk)
    {
        const int len = juce::jmin (chunk, numSamples - offset);
        auto sub = block.getSubBlock ((size_t) offset, (size_t) len);
        if (auto* os = oversamplers[currentQuality].get())
        {
            const float* scUp = nullptr;
            if (scData != nullptr && scOversamplers[currentQuality] != nullptr)
            {
                float* scPtr = scBuffer.getWritePointer (0) + offset;
                juce::dsp::AudioBlock<float> scBlock (&scPtr, 1, (size_t) len);
                scUp = scOversamplers[currentQuality]->processSamplesUp (scBlock).getChannelPointer (0);
            }
            auto upBlock = os->processSamplesUp (sub);
            runChain (upBlock, scUp, duckPtr != nullptr ? duckPtr + offset : nullptr);
            os->processSamplesDown (sub);
        }
        else
        {
            runChain (sub, scData != nullptr ? scData + offset : nullptr, duckPtr != nullptr ? duckPtr + offset : nullptr);
        }
    }

    // FASE NATURAL: despues de los IIR, el FIR corto que deja la fase igual a la del analogico
    if (currentLinRes >= 3 && linE != nullptr)
    {
        linE->setSpec (makeLinearSpec (currentLinRes, numChannels == 1));
        for (int offset = 0; offset < numSamples; offset += chunk)
        {
            const int len = juce::jmin (chunk, numSamples - offset);
            float* l = buf.getWritePointer (0) + offset;
            float* r = numChannels > 1 ? buf.getWritePointer (1) + offset : monoScratch.getWritePointer (0);
            if (numChannels == 1) std::copy (l, l + len, r);
            linE->process (l, r, len);
        }
    }

    // Dinamica espectral (despues de toda la curva; en fase lineal la dinamica esta en pausa)
    if (spectralOn && ! isLinearRes (currentLinRes))
    {
        for (int b = 0; b < gaux::kNumBands; ++b)
        {
            gaux::SpectralBand sb;
            const auto& p = params.bands[(size_t) b];
            if (p.spec->load() > 0.5f && p.on->load() > 0.5f && p.dyn->load() > 0.5f)
            {
                const auto s = params.band (b);
                if (gaux::typeCanBeDynamic (s.type))
                {
                    sb.on = true; sb.domain = s.stereo; sb.thresholdDb = s.thresholdDb; sb.rangeDb = s.rangeDb;
                    sb.attackMs = s.attackMs; sb.releaseMs = s.releaseMs; sb.autoThr = s.autoThr; sb.extSc = s.extSc;
                    sb.shape = s;
                }
            }
            spectral.setBand (b, sb);
        }
        spectral.process (buf.getWritePointer (0), numChannels > 1 ? buf.getWritePointer (1) : nullptr, scData, numSamples);
    }

    // Exportar la ganancia dinamica actual de cada banda (para la interfaz)
    if (ui) for (int b = 0; b < gaux::kNumBands; ++b)
    {
        const bool spec = spectralOn && params.bands[(size_t) b].spec->load() > 0.5f;
        dynGainDb[(size_t) b].store (spec ? spectral.exportMaxAction[(size_t) b].load() : (float) bands[(size_t) b].getDynamicGainDb());
        detectorDb[(size_t) b].store (spec ? spectral.exportMaxLevel[(size_t) b].load() : (float) bands[(size_t) b].getDetectorDb());
        thresholdDb[(size_t) b].store ((float) bands[(size_t) b].getThresholdDb());
    }

    const bool vintageMode = params.mode->load() > 0.5f;

    // Solo de banda (solo en modo parametrico)
    const int solo = vintageMode ? -1 : soloBand.load();
    if (solo != lastSolo) { for (auto& st : listenStates) st.reset(); lastSolo = solo; }
    if (solo >= 0 && solo < gaux::kNumBands)
    {
        const auto c = gaux::designListen (params.band (solo), currentSampleRate.load());
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* d = buf.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = (float) listenStates[(size_t) ch].process (d[i], c);
        }
    }

    // Auto Gain: compensa el cambio de volumen que produce la curva
    const bool autoGain = params.autoGain->load() > 0.5f;
    float agDb = 0.0f;
    if (autoGain)
    {
        // El calculo de la curva es caro: solo se rehace si cambio algun ajuste que la afecta
        std::array<float, kAgSigSize> sig {};
        int k = 0;
        sig[(size_t) k++] = vintageMode ? 1.0f : 0.0f;
        sig[(size_t) k++] = (float) currentSampleRate.load();
        sig[(size_t) k++] = params.gainScale->load();
        for (const auto& bp : params.bands)
            for (auto* a : { bp.on, bp.type, bp.freq, bp.gain, bp.q, bp.slope }) sig[(size_t) k++] = a->load();
        for (auto* a : { params.vLowFreq, params.vLowBoost, params.vLowAtten, params.vHighFreq, params.vHighBoost, params.vHighBw,
                         params.vAttenFreq, params.vHighAtten, params.vAirFreq, params.vAirGain, params.vLmFreq, params.vLmBoost,
                         params.vDipFreq, params.vDip, params.vHmFreq, params.vHmBoost, params.vLowCut, params.vHighCut,
                         params.vProgramIn, params.vMidIn, params.vFilterIn })
            sig[(size_t) k++] = a->load();
        if (sig != agSig || ! agValid) { agSig = sig; agValid = true; agCachedDb = juce::jlimit (-24.0f, 24.0f, -estimateCurveLoudnessDb (vintageMode)); }
        agDb = agCachedDb;
    }
    autoGainDb.store (agDb);
    autoGainSmooth.setTargetValue (juce::Decibels::decibelsToGain (agDb));

    // Ganancia de salida
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (params.output->load()));
    if (autoGainSmooth.isSmoothing() || std::abs (autoGainSmooth.getTargetValue() - 1.0f) > 1.0e-5f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = autoGainSmooth.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buf.getWritePointer (ch)[i] *= g;
        }
    }
    if (outputGain.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outputGain.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buf.getWritePointer (ch)[i] *= g;
        }
    }
    else
    {
        buf.applyGain (0, 0, numSamples, outputGain.getTargetValue());
        if (numChannels > 1) buf.applyGain (1, 0, numSamples, outputGain.getTargetValue());
    }

    // Fundido del cambio de calidad
    if (qualityFadeDir != 0)
    {
        const float step = 1.0f / (float) (currentSampleRate.load() * 0.006);
        for (int i = 0; i < numSamples; ++i)
        {
            if (qualityFadeDir == 1 && qualityFadeHold > 0) { --qualityFadeHold; for (int ch = 0; ch < numChannels; ++ch) buf.getWritePointer (ch)[i] = 0.0f; continue; }
            qualityFade = juce::jlimit (0.0f, 1.0f, qualityFade + (float) qualityFadeDir * step);
            const float g = qualityFade * qualityFade * (3.0f - 2.0f * qualityFade);
            for (int ch = 0; ch < numChannels; ++ch) buf.getWritePointer (ch)[i] *= g;
        }
        if (qualityFadeDir == 1 && qualityFade >= 1.0f) qualityFadeDir = 0;
    }

    // Bypass con fundido corto (sin clicks)
    bypassMix.setTargetValue (params.bypass->load() > 0.5f ? 1.0f : 0.0f);
    // La senal original sale retrasada exactamente la latencia que se reporta al DAW (HQ):
    // asi prender/apagar el plugin no corre el audio en el tiempo ni suena a "flanger"
    if ((bypassMix.isSmoothing() || bypassMix.getTargetValue() > 0.5f) && dryDelayMask > 0)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float b = bypassMix.getNextValue();
            const int idx = (dryBlockStart + i - lat) & dryDelayMask;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* d = buf.getWritePointer (ch);
                d[i] += b * (dryDelay.getSample (ch, idx) - d[i]);
            }
        }
    }

    // Medidores de salida (el pico siempre: lo usa el modo "dormido")
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto mm = juce::FloatVectorOperations::findMinAndMax (buf.getReadPointer (ch), numSamples);
            peak = juce::jmax (peak, -mm.getStart(), mm.getEnd());
        }
        lastOutPeak = peak;
        const int os = 1 << juce::jmax (0, currentQuality);
        const int nSat = numSamples * os * numChannels;
        if (ui)
        {
            float sumSq = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* d = buf.getReadPointer (ch);
                for (int i = 0; i < numSamples; ++i) sumSq += d[i] * d[i];
            }
            outputRms.store (std::sqrt (sumSq / (float) (numSamples * numChannels)));

            // Cuanta saturacion real se esta generando (energia de los armonicos vs la salida)
            double e = vintage.takeSatEnergy (nSat);
            for (auto& b : bands) e += b.takeSatEnergy (nSat);
            const double outMs = (double) sumSq / (double) (numSamples * numChannels);
            satDb.store (outMs > 1.0e-9 ? (float) (10.0 * std::log10 (std::max (1.0e-12, e) / outMs)) : -100.0f);

            outputPeak.store (juce::jmax (peak, outputPeak.load() * 0.9f));

            // True peak con retencion (se reinicia con un clic en el indicador)
            if (truePeakReset.exchange (false)) { tpHold = 0.0f; for (auto& h : tpHist) h.fill (0.0f); }
            const float* chans[2] = { buf.getReadPointer (0), numChannels > 1 ? buf.getReadPointer (1) : nullptr };
            tpHold = juce::jmax (tpHold, measureTruePeak (chans, numChannels, numSamples));
            truePeak.store (tpHold);

            // Indicador de clip para la mascota: sube rapido, baja lento
            const float hot = peak >= 0.999f ? 1.0f : juce::jlimit (0.0f, 1.0f, (peak - 0.7f) / 0.3f) * 0.6f;
            const float decay = std::exp (-(float) numSamples / (float) (currentSampleRate.load() * 0.8));
            clipHold = juce::jmax (hot, clipHold * decay);
            clipLevel.store (clipHold);
        }
        else
        {
            vintage.takeSatEnergy (nSat);
            for (auto& b : bands) b.takeSatEnergy (nSat);
        }
    }

    // Mandar la senal (mono) al analizador (y a los CHOQUES, solo si alguna ventana del proyecto los esta mirando)
    const bool toAnalyzer = ui && params.analyzer->load() > 0.5f;
    const bool toClash = clashHub->wanted();
    if ((toAnalyzer || toClash) && numSamples <= maxBlockSize)
    {
        const float scale = numChannels > 1 ? 0.5f : 1.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float s = buf.getReadPointer (0)[i];
            if (numChannels > 1) s += buf.getReadPointer (1)[i];
            monoBuffer[i] = s * scale;
        }
        if (toAnalyzer) analyzerFifo.push (monoBuffer.get(), numSamples);
        if (toClash) clashSource.fifo.push (monoBuffer.get(), numSamples);
    }
}

void GauxEqProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    // Interruptores y selectores: el DAW puede haberlos dejado en un valor intermedio (p. ej. 0.88 en un selector
    // de 2 opciones). Se guarda ese valor exacto para devolverle al DAW lo mismo al abrir la sesion.
    juce::String norms;
    for (auto* prm : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm); rp != nullptr && rp->isDiscrete())
            norms << rp->getParameterID() << "=" << juce::String (rp->getValue(), 6) << ";";
    state.setProperty ("hostNorm", norms, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void GauxEqProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            clashSource.setUserName (apvts.state.getProperty ("clashName").toString());   // nombre de la pista puesto a mano
            history.reset();
            ab.restore();
            // Interruptores y selectores: si el DAW los habia dejado en un valor intermedio (p. ej. 0.44),
            // replaceState no los toca porque el valor redondeado no cambio. Se dejan en el valor exacto de
            // la sesion para que el DAW lea siempre lo mismo al abrirla (sin avisarle: no es una edicion)
            std::map<juce::String, float> hostNorm;
            for (auto& kv : juce::StringArray::fromTokens (apvts.state.getProperty ("hostNorm").toString(), ";", ""))
                if (kv.contains ("=")) hostNorm[kv.upToFirstOccurrenceOf ("=", false, false)] = kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
            apvts.state.removeProperty ("hostNorm", nullptr);
            for (auto* prm : getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm); rp != nullptr && rp->isDiscrete())
                    if (auto* raw = apvts.getRawParameterValue (rp->getParameterID()))
                    {
                        float target = rp->convertTo0to1 (raw->load());
                        // el valor exacto que tenia el DAW, si corresponde a la misma opcion
                        if (auto it = hostNorm.find (rp->getParameterID()); it != hostNorm.end()
                            && std::abs (rp->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, it->second)) - raw->load()) < 1.0e-4f)
                            target = juce::jlimit (0.0f, 1.0f, it->second);
                        if (std::abs (rp->getValue() - target) > 1.0e-6f) rp->setValue (target);
                    }
        }
}

juce::AudioProcessorEditor* GauxEqProcessor::createEditor()
{
    return new GauxEqEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GauxEqProcessor();
}

//==============================================================================
// Copiar / pegar la curva
juce::String GauxEqProcessor::curveToText() const
{
    juce::String out = "GAUX-CURVA 1\n";
    for (int b = 0; b < gaux::kNumBands; ++b)
    {
        if (params.bands[(size_t) b].on->load() < 0.5f) continue;
        const juce::String prefix = "b" + juce::String (b + 1) + "_";
        juce::String line = "banda=" + juce::String (b + 1);
        for (auto* prm : getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                if (rp->getParameterID().startsWith (prefix))
                    line << " " << rp->getParameterID().substring (prefix.length()) << "=" << juce::String (rp->convertFrom0to1 (rp->getValue()), 6);
        out << line << "\n";
    }
    return out;
}

bool GauxEqProcessor::curveFromText (const juce::String& text)
{
    auto lines = juce::StringArray::fromLines (text.trim());
    if (lines.isEmpty() || ! lines[0].startsWith ("GAUX-CURVA")) return false;
    auto set = [this] (const juce::String& id, float v)
    {
        if (auto* p = apvts.getParameter (id)) { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 (v)); p->endChangeGesture(); }
    };
    // primero se apagan todas las bandas, despues se arman las de la curva (un solo paso de deshacer)
    for (int b = 0; b < gaux::kNumBands; ++b) set (gaux::ParamID::band (b, "on"), 0.0f);
    for (int i = 1; i < lines.size(); ++i)
    {
        auto tokens = juce::StringArray::fromTokens (lines[i], " ", "");
        if (tokens.isEmpty() || ! tokens[0].startsWith ("banda=")) continue;
        const int b = tokens[0].fromFirstOccurrenceOf ("=", false, false).getIntValue() - 1;
        if (b < 0 || b >= gaux::kNumBands) continue;
        for (int t = 1; t < tokens.size(); ++t)
        {
            const auto key = tokens[t].upToFirstOccurrenceOf ("=", false, false), val = tokens[t].fromFirstOccurrenceOf ("=", false, false);
            if (key.isNotEmpty() && key != "on") set (gaux::ParamID::band (b, key.toRawUTF8()), val.getFloatValue());
        }
        set (gaux::ParamID::band (b, "on"), 1.0f);
    }
    history.commit();
    return true;
}

