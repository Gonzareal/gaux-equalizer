// Gaux Equalizer - Espectro promedio de largo plazo (para EQ MATCH)
// Promedia la potencia de muchos bloques (ventana de Hann, 50 % de solape) y la entrega en dB, suavizada por
// fracciones de octava, en las frecuencias que se pidan. No corre en el hilo de audio.
#pragma once

#include "FastFFT.h"
#include <vector>
#include <cmath>
#include <memory>
#include <algorithm>

namespace gaux
{
class LongTermSpectrum
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        order = fs > 60000.0 ? 14 : 13;   // ~5.9 Hz por frecuencia a 48 kHz (graves bien resueltos)
        N = 1 << order; K = N / 2 + 1;
        fft = std::make_unique<RealFFT> (order);
        win.resize ((size_t) N);
        for (int i = 0; i < N; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / N));
        buf.assign ((size_t) N, 0.0f); work.assign ((size_t) (2 * N), 0.0f);
        clear();
    }

    void clear() { acc.assign ((size_t) K, 0.0); frames = 0; fill = 0; samples = 0; }

    void add (const float* x, int n)
    {
        if (fft == nullptr) return;
        for (int i = 0; i < n; ++i)
        {
            buf[(size_t) fill++] = x[i];
            ++samples;
            if (fill == N)
            {
                for (int k = 0; k < N; ++k) work[(size_t) k] = buf[(size_t) k] * win[(size_t) k];
                fft->performRealOnlyForwardTransform (work.data(), true);
                double e = 0;
                for (int k = 0; k < K; ++k)
                {
                    const double p = (double) work[(size_t) (2 * k)] * work[(size_t) (2 * k)] + (double) work[(size_t) (2 * k + 1)] * work[(size_t) (2 * k + 1)];
                    acc[(size_t) k] += p; e += p;
                }
                if (e > 0) ++frames;   // (los bloques en silencio no cuentan)
                // solape del 50 %
                std::copy (buf.begin() + N / 2, buf.end(), buf.begin());
                fill = N / 2;
            }
        }
    }

    double seconds() const { return samples / std::max (1.0, fs); }
    int blocks() const { return frames; }
    bool ready() const { return frames >= 6; }
    double sampleRate() const { return fs; }

    // dB (relativos) en cada frecuencia, promediando la potencia en +-smoothOct/2 octavas
    std::vector<double> toDb (const std::vector<double>& freqs, double smoothOct) const
    {
        std::vector<double> out (freqs.size(), -200.0);
        if (frames == 0) return out;
        // suma acumulada para promediar rangos rapido
        std::vector<double> pre ((size_t) K + 1, 0.0);
        for (int k = 0; k < K; ++k) pre[(size_t) k + 1] = pre[(size_t) k] + acc[(size_t) k];
        for (size_t i = 0; i < freqs.size(); ++i)
        {
            const double f0 = freqs[i] * std::pow (2.0, -smoothOct * 0.5), f1 = freqs[i] * std::pow (2.0, smoothOct * 0.5);
            int k0 = (int) std::floor (f0 * N / fs), k1 = (int) std::ceil (f1 * N / fs);
            k0 = std::clamp (k0, 1, K - 1); k1 = std::clamp (k1, k0, K - 1);
            const double p = (pre[(size_t) k1 + 1] - pre[(size_t) k0]) / (k1 - k0 + 1) / frames;
            out[i] = 10.0 * std::log10 (p + 1.0e-30);
        }
        return out;
    }

private:
    double fs = 48000.0;
    int order = 13, N = 8192, K = 4097, fill = 0, frames = 0;
    long long samples = 0;
    std::unique_ptr<RealFFT> fft;
    std::vector<float> win, buf, work;
    std::vector<double> acc;
};

// Curva que hay que aplicar para que 'src' suene con el balance de 'ref' (dB). El volumen general no cuenta
// (se iguala el promedio entre 60 Hz y 12 kHz); se limita a +-maxDb y se escala por 'amount' (0..1).
// weightOut: importancia de cada punto (poco donde la pista casi no tiene contenido o fuera de 40 Hz - 16 kHz).
// Promedio movil en frecuencia logaritmica (+-oct/2) de una curva en dB
inline std::vector<double> smoothCurveOct (const std::vector<double>& f, const std::vector<double>& v, double oct)
{
    std::vector<double> o (v.size());
    for (size_t i = 0; i < v.size(); ++i)
    {
        double s = 0, w = 0;
        for (size_t j = 0; j < v.size(); ++j)
        {
            const double d = std::abs (std::log2 (f[j] / f[i]));
            if (d > oct * 0.5) continue;
            const double wt = 1.0 - d / (oct * 0.5 + 1.0e-9) * 0.5;   // triangular suave
            s += wt * v[j]; w += wt;
        }
        o[i] = w > 0 ? s / w : v[i];
    }
    return o;
}

// detailOct: detalle del match (1/2, 1/3, 1/6 de octava). La diferencia se promedia con ese detalle: asi no se
// persiguen notas sueltas (el bajo de un tema en otra tonalidad), solo el balance.
inline std::vector<double> matchTarget (const std::vector<double>& f, const std::vector<double>& srcDb, const std::vector<double>& refDb,
                                        double amount, double maxDb, std::vector<double>& weightOut, double detailOct = 1.0 / 3.0)
{
    const size_t G = f.size();
    std::vector<double> d (G);
    for (size_t i = 0; i < G; ++i) d[i] = refDb[i] - srcDb[i];
    d = smoothCurveOct (f, d, detailOct);
    double s = 0, w = 0;
    for (size_t i = 0; i < G; ++i)
        if (f[i] >= 60.0 && f[i] <= 12000.0) { s += d[i]; w += 1.0; }
    const double off = w > 0 ? s / w : 0.0;
    double srcMax = -300; for (double v : srcDb) srcMax = std::max (srcMax, v);
    double refMax = -300; for (double v : refDb) refMax = std::max (refMax, v);
    weightOut.assign (G, 1.0);
    for (size_t i = 0; i < G; ++i)
    {
        d[i] = std::clamp (d[i] - off, -maxDb, maxDb) * amount;
        double wt = 1.0;
        if (f[i] < 40.0) wt = 0.15; else if (f[i] > 16000.0) wt = 0.3;
        // casi sin contenido (60 dB debajo de lo mas fuerte): no se fuerza la curva ahi
        const double lowSrc = std::clamp ((srcDb[i] - (srcMax - 60.0)) / 20.0, 0.1, 1.0);
        const double lowRef = std::clamp ((refDb[i] - (refMax - 60.0)) / 20.0, 0.1, 1.0);
        weightOut[i] = wt * std::min (lowSrc, lowRef);
    }
    return d;
}
// La campana mas angosta que tiene sentido para una curva medida con ese suavizado (fracciones de octava):
// un ancho de ~1.5 veces la resolucion (con 1/3 de octava -> Q ~3). Ademas es lo musical para un match.
inline double matchMaxQ (double smoothOct)
{
    const double bw = std::max (0.1, 1.5 * smoothOct);
    return std::clamp (1.0 / (2.0 * std::sinh (std::log (2.0) / 2.0 * bw)), 0.7, 8.0);
}
} // namespace gaux
