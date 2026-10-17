// Gaux Equalizer - FFT real rapida (PFFFT, con SIMD: SSE en PC, NEON en Apple Silicon)
// Misma interfaz y mismo formato que juce::dsp::FFT (performRealOnlyForwardTransform / Inverse), para usarla
// donde la FFT corre en el hilo de audio (convolucion de fase lineal, dinamica espectral).
// La FFT generica de JUCE en Windows/Linux es varias veces mas lenta.
#pragma once

#ifndef PFFFT_STATIC_DEFINE
 #define PFFFT_STATIC_DEFINE 1
#endif
#include "../ThirdParty/pffft/pffft.h"
#include <cstring>

namespace gaux
{
class RealFFT
{
public:
    explicit RealFFT (int order) : N (1 << order)
    {
        setup = pffft_new_setup (N, PFFFT_REAL);
        in = (float*) pffft_aligned_malloc ((size_t) N * sizeof (float));
        out = (float*) pffft_aligned_malloc ((size_t) N * sizeof (float));
        work = (float*) pffft_aligned_malloc ((size_t) N * sizeof (float));
    }
    ~RealFFT()
    {
        if (setup != nullptr) pffft_destroy_setup (setup);
        pffft_aligned_free (in); pffft_aligned_free (out); pffft_aligned_free (work);
    }
    RealFFT (const RealFFT&) = delete;
    RealFFT& operator= (const RealFFT&) = delete;

    int getSize() const { return N; }

    // d: 2N floats. Entrada: N muestras reales. Salida: bins 0..N/2 como (re, im) intercalados (como JUCE)
    void performRealOnlyForwardTransform (float* d, bool = true) const
    {
        std::memcpy (in, d, (size_t) N * sizeof (float));
        pffft_transform_ordered (setup, in, out, work, PFFFT_FORWARD);
        // formato ordenado de PFFFT: [Re0, ReN/2, Re1, Im1, Re2, Im2, ...]
        const float nyq = out[1];
        d[0] = out[0]; d[1] = 0.0f;
        std::memcpy (d + 2, out + 2, (size_t) (N - 2) * sizeof (float));
        d[N] = nyq; d[N + 1] = 0.0f;
    }

    // d: bins 0..N/2 intercalados. Salida: N muestras reales (escaladas por 1/N, como JUCE)
    void performRealOnlyInverseTransform (float* d) const
    {
        in[0] = d[0]; in[1] = d[N];
        std::memcpy (in + 2, d + 2, (size_t) (N - 2) * sizeof (float));
        pffft_transform_ordered (setup, in, out, work, PFFFT_BACKWARD);
        const float sc = 1.0f / (float) N;
        for (int i = 0; i < N; ++i) d[i] = out[i] * sc;
    }

private:
    int N;
    PFFFT_Setup* setup = nullptr;
    float* in = nullptr; float* out = nullptr; float* work = nullptr;   // (mutables: cada objeto es de un solo hilo)
};
} // namespace gaux
