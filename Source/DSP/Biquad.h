// Gaux Equalizer - Filtros biquad (formulas RBJ "Audio EQ Cookbook")
// Codigo independiente de JUCE para poder probarlo por separado.
#pragma once

#include <cmath>
#include <complex>
#include <algorithm>

namespace gaux
{
constexpr double kPi = 3.14159265358979323846;

struct BiquadCoeffs
{
    // y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2   (a0 normalizado a 1)
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static BiquadCoeffs identity() { return {}; }

    static BiquadCoeffs normalise (double B0, double B1, double B2, double A0, double A1, double A2)
    {
        BiquadCoeffs c;
        c.b0 = B0 / A0; c.b1 = B1 / A0; c.b2 = B2 / A0;
        c.a1 = A1 / A0; c.a2 = A2 / A0;
        return c;
    }

    // Magnitud lineal de la respuesta en una frecuencia (para dibujar la curva)
    double magnitude (double freq, double sampleRate) const
    {
        const double w = 2.0 * kPi * freq / sampleRate;
        const std::complex<double> z1 = std::polar (1.0, -w);
        const std::complex<double> z2 = z1 * z1;
        const auto num = b0 + b1 * z1 + b2 * z2;
        const auto den = 1.0 + a1 * z1 + a2 * z2;
        return std::abs (num / den);
    }
};

// Limita la frecuencia a un rango seguro respecto a Nyquist
inline double safeFreq (double f, double fs)
{
    return std::clamp (f, 5.0, fs * 0.49);
}

inline BiquadCoeffs makeBell (double fs, double f, double q, double gainDb)
{
    f = safeFreq (f, fs);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f / fs;
    const double alpha = std::sin (w0) / (2.0 * q);
    const double cw = std::cos (w0);
    return BiquadCoeffs::normalise (1 + alpha * A, -2 * cw, 1 - alpha * A,
                                    1 + alpha / A, -2 * cw, 1 - alpha / A);
}

inline BiquadCoeffs makeLowShelf (double fs, double f, double q, double gainDb)
{
    f = safeFreq (f, fs);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f / fs;
    const double cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    const double sA = 2.0 * std::sqrt (A) * alpha;
    return BiquadCoeffs::normalise (A * ((A + 1) - (A - 1) * cw + sA),
                                    2 * A * ((A - 1) - (A + 1) * cw),
                                    A * ((A + 1) - (A - 1) * cw - sA),
                                    (A + 1) + (A - 1) * cw + sA,
                                    -2 * ((A - 1) + (A + 1) * cw),
                                    (A + 1) + (A - 1) * cw - sA);
}

inline BiquadCoeffs makeHighShelf (double fs, double f, double q, double gainDb)
{
    f = safeFreq (f, fs);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f / fs;
    const double cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    const double sA = 2.0 * std::sqrt (A) * alpha;
    return BiquadCoeffs::normalise (A * ((A + 1) + (A - 1) * cw + sA),
                                    -2 * A * ((A - 1) + (A + 1) * cw),
                                    A * ((A + 1) + (A - 1) * cw - sA),
                                    (A + 1) - (A - 1) * cw + sA,
                                    2 * ((A - 1) - (A + 1) * cw),
                                    (A + 1) - (A - 1) * cw - sA);
}

inline BiquadCoeffs makeLowPass (double fs, double f, double q)
{
    f = safeFreq (f, fs);
    const double w0 = 2.0 * kPi * f / fs;
    const double cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    return BiquadCoeffs::normalise ((1 - cw) / 2, 1 - cw, (1 - cw) / 2,
                                    1 + alpha, -2 * cw, 1 - alpha);
}

inline BiquadCoeffs makeHighPass (double fs, double f, double q)
{
    f = safeFreq (f, fs);
    const double w0 = 2.0 * kPi * f / fs;
    const double cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    return BiquadCoeffs::normalise ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2,
                                    1 + alpha, -2 * cw, 1 - alpha);
}

// Pasa-banda con ganancia 0 dB en el pico
inline BiquadCoeffs makeAllPass (double fs, double f, double q)
{
    f = safeFreq (f, fs);
    const double w0 = 2.0 * kPi * f / fs;
    const double cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    return BiquadCoeffs::normalise (1 - alpha, -2 * cw, 1 + alpha,
                                    1 + alpha, -2 * cw, 1 - alpha);
}

//==============================================================================
// Filtros "emparejados con el analogico" (metodo de M. Vicanek): los polos se ubican como en el
// circuito analogico y los ceros se calculan para igualar la magnitud analogica en DC, en el centro
// y en Nyquist. Asi las curvas no se "aplastan" cerca de los 20 kHz como con la transformada bilineal.
struct AnalogBiquad { double n2, n1, n0, d2, d1, d0; };   // H(s) = (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0)

inline double analogMag2 (const AnalogBiquad& a, double omega)
{
    const std::complex<double> s (0.0, omega);
    const auto h = (a.n2 * s * s + a.n1 * s + a.n0) / (a.d2 * s * s + a.d1 * s + a.d0);
    return std::norm (h);
}

inline BiquadCoeffs matchedFromAnalog (const AnalogBiquad& an, double fs, double midFactor = 0.0)
{
    const double wp = std::sqrt (an.d0 / an.d2);           // frecuencia de los polos (rad/s)
    const double Qp = wp * an.d2 / an.d1;
    const double w = wp / fs, q = 0.5 / Qp;
    double a1, a2 = std::exp (-2.0 * q * w);
    if (q <= 1.0) a1 = -2.0 * std::exp (-q * w) * std::cos (std::sqrt (1.0 - q * q) * w);
    else          a1 = -2.0 * std::exp (-q * w) * std::cosh (std::sqrt (q * q - 1.0) * w);

    const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2), A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2), A2 = -4.0 * a2;
    // Punto de ajuste intermedio: la frecuencia de los polos (si cabe), si no, la mitad de la banda
    // (midFactor > 0: se ajusta en midFactor * frecuencia de los polos, dentro de la banda que pasa)
    const double wm = midFactor > 0.0 ? std::clamp (midFactor * w, 1.0e-4, 0.95 * kPi) : std::clamp (w, 0.05 * kPi, 0.9 * kPi);
    const double p1 = std::sin (0.5 * wm) * std::sin (0.5 * wm), p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
    const double T0 = analogMag2 (an, 0.0), Tn = analogMag2 (an, kPi * fs), Tm = analogMag2 (an, wm * fs);
    const double B0 = T0 * A0, B1 = Tn * A1;
    const double B2 = (Tm * (A0 * p0 + A1 * p1 + A2 * p2) - B0 * p0 - B1 * p1) / p2;
    const double sB0 = std::sqrt (std::max (0.0, B0)), sB1 = std::sqrt (std::max (0.0, B1));
    const double W = 0.5 * (sB0 + sB1);
    const double b0 = 0.5 * (W + std::sqrt (std::max (0.0, W * W + B2)));
    const double b1 = 0.5 * (sB0 - sB1);
    const double b2 = b0 != 0.0 ? -B2 / (4.0 * b0) : 0.0;
    BiquadCoeffs c;
    c.b0 = b0; c.b1 = b1; c.b2 = b2; c.a1 = a1; c.a2 = a2;
    return c;
}

inline BiquadCoeffs makeMatchedBell (double fs, double f, double q, double gainDb)
{
    if (f > 0.42 * fs) return makeBell (fs, std::min (f, fs * 0.45), q, gainDb);
    const double A = std::pow (10.0, gainDb / 40.0), w0 = 2.0 * kPi * f;
    return matchedFromAnalog ({ 1.0, A * w0 / q, w0 * w0, 1.0, w0 / (A * q), w0 * w0 }, fs);
}
inline BiquadCoeffs makeMatchedLowShelf (double fs, double f, double q, double gainDb)
{
    if (2.0 * f * std::pow (10.0, std::abs (gainDb) / 40.0) > 0.75 * fs) return makeLowShelf (fs, std::min (f, fs * 0.45), q, gainDb);
    const double A = std::pow (10.0, gainDb / 40.0), w0 = 2.0 * kPi * f, sA = std::sqrt (A);
    return matchedFromAnalog ({ A, A * sA / q * w0, A * A * w0 * w0, A, sA / q * w0, w0 * w0 }, fs);
}
inline BiquadCoeffs makeMatchedHighShelf (double fs, double f, double q, double gainDb)
{
    // Polos por encima de Nyquist (shelf "de aire" muy alto): la bilineal se comporta mejor ahi
    if (2.0 * f * std::pow (10.0, std::abs (gainDb) / 40.0) > 0.75 * fs) return makeHighShelf (fs, std::min (f, fs * 0.45), q, gainDb);
    const double A = std::pow (10.0, gainDb / 40.0), w0 = 2.0 * kPi * f, sA = std::sqrt (A);
    return matchedFromAnalog ({ A * A, A * sA / q * w0, A * w0 * w0, 1.0, sA / q * w0, A * w0 * w0 }, fs);
}
inline BiquadCoeffs makeMatchedLowPass (double fs, double f, double q)
{
    if (f > 0.42 * fs) return makeLowPass (fs, std::min (f, fs * 0.45), q);
    const double w0 = 2.0 * kPi * f;
    return matchedFromAnalog ({ 0.0, 0.0, w0 * w0, 1.0, w0 / q, w0 * w0 }, fs);
}
inline BiquadCoeffs makeMatchedHighPass (double fs, double f, double q)
{
    const double w0 = 2.0 * kPi * f;
    return matchedFromAnalog ({ 1.0, 0.0, 0.0, 1.0, w0 / q, w0 * w0 }, fs);
}

//==============================================================================
// Diseno "sin aplastamiento" para el Parametrico (curvas como las de un EQ analogico hasta 20 kHz,
// tambien a 44.1/48 kHz sin HQ). Se mezclan suavemente con el diseno clasico donde este es mejor,
// asi al barrer una banda nunca hay saltos, y siempre son filtros estables.
inline double smoothRamp (double x) { x = std::clamp (x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }

inline BiquadCoeffs blendBiquad (const BiquadCoeffs& a, const BiquadCoeffs& b, double w)
{
    // La zona de estabilidad de (a1, a2) es convexa: la mezcla de dos filtros estables es estable
    BiquadCoeffs c;
    c.b0 = a.b0 + w * (b.b0 - a.b0); c.b1 = a.b1 + w * (b.b1 - a.b1); c.b2 = a.b2 + w * (b.b2 - a.b2);
    c.a1 = a.a1 + w * (b.a1 - a.a1); c.a2 = a.a2 + w * (b.a2 - a.a2);
    return c;
}

// Comparacion rapida contra el analogico (para elegir el mejor diseno): magnitud^2 de un biquad usando
// cos(w) y cos(2w) ya calculados, en puntos fijos relativos a la frecuencia de muestreo
struct DesignProbes
{
    static constexpr int n = 9;
    double r[n] = { 0.01, 0.03, 0.07, 0.12, 0.18, 0.25, 0.32, 0.39, 0.45 };
    double c1[n], c2[n];
    DesignProbes() { for (int i = 0; i < n; ++i) { c1[i] = std::cos (2.0 * kPi * r[i]); c2[i] = std::cos (4.0 * kPi * r[i]); } }
    static const DesignProbes& get() { static const DesignProbes p; return p; }
};
inline double biquadMag2 (const BiquadCoeffs& c, double c1, double c2)
{
    const double num = c.b0 * c.b0 + c.b1 * c.b1 + c.b2 * c.b2 + 2.0 * (c.b0 * c.b1 + c.b1 * c.b2) * c1 + 2.0 * c.b0 * c.b2 * c2;
    const double den = 1.0 + c.a1 * c.a1 + c.a2 * c.a2 + 2.0 * (c.a1 + c.a1 * c.a2) * c1 + 2.0 * c.a2 * c2;
    return num / std::max (1.0e-300, den);
}
// Error maximo (dB) de dos disenos contra una magnitud^2 analogica dada, en los puntos de prueba (hasta 20 kHz)
template <typename Analog2>
inline void probeErrors (const BiquadCoeffs& a, const BiquadCoeffs& b, double fs, Analog2 analogMag2At, double& ea, double& eb)
{
    const auto& P = DesignProbes::get();
    ea = eb = 0.0;
    for (int i = 0; i < P.n; ++i)
    {
        const double f = P.r[i] * fs;
        if (f > 20000.0) break;
        const double t = std::max (1.0e-300, analogMag2At (f));
        ea = std::max (ea, std::abs (10.0 * std::log10 (std::max (1.0e-300, biquadMag2 (a, P.c1[i], P.c2[i])) / t)));
        eb = std::max (eb, std::abs (10.0 * std::log10 (std::max (1.0e-300, biquadMag2 (b, P.c1[i], P.c2[i])) / t)));
    }
}

// Campana con ganancia en Nyquist igual a la del analogico (S. Orfanidis, JAES 1997).
// GB: ganancia a la que se mide el ancho Dh (Hz).
inline bool orfanidisBell (double fs, double f0, double gainDb, double GB, double Dh, BiquadCoeffs& c)
{
    const double G0 = 1.0, G = std::pow (10.0, gainDb / 20.0);
    const double w0 = 2.0 * kPi * f0 / fs, Dw = 2.0 * kPi * Dh / fs;
    if (w0 >= 0.97 * kPi || Dw >= 0.97 * kPi) return false;
    const double F = std::abs (G * G - GB * GB), G00 = std::abs (G * G - G0 * G0), F00 = std::abs (GB * GB - G0 * G0);
    if (F < 1.0e-12 || G00 < 1.0e-12) return false;
    const double pw = (w0 * w0 - kPi * kPi) * (w0 * w0 - kPi * kPi);
    const double G1 = std::sqrt ((G0 * G0 * pw + G * G * F00 * kPi * kPi * Dw * Dw / F) / (pw + F00 * kPi * kPi * Dw * Dw / F));
    const double G01 = std::abs (G * G - G0 * G1), G11 = std::abs (G * G - G1 * G1);
    const double F01 = std::abs (GB * GB - G0 * G1), F11 = std::abs (GB * GB - G1 * G1);
    if (F11 < 1.0e-12) return false;
    const double t = std::tan (0.5 * w0);
    const double W2 = std::sqrt (G11 / G00) * t * t;
    const double DW = (1.0 + std::sqrt (F00 / F11) * W2) * std::tan (0.5 * Dw);
    const double C = F11 * DW * DW - 2.0 * W2 * (F01 - std::sqrt (F00 * F11));
    const double D = 2.0 * W2 * (G01 - std::sqrt (G00 * G11));
    if (C + D < 0.0 || G * G * C + GB * GB * D < 0.0) return false;
    const double A = std::sqrt ((C + D) / F), B = std::sqrt ((G * G * C + GB * GB * D) / F);
    const double n = 1.0 + W2 + A;
    c.b0 = (G1 + G0 * W2 + B) / n; c.b1 = -2.0 * (G1 - G0 * W2) / n; c.b2 = (G1 - B + G0 * W2) / n;
    c.a1 = -2.0 * (1.0 - W2) / n;  c.a2 = (1.0 + W2 - A) / n;
    return std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2) && std::isfinite (c.a1) && std::isfinite (c.a2)
        && std::abs (c.a2) < 1.0 && std::abs (c.a1) < 1.0 + c.a2;
}

// Respuesta (dB) de la campana analogica ideal
inline double analogBellDb (double f, double f0, double q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const std::complex<double> s (0.0, f / f0);
    return 20.0 * std::log10 (std::abs ((s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0)));
}

// Campana "analogica": igual al EQ analogico ideal tambien cerca de los 20 kHz
//  - Campanas normales y angostas (Q >= 0.7): diseno de Orfanidis (ganancia en Nyquist como el analogico)
//  - Campanas anchas (Q <= 0.45): entre el diseno clasico y el "matched" (polos exactos del analogico)
//    se usa en cada caso el que mas se parece al analogico (se miden ambos), mezclados de forma suave
//    para que al mover cualquier perilla la curva cambie sin saltos.
inline BiquadCoeffs makeAnalogBell (double fs, double f0, double q, double gainDb)
{
    f0 = safeFreq (f0, fs);
    const auto classic = makeBell (fs, f0, q, gainDb);
    const double ft = 0.42 * fs;                       // la campana tiene que caber hasta aca
    if (std::abs (gainDb) < 0.05 || f0 >= ft) return classic;

    const double wq = smoothRamp ((q - 0.45) / 0.25);   // 0 = ancha, 1 = normal/angosta
    BiquadCoeffs wide = classic;
    if (wq < 1.0)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w0 = 2.0 * kPi * f0;
        const auto m = matchedFromAnalog ({ 1.0, A * w0 / q, w0 * w0, 1.0, w0 / (A * q), w0 * w0 }, fs);
        if (std::isfinite (m.b0) && std::isfinite (m.b1) && std::isfinite (m.b2) && std::abs (m.a2) < 1.0 && std::abs (m.a1) < 1.0 + m.a2)
        {
            double eC = 0.0, eM = 0.0;
            const double iq2 = 1.0 / (q * q);
            probeErrors (classic, m, fs, [&] (double f)
            {
                const double W = f / f0, x = 1.0 - W * W;
                return (x * x + A * A * iq2 * W * W) / (x * x + iq2 * W * W / (A * A));
            }, eC, eM);
            const double wm = smoothRamp ((eC - eM) / 1.0 + 0.5) * smoothRamp ((ft - f0) / (0.05 * fs));
            wide = blendBiquad (classic, m, wm);
        }
    }
    if (wq <= 0.0) return wide;

    // Ancho a mitad de ganancia (en dB). Si el borde superior no cabe, se mide el ancho mas arriba
    // en el pico (donde la campana es mas angosta), de forma continua: nunca hay saltos.
    double Dh = f0 / q, k = 1.0;
    const double fu = 0.5 * (Dh + std::sqrt (Dh * Dh + 4.0 * f0 * f0));
    if (fu > ft) { Dh = (ft * ft - f0 * f0) / ft; k = q * Dh / f0; }
    const double A = std::pow (10.0, gainDb / 40.0);
    const double GB = std::sqrt (A * A * (k * k + A * A) / (1.0 + k * k * A * A));
    const double w = smoothRamp ((k - 0.12) / 0.12)            // muy pegado a Nyquist: clasico
                   * smoothRamp ((f0 / fs - 0.03) / 0.03);      // graves y medios: el clasico ya es exacto
    BiquadCoeffs o, narrow = classic;
    if (w > 0.0 && orfanidisBell (fs, f0, gainDb, GB, Dh, o)) narrow = blendBiquad (classic, o, w);
    return wq >= 1.0 ? narrow : blendBiquad (wide, narrow, wq);
}


// "Matched" con los polos limitados suavemente por debajo de Nyquist: sirve tambien cuando el analogico
// tiene polos por encima de Nyquist (shelf de agudos en 16-40 kHz a 44.1 kHz). El numerador se ajusta en
// DC, en Nyquist y en un punto medio dentro de la banda.
inline BiquadCoeffs softMatchedFromAnalog (const AnalogBiquad& an, double fs)
{
    const double wp0 = std::sqrt (an.d0 / an.d2), Qp = wp0 * an.d2 / an.d1;
    const double wN = kPi * fs;
    const double wp = wp0 / std::pow (1.0 + std::pow (wp0 / (1.1 * wN), 6.0), 1.0 / 6.0);
    const double w = wp / fs, q = 0.5 / Qp;
    double a1, a2 = std::exp (-2.0 * q * w);
    if (q <= 1.0) a1 = -2.0 * std::exp (-q * w) * std::cos (std::sqrt (1.0 - q * q) * w);
    else          a1 = -2.0 * std::exp (-q * w) * std::cosh (std::sqrt (q * q - 1.0) * w);
    const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2), A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2), A2 = -4.0 * a2;
    const double wraw = std::max (w, 0.05 * kPi);
    const double wm = wraw / std::pow (1.0 + std::pow (wraw / (0.4 * kPi), 4.0), 0.25);
    const double p1 = std::sin (0.5 * wm) * std::sin (0.5 * wm), p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
    const double T0 = analogMag2 (an, 0.0), Tn = analogMag2 (an, kPi * fs), Tm = analogMag2 (an, wm * fs);
    const double B0 = T0 * A0, B1 = Tn * A1, B2 = (Tm * (A0 * p0 + A1 * p1 + A2 * p2) - B0 * p0 - B1 * p1) / p2;
    const double sB0 = std::sqrt (std::max (0.0, B0)), sB1 = std::sqrt (std::max (0.0, B1)), W = 0.5 * (sB0 + sB1);
    BiquadCoeffs c;
    c.b0 = 0.5 * (W + std::sqrt (std::max (0.0, W * W + B2))); c.b1 = 0.5 * (sB0 - sB1);
    c.b2 = c.b0 != 0.0 ? -B2 / (4.0 * c.b0) : 0.0; c.a1 = a1; c.a2 = a2;
    return c;
}

inline bool isStableBiquad (const BiquadCoeffs& c)
{
    return std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2) && std::isfinite (c.a1) && std::isfinite (c.a2)
        && std::abs (c.a2) < 1.0 && std::abs (c.a1) < 1.0 + c.a2;
}

// Respuesta (dB) del shelf de agudos analogico ideal
inline double analogHighShelfDb (double f, double f0, double q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0), sA = std::sqrt (A);
    const std::complex<double> s (0.0, f / f0);
    return 20.0 * std::log10 (std::abs (A * (A * s * s + (sA / q) * s + 1.0) / (s * s + (sA / q) * s + A)));
}

// Shelf de agudos "analogico": igual al EQ analogico ideal hasta 20 kHz tambien a 44.1/48 kHz (el clasico
// se comprime cerca de Nyquist: un shelf en 12-16 kHz puede quedar 3-6 dB distinto). El recorte se disena
// con polos "matched"; el realce es exactamente el inverso del recorte (como en el analogico). Se compara
// con el clasico y se usa, mezclado suave, el que mas se parece al analogico: nunca hay saltos al mover.
inline BiquadCoeffs makeAnalogHighShelf (double fs, double f, double q, double gainDb)
{
    f = std::clamp (f, 5.0, 2.0 * fs);            // puede estar sobre Nyquist (bandas de "aire")
    const auto classic = makeHighShelf (fs, std::min (f, fs * 0.49), q, gainDb);
    if (std::abs (gainDb) < 0.05) return classic;
    auto cutOf = [&] (double g)
    {
        const double A = std::pow (10.0, g / 40.0), w0 = 2.0 * kPi * f, sA = std::sqrt (A);
        return softMatchedFromAnalog ({ A * A, A * sA / q * w0, A * w0 * w0, 1.0, sA / q * w0, A * w0 * w0 }, fs);
    };
    BiquadCoeffs m;
    if (gainDb < 0.0) m = cutOf (gainDb);
    else
    {
        const auto c = cutOf (-gainDb);
        const double z2 = c.b0 != 0.0 ? c.b2 / c.b0 : 2.0, z1 = c.b0 != 0.0 ? c.b1 / c.b0 : 0.0;
        if (! (std::abs (z2) < 1.0 && std::abs (z1) < 1.0 + z2)) return classic;   // el inverso no seria estable
        m.b0 = 1.0 / c.b0; m.b1 = c.a1 / c.b0; m.b2 = c.a2 / c.b0; m.a1 = c.b1 / c.b0; m.a2 = c.b2 / c.b0;
    }
    if (! isStableBiquad (m)) return classic;
    double eC = 0.0, eM = 0.0;
    {
        const double A = std::pow (10.0, gainDb / 40.0), aq = A / (q * q);
        probeErrors (classic, m, fs, [&] (double ff)
        {
            const double W = ff / f, W2 = W * W;
            const double nr = 1.0 - A * W2, dr = A - W2;
            return A * A * (nr * nr + aq * W2) / (dr * dr + aq * W2);
        }, eC, eM);
    }
    return blendBiquad (classic, m, smoothRamp ((eC - eM) / 1.0 + 0.5));
}

// Pasa bajos (High Cut) sin aplastamiento: polos en el lugar exacto del analogico y el numerador ajustado
// en DC, en Nyquist y en 0.8 x la frecuencia de corte (dentro de la banda que pasa). Asi la pendiente sobre
// el corte es la del analogico tambien cerca de 20 kHz (el clasico corta de mas). El punto de ajuste
// depende del Q de la seccion (las de Q alto se ajustan mas cerca del pico) para que las secciones de un
// corte de 24-48 dB/oct no sumen un "bulto" en la zona que pasa. Una sola formula para todo el rango: al
// barrer la frecuencia la curva cambia de forma continua, sin saltos.
inline BiquadCoeffs makeAnalogLowPass (double fs, double f, double q)
{
    f = safeFreq (f, fs);
    const double w0 = 2.0 * kPi * f;
    // punto de ajuste intermedio; la correccion (1 + ...) baja el error de las cascadas empinadas ~20-30 %
    // (probado en 845 combinaciones de fs, corte, pendiente y Q) y se desvanece pegado a Nyquist
    const double x = f / fs, wgt = std::clamp ((0.40 - x) / 0.07, 0.0, 1.0);
    const double k = std::clamp ((0.95 + 0.15 * std::log (q / 0.7071) - 0.30 * (2.0 * f / fs)) * (1.0 + (0.05 + 0.9 * x) * wgt), 0.1, 0.95);
    const auto c = matchedFromAnalog ({ 0.0, 0.0, w0 * w0, 1.0, w0 / q, w0 * w0 }, fs, k);
    if (! (std::abs (c.a2) < 1.0 && std::abs (c.a1) < 1.0 + c.a2) || ! std::isfinite (c.b0) || ! std::isfinite (c.b2))
        return makeLowPass (fs, f, q);
    return c;
}

// Pasa-banda (0 dB en el centro) emparejado con el analogico: el lado de agudos no se "aplasta" cerca de
// Nyquist (la bilineal pone un cero en Nyquist). Si el diseno no es estable, se usa el clasico.
inline BiquadCoeffs makeBandPass (double fs, double f, double q);
inline BiquadCoeffs makeAnalogBandPass (double fs, double f, double q)
{
    f = safeFreq (f, fs);
    const double w0 = 2.0 * kPi * f;
    auto c = matchedFromAnalog ({ 0.0, w0 / q, 0.0, 1.0, w0 / q, w0 * w0 }, fs);
    // el numerador debe anularse en DC (b0 + b1 + b2 = 0)
    const double dc = c.b0 + c.b1 + c.b2;
    c.b1 -= dc;
    if (! (std::abs (c.a2) < 1.0 && std::abs (c.a1) < 1.0 + c.a2) || ! std::isfinite (c.b0) || ! std::isfinite (c.b2))
        return makeBandPass (fs, f, q);
    // 0 dB exactos en el centro
    const double m = c.magnitude (f, fs);
    if (m > 1.0e-9) { c.b0 /= m; c.b1 /= m; c.b2 /= m; }
    return c;
}

inline BiquadCoeffs makeBandPass (double fs, double f, double q)
{
    f = safeFreq (f, fs);
    const double w0 = 2.0 * kPi * f / fs;
    const double cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    return BiquadCoeffs::normalise (alpha, 0, -alpha, 1 + alpha, -2 * cw, 1 - alpha);
}

inline BiquadCoeffs makeNotch (double fs, double f, double q)
{
    f = safeFreq (f, fs);
    const double w0 = 2.0 * kPi * f / fs;
    const double cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    return BiquadCoeffs::normalise (1, -2 * cw, 1, 1 + alpha, -2 * cw, 1 - alpha);
}

// Estado de un biquad (Transposed Direct Form II), estable para modulacion lenta
//==============================================================================
// Realizacion SVF trapezoidal (estructura de Andrew Simper / Cytomic) de cualquier biquad estable.
// Misma curva exacta que el biquad, pero muy estable cuando los coeficientes se mueven rapido
// (automatizacion, barridos, EQ dinamica): no genera picos ni transitorios por la modulacion.
struct SvfCoeffs
{
    double a1 = 1.0, a2 = 0.0, a3 = 0.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;

    static SvfCoeffs fromBiquad (const BiquadCoeffs& c)
    {
        SvfCoeffs r;
        const double p = 1.0 + c.a1 + c.a2, q = 1.0 - c.a1 + c.a2;
        if (p <= 1.0e-12 || q <= 1.0e-12 || 1.0 + c.a2 <= 1.0e-12) { r.m0 = c.b0; return r; }   // fuera de rango: paso directo
        const double g = std::sqrt (p / q);
        const double D = 2.0 * (1.0 + g * g) / (1.0 + c.a2);
        const double k = (D - 1.0 - g * g) / g;
        // Numerador analogico equivalente (inversa de la bilineal con la misma g)
        const double S = 0.5 * (c.b0 + c.b2) * D, T = 0.5 * c.b1 * D;
        const double n2 = 0.5 * (S - T), n0 = 0.5 * (S + T) / (g * g), n1 = D * (c.b0 - c.b2) / (2.0 * g);
        r.a1 = 1.0 / (1.0 + g * (g + k));
        r.a2 = g * r.a1;
        r.a3 = g * r.a2;
        r.m0 = n2;
        r.m1 = n1 - k * n2;
        r.m2 = n0 - n2;
        return r;
    }
};

// Interpolacion de coeficientes muestra a muestra (sin "zipper" al mover perillas)
inline SvfCoeffs svfLerpStep (const SvfCoeffs& from, const SvfCoeffs& to, double steps)
{
    SvfCoeffs d;
    d.a1 = (to.a1 - from.a1) / steps; d.a2 = (to.a2 - from.a2) / steps; d.a3 = (to.a3 - from.a3) / steps;
    d.m0 = (to.m0 - from.m0) / steps; d.m1 = (to.m1 - from.m1) / steps; d.m2 = (to.m2 - from.m2) / steps;
    return d;
}
inline void svfAdd (SvfCoeffs& c, const SvfCoeffs& d)
{
    c.a1 += d.a1; c.a2 += d.a2; c.a3 += d.a3; c.m0 += d.m0; c.m1 += d.m1; c.m2 += d.m2;
}

struct SvfState
{
    double ic1 = 0.0, ic2 = 0.0;
    void reset() { ic1 = ic2 = 0.0; }
    inline double process (double v0, const SvfCoeffs& c)
    {
        const double v3 = v0 - ic2;
        const double v1 = c.a1 * ic1 + c.a2 * v3;
        const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
    }
};

struct BiquadState
{
    double z1 = 0, z2 = 0;

    void reset() { z1 = z2 = 0; }

    inline double process (double x, const BiquadCoeffs& c)
    {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

// Bloqueador de DC de un polo (~10 Hz)
struct DcBlocker
{
    double x1 = 0, y1 = 0, r = 0.9995;

    void prepare (double fs) { r = 1.0 - (2.0 * kPi * 10.0 / fs); reset(); }
    void reset() { x1 = y1 = 0; }

    inline double process (double x)
    {
        const double y = x - x1 + r * y1;
        x1 = x; y1 = y;
        return y;
    }
};
} // namespace gaux
