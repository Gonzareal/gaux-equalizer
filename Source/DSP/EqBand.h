// Gaux Equalizer - Banda parametrica con saturacion opcional (estilo Spectre)
#pragma once

#include "Biquad.h"
#include "Saturation.h"
#include <array>
#include <vector>
#include <initializer_list>

namespace gaux
{
// Los tipos nuevos van al final: las sesiones y presets guardados siguen apuntando al mismo tipo
enum class BandType { Bell = 0, LowShelf, HighShelf, LowCut, HighCut, Notch, BandPass, TiltShelf, AllPass };

inline const char* const kBandTypeNames[] = { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt", "All Pass" };
constexpr int kNumBandTypes = 9;

// Pendientes. El indice guardado no cambia de significado (0..3 = 12/24/36/48 como siempre); las nuevas
// van al final. En pantalla se muestran ordenadas de suave a empinada (kSlopeDisplayOrder).
inline const char* const kSlopeNames[] = { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "6 dB/oct", "18 dB/oct", "72 dB/oct", "96 dB/oct", "Brickwall" };
constexpr int kNumSlopes = 9;
constexpr int kSlopeBrick = 8;
inline constexpr int kSlopeDisplayOrder[] = { 4, 0, 5, 1, 2, 3, 6, 7, 8 };
inline const char* const kSlopeShort[] = { "12", "24", "36", "48", "6", "18", "72", "96", "BRICK" };
// dB/oct de cada indice (Brickwall: 144 dB/oct en fase minima; en los modos lineales es una pared ideal)
inline int slopeDbPerOct (int slope)
{
    static const int db[] = { 12, 24, 36, 48, 6, 18, 72, 96, 144 };
    return db[std::clamp (slope, 0, kNumSlopes - 1)];
}

constexpr int kMaxSections = 12;
constexpr int kMaxChannels = 2;

struct BandSettings
{
    bool   on      = false;
    int    type    = 0;      // BandType
    double freq    = 1000.0; // Hz
    double gainDb  = 0.0;
    double q       = 0.707;
    int    slope   = 1;      // indice de pendiente (ver kSlopeNames): cortes y pasa-banda
    int    satType = 0;      // SatType
    double driveDb = 0.0;    // 0 = saturacion apagada
    double mix     = 1.0;    // 0..1

    int    stereo  = 0;      // StereoMode
    bool   dynOn   = false;  // EQ dinamica
    double thresholdDb = -24.0;
    double rangeDb = -6.0;   // cuanto cambia la ganancia al superar el umbral
    double attackMs = 10.0;
    double releaseMs = 120.0;
    bool   autoThr = false;  // threshold automatico (sigue el nivel promedio de la banda)
    bool   extSc   = false;  // detector escucha el sidechain externo
    bool   spectral = false; // dinamica espectral (la hace SpectralDynamics, no el filtro de la banda)
};

enum class StereoMode { Stereo = 0, Left, Right, Mid, Side };
inline const char* const kStereoNames[] = { "Stereo", "Left", "Right", "Mid", "Side" };
constexpr int kNumStereoModes = 5;

inline bool typeHasGain (int type)
{
    return type == (int) BandType::Bell || type == (int) BandType::LowShelf || type == (int) BandType::HighShelf || type == (int) BandType::TiltShelf;
}
inline bool typeHasSlope (int type)
{
    return type == (int) BandType::LowCut || type == (int) BandType::HighCut || type == (int) BandType::BandPass;
}

inline bool typeCanSaturate (int type) { return typeHasGain (type); }
inline bool typeCanBeDynamic (int type) { return typeHasGain (type); }

// Q de las secciones Butterworth para pendientes de 24/36/48 dB/oct (tabla historica: mismos valores de siempre)
inline double butterworthQ (int numSections, int index)
{
    static const double q2[] = { 0.5412, 1.3066 };
    static const double q3[] = { 0.5176, 0.7071, 1.9319 };
    static const double q4[] = { 0.5098, 0.6013, 0.9000, 2.5629 };
    switch (numSections)
    {
        case 2: return q2[index];
        case 3: return q3[index];
        case 4: return q4[index];
        default: return 0.7071;
    }
}

// Secciones Butterworth de un filtro de 'poles' polos: Q de cada biquad (de la menos a la mas resonante)
// y si sobra un polo real (seccion de primer orden). Para 2/4/6/8 polos usa la tabla historica.
struct ButterworthPlan { int biquads = 0; bool firstOrder = false; double q[12] {}; };
inline ButterworthPlan butterworthPlan (int poles)
{
    ButterworthPlan p;
    p.biquads = poles / 2; p.firstOrder = (poles % 2) != 0;
    if (! p.firstOrder && p.biquads >= 1 && p.biquads <= 4)
    {
        for (int i = 0; i < p.biquads; ++i) p.q[i] = p.biquads == 1 ? 0.7071 : butterworthQ (p.biquads, i);
        return p;
    }
    for (int k = 0; k < p.biquads; ++k)
    {
        // polos en angulos (2k+1)pi/(2N) desde el eje imaginario -> Q = 1 / (2 cos(theta))
        const double theta = kPi * (2.0 * k + 1.0 + (p.firstOrder ? 1.0 : 0.0)) / (2.0 * poles);   // de menos a mas resonante
        p.q[k] = 1.0 / (2.0 * std::cos (theta));
    }
    return p;
}

inline int cutPoles (int slope) { return slopeDbPerOct (slope) / 6; }

// Primer orden: pasa-altos (bilineal, sin error en altas) y pasa-bajos emparejado con el analogico en Nyquist
inline BiquadCoeffs makeFirstOrderHighPass (double fs, double f)
{
    f = safeFreq (f, fs);
    const double k = std::tan (kPi * f / fs);
    const double n = 1.0 / (1.0 + k);
    BiquadCoeffs c; c.b0 = n; c.b1 = -n; c.b2 = 0; c.a1 = (k - 1.0) * n; c.a2 = 0;
    return c;
}
inline BiquadCoeffs makeFirstOrderLowPass (double fs, double f)
{
    f = safeFreq (f, fs);
    const double p = std::exp (-2.0 * kPi * f / fs);                // polo en el mismo lugar que el analogico
    const double fn = fs * 0.5;
    const double mN = 1.0 / std::sqrt (1.0 + (fn / f) * (fn / f));   // magnitud analogica en Nyquist
    const double r = mN * (1.0 + p) / (1.0 - p);
    const double b = std::clamp ((1.0 - r) / (1.0 + r), -1.0, 1.0);
    const double kk = (1.0 - p) / (1.0 + b);                        // 0 dB en DC
    BiquadCoeffs c; c.b0 = kk; c.b1 = kk * b; c.b2 = 0; c.a1 = -p; c.a2 = 0;
    return c;
}

// Pasa-banda de m secciones iguales: la Q de cada una se ajusta para que el ancho total a -3 dB sea f/Q
inline int bandPassSections (int slope) { return std::clamp (slopeDbPerOct (slope) / 6, 1, kMaxSections); }
inline double bandPassSectionQ (double q, int m)
{
    return m <= 1 ? q : q / std::sqrt (std::pow (2.0, 1.0 / m) - 1.0);
}

// Calcula los biquads de una banda. Devuelve cuantas secciones usa.
// Limites de seguridad musicales:
//  - Ganancia total de una banda (fija + dinamica) entre -36 y +30 dB.
//  - Q de los shelf hasta 2: suficiente para el "bump" resonante clasico; mas alla el shelf
//    genera resonancias de +40 dB que no son musicales (el resto de los tipos usa la Q completa).
constexpr double kShelfMaxQ = 2.0;
inline double safeGain (double g) { return std::clamp (g, -36.0, 30.0); }

inline int designBand (const BandSettings& s, double fs, BiquadCoeffs* out)
{
    switch ((BandType) s.type)
    {
        case BandType::Bell:      out[0] = makeAnalogBell (fs, s.freq, s.q, safeGain (s.gainDb)); return 1;
        case BandType::LowShelf:  out[0] = makeLowShelf (fs, s.freq, std::min (s.q, kShelfMaxQ), safeGain (s.gainDb));  return 1;
        case BandType::HighShelf: out[0] = makeAnalogHighShelf (fs, s.freq, std::min (s.q, kShelfMaxQ), safeGain (s.gainDb)); return 1;
        case BandType::Notch:     out[0] = makeNotch (fs, s.freq, s.q);               return 1;
        case BandType::AllPass:   out[0] = makeAllPass (fs, s.freq, s.q);             return 1;
        case BandType::TiltShelf:
        {
            // Tilt: -g/2 abajo y +g/2 arriba, con el pivote en la frecuencia (0 dB ahi)
            const double g = safeGain (s.gainDb);
            out[0] = makeAnalogHighShelf (fs, s.freq, std::min (s.q, kShelfMaxQ), g);
            const double k = std::pow (10.0, -g / 40.0);
            out[0].b0 *= k; out[0].b1 *= k; out[0].b2 *= k;
            return 1;
        }
        case BandType::BandPass:
        {
            const int m = bandPassSections (s.slope);
            const double q = bandPassSectionQ (std::max (0.1, s.q), m);
            const auto c = makeAnalogBandPass (fs, s.freq, q);
            for (int i = 0; i < m; ++i) out[i] = c;
            return m;
        }
        case BandType::LowCut:
        case BandType::HighCut:
        {
            const bool lowCut = s.type == (int) BandType::LowCut;
            const auto plan = butterworthPlan (cutPoles (s.slope));
            int n = 0;
            if (plan.firstOrder) out[n++] = lowCut ? makeFirstOrderHighPass (fs, s.freq) : makeFirstOrderLowPass (fs, s.freq);
            for (int i = 0; i < plan.biquads; ++i)
            {
                // En 12 dB/oct la Q controla la resonancia; en el resto se
                // escala la seccion mas resonante para conservar el control.
                double q = (plan.biquads == 1 && ! plan.firstOrder) ? s.q : plan.q[i];
                if ((plan.biquads > 1 || plan.firstOrder) && i == plan.biquads - 1)
                    q *= s.q / 0.7071;
                out[n++] = lowCut ? makeHighPass (fs, s.freq, q) : makeAnalogLowPass (fs, s.freq, q);
            }
            return n;
        }
    }
    out[0] = BiquadCoeffs::identity();
    return 1;
}

// Filtro que aisla la zona del espectro que se va a saturar
inline BiquadCoeffs designSatRegion (const BandSettings& s, double fs)
{
    switch ((BandType) s.type)
    {
        case BandType::LowShelf:  return makeLowPass (fs, s.freq, 0.7071);
        case BandType::HighShelf:
        case BandType::TiltShelf: return makeHighPass (fs, s.freq, 0.7071);
        case BandType::Bell:
        case BandType::LowCut:
        case BandType::HighCut:
        case BandType::Notch:
        case BandType::BandPass:
        case BandType::AllPass:
        default:                  return makeBandPass (fs, s.freq, std::max (0.3, s.q));
    }
}

// Filtro de "solo": deja oir solo la zona que la banda esta tocando
inline BiquadCoeffs designListen (const BandSettings& s, double fs)
{
    switch ((BandType) s.type)
    {
        case BandType::LowShelf:  return makeLowPass (fs, s.freq, 0.7071);
        case BandType::HighShelf:
        case BandType::TiltShelf: return makeHighPass (fs, s.freq, 0.7071);
        case BandType::LowCut:    return makeLowPass (fs, s.freq, 0.7071);   // lo que se esta quitando
        case BandType::HighCut:   return makeHighPass (fs, s.freq, 0.7071);
        case BandType::Bell:
        case BandType::Notch:
        default:                  return makeBandPass (fs, s.freq, std::max (0.5, s.q));
    }
}

inline double magnitudeOfBand (const BandSettings& s, double freq, double fs)
{
    if (! s.on) return 1.0;
    BiquadCoeffs c[kMaxSections];
    const int n = designBand (s, fs, c);
    double m = 1.0;
    for (int i = 0; i < n; ++i)
        m *= c[i].magnitude (freq, fs);
    return m;
}

// Respuesta del EQ analogico ideal de la banda (la que se usa en el modo de fase lineal: la curva es
// exactamente la analogica, sin ninguna compresion cerca de Nyquist)
inline double analogMagnitudeOfBand (const BandSettings& s, double f)
{
    if (! s.on) return 1.0;
    using Cx = std::complex<double>;
    const double f0 = std::max (1.0, s.freq);
    const Cx sv (0.0, std::max (1.0e-6, f) / f0);
    switch ((BandType) s.type)
    {
        case BandType::Bell:      return std::pow (10.0, analogBellDb (std::max (1.0e-6, f), f0, s.q, safeGain (s.gainDb)) / 20.0);
        case BandType::HighShelf: return std::pow (10.0, analogHighShelfDb (std::max (1.0e-6, f), f0, std::min (s.q, kShelfMaxQ), safeGain (s.gainDb)) / 20.0);
        case BandType::LowShelf:
        {
            const double A = std::pow (10.0, safeGain (s.gainDb) / 40.0), sA = std::sqrt (A), q = std::min (s.q, kShelfMaxQ);
            return std::abs (A * (sv * sv + (sA / q) * sv + A) / (A * sv * sv + (sA / q) * sv + 1.0));
        }
        case BandType::Notch:     return std::abs ((sv * sv + 1.0) / (sv * sv + sv / s.q + 1.0));
        case BandType::AllPass:   return 1.0;
        case BandType::TiltShelf: return std::pow (10.0, (analogHighShelfDb (std::max (1.0e-6, f), f0, std::min (s.q, kShelfMaxQ), safeGain (s.gainDb)) - 0.5 * safeGain (s.gainDb)) / 20.0);
        case BandType::BandPass:
        {
            const int m = bandPassSections (s.slope);
            const double q = bandPassSectionQ (std::max (0.1, s.q), m);
            return std::pow (std::abs ((sv / q) / (sv * sv + sv / q + 1.0)), (double) m);
        }
        case BandType::LowCut:
        case BandType::HighCut:
        {
            const bool lowCut = s.type == (int) BandType::LowCut;
            if (s.slope == kSlopeBrick)
            {
                // Pared ideal (modos lineales): Butterworth de orden 64, plana hasta el corte y vertical despues
                const double x = lowCut ? f0 / std::max (1.0e-6, f) : std::max (1.0e-6, f) / f0;
                return 1.0 / std::sqrt (1.0 + std::pow (x, 128.0));
            }
            const auto plan = butterworthPlan (cutPoles (s.slope));
            double m = 1.0;
            if (plan.firstOrder) m *= std::abs ((lowCut ? sv : Cx (1.0)) / (sv + 1.0));
            for (int i = 0; i < plan.biquads; ++i)
            {
                double q = (plan.biquads == 1 && ! plan.firstOrder) ? s.q : plan.q[i];
                if ((plan.biquads > 1 || plan.firstOrder) && i == plan.biquads - 1) q *= s.q / 0.7071;
                const Cx d = sv * sv + sv / q + 1.0;
                m *= std::abs ((lowCut ? sv * sv : Cx (1.0)) / d);
            }
            return m;
        }
    }
    return 1.0;
}

// La curva de la banda tal como suena: la del filtro digital (fase minima) o la analogica (fase lineal)
inline double magnitudeOfBand (const BandSettings& s, double freq, double fs, bool linearPhase)
{
    return linearPhase ? analogMagnitudeOfBand (s, freq) : magnitudeOfBand (s, freq, fs);
}

// La misma curva para muchas frecuencias a la vez (para la pantalla): el filtro se disena una sola vez.
// Resultado en dB, limitado a -60 dB como la pantalla.
inline void responseOfBandDb (const BandSettings& s, const double* freqs, int n, double fs, bool linearPhase, float* outDb)
{
    if (! s.on) { std::fill (outDb, outDb + n, 0.0f); return; }
    if (linearPhase)
    {
        for (int i = 0; i < n; ++i)
            outDb[i] = (float) (20.0 * std::log10 (std::max (1.0e-3, analogMagnitudeOfBand (s, freqs[i]))));
        return;
    }
    BiquadCoeffs c[kMaxSections];
    const int ns = designBand (s, fs, c);
    for (int i = 0; i < n; ++i)
    {
        double m = 1.0;
        for (int k = 0; k < ns; ++k) m *= c[k].magnitude (freqs[i], fs);
        outDb[i] = (float) (20.0 * std::log10 (std::max (1.0e-3, m)));
    }
}

// Dos ajustes dan la misma curva (para no recalcular lo que no cambio)
inline bool sameCurve (const BandSettings& a, const BandSettings& b)
{
    if (a.on != b.on) return false;
    if (! a.on) return true;
    return a.type == b.type && a.freq == b.freq && a.gainDb == b.gainDb && a.q == b.q && a.slope == b.slope;
}

// Suavizado de un polo (se actualiza cada "tick" de control)
struct Smoother
{
    // Suavizado de 2 etapas (critico): arranca y frena suave, como una mano girando una perilla.
    // Asi un salto brusco de automatizacion no deja marca audible.
    double current = 0, mid = 0, target = 0, coeff = 0.5;

    void setTime (double tickRate, double seconds) { coeff = std::exp (-2.0 / (tickRate * seconds)); }
    void snap (double v) { current = mid = target = v; }
    bool isMoving() const
    {
        const double tol = 1.0e-6 * (1.0 + std::abs (target));
        return std::abs (current - target) > tol || std::abs (mid - target) > tol;
    }
    double tick()
    {
        mid = target + (mid - target) * coeff;
        current = mid + (current - mid) * coeff;
        if (! isMoving()) current = mid = target;
        return current;
    }
};

class EqBand
{
public:
    static constexpr int kTickSize = 16;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        const double tickRate = fs / kTickSize;
        structFade.setTime (fs / kTickSize, 0.004);
        satXfadeStepType = 1.0 / (fs * 0.03);   // fundido de 30 ms entre modos de saturacion
        mixSmoothCoeff = 1.0 - std::exp (-1.0 / (fs * 0.004));
        structFade.snap (1.0);
        // Ganancia, drive, mix y encendido: 30 ms (sin "zipper" aunque la automatizacion llegue a saltos).
        // Frecuencia y Q: 12 ms, para que siga LFOs rapidos (filtros de acid/techno a 1/16) casi completos
        // (a 8 Hz recorre el 92 % del barrido) sin agregar ruido por los saltos de bloque.
        for (auto* sm : { &gain, &driveDb, &mix, &onAmount })
            sm->setTime (tickRate, 0.03);
        for (auto* sm : { &logFreq, &logQ })
            sm->setTime (tickRate, 0.012);
        avgCoeff = 1.0 - std::exp (-1.0 / (tickRate * 2.0));
        for (int ch = 0; ch < kMaxChannels; ++ch)
            dc[(size_t) ch].prepare (fs);
        reset();
        firstUpdate = true;
    }

    void reset()
    {
        for (auto& chStates : states)
            for (auto& st : chStates) st.reset();
        for (auto& st : satStates) st.reset();
        for (auto& st : regionStates) st.reset();
        regionSnap = true;
        for (auto& sc : satChan) sc.reset();
        for (auto& sc : satChanOld) sc.reset();
        satXfade = 0.0;
        snapCoeffs = true;
        for (auto& st : detStates) st.reset();
        for (auto& d : dc) d.reset();
        env = 0.0;
        dynGainDb = 0.0;
    }

    // Llamar una vez por bloque con los parametros actuales
    void setTarget (const BandSettings& s)
    {
        if (firstUpdate)
        {
            logFreq.snap (std::log (s.freq));
            gain.snap (s.gainDb);
            logQ.snap (std::log (s.q));
            driveDb.snap (s.driveDb);
            mix.snap (s.mix);
            mixNow = s.mix;
            onAmount.snap (s.on ? 1.0 : 0.0);
            firstUpdate = false;
            dirty = true;
        }
        // Cambio de estructura (tipo, pendiente o canal): no se aplica de golpe. La banda se apaga en
        // unos ms, cambia con los filtros limpios y vuelve a entrar. Asi no hay saltos ni transitorios.
        const bool structural = s.type != applied.type || s.slope != applied.slope || s.stereo != applied.stereo;
        if (firstApply) { applied = s; firstApply = false; reset(); dirty = true; }
        else if (structural) { pendingStruct = true; pending = s; }

        if (s.satType != target.satType || s.dynOn != target.dynOn || s.spectral != target.spectral) dirty = true;
        if (s.extSc != target.extSc) for (auto& st : detStates) st.reset();

        target = s;
        target.type = applied.type; target.slope = applied.slope; target.stereo = applied.stereo;
        logFreq.target = std::log (s.freq);
        gain.target = s.gainDb;
        logQ.target = std::log (s.q);
        driveDb.target = typeCanSaturate (s.type) ? s.driveDb : 0.0;
        mix.target = s.mix;
        onAmount.target = s.on ? 1.0 : 0.0;
        if (s.on) sleeping = false;

        if (s.attackMs != lastAtkMs || s.releaseMs != lastRelMs || fs != lastCoeffFs)
        {
            attackCoeff = std::exp (-1.0 / (fs * std::max (0.1, s.attackMs) * 0.001));
            releaseCoeff = std::exp (-1.0 / (fs * std::max (1.0, s.releaseMs) * 0.001));
            lastAtkMs = s.attackMs; lastRelMs = s.releaseMs; lastCoeffFs = fs;
        }
    }

    bool isActive() const { return target.on || onAmount.current > 1.0e-4; }
    // Apagada, en silencio y sin cambios pendientes: no hace falta ni leer sus parametros
    bool isAsleep() const { return sleeping && ! target.on && onAmount.current <= 0.0 && ! onAmount.isMoving() && ! pendingStruct; }

    // Sidechain general: 0 = curva completa, 1 = plana (se aplica al siguiente tic, sin suavizado lento)
    void setDuck (double d) { duck = std::clamp (d, 0.0, 1.0); }
    bool isDynamic() const { return current.dynOn && ! current.spectral && typeCanBeDynamic (current.type); }

    // Ganancia dinamica aplicada ahora mismo (para dibujarla en la interfaz)
    double getDynamicGainDb() const { return dynGainDb; }
    // Nivel del detector de la banda en dB (para el medidor de threshold)
    double getDetectorDb() const { return 20.0 * std::log10 (std::max (env, 1.0e-9)); }
    // Threshold efectivo (el automatico o el del usuario)
    double getThresholdDb() const { return effThrDb; }

    // Avanza los suavizados y recalcula coeficientes si hace falta
    void controlTick()
    {
        // Banda apagada y ya en silencio: no hay nada que calcular (asi 16 bandas apagadas no cuestan nada)
        if (! target.on && onAmount.current <= 0.0 && ! onAmount.isMoving() && ! pendingStruct)
        {
            if (! sleeping) { reset(); dynGainDb = 0.0; sleeping = true; }
            return;
        }

        // Banda quieta (lo normal la mayor parte del tiempo): no hay nada que recalcular
        if (! interpolating && ! regionInterp && ! dirty && ! pendingStruct && ! (target.dynOn && ! target.spectral) && duck == duckApplied
            && ! logFreq.isMoving() && ! gain.isMoving() && ! logQ.isMoving() && ! driveDb.isMoving()
            && ! mix.isMoving() && ! onAmount.isMoving() && ! structFade.isMoving())
            return;

        // El tic anterior ya llego a su destino: fijar exacto y detener la interpolacion
        for (int i = 0; i < numSections; ++i) svfCur[(size_t) i] = svf[(size_t) i];
        interpolating = false;
        if (regionInterp) { regionCur = regionTgt; regionInterp = false; }

        const bool moving = logFreq.isMoving() || gain.isMoving() || logQ.isMoving()
                         || driveDb.isMoving() || mix.isMoving() || onAmount.isMoving();
        logFreq.tick(); gain.tick(); logQ.tick(); driveDb.tick(); mix.tick(); onAmount.tick();

        // Cambio de estructura con fundido: bajar, cambiar en silencio, subir
        structFade.target = pendingStruct ? 0.0 : 1.0;
        structFade.tick();
        if (pendingStruct && structFade.current < 1.0e-3)
        {
            applied.type = pending.type; applied.slope = pending.slope; applied.stereo = pending.stereo;
            target.type = applied.type; target.slope = applied.slope; target.stereo = applied.stereo;
            driveDb.target = typeCanSaturate (target.type) ? target.driveDb : 0.0;
            pendingStruct = false;
            reset();
            structFade.snap (0.0);
            dirty = true;
        }

        // EQ dinamica: la ganancia se mueve segun cuanto supera el umbral la senal de la banda
        double newDyn = 0.0;
        const bool dyn = target.dynOn && ! target.spectral && typeCanBeDynamic (target.type);
        if (dyn)
        {
            const double envDb = 20.0 * std::log10 (std::max (env, 1.0e-9));
            // Promedio lento del nivel (unos 2 s) para el threshold automatico
            if (envDb > -90.0) avgDb += (envDb - avgDb) * avgCoeff;
            effThrDb = target.autoThr ? avgDb + 3.0 : target.thresholdDb;
            const double over = envDb - effThrDb;
            const double amount = std::clamp (over / 12.0, 0.0, 1.0);   // rodilla de 12 dB
            newDyn = target.rangeDb * amount;
        }
        const bool dynChanged = std::abs (newDyn - dynGainDb) > 0.005 || std::abs (duck - duckApplied) > 1.0e-4;
        dynGainDb = newDyn;
        duckApplied = duck;

        if (! moving && ! dirty && ! dynChanged) return;
        dirty = false;

        current = target;
        current.freq = std::exp (logFreq.current);
        current.gainDb = gain.current * (1.0 - duckApplied) + dynGainDb;
        current.q = std::exp (logQ.current);
        current.driveDb = driveDb.current;
        current.mix = mix.current;

        const int prevSections = numSections;
        numSections = designBand (current, fs, coeffs.data());
        for (int i = 0; i < numSections; ++i) svf[(size_t) i] = SvfCoeffs::fromBiquad (coeffs[(size_t) i]);
        if (snapCoeffs || prevSections != numSections)
        {
            for (int i = 0; i < numSections; ++i) svfCur[(size_t) i] = svf[(size_t) i];
            snapCoeffs = false;
        }
        else
        {
            for (int i = 0; i < numSections; ++i) svfInc[(size_t) i] = svfLerpStep (svfCur[(size_t) i], svf[(size_t) i], (double) kTickSize);
            interpolating = true;
        }
        const bool wasEnabled = satEnabled;
        satEnabled = typeCanSaturate (current.type) && current.driveDb > 0.01 && current.mix > 0.001;
        // La zona a saturar se filtra solo si hay (o va a haber) drive: sin saturacion no cuesta nada. Al subir
        // el drive desde 0 la zona arranca limpia y el fundido de entrada del saturador evita cualquier clic.
        {
            const bool wanted = typeCanSaturate (current.type) && (driveDb.target > 0.01 || current.driveDb > 0.01);
            if (wanted && ! satRegionOn) { for (auto& st : regionStates) st.reset(); regionSnap = true; }
            satRegionOn = wanted;
        }
        if (satEnabled && ! wasEnabled)
        {
            // La saturacion se vuelve a encender: arranca limpia (sin estados viejos ni fundidos pendientes)
            for (auto& sc : satChan) sc.reset();
            for (auto& sc : satChanOld) sc.reset();
            for (auto& d : dc) d.reset();
            satXfade = 0.0;
            lastSatType = -1;
        }
        if (satRegionOn || dyn)
        {
            satCoeffs = designSatRegion (current, fs);   // la misma zona sirve de detector
            const auto tgt = SvfCoeffs::fromBiquad (satCoeffs);
            if (regionSnap) { regionCur = tgt; regionInc = {}; regionInc.a1 = regionInc.m0 = 0.0; regionSnap = false; regionInterp = false; }
            else { regionInc = svfLerpStep (regionCur, tgt, (double) kTickSize); regionInterp = true; }
            regionTgt = tgt;
        }
        if (satEnabled)
        {
            // Cambio de modo de saturacion: el modo anterior sigue sonando y se funde con el nuevo (sin clics)
            if (lastSatType >= 0 && lastSatType != current.satType)
            {
                // Cambio de modo: fundido de 30 ms entre el modo viejo y el nuevo
                satOld = saturator;
                satChanOld = satChan;
                satXfade = 1.0;
                satXfadeStep = satXfadeStepType;
            }
            else if (lastSatType >= 0 && std::abs (current.driveDb - lastDrive) > 1.0e-4 && satXfade <= 0.0)
            {
                // El drive se esta moviendo: cada paso se funde con el anterior a lo largo del tic (sin "zipper")
                satOld = saturator;
                satChanOld = satChan;
                satXfade = 1.0;
                satXfadeStep = 1.0 / kTickSize;
            }
            lastSatType = current.satType;
            lastDrive = current.driveDb;
            saturator.setup (fs, (SatType) current.satType, current.driveDb);
        }
        else lastSatType = -1;
        if (! isActive()) reset();
    }

    // Procesa un "frame" estereo (l, r) segun el modo de canal de la banda
    // sc: muestra (mono) del sidechain externo, o nullptr si no hay
    inline void processFrame (double& l, double& r, bool stereoInput, const double* sc = nullptr)
    {
        if (interpolating)
            for (int i = 0; i < numSections; ++i) svfAdd (svfCur[(size_t) i], svfInc[(size_t) i]);
        if (regionInterp) svfAdd (regionCur, regionInc);
        if (satXfade > 0.0) satXfade = std::max (0.0, satXfade - satXfadeStep);   // avanza una vez por muestra
        mixNow += (current.mix - mixNow) * mixSmoothCoeff;                       // MIX suave muestra a muestra
        const double amt = onAmount.current;
        if (amt <= 1.0e-4) return;

        // Detector con sidechain externo: escucha la zona de la banda en la senal externa
        const bool ext = sc != nullptr && target.extSc && isDynamic();
        if (ext) detect (std::abs (detect1 (0, *sc)));
        const bool dynInt = isDynamic() && ! ext;

        const int mode = stereoInput ? target.stereo : (int) StereoMode::Stereo;
        switch ((StereoMode) mode)
        {
            case StereoMode::Stereo:
                if (dynInt) detect (stereoInput ? std::max (std::abs (detect1 (0, l)), std::abs (detect1 (1, r))) : std::abs (detect1 (0, l)));
                l = processChannel (0, l, amt);
                if (stereoInput) r = processChannel (1, r, amt);
                break;
            case StereoMode::Left:
                if (dynInt) detect (std::abs (detect1 (0, l)));
                l = processChannel (0, l, amt);
                break;
            case StereoMode::Right:
                if (dynInt) detect (std::abs (detect1 (1, r)));
                r = processChannel (1, r, amt);
                break;
            case StereoMode::Mid:
            case StereoMode::Side:
            {
                double m = 0.5 * (l + r), s = 0.5 * (l - r);
                double& target_ = mode == (int) StereoMode::Mid ? m : s;
                if (dynInt) detect (std::abs (detect1 (0, target_)));
                target_ = processChannel (0, target_, amt);
                l = m + s;
                r = m - s;
                break;
            }
        }
    }

    // Un tramo de muestras (dentro de un mismo tic): l/r en doble precision, sc = sidechain (o nullptr)
    inline void processSpan (double* l, double* r, int n, bool stereoInput, const float* sc)
    {
        // Camino rapido (el caso de casi todas las bandas, casi todo el tiempo): banda quieta, sin saturacion ni
        // dinamica. Cada seccion procesa el tramo entero con su estado en registros y L/R intercalados.
        const bool fast = ! interpolating && ! regionInterp && ! satRegionOn && ! satEnabled && satXfade <= 0.0
                       && ! isDynamic() && onAmount.current >= 1.0 && structFade.current >= 1.0;
        if (fast)
        {
            mixNow = current.mix;
            const int mode = stereoInput ? target.stereo : (int) StereoMode::Stereo;
            if (mode == (int) StereoMode::Stereo)
            {
                if (stereoInput) runSectionsStereo (l, r, n); else runSections (0, l, n);
            }
            else if (mode == (int) StereoMode::Left)  runSections (0, l, n);
            else if (mode == (int) StereoMode::Right) runSections (1, r, n);
            else
            {
                double m[kTickSize], sd[kTickSize];
                for (int j = 0; j < n; ++j) { m[j] = 0.5 * (l[j] + r[j]); sd[j] = 0.5 * (l[j] - r[j]); }
                runSections (0, mode == (int) StereoMode::Mid ? m : sd, n);
                for (int j = 0; j < n; ++j) { l[j] = m[j] + sd[j]; r[j] = m[j] - sd[j]; }
            }
            return;
        }
        if (sc == nullptr) { for (int j = 0; j < n; ++j) processFrame (l[j], r[j], stereoInput, nullptr); return; }
        for (int j = 0; j < n; ++j) { const double v = sc[j]; processFrame (l[j], r[j], stereoInput, &v); }
    }

    // Version mono (para pruebas)
    inline double processSample (int, double x)
    {
        double r = 0.0;
        processFrame (x, r, false);
        return x;
    }

private:
    // Todas las secciones de la banda sobre un tramo (coeficientes quietos), un canal
    inline void runSections (int ch, double* x, int n)
    {
        for (int i = 0; i < numSections; ++i)
        {
            auto& st = states[(size_t) ch][(size_t) i];
            const SvfCoeffs c = svfCur[(size_t) i];
            double ic1 = st.ic1, ic2 = st.ic2;
            for (int j = 0; j < n; ++j)
            {
                const double v0 = x[j], v3 = v0 - ic2;
                const double v1 = c.a1 * ic1 + c.a2 * v3, v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
                ic1 = 2.0 * v1 - ic1; ic2 = 2.0 * v2 - ic2;
                x[j] = c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
            }
            st.ic1 = ic1; st.ic2 = ic2;
        }
    }
    // Lo mismo con L y R intercalados (dos cadenas independientes: el procesador las corre en paralelo)
    inline void runSectionsStereo (double* l, double* r, int n)
    {
        for (int i = 0; i < numSections; ++i)
        {
            auto& sl = states[0][(size_t) i];
            auto& sr = states[1][(size_t) i];
            const SvfCoeffs c = svfCur[(size_t) i];
            double a1 = sl.ic1, a2 = sl.ic2, b1 = sr.ic1, b2 = sr.ic2;
            for (int j = 0; j < n; ++j)
            {
                const double x0 = l[j], y0 = r[j];
                const double x3 = x0 - a2, y3 = y0 - b2;
                const double x1 = c.a1 * a1 + c.a2 * x3, y1 = c.a1 * b1 + c.a2 * y3;
                const double x2 = a2 + c.a2 * a1 + c.a3 * x3, y2 = b2 + c.a2 * b1 + c.a3 * y3;
                a1 = 2.0 * x1 - a1; a2 = 2.0 * x2 - a2; b1 = 2.0 * y1 - b1; b2 = 2.0 * y2 - b2;
                l[j] = c.m0 * x0 + c.m1 * x1 + c.m2 * x2;
                r[j] = c.m0 * y0 + c.m1 * y1 + c.m2 * y2;
            }
            sl.ic1 = a1; sl.ic2 = a2; sr.ic1 = b1; sr.ic2 = b2;
        }
    }

    inline double detect1 (int ch, double x) { return detStates[(size_t) ch].process (x, satCoeffs); }

    inline void detect (double level)
    {
        const double c = level > env ? attackCoeff : releaseCoeff;
        env = level + (env - level) * c;
    }

    inline double processChannel (int ch, double x, double amt)
    {
        double y = x;
        for (int i = 0; i < numSections; ++i)
            y = states[(size_t) ch][(size_t) i].process (y, svfCur[(size_t) i]);

        double region = 0.0;
        if (satRegionOn) region = regionStates[(size_t) ch].process (y, regionCur);   // siempre al dia (SVF suave)
        if (satEnabled)
        {
            // Aislar la zona de la banda, saturarla y sumar solo los armonicos generados
            double delta = saturator.process (satChan[(size_t) ch], region);
            if (satXfade > 0.0)
            {
                const double dOld = satOld.process (satChanOld[(size_t) ch], region);
                const double w = satXfade * satXfade * (3.0 - 2.0 * satXfade);   // curva en S
                delta += w * (dOld - delta);
            }
            delta = dc[(size_t) ch].process (delta);
            y += mixNow * delta;
            satAcc += mixNow * mixNow * delta * delta;   // cuanta saturacion real hay (para la calavera)
        }
        return x + amt * structFade.current * (y - x);
    }

    double fs = 44100.0;
    BandSettings target, current;
    Smoother logFreq, gain, logQ, driveDb, mix, onAmount, structFade;
    BandSettings applied, pending;
    bool pendingStruct = false, firstApply = true;
    bool dirty = true, firstUpdate = true, satEnabled = false, sleeping = false;
    double driveLin = 1.0;

    double env = 0.0, dynGainDb = 0.0, attackCoeff = 0.99, releaseCoeff = 0.999;
    double duck = 0.0, duckApplied = 0.0;
    double lastAtkMs = -1.0, lastRelMs = -1.0, lastCoeffFs = -1.0;
    double avgDb = -40.0, avgCoeff = 0.001, effThrDb = -24.0;

    std::array<BiquadCoeffs, kMaxSections> coeffs {};
    int numSections = 1;
    std::array<std::array<SvfState, kMaxSections>, kMaxChannels> states {};   // SVF: estable al automatizar
    std::array<SvfCoeffs, kMaxSections> svf {}, svfCur {}, svfInc {};
    bool interpolating = false, snapCoeffs = true;
    SvfCoeffs regionCur, regionTgt, regionInc;
    std::array<SvfState, kMaxChannels> regionStates {};
    bool regionInterp = false, regionSnap = true;

    BiquadCoeffs satCoeffs;
    std::array<BiquadState, kMaxChannels> satStates {}, detStates {};
    Saturator saturator, satOld;
    std::array<SatChannelState, kMaxChannels> satChanOld {};
    double satXfade = 0.0, satXfadeStep = 0.001, satXfadeStepType = 0.001, lastDrive = 0.0;
    double mixNow = 1.0, mixSmoothCoeff = 0.01;
    bool satRegionOn = false;
    int lastSatType = -1;
public:
    // Energia media de la saturacion desde la ultima lectura (por muestra y canal)
    double takeSatEnergy (int samplesTimesChannels)
    {
        const double v = satAcc / std::max (1, samplesTimesChannels);
        satAcc = 0.0;
        return v;
    }
private:
    double satAcc = 0.0;
    std::array<SatChannelState, kMaxChannels> satChan {};
    std::array<DcBlocker, kMaxChannels> dc {};
};
} // namespace gaux
