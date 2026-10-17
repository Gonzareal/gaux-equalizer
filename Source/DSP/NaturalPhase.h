// Gaux Equalizer - FASE NATURAL
//
// Los filtros digitales de fase minima (sin latencia) se apartan del analogico cerca de los 20 kHz: la curva
// del Gaux ya esta emparejada con el analogico, pero la FASE se "comprime" (y en un corte de agudos la fase
// cambia mucho). La fase natural suena como el EQ analogico ideal: misma curva y misma fase en todo el rango.
//
// Como lo hace (distinto a la fase lineal):
//  - Todo sigue pasando por los filtros IIR del Gaux (dinamica, saturacion, sidechain, M/S siguen funcionando).
//  - Despues, un FIR corto CORRIGE la diferencia: C = H_analogico * H_digital^-1 (matriz 2x2 por frecuencia, para
//    L/R/M/S), retrasada D muestras. Esa diferencia es casi 1 en graves y medios y solo cambia en agudos, asi que
//    el FIR es corto: latencia de unos 2.7 ms (128 muestras a 48 kHz) en vez de 170 ms de la fase lineal.
//  - El oversampling (HQ) tambien se corrige: su fase se mide una vez y entra en H_digital.
// Diseno en el hilo de fondo (como la fase lineal), nunca en el de audio.
#pragma once

#include "EqBand.h"
#include "FastFFT.h"
#include <complex>
#include <vector>
#include <array>

namespace gaux
{
using Cx = std::complex<double>;

// Respuesta compleja del filtro analogico ideal de una banda (la misma curva que analogMagnitudeOfBand)
inline Cx analogResponseOfBand (const BandSettings& s, double f)
{
    if (! s.on) return 1.0;
    const double f0 = std::max (1.0, s.freq);
    const Cx sv (0.0, std::max (1.0e-6, f) / f0);
    const double g = safeGain (s.gainDb);
    switch ((BandType) s.type)
    {
        case BandType::Bell:
        {
            const double A = std::pow (10.0, g / 40.0), q = s.q;
            return (sv * sv + sv * (A / q) + 1.0) / (sv * sv + sv / (A * q) + 1.0);
        }
        case BandType::HighShelf:
        case BandType::TiltShelf:
        {
            const double A = std::pow (10.0, g / 40.0), sA = std::sqrt (A), q = std::min (s.q, kShelfMaxQ);
            Cx h = A * (A * sv * sv + (sA / q) * sv + 1.0) / (sv * sv + (sA / q) * sv + A);
            if (s.type == (int) BandType::TiltShelf) h *= std::pow (10.0, -g / 40.0);
            return h;
        }
        case BandType::LowShelf:
        {
            const double A = std::pow (10.0, g / 40.0), sA = std::sqrt (A), q = std::min (s.q, kShelfMaxQ);
            return A * (sv * sv + (sA / q) * sv + A) / (A * sv * sv + (sA / q) * sv + 1.0);
        }
        case BandType::Notch:   return (sv * sv + 1.0) / (sv * sv + sv / s.q + 1.0);
        case BandType::AllPass: return (sv * sv - sv / s.q + 1.0) / (sv * sv + sv / s.q + 1.0);
        case BandType::BandPass:
        {
            const int m = bandPassSections (s.slope);
            const double q = bandPassSectionQ (std::max (0.1, s.q), m);
            return std::pow ((sv / q) / (sv * sv + sv / q + 1.0), m);
        }
        case BandType::LowCut:
        case BandType::HighCut:
        {
            // (la pared BRICK en fase minima/natural es el Butterworth de 144 dB/oct que usan los IIR)
            const bool lowCut = s.type == (int) BandType::LowCut;
            const auto plan = butterworthPlan (cutPoles (s.slope));
            Cx h = 1.0;
            if (plan.firstOrder) h *= (lowCut ? sv : Cx (1.0)) / (sv + 1.0);
            for (int i = 0; i < plan.biquads; ++i)
            {
                double q = (plan.biquads == 1 && ! plan.firstOrder) ? s.q : plan.q[i];
                if ((plan.biquads > 1 || plan.firstOrder) && i == plan.biquads - 1) q *= s.q / 0.7071;
                h *= (lowCut ? sv * sv : Cx (1.0)) / (sv * sv + sv / q + 1.0);
            }
            return h;
        }
    }
    return 1.0;
}

inline Cx biquadResponse (const BiquadCoeffs& c, double f, double fs)
{
    const double w = 2.0 * 3.14159265358979323846 * f / fs;
    const Cx z1 = std::polar (1.0, -w), z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}

// Matriz 2x2 compleja (L/R)
struct Mat2
{
    Cx a = 1.0, b = 0.0, c = 0.0, d = 1.0;   // [[a b] [c d]]
    Mat2 operator* (const Mat2& o) const { return { a * o.a + b * o.c, a * o.b + b * o.d, c * o.a + d * o.c, c * o.b + d * o.d }; }
    Mat2 operator+ (const Mat2& o) const { return { a + o.a, b + o.b, c + o.c, d + o.d }; }
    Mat2 scaled (Cx k) const { return { a * k, b * k, c * k, d * k }; }
    static Mat2 scalar (Cx h) { return { h, 0.0, 0.0, h }; }
    // Una banda que actua en su dominio, vista en L/R
    static Mat2 ofDomain (Cx h, int stereoMode)
    {
        switch ((StereoMode) stereoMode)
        {
            case StereoMode::Left:  return { h, 0.0, 0.0, 1.0 };
            case StereoMode::Right: return { 1.0, 0.0, 0.0, h };
            case StereoMode::Mid:   return { 0.5 * (h + 1.0), 0.5 * (h - 1.0), 0.5 * (h - 1.0), 0.5 * (h + 1.0) };
            case StereoMode::Side:  return { 0.5 * (1.0 + h), 0.5 * (1.0 - h), 0.5 * (1.0 - h), 0.5 * (1.0 + h) };
            case StereoMode::Stereo: default: return scalar (h);
        }
    }
};

constexpr int kNaturalBands = 24;   // = kNumBands (comprobado en PluginProcessor.cpp)

struct NaturalSpec
{
    std::array<BandSettings, kNaturalBands> bands {};
    bool zoneOn = false;
    double zoneLo = 20.0, zoneHi = 20000.0;
    double fs = 48000.0;
    int osFactor = 1;
    bool mono = false;
};

// Latencia de la fase natural (muestras a la frecuencia del proyecto) y largo del FIR de correccion.
// Latencia total = delay + partition: 64 muestras a 44.1/48 kHz (1.3 ms); el mismo tiempo a 88.2/96/192 kHz.
// Con HQ (oversampling) hay que corregir tambien la fase de sus filtros de media banda, que es mas larga:
// 224 muestras con 2x (4.7 ms a 48 kHz), 288 con 4x (6 ms).
struct NaturalSizes { int delay = 32, length = 256, fft = 2048, partition = 32; };
inline NaturalSizes naturalSizes (double fs, int osFactor = 1)
{
    NaturalSizes z;
    int k = 1; while (k < 4 && fs > 52000.0 * k) k *= 2;
    const int d = osFactor >= 4 ? 256 : osFactor >= 2 ? 192 : 32;
    z.delay = d * k; z.length = 4 * d * k; z.fft = (osFactor > 1 ? 4096 : 2048) * k; z.partition = 32 * k;
    if (osFactor == 1) z.length = 256 * k;
    return z;
}

// Los filtros digitales de cada banda, disenados una sola vez (luego se evaluan en muchas frecuencias)
struct NaturalPlan
{
    struct B { BandSettings s; BiquadCoeffs c[kMaxSections]; int ns = 0; int dom = 0; };
    std::vector<B> bands;
    double fsUp = 48000.0;
    explicit NaturalPlan (const NaturalSpec& sp)
    {
        fsUp = sp.fs * sp.osFactor;
        for (const auto& b : sp.bands)
        {
            if (! b.on) continue;
            B x; x.s = b; x.ns = designBand (b, fsUp, x.c);
            x.dom = sp.mono ? (int) StereoMode::Stereo : b.stereo;
            bands.push_back (x);
        }
    }
};

// Respuestas digital (como suena el Gaux con IIR, a la frecuencia interna) y analogica, en L/R
inline void naturalResponses (const NaturalSpec& sp, const NaturalPlan& plan, double f, Mat2& hd, Mat2& ha)
{
    const double fsUp = plan.fsUp;
    Mat2 td, ta;
    for (const auto& b : plan.bands)
    {
        Cx d = 1.0;
        for (int k = 0; k < b.ns; ++k) d *= biquadResponse (b.c[k], f, fsUp);
        td = Mat2::ofDomain (d, b.dom) * td;
        ta = Mat2::ofDomain (analogResponseOfBand (b.s, f), b.dom) * ta;
    }
    if (sp.zoneOn)
    {
        // ZONE: 3 partes con cruces Linkwitz-Riley; solo la del medio pasa por el EQ (igual que el IIR)
        const double lo = std::clamp (sp.zoneLo, 20.0, 20000.0), hi = std::clamp (sp.zoneHi, lo * 1.05, 20000.0);
        const double hiF = std::min (hi, fsUp * 0.45);
        const Cx lpL = biquadResponse (makeLowPass (fsUp, lo, 0.7071), f, fsUp), hpL = biquadResponse (makeHighPass (fsUp, lo, 0.7071), f, fsUp);
        const Cx lpH = biquadResponse (makeLowPass (fsUp, hiF, 0.7071), f, fsUp), hpH = biquadResponse (makeHighPass (fsUp, hiF, 0.7071), f, fsUp);
        const Cx apH = biquadResponse (makeAllPass (fsUp, hiF, 0.7071), f, fsUp);
        const Cx outD = lpL * lpL * apH + hpL * hpL * hpH * hpH, midD = hpL * hpL * lpH * lpH;
        td = Mat2::scalar (outD) + td.scaled (midD);
        auto an = [&] (double fc, int kind)
        {
            const Cx s (0.0, f / fc);
            const Cx den = s * s + s / 0.7071 + 1.0;
            return kind == 0 ? 1.0 / den : kind == 1 ? s * s / den : (s * s - s / 0.7071 + 1.0) / den;
        };
        const Cx alpL = an (lo, 0), ahpL = an (lo, 1), alpH = an (hi, 0), ahpH = an (hi, 1), aapH = an (hi, 2);
        ta = Mat2::scalar (alpL * alpL * aapH + ahpL * ahpL * ahpH * ahpH) + ta.scaled (ahpL * ahpL * alpH * alpH);
    }
    hd = td; ha = ta;
}
inline void naturalResponses (const NaturalSpec& sp, double f, Mat2& hd, Mat2& ha)
{
    const NaturalPlan plan (sp);
    naturalResponses (sp, plan, f, hd, ha);
}

// Disena el FIR de correccion (4 filtros: C11, C22, C12, C21), de 'z.length' muestras.
// osResp: respuesta compleja medida del oversampling (subir y bajar) en los bins 0..fft/2, o vacio si es 1x.
// Devuelve si hacen falta los cruces (bandas L/R/M/S).
inline bool designNaturalPhase (const NaturalSpec& sp, const std::vector<Cx>& osResp, const NaturalSizes& z, std::array<std::vector<float>, 4>& irs)
{
    const int N = z.fft, K = N / 2 + 1;
    std::vector<Cx> c[4];
    for (auto& v : c) v.assign ((size_t) K, 0.0);
    bool cross = false;
    const NaturalPlan plan (sp);
    for (int k = 0; k < K; ++k)
    {
        const double f = std::max (0.5, k * sp.fs / N);
        Mat2 hd, ha;
        naturalResponses (sp, plan, f, hd, ha);
        if (! osResp.empty()) hd = hd.scaled (osResp[(size_t) k]);
        // C = Ha * Hd^-1 (con un piso: donde el digital ya no deja pasar nada no se intenta "revivir" la senal)
        const Cx det = hd.a * hd.d - hd.b * hd.c;
        const double nrm = std::norm (hd.a) + std::norm (hd.b) + std::norm (hd.c) + std::norm (hd.d);
        Mat2 C;
        if (std::abs (det) > 1.0e-6 * std::max (1.0e-12, nrm))
        {
            const Mat2 inv { hd.d / det, -hd.b / det, -hd.c / det, hd.a / det };
            C = ha * inv;
        }
        // limite de +-12 dB en la correccion (cerca de Nyquist el digital llega a cero: ahi no hay nada que corregir)
        for (Cx* e : { &C.a, &C.b, &C.c, &C.d })
            if (std::abs (*e) > 4.0) *e *= 4.0 / std::abs (*e);
        if (! std::isfinite (C.a.real()) || ! std::isfinite (C.d.real())) C = Mat2();
        // el retraso D (lo que permite corregir "hacia atras" lo que el digital adelanta)
        const Cx dl = std::polar (1.0, -2.0 * 3.14159265358979323846 * k * z.delay / N);
        c[0][(size_t) k] = C.a * dl; c[1][(size_t) k] = C.d * dl; c[2][(size_t) k] = C.b * dl; c[3][(size_t) k] = C.c * dl;
        if (std::abs (C.b) > 1.0e-6 || std::abs (C.c) > 1.0e-6) cross = true;
    }
    // a impulso (FFT inversa real) y ventana: sube en los D primeros (lo de antes del centro) y baja al final
    RealFFT fft ([N] { int o = 0; while ((1 << o) < N) ++o; return o; }());
    std::vector<float> buf ((size_t) (2 * N));
    for (int e = 0; e < 4; ++e)
    {
        irs[(size_t) e].assign ((size_t) z.length, 0.0f);
        if (e >= 2 && ! cross) continue;
        for (int k = 0; k < K; ++k)
        {
            buf[(size_t) (2 * k)] = (float) c[e][(size_t) k].real();
            buf[(size_t) (2 * k + 1)] = (k == 0 || k == K - 1) ? 0.0f : (float) c[e][(size_t) k].imag();
        }
        fft.performRealOnlyInverseTransform (buf.data());
        const int D = z.delay, L = z.length, tail = L - D;
        for (int n = 0; n < L; ++n)
        {
            double w = 1.0;
            if (n < D) w = 0.5 - 0.5 * std::cos (3.14159265358979323846 * (n + 0.5) / D);
            else
            {
                const double t = (double) (n - D) / tail;   // 0..1 despues del centro
                if (t > 0.5) w = 0.5 + 0.5 * std::cos (3.14159265358979323846 * (t - 0.5) / 0.5);
            }
            irs[(size_t) e][(size_t) n] = (float) (buf[(size_t) n] * w);
        }
    }
    return cross;
}
} // namespace gaux
