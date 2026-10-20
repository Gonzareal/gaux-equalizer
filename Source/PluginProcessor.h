// Gaux Equalizer - Procesador de audio
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "Parameters.h"
#include "Presets.h"
#include "DSP/LinearPhase.h"
#include "DSP/SpectralDynamics.h"
#include "MatchEngine.h"
#include "Collisions.h"

// Cola sin bloqueos para mandar audio del hilo de audio al analizador de la interfaz
class AnalyzerFifo
{
public:
    static constexpr int kSize = 1 << 15;

    void push (const float* data, int n)
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (n, s1, n1, s2, n2);
        if (n1 > 0) std::copy (data, data + n1, buffer.begin() + s1);
        if (n2 > 0) std::copy (data + n1, data + n1 + n2, buffer.begin() + s2);
        fifo.finishedWrite (n1 + n2);
    }

    int pull (float* dest, int maxN)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (std::min (maxN, fifo.getNumReady()), s1, n1, s2, n2);
        if (n1 > 0) std::copy (buffer.begin() + s1, buffer.begin() + s1 + n1, dest);
        if (n2 > 0) std::copy (buffer.begin() + s2, buffer.begin() + s2 + n2, dest + n1);
        fifo.finishedRead (n1 + n2);
        return n1 + n2;
    }

private:
    juce::AbstractFifo fifo { kSize };
    std::array<float, kSize> buffer {};
};

class GauxEqProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    GauxEqProcessor();
    ~GauxEqProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    gaux::ParamRefs params;
    gaux::UndoHistory history { *this };     // deshacer / rehacer
    gaux::PresetManager presets { apvts };
    gaux::ABCompare ab { *this, apvts };     // comparacion A/B
    AnalyzerFifo analyzerFifo;     // espectro despues del EQ
    AnalyzerFifo preFifo;          // espectro antes del EQ
    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<float> outputRms { 0.0f };   // nivel RMS de salida (para el VU)
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<float> inputRms { 0.0f }, inputPeak { 0.0f };
    std::atomic<float> clipLevel { 0.0f };   // 0..1, sube cuando la salida clipea (para la mascota)
    std::atomic<float> autoGainDb { 0.0f };
    std::atomic<int> soloBand { -1 };        // banda en solo (-1 = ninguna)
    std::array<std::atomic<float>, gaux::kNumBands> dynGainDb {};
    std::array<std::atomic<float>, gaux::kNumBands> detectorDb {};
    std::array<std::atomic<float>, gaux::kNumBands> thresholdDb {};
    std::atomic<double> engineSampleRate { 44100.0 };   // frecuencia interna (con oversampling)
    std::atomic<float> satDb { -100.0f };          // saturacion real vs salida (dB) para la calavera
    std::atomic<int> beatCount { 0 };              // cuenta de golpes (kick/bajo) detectados en el audio
    std::atomic<float> beatStrength { 0.0f };
    std::atomic<float> sidechainDb { -100.0f };   // nivel del sidechain (para la interfaz)
    std::atomic<float> scDuckNow { 0.0f };        // 0..1: cuanto se esta encogiendo la curva ahora
    std::atomic<bool> sidechainActive { false };   // el DAW esta mandando senal al sidechain
    std::atomic<int> uiActive { 0 };               // ventanas abiertas (medidores y analizador solo si hay alguna)
    std::atomic<bool> sleepingNow { false };       // la pista esta en silencio y el plugin no gasta CPU
    std::atomic<bool> linearActiveNow { false };
    std::atomic<bool> naturalActiveNow { false };
    std::atomic<int> dspResets { 0 };
    // Fase lineal: resolucion del FIR en uso (Hz) para dibujar el BRICK que realmente suena (0 si no hay fase lineal)
    double linearBrickResolution() const
    {
        if (! linearActiveNow.load()) return 0.0;
        auto* e = linEngine.load();
        return e != nullptr ? e->getSampleRate() / juce::jmax (1, e->getLength()) : 0.0;
    }   // (veces que la red de seguridad limpio todo por un NaN interno)   // fase natural en uso (la pantalla muestra la curva analogica)
    std::atomic<bool> spectralActiveNow { false };   // alguna banda usa dinamica espectral (hay latencia)
    const gaux::SpectralDynamics& getSpectral() const { return spectral; }

    // EQ MATCH (lo maneja la interfaz; el audio solo llena las colas mientras se aprende)
    gaux::MatchEngine match;
    // CHOQUES: este Gaux visto por los demas del proyecto (y el centro compartido por todos)
    juce::SharedResourcePointer<gaux::ClashHub> clashHub;
    gaux::ClashSource clashSource;
    // Copiar / pegar la curva (todas las bandas) como texto: sirve entre instancias y entre sesiones
    juce::String curveToText() const;
    bool curveFromText (const juce::String& text);   // false si el texto no es una curva del Gaux
    void setUserTrackName (const juce::String& n) { clashSource.setUserName (n); apvts.state.setProperty ("clashName", n, nullptr); }
    void updateTrackProperties (const TrackProperties& p) override
    {
        clashSource.setTrack (p.name.value_or (juce::String()), p.colourARGB.has_value() ? std::optional<juce::Colour> (juce::Colour (*p.colourARGB)) : std::nullopt);
    }
    AnalyzerFifo matchSrcFifo, matchRefFifo;
    // Aplicar bandas calculadas (match o curva dibujada) en los lugares libres. Devuelve cuantas puso.
    int applyFittedBands (const std::vector<gaux::BandSettings>& bands);
    std::vector<gaux::BandSettings> currentStaticBands() const;
    std::atomic<float> truePeak { 0.0f };          // pico real entre muestras (4x), lineal, con retencion
    void resetTruePeak() { truePeakReset.store (true); }   // el Parametrico esta en fase lineal (la pantalla dibuja la curva analogica)
    static inline std::atomic<bool> forceUiForTests { false };
    static inline std::atomic<bool> allowSleep { true };   // (las pruebas lo apagan para comparar)
private:
    double scEnv = 0.0, scDuckSmooth = 0.0;
    double beatLp = 0.0, beatFast = 0.0, beatSlow = 0.0; int beatHold = 0;
    double zoneLoLog = -1.0, zoneHiLog = -1.0, zoneFs = 0.0;
public:

    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter (gaux::ParamID::bypass); }

    // Cuanto cambia el volumen promedio con la curva actual (para Auto Gain)
    float estimateCurveLoudnessDb (bool vintageMode) const;

private:
    void prepareEngines (int osFactorIndex);
    void timerCallback() override;
    gaux::LinearSpec makeLinearSpec (int resolution, bool mono) const;
    void buildLinearEngine (int resolution);
    void processChain (juce::dsp::AudioBlock<float>& block, int numChannels, const float* sc, const float* duck, int osFactor);
    void splitProcessRange (juce::dsp::AudioBlock<float>& blk, int numChannels, double fs);
    void addBackOutOfRange (juce::dsp::AudioBlock<float>& blk, int numChannels);

    std::unique_ptr<juce::dsp::Oversampling<float>> oversamplers[3], scOversamplers[3];
    juce::AudioBuffer<float> scBuffer;

    // Rango de proceso: lo que queda fuera se separa antes y se devuelve intacto despues
    juce::SmoothedValue<float> rangeMix;
    std::array<std::array<gaux::SvfState, 9>, 2> rangeStates {};
    juce::AudioBuffer<float> outOfRange;
    int currentQuality = -1;

    std::array<gaux::EqBand, gaux::kNumBands> bands;
    gaux::VintageEq vintage;
    gaux::SpectralDynamics spectral;
    bool spectralOn = false, pendingSpectral = false;
    int qualityFadeHold = 0;
    std::vector<float> matchScratch;   // muestras de silencio mientras un motor recien encendido se llena
    int osLatency = 0;
    bool wantsSpectral() const;
    void applyLatency();
    gaux::Smoother modeMix;   // 0 = parametrico, 1 = vintage
    int tickCounter = 0;
    std::array<int, gaux::kNumBands> activeBands {};   // bandas encendidas (se arma en cada tic)
    int numActiveBands = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outputGain;
    juce::SmoothedValue<float> bypassMix, autoGainSmooth;
    juce::AudioBuffer<float> dryBuffer;
    juce::AudioBuffer<float> dryDelay;          // senal original retrasada (bypass alineado con la latencia)
    int dryDelayMask = 0, dryDelayWrite = 0, dryBlockStart = 0;
    std::array<gaux::BiquadState, 2> listenStates {};
    int listenActive = -1;      // banda que suena en solo ahora (cambia solo con el fundido en 0)
    double listenMix = 0.0;     // 0 = mezcla normal, 1 = solo de la banda (fundido de ~12 ms: sin clics)
    float clipHold = 0.0f;
    juce::HeapBlock<float> monoBuffer, duckBuf;
    float qualityFade = 1.0f; int qualityFadeDir = 0, pendingQuality = -1;   // cambio de calidad sin clic
    int maxBlockSize = 0;
    int silentInSamples = 0;
    // Fase lineal: el motor se arma fuera del hilo de audio y se publica con un puntero atomico
    std::atomic<gaux::LinearPhaseEngine*> linEngine { nullptr };
    std::vector<std::unique_ptr<gaux::LinearPhaseEngine>> linOwned;
    std::vector<std::pair<std::unique_ptr<gaux::LinearPhaseEngine>, juce::uint32>> linRetired;
    std::atomic<int> linWanted { 0 };
    int currentLinRes = 0, pendingLinRes = 0;
    static bool isLinearRes (int r) { return r == 1 || r == 2; }   // (3 = fase natural)
    bool preparedOnce = false;
    juce::AudioBuffer<float> monoScratch;
    // Medidor de true peak (ITU-R BS.1770: sobremuestreo 4x)
    static constexpr int kTpTaps = 12;
    std::array<std::array<float, kTpTaps>, 4> tpCoeffs {};
    std::array<std::array<float, 2 * kTpTaps>, 2> tpHist {};   // historial duplicado: lectura contigua
    int tpPos = 0;
    float tpHold = 0.0f;
    std::atomic<bool> truePeakReset { false };
    void prepareTruePeak();
    float measureTruePeak (const float* const* ch, int numChannels, int n);
    static constexpr int kAgSigSize = 3 + gaux::kNumBands * 7 + 21;
    std::array<float, kAgSigSize> agSig {};
    bool agValid = false; float agCachedDb = 0.0f;
    float lastOutPeak = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GauxEqProcessor)
};
