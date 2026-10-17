// Gaux Equalizer - EQ MATCH: hace que esta pista suene con el balance de una referencia
//
// Referencia: un archivo de audio (WAV, AIFF, FLAC, MP3...) o lo que entra por el sidechain.
// Esta pista: se aprende mientras suena (antes del EQ). Con los dos espectros promedio se calcula la curva
// que falta (curva objetivo) y el ajustador la convierte en bandas reales en los lugares libres.
// Todo corre fuera del hilo de audio (el audio solo copia muestras a dos colas mientras se aprende).
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "DSP/CurveFit.h"
#include "DSP/SpectrumLearner.h"

class GauxEqProcessor;

namespace gaux
{
class MatchEngine : private juce::Thread
{
public:
    enum class RefKind { None, File, Sidechain };

    MatchEngine() : juce::Thread ("Gaux match: analizar referencia") { formats.registerBasicFormats(); }
    ~MatchEngine() override { stopThread (4000); }


    // ---- Referencia ----
    juce::String getWildcard() const { return formats.getWildcardForAllFormats(); }

    void loadReferenceFile (const juce::File& f)
    {
        stopThread (4000);
        pendingFile = f;
        fileProgress.store (0.0f);
        fileState.store (1);   // analizando
        refKind = RefKind::File; refName = f.getFileName();
        startThread (juce::Thread::Priority::low);
    }

    void useSidechain (bool learn)
    {
        if (refKind != RefKind::Sidechain) { stopThread (4000); refKind = RefKind::Sidechain; refName = "Sidechain"; ref.prepare (lastFs > 0 ? lastFs : 48000.0); fileState.store (0); }
        learnRef.store (learn);
    }

    // ---- Esta pista ----
    void setLearnSource (bool on) { learnSrc.store (on); }
    void clearSource() { src.clear(); }
    void clearReference() { stopThread (4000); ref.clear(); refKind = RefKind::None; refName = {}; fileState.store (0); learnRef.store (false); }

    // Hilo de mensajes: traer lo que el audio dejo en las colas (llamar seguido mientras el panel esta abierto)
    template <typename Fifo>
    void poll (Fifo& srcFifo, Fifo& refFifo, double fs)
    {
        if (fs > 0 && fs != lastFs)
        {
            // frecuencia de muestreo nueva: lo aprendido de esta pista (y del sidechain) ya no vale
            lastFs = fs; src.prepare (fs);
            if (refKind == RefKind::Sidechain) ref.prepare (fs);
        }
        float tmp[4096];
        for (int n; (n = srcFifo.pull (tmp, 4096)) > 0;) if (learnSrc.load()) src.add (tmp, n);
        for (int n; (n = refFifo.pull (tmp, 4096)) > 0;) if (learnRef.load() && refKind == RefKind::Sidechain) ref.add (tmp, n);
        if (fileState.load() == 2 && fileSpec != nullptr)
        {
            // el hilo termino de analizar el archivo
            const juce::ScopedLock sl (fileLock);
            ref = std::move (*fileSpec); fileSpec.reset();
            fileState.store (3);
        }
    }

    bool sourceReady() const { return src.ready(); }
    bool referenceReady() const { return refKind != RefKind::None && ref.ready() && fileState.load() != 1; }
    double sourceSeconds() const { return src.seconds(); }
    double referenceSeconds() const { return ref.seconds(); }
    RefKind referenceKind() const { return refKind; }
    juce::String referenceName() const { return refName; }
    float referenceProgress() const { return fileProgress.load(); }
    bool analyzingFile() const { return fileState.load() == 1; }
    bool fileFailed() const { return fileState.load() == 4; }

    std::atomic<bool> learnSrc { false }, learnRef { false };

    // ---- Calculo ----
    struct Settings { double amount = 1.0; int bands = 8; double detailOct = 1.0 / 3.0; };

    // Curva objetivo (dB) en la grilla de ajuste, con las bandas existentes descontadas
    bool computeTarget (const Settings& s, const std::vector<BandSettings>& existing)
    {
        if (! sourceReady() || ! referenceReady()) return false;
        grid = fitGrid (20, 20000, 24);
        const auto sDb = src.toDb (grid, 1.0 / 6.0), rDb = ref.toDb (grid, 1.0 / 6.0);
        target = matchTarget (grid, sDb, rDb, s.amount, 12.0, weight, s.detailOct);
        fixed.assign (grid.size(), 0.0);
        for (auto& b : existing)
            if (b.on) for (size_t i = 0; i < grid.size(); ++i) fixed[i] += 20.0 * std::log10 (std::max (1.0e-6, analogMagnitudeOfBand (b, grid[i])));
        // las bandas de antes ya hacen parte del trabajo (el objetivo es el total)
        targetValid = true;
        ++targetVersion;
        return true;
    }

    FitResult fit (const Settings& s) const
    {
        FitOptions o; o.maxBands = juce::jlimit (1, 12, s.bands); o.toleranceDb = 0.5; o.maxQ = matchMaxQ (s.detailOct);
        return fitCurve (grid, target, weight, o, &fixed);
    }

    bool hasTarget() const { return targetValid; }
    int getTargetVersion() const { return targetVersion; }
    const std::vector<double>& targetGrid() const { return grid; }
    const std::vector<double>& targetDb() const { return target; }

    // Para la pantalla: mostrar la curva objetivo mientras el panel esta abierto
    std::atomic<bool> previewVisible { false };

private:
    void run() override
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (pendingFile));
        if (reader == nullptr) { fileState.store (4); return; }
        auto spec = std::make_unique<LongTermSpectrum>();
        spec->prepare (reader->sampleRate);
        const juce::int64 total = std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 600.0));   // hasta 10 minutos
        const int chunk = 16384;
        juce::AudioBuffer<float> buf ((int) juce::jmax (1u, reader->numChannels), chunk);
        std::vector<float> mono ((size_t) chunk);
        for (juce::int64 pos = 0; pos < total && ! threadShouldExit(); pos += chunk)
        {
            const int n = (int) std::min<juce::int64> (chunk, total - pos);
            reader->read (&buf, 0, n, pos, true, true);
            for (int i = 0; i < n; ++i)
            {
                float v = 0; for (int c = 0; c < buf.getNumChannels(); ++c) v += buf.getSample (c, i);
                mono[(size_t) i] = v / (float) buf.getNumChannels();
            }
            spec->add (mono.data(), n);
            fileProgress.store ((float) ((double) (pos + n) / (double) std::max<juce::int64> (1, total)));
        }
        if (threadShouldExit()) return;
        if (! spec->ready()) { fileState.store (4); return; }
        const juce::ScopedLock sl (fileLock);
        fileSpec = std::move (spec);
        fileState.store (2);
    }

    juce::AudioFormatManager formats;
    LongTermSpectrum src, ref;
    std::unique_ptr<LongTermSpectrum> fileSpec;
    juce::CriticalSection fileLock;
    juce::File pendingFile;
    std::atomic<float> fileProgress { 0.0f };
    std::atomic<int> fileState { 0 };   // 0 nada, 1 analizando, 2 listo (sin recoger), 3 listo, 4 error
    RefKind refKind = RefKind::None;
    juce::String refName;
    double lastFs = 0.0;
    std::vector<double> grid, target, weight, fixed;
    bool targetValid = false;
    int targetVersion = 0;
};
} // namespace gaux
