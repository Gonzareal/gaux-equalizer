// Gaux Equalizer - CHOQUES: ver el espectro de otra pista (otro Gaux en el proyecto) y donde chocan
//
// Todas las instancias del Gaux en el mismo DAW comparten este "centro" (un objeto compartido que nace con la
// primera y muere con la ultima). Cada instancia es una "fuente": su nombre de pista (si el DAW lo da) y una cola
// con su audio (despues del EQ, en mono).
//  - El audio solo se copia a la cola mientras alguien esta mirando los choques (si nadie mira: costo cero).
//  - El analisis (FFT) de todas las fuentes lo hace el centro, en el hilo de la interfaz, una sola vez para
//    todas las ventanas abiertas (~30 veces por segundo).
//  - Un choque es una zona donde las dos pistas suenan fuerte y parecido: alli se tapan (enmascaramiento).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/FastFFT.h"
#include <array>
#include <vector>
#include <atomic>
#include <memory>
#include <optional>

namespace gaux
{
constexpr int kClashPoints = 160;                 // 1/16 de octava, 20 Hz - 20 kHz
inline double clashFreq (int i) { return 20.0 * std::pow (2.0, i / 16.0); }

class ClashFifo
{
public:
    static constexpr int kSize = 1 << 14;
    void push (const float* d, int n)
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (n, s1, n1, s2, n2);
        if (n1 > 0) std::copy (d, d + n1, buf.begin() + s1);
        if (n2 > 0) std::copy (d + n1, d + n1 + n2, buf.begin() + s2);
        fifo.finishedWrite (n1 + n2);
    }
    int pull (float* d, int maxN)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (std::min (maxN, fifo.getNumReady()), s1, n1, s2, n2);
        if (n1 > 0) std::copy (buf.begin() + s1, buf.begin() + s1 + n1, d);
        if (n2 > 0) std::copy (buf.begin() + s2, buf.begin() + s2 + n2, d + n1);
        fifo.finishedRead (n1 + n2);
        return n1 + n2;
    }
private:
    juce::AbstractFifo fifo { kSize };
    std::array<float, kSize> buf {};
};

// Una instancia del Gaux vista por las demas
struct ClashSource
{
    int id = 0;
    ClashFifo fifo;                      // audio (hilo de audio -> centro)
    std::atomic<double> sampleRate { 48000.0 };

    juce::String getName() const { const juce::SpinLock::ScopedLockType sl (nameLock); return name; }
    juce::Colour getColour() const { const juce::SpinLock::ScopedLockType sl (nameLock); return colour; }
    bool hasTrackColour() const { const juce::SpinLock::ScopedLockType sl (nameLock); return colourSet; }
    void setTrack (const juce::String& n, std::optional<juce::Colour> c)
    {
        const juce::SpinLock::ScopedLockType sl (nameLock);
        if (n.isNotEmpty()) { dawName = n; if (! userNamed) name = n; }
        if (c.has_value()) { colour = *c; colourSet = true; }
    }
    // Nombre puesto a mano (manda sobre el del DAW). Vacio = volver al del DAW
    void setUserName (const juce::String& n)
    {
        const juce::SpinLock::ScopedLockType sl (nameLock);
        userNamed = n.isNotEmpty();
        name = userNamed ? n : (dawName.isNotEmpty() ? dawName : "Gaux " + juce::String (id));
    }

    // Lo calcula el centro (hilo de la interfaz): espectro suavizado (dB, con la misma inclinacion de +4.5 dB/oct
    // que el analizador) y un nivel "de golpe" lento (sube al instante, baja 10 dB/s): asi un kick (que dura poco)
    // cuenta con su nivel real y los choques no parpadean
    std::array<float, kClashPoints> spectrum {}, slow {};
    bool hasAudio = false;
    double lastAudioMs = 0;

private:
    friend class ClashHub;
    mutable juce::SpinLock nameLock;
    juce::String name, dawName;
    bool userNamed = false;
    juce::Colour colour { 0xff38d6c8 };
    bool colourSet = false;
    std::vector<float> ring, work;
    int ringPos = 0;
};

class ClashHub : private juce::Timer
{
public:
    ClashHub() = default;
    ~ClashHub() override { stopTimer(); }   // (lo crea y destruye juce::SharedResourcePointer, en el hilo de la interfaz)

    // ---- Fuentes (constructor / destructor de cada instancia) ----
    void add (ClashSource* s)
    {
        const juce::ScopedLock sl (lock);
        s->id = ++nextId;
        {
            const juce::SpinLock::ScopedLockType l2 (s->nameLock);
            if (s->name.isEmpty()) s->name = "Gaux " + juce::String (s->id);   // (hasta que el DAW mande el nombre de la pista)
            // un color distinto para cada pista (si el DAW no manda el suyo)
            static const juce::uint32 cols[] = { 0xff38d6c8, 0xff4aa8ff, 0xffb36bff, 0xffffc233, 0xff7cff6b, 0xffff7ae0 };
            if (! s->colourSet) s->colour = juce::Colour (cols[(size_t) (s->id % 6)]);
        }
        sources.push_back (s);
        ++version;
    }
    void remove (ClashSource* s)
    {
        const juce::ScopedLock sl (lock);
        sources.erase (std::remove (sources.begin(), sources.end(), s), sources.end());
        ++version;
    }

    // ---- Quien mira (las pantallas con los choques prendidos) ----
    // (hilo de la interfaz)
    void subscribe() { if (subscribers.fetch_add (1) == 0) { lastTick = juce::Time::getMillisecondCounterHiRes(); startTimerHz (30); } }
    void unsubscribe() { if (subscribers.fetch_sub (1) == 1) stopTimer(); }
    // Hilo de audio: hace falta mandar el audio?
    bool wanted() const { return subscribers.load (std::memory_order_relaxed) > 0; }

    struct Info { int id; juce::String name; juce::Colour colour; bool hasAudio; };
    std::vector<Info> list() const
    {
        const juce::ScopedLock sl (lock);
        std::vector<Info> v;
        for (auto* s : sources) v.push_back ({ s->id, s->getName(), s->getColour(), s->hasAudio });
        return v;
    }
    int getVersion() const { return version.load(); }
    int getTick() const { return tick.load(); }

    // Copia el analisis de una fuente (false si ya no existe)
    bool read (int id, std::array<float, kClashPoints>& spec, std::array<float, kClashPoints>& slow, bool& hasAudio) const
    {
        const juce::ScopedLock sl (lock);
        for (auto* s : sources)
            if (s->id == id) { spec = s->spectrum; slow = s->slow; hasAudio = s->hasAudio; return true; }
        return false;
    }

    // Para pruebas: un paso del analisis sin esperar al temporizador
    void pumpForTests (float dt) { analyse (dt); }

private:
    void timerCallback() override
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = (float) juce::jlimit (0.001, 0.2, (now - lastTick) * 0.001);
        lastTick = now;
        analyse (dt);
    }

    void analyse (float dt)
    {
        constexpr int order = 12, N = 1 << order;
        if (fft == nullptr) { fft = std::make_unique<RealFFT> (order); win.resize (N); for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * i / N); }
        const juce::ScopedLock sl (lock);
        const double now = juce::Time::getMillisecondCounterHiRes();
        float tmp[4096];
        for (auto* s : sources)
        {
            if (s->ring.size() != (size_t) N) { s->ring.assign ((size_t) N, 0.0f); s->work.assign ((size_t) (2 * N), 0.0f); s->spectrum.fill (-140.0f); s->slow.fill (-140.0f); }
            int got = 0;
            for (int n; (n = s->fifo.pull (tmp, 4096)) > 0;)
            {
                for (int i = 0; i < n; ++i) { s->ring[(size_t) s->ringPos] = tmp[i]; s->ringPos = (s->ringPos + 1) % N; }
                got += n;
            }
            const float fall = 40.0f * dt;
            if (got == 0)
            {
                // sin audio nuevo (la pista no suena o esta en reposo): el espectro cae
                for (int i = 0; i < kClashPoints; ++i) { s->spectrum[(size_t) i] = std::max (-140.0f, s->spectrum[(size_t) i] - fall); s->slow[(size_t) i] = std::max (-140.0f, s->slow[(size_t) i] - 10.0f * dt); }
                if (now - s->lastAudioMs > 1500.0) s->hasAudio = false;
                continue;
            }
            s->lastAudioMs = now;
            for (int i = 0; i < N; ++i) s->work[(size_t) i] = s->ring[(size_t) ((s->ringPos + i) % N)] * win[(size_t) i];
            fft->performRealOnlyForwardTransform (s->work.data());
            const double fs = s->sampleRate.load();
            const float norm = 4.0f / (float) N;
            const float ka = 1.0f - std::exp (-dt / 0.03f);
            bool loud = false;
            for (int i = 0; i < kClashPoints; ++i)
            {
                const double f = clashFreq (i), f0 = f * std::pow (2.0, -1.0 / 32.0), f1 = f * std::pow (2.0, 1.0 / 32.0);
                auto pw = [&] (int k) { const double re = s->work[(size_t) (2 * k)], im = s->work[(size_t) (2 * k + 1)]; return re * re + im * im; };
                double p = 0;
                const double kf0 = f0 * N / fs, kf1 = f1 * N / fs;
                if (kf1 - kf0 < 2.0)
                {
                    // en graves el punto es mas angosto que una frecuencia de la FFT: se interpola (sin escalones)
                    const double kc = juce::jlimit (1.0, N / 2 - 1.0, f * N / fs);
                    const int k = (int) kc; const double t = kc - k;
                    p = pw (k) * (1.0 - t) + pw (k + 1) * t;
                }
                else
                {
                    const int k0 = juce::jlimit (1, N / 2, (int) std::floor (kf0)), k1 = juce::jlimit (k0, N / 2, (int) std::ceil (kf1));
                    for (int k = k0; k <= k1; ++k) p = std::max (p, pw (k));
                }
                const float db = (f < fs * 0.5 ? (float) (10.0 * std::log10 (p * norm * norm + 1.0e-20)) : -140.0f) + (float) (4.5 * std::log2 (f / 1000.0));
                float& sp = s->spectrum[(size_t) i];
                sp = db > sp ? sp + (db - sp) * ka : std::max (db, sp - fall);
                s->slow[(size_t) i] = std::max (db, s->slow[(size_t) i] - 10.0f * dt);
                loud = loud || db > -90.0f;
            }
            s->hasAudio = s->hasAudio || loud;
        }
        ++tick;
    }

    mutable juce::CriticalSection lock;
    std::vector<ClashSource*> sources;
    std::atomic<int> subscribers { 0 }, version { 0 }, tick { 0 };
    int nextId = 0;
    double lastTick = 0;
    std::unique_ptr<RealFFT> fft;
    std::vector<float> win;
};

// Zonas de choque entre dos espectros (lentos): donde las dos pistas suenan fuerte y parecido
struct ClashZone { double fLo = 0, fHi = 0, fCenter = 0; float strength = 0; };
inline void clashStrength (const std::array<float, kClashPoints>& mine, const std::array<float, kClashPoints>& other, std::array<float, kClashPoints>& out)
{
    float topM = -140, topO = -140;
    for (int i = 0; i < kClashPoints; ++i) { topM = std::max (topM, mine[(size_t) i]); topO = std::max (topO, other[(size_t) i]); }
    for (int i = 0; i < kClashPoints; ++i)
    {
        const float a = mine[(size_t) i], b = other[(size_t) i];
        // las dos tienen que ser importantes ahi (a menos de ~20 dB de lo mas fuerte de cada una): ahi la mas fuerte
        // tapa a la otra. Si una esta muy por encima (> 18 dB) ya casi no compiten.
        const float presA = juce::jlimit (0.0f, 1.0f, (a - (topM - 24.0f)) / 10.0f), presB = juce::jlimit (0.0f, 1.0f, (b - (topO - 24.0f)) / 10.0f);
        const float close = juce::jlimit (0.0f, 1.0f, 1.0f - (std::abs (a - b) - 6.0f) / 12.0f);
        out[(size_t) i] = (a > -100.0f && b > -100.0f) ? presA * presB * close : 0.0f;
    }
    // suavizado (1/8 de octava)
    std::array<float, kClashPoints> t = out;
    for (int i = 0; i < kClashPoints; ++i)
    {
        float s = 0, w = 0;
        for (int j = std::max (0, i - 1); j <= std::min (kClashPoints - 1, i + 1); ++j) { const float k = j == i ? 2.0f : 1.0f; s += t[(size_t) j] * k; w += k; }
        out[(size_t) i] = s / w;
    }
}
inline std::vector<ClashZone> clashZones (const std::array<float, kClashPoints>& st, float threshold = 0.35f)
{
    std::vector<ClashZone> z;
    for (int i = 0; i < kClashPoints;)
    {
        if (st[(size_t) i] < threshold) { ++i; continue; }
        int j = i; float peak = 0; double wsum = 0, lsum = 0;
        while (j < kClashPoints && st[(size_t) j] >= threshold) { peak = std::max (peak, st[(size_t) j]); wsum += st[(size_t) j]; lsum += st[(size_t) j] * std::log (clashFreq (j)); ++j; }
        if (j - i >= 2)   // (al menos 1/8 de octava)
        {
            ClashZone c; c.fLo = clashFreq (i) * std::pow (2.0, -1.0 / 32.0); c.fHi = clashFreq (j - 1) * std::pow (2.0, 1.0 / 32.0);
            c.fCenter = std::exp (lsum / wsum); c.strength = peak;
            z.push_back (c);
        }
        i = j;
    }
    // zonas separadas por un hueco chico (< 1/8 de octava, p. ej. entre armonicos de un bajo) son la misma
    for (size_t k = 1; k < z.size();)
    {
        if (std::log2 (z[k].fLo / z[k - 1].fHi) < 0.125)
        {
            auto& a = z[k - 1]; const auto& b = z[k];
            a.fCenter = std::exp (0.5 * (std::log (a.fCenter) + std::log (b.fCenter)));
            a.fHi = b.fHi; a.strength = std::max (a.strength, b.strength);
            z.erase (z.begin() + (long) k);
        }
        else ++k;
    }
    return z;
}
} // namespace gaux
