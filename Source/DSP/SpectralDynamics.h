// Gaux Equalizer - Dinamica espectral por banda
//
// Una banda en modo SPECTRAL no mueve su ganancia entera: cada frecuencia dentro de su zona tiene su propio
// detector y solo se procesa la que pasa el umbral (las demas quedan intactas). Ideal para resonancias,
// silbidos, "honk" de voces, notas del bajo que sobresalen, etc.
//
//  - Analisis/sintesis por bloques solapados (STFT) con ventana raiz de Hann: reconstruccion perfecta cuando
//    no hay accion (la senal sale identica, solo retrasada la latencia).
//  - Nivel de cada frecuencia = amplitud de un tono puro (calibrado): un tono a -12 dBFS se lee -12 dB.
//    El threshold es el nivel que debe alcanzar cada frecuencia por si sola.
//  - AUTO (propio de Gaux): el threshold de cada frecuencia es el promedio de sus vecinas (1/3 de octava) +3 dB:
//    doma solo lo que sobresale de su vecindario (supresor de resonancias).
//  - La accion se reparte con la forma de la banda (una campana actua mas en su centro) y se suaviza un poco
//    entre frecuencias vecinas (sin "huecos" bruscos ni sonido metalico).
//  - Estereo, L, R, Mid y Side, y sidechain externo por banda.
#pragma once

#include <juce_dsp/juce_dsp.h>
#include "EqBand.h"
#include "FastFFT.h"
#include <atomic>

namespace gaux
{
constexpr int kSpectralMaxBands = 24;

// log2 / exp2 rapidos (error < 0.01 dB en el uso de abajo): el analisis los usa miles de veces por cuadro
inline float fastLog2 (float x)
{
    union { float f; std::uint32_t i; } v { x };
    const float e = (float) ((int) ((v.i >> 23) & 255) - 127);
    v.i = (v.i & 0x007FFFFFu) | 0x3F800000u;           // mantisa en [1, 2)
    const float t = v.f - 1.0f;
    // log2 (1 + t), ajuste por minimos cuadrados (error < 3e-5, es decir < 0.0001 dB)
    return e + t * (1.44182580f + t * (-0.70868210f + t * (0.41542195f + t * (-0.19442267f + t * 0.04588553f))));
}
inline float fastExp2 (float x)
{
    x = juce::jlimit (-126.0f, 126.0f, x);
    const float fi = std::floor (x), f = x - fi;
    // 2^f en [0,1), ajuste por minimos cuadrados (error relativo < 3e-7)
    const float p = 0.99999977f + f * (0.69315678f + f * (0.24013169f + f * (0.05587656f + f * (0.00894058f + f * 0.00189438f))));
    union { float f; std::uint32_t i; } v { p };
    v.i += ((std::uint32_t) (int) fi) << 23;
    return v.f;
}
inline float powToDb (float p) { return 3.01029996f * fastLog2 (p); }     // 10 log10 (p)
inline float dbToGain (float db) { return fastExp2 (db * 0.16609640f); } // 10^(db/20)
constexpr int kSpectralExportPoints = 96;   // curva de accion para la pantalla (frecuencias en escala log)

struct SpectralBand
{
    bool on = false;
    int domain = 0;            // 0 estereo (detector enlazado), 1 L, 2 R, 3 Mid, 4 Side
    double thresholdDb = -24.0, rangeDb = -6.0, attackMs = 10.0, releaseMs = 120.0;
    bool autoThr = false, extSc = false;
    BandSettings shape;        // forma de la banda (para repartir la accion)
};

class SpectralDynamics
{
public:
    static int fftOrderFor (double fs) { return fs <= 50000.0 ? 11 : (fs <= 100000.0 ? 12 : 13); }

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        order = fftOrderFor (fs);
        N = 1 << order; H = N / 4; K = N / 2 + 1;
        fft = std::make_unique<RealFFT> (order);
        win.resize ((size_t) N);
        for (int i = 0; i < N; ++i) win[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * kPi * i / N));   // raiz de Hann periodica
        // sqrt(Hann) x sqrt(Hann) solapada al 75 % suma 2: se compensa en la sintesis
        olaGain = 0.5f;
        for (auto& c : inRing) c.assign ((size_t) N, 0.0f);
        scRing.assign ((size_t) N, 0.0f);
        for (auto& c : outAcc) c.assign ((size_t) N, 0.0f);
        for (auto& c : spec) c.assign ((size_t) (2 * N), 0.0f);
        scSpec.assign ((size_t) (2 * N), 0.0f);
        for (auto& p : pow) p.assign ((size_t) K, 0.0f);
        for (auto& g : gainDb) g.assign ((size_t) K, 0.0f);
        tmp.assign ((size_t) K, 0.0f); tmp2.assign ((size_t) K, 0.0f); prefix.assign ((size_t) K + 1, 0.0);
        for (auto& b : st) { b.level.assign ((size_t) K, -140.0f); b.weight.assign ((size_t) K, 0.0f); b.gain.assign ((size_t) K, 0.0f); b.shapeValid = false; }
        // limites de vecindario (1/3 de octava) por frecuencia
        nbLo.resize ((size_t) K); nbHi.resize ((size_t) K);
        for (int k = 0; k < K; ++k)
        {
            nbLo[(size_t) k] = juce::jlimit (1, K - 1, (int) std::floor (k / 1.12) - 1);
            nbHi[(size_t) k] = juce::jlimit (1, K - 1, (int) std::ceil (k * 1.12) + 1);
        }
        // puntos de la curva exportada
        for (int i = 0; i < kSpectralExportPoints; ++i)
        {
            const double f0 = 20.0 * std::pow (1000.0, i / (double) kSpectralExportPoints), f1 = 20.0 * std::pow (1000.0, (i + 1) / (double) kSpectralExportPoints);
            exLo[(size_t) i] = juce::jlimit (1, K - 1, (int) std::floor (f0 * N / fs));
            exHi[(size_t) i] = juce::jlimit (exLo[(size_t) i], K - 1, (int) std::ceil (f1 * N / fs));
        }
        calibrate();
        reset();
    }

    // latencia: un cuadro completo (cada muestra sale cuando ya se sumaron todos los cuadros que la cubren)
    int getLatency() const { return N; }

    void reset()
    {
        for (auto& c : inRing) std::fill (c.begin(), c.end(), 0.0f);
        std::fill (scRing.begin(), scRing.end(), 0.0f);
        for (auto& c : outAcc) std::fill (c.begin(), c.end(), 0.0f);
        for (auto& b : st) { std::fill (b.level.begin(), b.level.end(), -140.0f); std::fill (b.gain.begin(), b.gain.end(), 0.0f); b.maxAction = 0; b.maxLevel = -140; }
        pos = 0; hopCount = 0;
        for (auto& e : exportDb) for (auto& v : e) v.store (0.0f);
    }

    // Hilo de audio: ajustes de cada banda (se aplican en el proximo cuadro)
    void setBand (int b, const SpectralBand& s) { bands[(size_t) b] = s; }

    bool anyActive() const { for (auto& b : bands) if (b.on) return true; return false; }

    // Hilo de audio: l y r (r puede ser nullptr en pistas mono), sc = sidechain mono (o nullptr)
    void process (float* l, float* r, const float* sc, int n)
    {
        const bool stereo = r != nullptr;
        for (int i = 0; i < n; ++i)
        {
            inRing[0][(size_t) pos] = l[i];
            if (stereo) inRing[1][(size_t) pos] = r[i];
            scRing[(size_t) pos] = sc != nullptr ? sc[i] : 0.0f;
            // salida: la muestra de hace N (ya tiene sumados todos los cuadros que la cubren)
            l[i] = outAcc[0][(size_t) pos];
            outAcc[0][(size_t) pos] = 0.0f;
            if (stereo) { r[i] = outAcc[1][(size_t) pos]; outAcc[1][(size_t) pos] = 0.0f; }
            pos = (pos + 1) % N;
            if (++hopCount == H) { hopCount = 0; frame (stereo, sc != nullptr); }
        }
    }

    // Para la pantalla: accion actual (dB) de cada banda en kSpectralExportPoints frecuencias de 20 Hz a 20 kHz
    std::array<std::array<std::atomic<float>, kSpectralExportPoints>, kSpectralMaxBands> exportDb;
    std::array<std::atomic<float>, kSpectralMaxBands> exportMaxAction {}, exportMaxLevel {};
    static double exportFreq (int i) { return 20.0 * std::pow (1000.0, (i + 0.5) / (double) kSpectralExportPoints); }

    // Para pruebas
    double levelCalibration() const { return cal; }

private:
    struct BandState
    {
        std::vector<float> level, weight, gain;
        int kLo = 1, kHi = 0, gLo = 0, gHi = -1;
        BandSettings shapeFor;
        double rangeFor = 0;
        bool shapeValid = false;
        float maxAction = 0, maxLevel = -140;
    };

    // Un tono puro de amplitud 1 en el centro de una frecuencia debe leerse 0 dB
    void calibrate()
    {
        std::vector<float> d ((size_t) (2 * N), 0.0f);
        const int k0 = N / 8;
        for (int i = 0; i < N; ++i) d[(size_t) i] = (float) std::sin (2.0 * kPi * k0 * i / N) * win[(size_t) i];
        fft->performRealOnlyForwardTransform (d.data(), true);
        double p = 0;
        for (int k = k0 - 1; k <= k0 + 1; ++k) p += (double) d[(size_t) (2 * k)] * d[(size_t) (2 * k)] + (double) d[(size_t) (2 * k + 1)] * d[(size_t) (2 * k + 1)];
        cal = 1.0 / p;
    }

    void updateWeights (int b)
    {
        auto& s = st[(size_t) b];
        const auto& bd = bands[(size_t) b];
        BandSettings sh = bd.shape; sh.on = true; sh.gainDb = bd.rangeDb;
        if (s.shapeValid && sameCurve (sh, s.shapeFor) && s.rangeFor == bd.rangeDb) return;
        s.shapeFor = sh; s.rangeFor = bd.rangeDb; s.shapeValid = true;
        const double r = std::abs (bd.rangeDb) > 1.0e-6 ? bd.rangeDb : -1.0;
        if (std::abs (bd.rangeDb) <= 1.0e-6) sh.gainDb = -1.0;
        for (int k = 0; k < K; ++k)
        {
            const double f = std::max (1.0, k * fs / N);
            const double db = 20.0 * std::log10 (std::max (1.0e-6, analogMagnitudeOfBand (sh, f)));
            float w = (float) std::clamp (db / r, -1.0, 1.0);   // 1 = accion completa (centro de una campana)
            if (std::abs (w) < 0.02f) w = 0.0f;                     // fuera de la zona de la banda: no se analiza
            s.weight[(size_t) k] = w;
        }
        // rango de frecuencias donde la banda actua (solo ahi se trabaja)
        s.kLo = K; s.kHi = 0;
        for (int k = 1; k < K - 1; ++k) if (s.weight[(size_t) k] != 0.0f) { s.kLo = std::min (s.kLo, k); s.kHi = std::max (s.kHi, k); }
    }

    void frame (bool stereo, bool haveSc)
    {
        const int chans = stereo ? 2 : 1;
        // 1) analisis: ultimas N muestras con ventana
        for (int c = 0; c < chans; ++c)
        {
            auto& d = spec[(size_t) c];
            for (int i = 0; i < N; ++i) d[(size_t) i] = inRing[(size_t) c][(size_t) ((pos + i) % N)] * win[(size_t) i];
            std::fill (d.begin() + N, d.end(), 0.0f);
            fft->performRealOnlyForwardTransform (d.data(), true);
        }
        bool needSc = false, needMS = false;
        for (auto& b : bands) if (b.on) { needSc = needSc || (b.extSc && haveSc); needMS = needMS || b.domain >= 3; }
        if (needSc)
        {
            for (int i = 0; i < N; ++i) scSpec[(size_t) i] = scRing[(size_t) ((pos + i) % N)] * win[(size_t) i];
            std::fill (scSpec.begin() + N, scSpec.end(), 0.0f);
            fft->performRealOnlyForwardTransform (scSpec.data(), true);
        }
        // potencias (solo en la zona que usan las bandas): 0 L, 1 R, 2 max(L,R), 3 Mid, 4 Side, 5 sidechain
        auto p2 = [] (const std::vector<float>& d, int k) { return d[(size_t) (2 * k)] * d[(size_t) (2 * k)] + d[(size_t) (2 * k + 1)] * d[(size_t) (2 * k + 1)]; };
        int zLo = K, zHi = -1;
        for (int b = 0; b < kSpectralMaxBands; ++b)
            if (bands[(size_t) b].on) { updateWeights (b); zLo = std::min (zLo, st[(size_t) b].kLo - 1); zHi = std::max (zHi, st[(size_t) b].kHi + 1); }
        zLo = std::max (0, zLo); zHi = std::min (K - 1, zHi);
        for (int k = zLo; k <= zHi; ++k)
        {
            const float pl = p2 (spec[0], k), pr = stereo ? p2 (spec[1], k) : pl;
            pow[0][(size_t) k] = pl; pow[1][(size_t) k] = pr; pow[2][(size_t) k] = std::max (pl, pr);
            if (needMS && stereo)
            {
                const float mr = 0.5f * (spec[0][(size_t) (2 * k)] + spec[1][(size_t) (2 * k)]), mi = 0.5f * (spec[0][(size_t) (2 * k + 1)] + spec[1][(size_t) (2 * k + 1)]);
                const float sr = 0.5f * (spec[0][(size_t) (2 * k)] - spec[1][(size_t) (2 * k)]), si = 0.5f * (spec[0][(size_t) (2 * k + 1)] - spec[1][(size_t) (2 * k + 1)]);
                pow[3][(size_t) k] = mr * mr + mi * mi; pow[4][(size_t) k] = sr * sr + si * si;
            }
            else if (needMS) { pow[3][(size_t) k] = pl; pow[4][(size_t) k] = 0.0f; }
            if (needSc) pow[5][(size_t) k] = p2 (scSpec, k);
        }

        // 2) cada banda: nivel por frecuencia -> accion
        for (auto& g : gainDb) std::fill (g.begin(), g.end(), 0.0f);
        gLoAll = K; gHiAll = -1;
        const double hopSec = H / fs;
        for (int b = 0; b < kSpectralMaxBands; ++b)
        {
            const auto& bd = bands[(size_t) b];
            auto& s = st[(size_t) b];
            if (! bd.on)
            {
                if (s.maxAction != 0.0f || s.maxLevel > -139.0f)
                {
                    std::fill (s.level.begin(), s.level.end(), -140.0f); std::fill (s.gain.begin(), s.gain.end(), 0.0f);
                    s.maxAction = 0; s.maxLevel = -140;
                    for (auto& v : exportDb[(size_t) b]) v.store (0.0f);
                    exportMaxAction[(size_t) b].store (0.0f); exportMaxLevel[(size_t) b].store (-140.0f);
                }
                continue;
            }
            const int src = bd.extSc && haveSc ? 5 : (bd.domain == 0 ? 2 : bd.domain == 1 ? 0 : bd.domain == 2 ? 1 : bd.domain);
            const auto& P = pow[(size_t) src];
            const float aA = (float) std::exp (-hopSec / std::max (1.0e-4, bd.attackMs * 0.001));
            const float aR = (float) std::exp (-hopSec / std::max (1.0e-3, bd.releaseMs * 0.001));
            float maxLv = -140.0f;
            // nivel calibrado (3 frecuencias: un tono puro se lee con su amplitud), con balistica
            const int kLo = s.kLo, kHi = s.kHi;
            for (int k = kLo; k <= kHi; ++k)
            {
                if (s.weight[(size_t) k] == 0.0f) { s.level[(size_t) k] = -140.0f; continue; }
                const float e = (P[(size_t) (k - 1)] + P[(size_t) k] + P[(size_t) (k + 1)]) * (float) cal;
                const float lv = powToDb (e + 1.0e-14f);
                float& L = s.level[(size_t) k];
                L = lv > L ? lv + aA * (L - lv) : lv + aR * (L - lv);
                if (std::abs (s.weight[(size_t) k]) > 0.5f) maxLv = std::max (maxLv, L);
            }
            // threshold: fijo, o el promedio de las vecinas (1/3 de octava) + 3 dB
            if (bd.autoThr)
            {
                prefix[0] = 0.0;
                for (int k = 0; k < K; ++k) prefix[(size_t) k + 1] = prefix[(size_t) k] + std::max (-120.0f, s.level[(size_t) k]);
                for (int k = kLo; k <= kHi; ++k)
                {
                    const int lo = nbLo[(size_t) k], hi = nbHi[(size_t) k];
                    tmp[(size_t) k] = (float) ((prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (hi - lo + 1)) + 3.0f;
                }
            }
            float maxAct = 0.0f;
            const int gLo = std::max (0, kLo - 4), gHi = std::min (K - 1, kHi + 4);
            for (int k = gLo; k <= gHi; ++k) tmp2[(size_t) k] = 0.0f;
            for (int k = kLo; k <= kHi; ++k)
            {
                const float w = s.weight[(size_t) k];
                if (w == 0.0f) continue;
                const float thr = bd.autoThr ? tmp[(size_t) k] : (float) bd.thresholdDb;
                const float amount = juce::jlimit (0.0f, 1.0f, (s.level[(size_t) k] - thr) / 12.0f);   // rodilla de 12 dB (como la dinamica normal)
                tmp2[(size_t) k] = (float) bd.rangeDb * amount * w;
            }
            // la accion mas fuerte entre vecinas (un tono ocupa ~3 frecuencias) y luego suavizado 1-2-3-2-1:
            // el pico recibe la accion completa y no quedan "huecos" bruscos
            for (int k = gLo; k <= gHi; ++k)
            {
                float m = tmp2[(size_t) k];
                if (k > gLo && std::abs (tmp2[(size_t) k - 1]) > std::abs (m)) m = tmp2[(size_t) k - 1];
                if (k < gHi && std::abs (tmp2[(size_t) k + 1]) > std::abs (m)) m = tmp2[(size_t) k + 1];
                tmp[(size_t) k] = m;
            }
            std::fill (s.gain.begin(), s.gain.end(), 0.0f);
            for (int k = gLo; k <= gHi; ++k)
            {
                float acc = 0, wsum = 0;
                for (int j = -2; j <= 2; ++j)
                {
                    const int kk = k + j; if (kk < gLo || kk > gHi) continue;
                    const float wj = (float) (3 - std::abs (j));
                    acc += wj * tmp[(size_t) kk]; wsum += wj;
                }
                const float g = acc / wsum;
                s.gain[(size_t) k] = g;
                if (std::abs (g) > std::abs (maxAct)) maxAct = g;
            }
            s.maxAction = maxAct; s.maxLevel = maxLv;
            s.gLo = gLo; s.gHi = gHi;
            // suma por dominio: 0 estereo, 1 L, 2 R, 3 M, 4 S
            auto& G = gainDb[(size_t) bd.domain];
            for (int k = gLo; k <= gHi; ++k) G[(size_t) k] += s.gain[(size_t) k];
            gLoAll = std::min (gLoAll, gLo); gHiAll = std::max (gHiAll, gHi);
            // curva para la pantalla
            for (int i = 0; i < kSpectralExportPoints; ++i)
            {
                float m = 0.0f;
                if (exHi[(size_t) i] >= gLo && exLo[(size_t) i] <= gHi)
                    for (int k = std::max (gLo, exLo[(size_t) i]); k <= std::min (gHi, exHi[(size_t) i]); ++k) if (std::abs (s.gain[(size_t) k]) > std::abs (m)) m = s.gain[(size_t) k];
                exportDb[(size_t) b][(size_t) i].store (m, std::memory_order_relaxed);
            }
            exportMaxAction[(size_t) b].store (maxAct, std::memory_order_relaxed);
            exportMaxLevel[(size_t) b].store (maxLv, std::memory_order_relaxed);
        }

        // 3) aplicar: estereo y L/R directo; Mid/Side en su dominio
        auto lin = [] (float db) { return db == 0.0f ? 1.0f : dbToGain (db); };
        bool anyMS = false;
        for (int k = std::max (0, gLoAll); k <= gHiAll; ++k) if (gainDb[3][(size_t) k] != 0.0f || gainDb[4][(size_t) k] != 0.0f) { anyMS = true; break; }
        for (int k = std::max (0, gLoAll); k <= gHiAll; ++k)
        {
            const float gst = gainDb[0][(size_t) k];
            const float gl = lin (gst + gainDb[1][(size_t) k]);
            spec[0][(size_t) (2 * k)] *= gl; spec[0][(size_t) (2 * k + 1)] *= gl;
            if (stereo)
            {
                const float gr = lin (gst + gainDb[2][(size_t) k]);
                spec[1][(size_t) (2 * k)] *= gr; spec[1][(size_t) (2 * k + 1)] *= gr;
                if (anyMS)
                {
                    const float gm = lin (gainDb[3][(size_t) k]), gs = lin (gainDb[4][(size_t) k]);
                    for (int part = 0; part < 2; ++part)
                    {
                        float& a = spec[0][(size_t) (2 * k + part)];
                        float& c = spec[1][(size_t) (2 * k + part)];
                        const float m = 0.5f * (a + c) * gm, sd = 0.5f * (a - c) * gs;
                        a = m + sd; c = m - sd;
                    }
                }
            }
            else if (anyMS)
            {
                const float gm = lin (gainDb[3][(size_t) k]);   // pista mono: Mid = la pista
                spec[0][(size_t) (2 * k)] *= gm; spec[0][(size_t) (2 * k + 1)] *= gm;
            }
        }

        // 4) sintesis con ventana y suma solapada
        for (int c = 0; c < chans; ++c)
        {
            auto& d = spec[(size_t) c];
            fft->performRealOnlyInverseTransform (d.data());
            auto& o = outAcc[(size_t) c];
            for (int i = 0; i < N; ++i)
            {
                const int idx = (pos + i) % N;
                o[(size_t) idx] += d[(size_t) i] * win[(size_t) i] * olaGain;
            }
        }
    }

    double fs = 48000.0, cal = 1.0;
    int order = 11, N = 2048, H = 512, K = 1025, pos = 0, hopCount = 0, gLoAll = 0, gHiAll = -1;
    float olaGain = 0.5f;
    std::unique_ptr<RealFFT> fft;
    std::vector<float> win;
    std::array<std::vector<float>, 2> inRing, outAcc, spec;
    std::vector<float> scRing, scSpec, tmp, tmp2;
    std::vector<double> prefix;
    std::array<std::vector<float>, 6> pow;
    std::array<std::vector<float>, 5> gainDb;
    std::array<SpectralBand, kSpectralMaxBands> bands {};
    std::array<BandState, kSpectralMaxBands> st;
    std::vector<int> nbLo, nbHi;
    std::array<int, kSpectralExportPoints> exLo {}, exHi {};
};
} // namespace gaux
