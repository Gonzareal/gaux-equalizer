// Gaux Equalizer - Curvas de saturacion
#pragma once

#include <cmath>
#include <algorithm>
#include "Biquad.h"

namespace gaux
{
enum class SatType { Tube = 0, Tape, Soft, Hard, Fuzz };

inline const char* const kSatTypeNames[] = { "Tube", "Tape", "Soft Clip", "Hard Clip", "Fuzz" };
constexpr int kNumSatTypes = 5;

// Aplica la curva elegida. 'x' ya viene multiplicado por el drive.
inline double saturate (double x, SatType type)
{
    switch (type)
    {
        case SatType::Tube:
        {
            // Asimetrica: genera armonicos pares (calidez tipo valvula)
            constexpr double bias = 0.25;
            return std::tanh (x + bias) - std::tanh (bias);
        }
        case SatType::Tape:
        {
            // Compresion suave y redonda tipo cinta
            return (2.0 / kPi) * std::atan (x * kPi * 0.5);
        }
        case SatType::Soft:
        {
            // Clip cubico suave
            const double c = std::clamp (x, -1.5, 1.5);
            return c - (4.0 / 27.0) * c * c * c;
        }
        case SatType::Hard:
            return std::clamp (x, -1.0, 1.0);

        case SatType::Fuzz:
        {
            // Mezcla de tanh + rectificado: agresivo, muchos armonicos
            // (la componente DC del rectificado se elimina despues)
            const double t = std::tanh (x * 1.5);
            return 0.7 * t + 0.3 * std::abs (t);
        }
    }
    return x;
}

//==============================================================================
// Motor de saturacion de las bandas (v2)
//  - Cada modo tiene su propia curva y su propio caracter armonico:
//      TUBE  triodo asimetrico: domina el 2do armonico (calidez), comprime suave.
//      TAPE  cinta: curva exponencial muy redonda + pre/de-enfasis (los agudos
//            saturan antes, como en la cinta real) -> 3er armonico suave, "pegamento".
//      SOFT  consola/transistor: limpio hasta la rodilla y luego redondea (3er armonico).
//      HARD  clip digital/diodo: borde duro, serie impar larga, mucho mordisco.
//      FUZZ  fuzz de silicio: asimetrico + rectificado (octava arriba), pares e impares.
//  - Antialiasing ADAA (anti-derivada de 1er orden): mucho menos aliasing aun en 1x.
//  - Compensacion de nivel: al subir el drive la banda no se hunde; gana armonicos.
namespace SatCurve
{
    inline double logCosh (double x) { const double a = std::abs (x); return a + std::log1p (std::exp (-2.0 * a)) - 0.6931471805599453; }
    constexpr double kTubeNeg = 0.55;            // lado negativo del triodo mas abierto -> asimetria
    constexpr double kFuzzBias = 0.45;

    inline double f (double x, SatType t)
    {
        switch (t)
        {
            case SatType::Tube: return x >= 0.0 ? std::tanh (x) : std::tanh (kTubeNeg * x) / kTubeNeg;
            case SatType::Tape: return (x >= 0.0 ? 1.0 : -1.0) * (1.0 - std::exp (-std::abs (x)));
            case SatType::Soft: { const double c = std::clamp (x, -1.5, 1.5); return c - (4.0 / 27.0) * c * c * c; }
            case SatType::Hard: return std::clamp (x, -1.0, 1.0);
            case SatType::Fuzz: return 0.70 * (std::tanh (2.0 * x + kFuzzBias) - std::tanh (kFuzzBias)) + 0.45 * std::abs (std::tanh (1.6 * x));
        }
        return x;
    }

    // Anti-derivada de cada curva (para el antialiasing)
    inline double F (double x, SatType t)
    {
        switch (t)
        {
            case SatType::Tube: return x >= 0.0 ? logCosh (x) : logCosh (kTubeNeg * x) / (kTubeNeg * kTubeNeg);
            case SatType::Tape: { const double a = std::abs (x); return a + std::exp (-a) - 1.0; }
            case SatType::Soft:
            {
                const double a = std::abs (x);
                if (a <= 1.5) return 0.5 * x * x - x * x * x * x / 27.0;
                return 0.9375 + (a - 1.5);
            }
            case SatType::Hard: { const double a = std::abs (x); return a <= 1.0 ? 0.5 * x * x : a - 0.5; }
            case SatType::Fuzz:
                return 0.70 * (0.5 * logCosh (2.0 * x + kFuzzBias) - std::tanh (kFuzzBias) * x)
                     + 0.45 * (x >= 0.0 ? 1.0 : -1.0) * logCosh (1.6 * x) / 1.6;
        }
        return 0.5 * x * x;
    }

    // Respuesta "media" a un pico v (promedio de los dos lados, para curvas asimetricas)
    inline double peakResponse (double v, SatType t) { return 0.5 * (f (v, t) - f (-v, t)); }
}

// Estado por canal
struct SatChannelState
{
    double x1 = 0.0, F1 = 0.0, r1 = 0.0;
    int curveTag = -1;                 // curva con la que se calculo F1
    BiquadState pre, post, soft;
    void reset() { x1 = F1 = r1 = 0.0; curveTag = -1; pre.reset(); post.reset(); soft.reset(); }
};

class Saturator
{
public:
    void setup (double sampleRate, SatType t, double driveDb)
    {
        type = t;
        drive = std::pow (10.0, driveDb / 20.0);
        // Con muy poco drive el efecto entra suave (continuidad total al subir la perilla desde 0)
        fadeIn = std::clamp (driveDb / 6.0, 0.0, 1.0);
        // Compensacion: punto medio (geometrico) entre "dividir por el drive" (la banda se hunde)
        // y "mantener el nivel de referencia" (-15 dBFS), para que suba la densidad sin saltos de volumen.
        constexpr double ref = 0.18, a = 0.72;
        // Punto de trabajo (bias) fijo como en una valvula/transistor real: la asimetria se mantiene
        // aun con mucho drive, asi TUBE y FUZZ conservan sus armonicos pares.
        // El bias crece con el drive pero con tope, para que los pasajes suaves no se apaguen.
        if (t == SatType::Tube)      bias = std::min (0.55, 0.22 * std::sqrt (drive));
        else if (t == SatType::Fuzz) bias = std::min (0.30, 0.12 * std::sqrt (drive));
        else                         bias = 0.0;
        const double b = bias;
        const double resp = 0.5 * (SatCurve::f (ref * drive + b, t) - SatCurve::f (-ref * drive + b, t));
        const double full = ref / std::max (1.0e-9, resp);
        makeup = std::pow (1.0 / drive, 1.0 - a) * std::pow (full, a);
        restOut = SatCurve::f (b, t);            // salida en reposo (se resta: sin saltos de DC)
        // Ganancia para senales suaves (pendiente de la curva en el punto de trabajo): maximo +6 dB
        const double eps = 1.0e-4;
        const double slope = (SatCurve::f (b + eps, t) - SatCurve::f (b - eps, t)) / (2.0 * eps);
        const double smallGain = makeup * drive * std::max (1.0e-3, slope);
        if (smallGain > 2.0) makeup *= 2.0 / smallGain;
        if (smallGain < 0.7) makeup *= 0.7 / smallGain;
        tapeEmphasis = t == SatType::Tape;
        // TAPE: los armonicos salen mas suaves arriba (perdida de agudos de la cinta)
        tapeSoften = makeLowPass (sampleRate, std::min (9000.0, sampleRate * 0.4), 0.6);
        if (tapeEmphasis)
        {
            const double fc = std::min (3200.0, sampleRate * 0.2);
            pre = makeHighShelf (sampleRate, fc, 0.55, 6.0);
            post = makeHighShelf (sampleRate, fc, 0.55, -6.0);
        }
    }

    // Recibe la zona aislada de la banda; devuelve lo que hay que sumar (armonicos + compresion)
    inline double process (SatChannelState& s, double region)
    {
        double u = region;
        if (tapeEmphasis) u = s.pre.process (u, pre);
        u = u * drive + bias;

        // Arranque limpio: la "memoria" del antialiasing empieza exactamente en el punto actual.
        // Si cambio el modo, la anti-derivada anterior se recalcula con la curva nueva (sin saltos).
        if (s.curveTag == -1) { s.x1 = u; s.r1 = region; }
        if (s.curveTag != (int) type) { s.F1 = SatCurve::F (s.x1, type); s.curveTag = (int) type; }

        // ADAA de 1er orden: promedio exacto de la curva entre la muestra anterior y la actual
        const double Fx = SatCurve::F (u, type);
        const double dx = u - s.x1;
        double y = std::abs (dx) > 1.0e-6 ? (Fx - s.F1) / dx : SatCurve::f (0.5 * (u + s.x1), type);
        s.x1 = u; s.F1 = Fx;

        y = (y - restOut) * makeup;
        if (tapeEmphasis) y = s.post.process (y, post);

        // La salida ADAA queda medio sample atras: se compara con la zona alineada igual
        const double aligned = 0.5 * (region + s.r1);
        s.r1 = region;
        double delta = y - aligned;
        if (tapeEmphasis) delta = s.soft.process (delta, tapeSoften);
        return delta * fadeIn;
    }

private:
    SatType type = SatType::Tube;
    double drive = 1.0, makeup = 1.0, bias = 0.0, restOut = 0.0, fadeIn = 1.0;
    bool tapeEmphasis = false;
    BiquadCoeffs pre, post, tapeSoften;
};

// Devuelve la senal saturada normalizada por el drive, para que el nivel
// no se dispare al subir el drive (solo cambia el "color").
inline double saturateNormalised (double x, double drive, SatType type)
{
    return saturate (x * drive, type) / drive;
}
} // namespace gaux
