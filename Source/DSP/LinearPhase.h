// Gaux Equalizer - Modo de fase lineal del Parametrico
// La curva completa (todas las bandas con su forma analogica exacta, sin compresion cerca de Nyquist)
// se convierte en un filtro FIR de fase lineal y se aplica por convolucion. Sin corrimiento de fase:
// ideal para mastering, grupos de pistas y graves que se suman en paralelo.
//
//  - Estereo, L/R y M/S: el sistema completo es una matriz 2x2 de filtros de fase lineal
//      L' = C11*L + C12*R      R' = C21*L + C22*R
//    (si no hay bandas M/S, C12 = C21 = 0 y solo se calculan dos convoluciones).
//  - El diseno se hace en un hilo aparte; el hilo de audio nunca espera ni reserva memoria.
//  - Latencia = la mitad del largo del filtro (se informa al DAW, que la compensa).
#pragma once

#include <juce_dsp/juce_dsp.h>
#include "EqBand.h"
#include "FastFFT.h"
#include "NaturalPhase.h"

namespace gaux
{
constexpr int kLinearBands = 24;   // = kNumBands (comprobado en PluginProcessor.cpp)

struct LinearSpec
{
    std::array<BandSettings, kLinearBands> bands {};
    bool zoneOn = false;
    double zoneLo = 20.0, zoneHi = 20000.0;
    double fs = 48000.0;
    int length = 16384;
    bool mono = false;   // pista mono: todas las bandas actuan sobre el unico canal
    bool natural = false; // FASE NATURAL: FIR corto que corrige la fase de los IIR (en vez de la curva entera)
    int osFactor = 1;     // (fase natural) oversampling de los IIR

    bool sameAs (const LinearSpec& o) const
    {
        if (zoneOn != o.zoneOn || fs != o.fs || length != o.length || mono != o.mono || natural != o.natural || osFactor != o.osFactor) return false;
        if (zoneOn && (zoneLo != o.zoneLo || zoneHi != o.zoneHi)) return false;
        for (size_t i = 0; i < bands.size(); ++i)
        {
            const auto& a = bands[i]; const auto& b = o.bands[i];
            if (a.on != b.on) return false;
            if (! a.on) continue;
            if (a.type != b.type || a.freq != b.freq || a.gainDb != b.gainDb || a.q != b.q || a.slope != b.slope || a.stereo != b.stereo)
                return false;
        }
        return true;
    }
};

// Largo del filtro segun la resolucion elegida y la frecuencia de muestreo
inline int linearPhaseLength (int resolution, double fs)
{
    int scale = 1;
    while (scale < 4 && fs > 52000.0 * scale) scale *= 2;
    const int base = resolution >= 2 ? 65536 : 16384;
    return juce::jmin (base * scale, 131072);
}

// Diseno de los filtros (en el hilo de fondo)
inline void designLinearPhase (const LinearSpec& sp, juce::AudioBuffer<float>& direct, juce::AudioBuffer<float>& cross, bool& needCross)
{
    const int N = sp.length, M = 2 * N;
    int order = 0; while ((1 << order) < M) ++order;
    juce::dsp::FFT fft (order);
    std::vector<float> c11 ((size_t) (2 * M), 0.0f), c22 ((size_t) (2 * M), 0.0f), c12 ((size_t) (2 * M), 0.0f), c21 ((size_t) (2 * M), 0.0f);
    needCross = false;
    for (int k = 0; k <= M / 2; ++k)
    {
        const double f = juce::jmax (0.5, k * sp.fs / M);
        double hst = 1.0, hl = 1.0, hr = 1.0, hm = 1.0, hs = 1.0;
        for (const auto& b : sp.bands)
        {
            if (! b.on) continue;
            const double m = analogMagnitudeOfBand (b, f, true, sp.fs / sp.length);
            switch (sp.mono ? StereoMode::Stereo : (StereoMode) b.stereo)
            {
                case StereoMode::Stereo: hst *= m; break;
                case StereoMode::Left:   hl *= m; break;
                case StereoMode::Right:  hr *= m; break;
                case StereoMode::Mid:    hm *= m; break;
                case StereoMode::Side:   hs *= m; break;
            }
        }
        double hL = hst * hl, hR = hst * hr;
        if (sp.zoneOn)
        {
            // ZONE: fuera del rango la curva vuelve a plano con la forma de un cruce de 24 dB/oct
            const double w = 1.0 / (1.0 + std::pow (sp.zoneLo / f, 4.0)) / (1.0 + std::pow (f / sp.zoneHi, 4.0));
            hL = 1.0 + w * (hL - 1.0); hR = 1.0 + w * (hR - 1.0);
            hm = 1.0 + w * (hm - 1.0); hs = 1.0 + w * (hs - 1.0);
        }
        const double sum = 0.5 * (hm + hs), dif = 0.5 * (hm - hs);
        if (std::abs (dif) > 1.0e-7) needCross = true;
        // espectro real (fase cero): parte real en el bin k, imaginaria 0
        c11[(size_t) (2 * k)] = (float) (sum * hL);
        c22[(size_t) (2 * k)] = (float) (sum * hR);
        c12[(size_t) (2 * k)] = (float) (dif * hR);
        c21[(size_t) (2 * k)] = (float) (dif * hL);
    }
    auto toImpulse = [&] (std::vector<float>& spec, float* out)
    {
        fft.performRealOnlyInverseTransform (spec.data());   // (JUCE ya divide por M)
        for (int n = 0; n < N; ++n)
        {
            const int idx = n - N / 2;
            const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (n + 0.5) / N);   // ventana de Hann
            out[n] = (float) (spec[(size_t) ((idx + M) % M)] * w);
        }
    };
    direct.setSize (2, N, false, false, true);
    toImpulse (c11, direct.getWritePointer (0));
    toImpulse (c22, direct.getWritePointer (1));
    cross.setSize (2, N, false, false, true);
    if (needCross) { toImpulse (c12, cross.getWritePointer (0)); toImpulse (c21, cross.getWritePointer (1)); }
    else cross.clear();
}

// Respuesta compleja del oversampling del Gaux (subir y bajar, filtros IIR de media banda) en los bins 0..N/2
inline std::vector<Cx> measureOversampling (int osFactor, int N)
{
    std::vector<Cx> out;
    int lg = 0; while ((1 << lg) < osFactor) ++lg;
    if (lg == 0) return out;
    juce::dsp::Oversampling<float> os (1, (size_t) lg, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    os.initProcessing ((size_t) N);
    std::vector<float> x ((size_t) (2 * N), 0.0f);
    x[0] = 1.0f;
    float* ptr = x.data();
    juce::dsp::AudioBlock<float> blk (&ptr, 1, (size_t) N);
    os.processSamplesUp (blk);
    os.processSamplesDown (blk);
    int order = 0; while ((1 << order) < N) ++order;
    RealFFT fft (order);
    std::fill (x.begin() + N, x.end(), 0.0f);
    fft.performRealOnlyForwardTransform (x.data());
    out.resize ((size_t) (N / 2 + 1));
    for (int k = 0; k <= N / 2; ++k) out[(size_t) k] = Cx (x[(size_t) (2 * k)], x[(size_t) (2 * k + 1)]);
    return out;
}

// Tamano de particion de la convolucion (latencia extra = una particion): ~32 particiones por filtro
inline int linearPartitionSize (int length)
{
    int b = 256;
    while (b < 4096 && b * 32 < length) b *= 2;
    return b;
}

// Convolucion particionada uniforme (overlap-save en frecuencia) con una matriz 2x2 de filtros:
//   L' = C11*L + C12*R      R' = C21*L + C22*R
// El historial del audio (los espectros de las ultimas entradas) vive aparte del filtro: cambiar el
// filtro es instantaneo y correcto desde la primera muestra. Durante el cambio se calculan los dos filtros
// sobre el mismo historial y se pasa de uno al otro con un fundido corto (sin huecos ni golpes).
class PartitionedMatrixConvolver
{
public:
    struct Filter
    {
        std::array<std::vector<float>, 4> re, im;   // [particion * (B + 1) + bin]  (0 = C11, 1 = C22, 2 = C12, 3 = C21)
        bool cross = false;
    };

    void prepare (int irLength, int partition)
    {
        B = partition; P = juce::jmax (1, (irLength + B - 1) / B); K = B + 1;
        int order = 0; while ((1 << order) < 2 * B) ++order;
        fft = std::make_unique<RealFFT> (order);
        for (auto& c : xre) c.assign ((size_t) (P * K), 0.0f);
        for (auto& c : xim) c.assign ((size_t) (P * K), 0.0f);
        for (auto& c : inBuf) c.assign ((size_t) (2 * B), 0.0f);
        for (auto& c : outBuf) c.assign ((size_t) B, 0.0f);
        work.assign ((size_t) (4 * B + 4), 0.0f);
        for (auto* a : { &accRe, &accIm, &accRe2, &accIm2 }) a->assign ((size_t) K, 0.0f);
        tmpA.assign ((size_t) B, 0.0f); tmpB.assign ((size_t) B, 0.0f);
        reset();
    }

    void allocate (Filter& f) const
    {
        for (int i = 0; i < 4; ++i) { f.re[(size_t) i].assign ((size_t) (P * K), 0.0f); f.im[(size_t) i].assign ((size_t) (P * K), 0.0f); }
    }

    // Fuera del hilo de audio: pasa las respuestas al impulso a espectros por particion
    void makeFilter (const float* const irs[4], int irLength, bool cross, Filter& f, RealFFT& fftLocal) const
    {
        std::vector<float> buf ((size_t) (4 * B + 4));
        f.cross = cross;
        for (int which = 0; which < 4; ++which)
        {
            if (which >= 2 && ! cross) { std::fill (f.re[(size_t) which].begin(), f.re[(size_t) which].end(), 0.0f); std::fill (f.im[(size_t) which].begin(), f.im[(size_t) which].end(), 0.0f); continue; }
            for (int p = 0; p < P; ++p)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < B; ++i) { const int idx = p * B + i; if (idx < irLength) buf[(size_t) i] = irs[which][idx]; }
                fftLocal.performRealOnlyForwardTransform (buf.data(), true);
                float* r = f.re[(size_t) which].data() + p * K; float* m = f.im[(size_t) which].data() + p * K;
                for (int k = 0; k < K; ++k) { r[k] = buf[(size_t) (2 * k)]; m[k] = buf[(size_t) (2 * k + 1)]; }
            }
        }
    }

    int getPartition() const { return B; }
    int fftOrder() const { int o = 0; while ((1 << o) < 2 * B) ++o; return o; }

    void reset()
    {
        for (auto& c : xre) std::fill (c.begin(), c.end(), 0.0f);
        for (auto& c : xim) std::fill (c.begin(), c.end(), 0.0f);
        for (auto& c : inBuf) std::fill (c.begin(), c.end(), 0.0f);
        for (auto& c : outBuf) std::fill (c.begin(), c.end(), 0.0f);
        inPos = 0; head = 0;
    }

    // Procesa n muestras con el filtro 'cur'; si 'next' no es nulo, va pasando de cur a next.
    // fadeStep = cuanto avanza el fundido por muestra (1 / largo del fundido). Devuelve true al terminar el fundido.
    bool process (float* l, float* r, int n, const Filter& cur, const Filter* next, float& fade, float fadeStep)
    {
        bool finished = false;
        int i = 0;
        while (i < n)
        {
            const int chunk = juce::jmin (n - i, B - inPos);
            for (int j = 0; j < chunk; ++j)
            {
                const float a = l[i + j], b = r[i + j];
                l[i + j] = outBuf[0][(size_t) (inPos + j)];
                r[i + j] = outBuf[1][(size_t) (inPos + j)];
                inBuf[0][(size_t) (B + inPos + j)] = a;
                inBuf[1][(size_t) (B + inPos + j)] = b;
            }
            inPos += chunk; i += chunk;
            if (inPos == B)
            {
                inPos = 0;
                if (processBlock (cur, next, fade, fadeStep)) finished = true;
            }
        }
        return finished;
    }

private:
    bool processBlock (const Filter& cur, const Filter* next, float& fade, float fadeStep)
    {
        // 1) espectro de la entrada (ultimas 2B muestras) al historial
        for (int ch = 0; ch < 2; ++ch)
        {
            std::copy (inBuf[(size_t) ch].begin(), inBuf[(size_t) ch].end(), work.begin());
            std::fill (work.begin() + 2 * B, work.end(), 0.0f);
            fft->performRealOnlyForwardTransform (work.data(), true);
            float* r = xre[(size_t) ch].data() + head * K; float* m = xim[(size_t) ch].data() + head * K;
            for (int k = 0; k < K; ++k) { r[k] = work[(size_t) (2 * k)]; m[k] = work[(size_t) (2 * k + 1)]; }
            std::copy (inBuf[(size_t) ch].begin() + B, inBuf[(size_t) ch].end(), inBuf[(size_t) ch].begin());
        }
        // 2) salida de cada canal = suma sobre particiones (historial x filtro)
        bool finished = false;
        for (int out = 0; out < 2; ++out)
        {
            convolve (cur, out, accRe, accIm);
            inverse (accRe, accIm, tmpA);
            if (next != nullptr)
            {
                convolve (*next, out, accRe2, accIm2);
                inverse (accRe2, accIm2, tmpB);
                float f = fade;
                for (int k = 0; k < B; ++k)
                {
                    f = juce::jmin (1.0f, f + fadeStep);
                    // fundido de igual potencia no hace falta: los dos filtros tienen la misma fase (lineal)
                    outBuf[(size_t) out][(size_t) k] = tmpA[(size_t) k] + f * (tmpB[(size_t) k] - tmpA[(size_t) k]);
                }
                if (out == 1) { fade = f; finished = f >= 1.0f; }
            }
            else std::copy (tmpA.begin(), tmpA.end(), outBuf[(size_t) out].begin());
        }
        head = (head + 1) % P;
        return finished;
    }

    void convolve (const Filter& f, int out, std::vector<float>& ar, std::vector<float>& ai)
    {
        std::fill (ar.begin(), ar.end(), 0.0f); std::fill (ai.begin(), ai.end(), 0.0f);
        const int direct = out == 0 ? 0 : 1, crossIdx = out == 0 ? 2 : 3;
        const int inDirect = out, inCross = 1 - out;
        float* __restrict accR = ar.data(); float* __restrict accI = ai.data();
        for (int p = 0; p < P; ++p)
        {
            const int slot = (head - p + P) % P;
            for (int pass = 0; pass < (f.cross ? 2 : 1); ++pass)
            {
                const int which = pass == 0 ? direct : crossIdx, in = pass == 0 ? inDirect : inCross;
                const float* __restrict hr = f.re[(size_t) which].data() + p * K;
                const float* __restrict hi = f.im[(size_t) which].data() + p * K;
                const float* __restrict xr = xre[(size_t) in].data() + slot * K;
                const float* __restrict xi = xim[(size_t) in].data() + slot * K;
                for (int k = 0; k < K; ++k)
                {
                    accR[k] += xr[k] * hr[k] - xi[k] * hi[k];
                    accI[k] += xr[k] * hi[k] + xi[k] * hr[k];
                }
            }
        }
    }

    void inverse (const std::vector<float>& ar, const std::vector<float>& ai, std::vector<float>& dst)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int k = 0; k < K; ++k) { work[(size_t) (2 * k)] = ar[(size_t) k]; work[(size_t) (2 * k + 1)] = ai[(size_t) k]; }
        fft->performRealOnlyInverseTransform (work.data());
        std::copy (work.begin() + B, work.begin() + 2 * B, dst.begin());   // overlap-save: la segunda mitad es valida
    }

    int B = 512, P = 1, K = 513, inPos = 0, head = 0;
    std::unique_ptr<RealFFT> fft;
    std::array<std::vector<float>, 2> xre, xim, inBuf, outBuf;
    std::vector<float> work, accRe, accIm, accRe2, accIm2, tmpA, tmpB;
};

class LinearPhaseEngine : private juce::Thread
{
public:
    LinearPhaseEngine() : juce::Thread ("Gaux fase lineal") {}
    ~LinearPhaseEngine() override { stopThread (4000); }

    // Fuera del hilo de audio: prepara todo y carga el primer filtro (asi suena bien desde la primera muestra)
    void prepare (const LinearSpec& spec, int maxBlock)
    {
        stopThread (4000);
        current = spec;
        maxBlockSize = maxBlock;
        conv.prepare (spec.length, spec.natural ? naturalSizes (spec.fs, spec.osFactor).partition : linearPartitionSize (spec.length));
        designFft = std::make_unique<RealFFT> (conv.fftOrder());
        for (auto& s : slots) { conv.allocate (s.filter); s.state.store (0); s.serial = 0; }
        design (spec, slots[0].filter);
        slots[0].state.store (3);
        active = 0; next = -1; fade = 0.0f;
        fadeStep = 1.0f / (float) juce::jmax (2048, 2 * conv.getPartition());
        {
            const juce::SpinLock::ScopedLockType sl (requestLock);
            request = spec; requestSerial = 1;
        }
        lastRequested = spec;   // el primer bloque no pide un rediseno innecesario
        doneSerial = 1;
        startThread (juce::Thread::Priority::low);
    }

    // latencia total: mitad del filtro (fase lineal) + una particion de la convolucion
    int getLatency() const { return (current.natural ? naturalSizes (current.fs, current.osFactor).delay : current.length / 2) + conv.getPartition(); }
    bool isNatural() const { return current.natural; }
    int getLength() const { return current.length; }
    double getSampleRate() const { return current.fs; }
    int getMaxBlock() const { return maxBlockSize; }
    int resolution = 0;   // 1 = Lineal, 2 = Lineal HQ, 3/4/5 = Natural con oversampling 1x/2x/4x

    // Hilo de audio: pedir una curva nueva (no bloquea: si el hilo de diseno esta leyendo, se pide en el bloque siguiente)
    void setSpec (const LinearSpec& spec)
    {
        if (spec.sameAs (lastRequested)) return;
        const juce::SpinLock::ScopedTryLockType sl (requestLock);
        if (! sl.isLocked()) return;
        request = spec; ++requestSerial;
        lastRequested = spec;
        notify();
    }

    // Hilo de audio
    void process (float* l, float* r, int n)
    {
        if (next < 0)
        {
            // filtro nuevo listo? (el mas reciente; los anteriores se liberan)
            int best = -1;
            for (int i = 0; i < kSlots; ++i)
                if (slots[(size_t) i].state.load (std::memory_order_acquire) == 1 && (best < 0 || slots[(size_t) i].serial > slots[(size_t) best].serial)) best = i;
            if (best >= 0)
            {
                for (int i = 0; i < kSlots; ++i)
                    if (i != best && slots[(size_t) i].state.load (std::memory_order_acquire) == 1) slots[(size_t) i].state.store (0, std::memory_order_release);
                slots[(size_t) best].state.store (3, std::memory_order_release);
                next = best; fade = 0.0f;
            }
        }
        const auto* nf = next >= 0 ? &slots[(size_t) next].filter : nullptr;
        if (conv.process (l, r, n, slots[(size_t) active].filter, nf, fade, fadeStep) && next >= 0)
        {
            slots[(size_t) active].state.store (0, std::memory_order_release);
            active = next; next = -1; fade = 0.0f;
        }
    }

    void reset()
    {
        conv.reset();
        if (next >= 0) { slots[(size_t) active].state.store (0, std::memory_order_release); active = next; next = -1; fade = 0.0f; }
    }

private:
    void design (const LinearSpec& sp, PartitionedMatrixConvolver::Filter& f)
    {
        if (sp.natural)
        {
            const auto z = naturalSizes (sp.fs, sp.osFactor);
            if (osRespFactor != sp.osFactor || osRespFs != sp.fs) { osResp = measureOversampling (sp.osFactor, z.fft); osRespFactor = sp.osFactor; osRespFs = sp.fs; }
            NaturalSpec ns;
            ns.bands = sp.bands; ns.zoneOn = sp.zoneOn; ns.zoneLo = sp.zoneLo; ns.zoneHi = sp.zoneHi;
            ns.fs = sp.fs; ns.osFactor = sp.osFactor; ns.mono = sp.mono;
            std::array<std::vector<float>, 4> irs;
            const bool cross = designNaturalPhase (ns, osResp, z, irs);
            const float* p[4] = { irs[0].data(), irs[1].data(), irs[2].data(), irs[3].data() };
            conv.makeFilter (p, sp.length, cross, f, *designFft);
            return;
        }
        juce::AudioBuffer<float> a, b; bool needCross = false;
        designLinearPhase (sp, a, b, needCross);
        const float* irs[4] = { a.getReadPointer (0), a.getReadPointer (1), b.getReadPointer (0), b.getReadPointer (1) };
        conv.makeFilter (irs, sp.length, needCross, f, *designFft);
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            wait (40);
            if (threadShouldExit()) break;
            LinearSpec sp; int serial = 0;
            {
                const juce::SpinLock::ScopedLockType sl (requestLock);
                serial = requestSerial; sp = request;
            }
            if (serial == doneSerial) continue;
            Slot* freeSlot = nullptr;
            for (auto& s : slots) if (s.state.load (std::memory_order_acquire) == 0) { freeSlot = &s; break; }
            if (freeSlot == nullptr) continue;
            freeSlot->state.store (2);
            design (sp, freeSlot->filter);
            freeSlot->serial = serial;
            freeSlot->state.store (1, std::memory_order_release);
            doneSerial = serial;
        }
    }

    static constexpr int kSlots = 4;
    struct Slot
    {
        PartitionedMatrixConvolver::Filter filter;
        int serial = 0;
        std::atomic<int> state { 0 };   // 0 libre, 2 disenando, 1 listo, 3 en uso por el audio
    };
    std::array<Slot, kSlots> slots;
    PartitionedMatrixConvolver conv;
    std::unique_ptr<RealFFT> designFft;
    int active = 0, next = -1;
    float fade = 0.0f, fadeStep = 1.0f / 2048.0f;
    LinearSpec current, request, lastRequested;
    std::vector<Cx> osResp;   // (fase natural) respuesta medida del oversampling
    int osRespFactor = 0; double osRespFs = 0;
    juce::SpinLock requestLock;
    int requestSerial = 0, doneSerial = 0, maxBlockSize = 512;
};
} // namespace gaux
