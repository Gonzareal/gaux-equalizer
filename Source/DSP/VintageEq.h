// Gaux Equalizer - Modo Vintage: rack de 3 unidades inspirado en equipos pasivos de tubo clasicos
//  PROGRAM (graves con el truco boost+atten, agudos con ancho, soften, air, tubo y salida)
//  MID     (realce de medios-graves, recorte de medios, realce de medios-agudos)
//  FILTER  (pasa altos y pasa bajos pasivos de 12 dB/oct)
//  + transformador de entrada (IRON) y valvula (GLOW)
#pragma once

#include "EqBand.h"

namespace gaux
{
inline const double kVLowFreqs[]   = { 20, 30, 60, 100 };
inline const char* const kVLowFreqNames[] = { "20", "30", "60", "100" };
constexpr int kNumVLowFreqs = 4;

inline const double kVHighFreqs[]  = { 3000, 4000, 5000, 8000, 10000, 12000, 16000 };
inline const char* const kVHighFreqNames[] = { "3k", "4k", "5k", "8k", "10k", "12k", "16k" };
constexpr int kNumVHighFreqs = 7;

inline const double kVAttenFreqs[] = { 5000, 10000, 20000 };
inline const char* const kVAttenFreqNames[] = { "5k", "10k", "20k" };
constexpr int kNumVAttenFreqs = 3;

inline const double kVAirFreqs[]   = { 2500, 5000, 10000, 20000, 40000 };
inline const char* const kVAirFreqNames[] = { "2.5k", "5k", "10k", "20k", "40k" };
constexpr int kNumVAirFreqs = 5;

// Unidad MID
inline const double kVLmFreqs[]  = { 200, 300, 500, 700, 1000 };
inline const char* const kVLmFreqNames[] = { "200", "300", "500", "700", "1k" };
constexpr int kNumVLmFreqs = 5;
inline const double kVDipFreqs[] = { 200, 300, 500, 700, 1000, 1500, 2000, 3000, 4000, 5000, 7000 };
inline const char* const kVDipFreqNames[] = { "200", "300", "500", "700", "1k", "1.5k", "2k", "3k", "4k", "5k", "7k" };
constexpr int kNumVDipFreqs = 11;
inline const double kVHmFreqs[]  = { 1500, 2000, 3000, 4000, 5000 };
inline const char* const kVHmFreqNames[] = { "1.5k", "2k", "3k", "4k", "5k" };
constexpr int kNumVHmFreqs = 5;

// Unidad FILTER (0 = OFF en el pasa altos; el ultimo = OFF en el pasa bajos)
inline const double kVLowCuts[]  = { 0, 50, 80, 100, 150, 250, 500, 750, 1000, 1500, 2000 };
inline const char* const kVLowCutNames[] = { "OFF", "50", "80", "100", "150", "250", "500", "750", "1k", "1.5k", "2k" };
constexpr int kNumVLowCuts = 11;
inline const double kVHighCuts[] = { 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 15000, 0 };
inline const char* const kVHighCutNames[] = { "1.5k", "2k", "3k", "4k", "5k", "6k", "8k", "10k", "12k", "15k", "OFF" };
constexpr int kNumVHighCuts = 11;

struct VintageSettings
{
    int    lowFreq = 2;      // indice
    double lowBoost = 0;     // 0..10
    double lowAtten = 0;     // 0..10
    int    highFreq = 4;
    double highBoost = 0;    // 0..10
    double highBandwidth = 5;// 0 (estrecho) .. 10 (ancho)
    int    attenFreq = 1;
    double highAtten = 0;    // 0..10
    int    airFreq = 3;
    double airGain = 0;      // 0..10 dB
    double tube = 0;         // 0..100 %

    // MID
    int    lmFreq = 1;  double lmBoost = 0;   // 0..10
    int    dipFreq = 4; double dip = 0;       // 0..10
    int    hmFreq = 2;  double hmBoost = 0;   // 0..10
    // FILTER
    int    lowCut = 0;                        // 0 = OFF
    int    highCut = kNumVHighCuts - 1;       // ultimo = OFF
    // Transformador de entrada
    double iron = 0;                          // 0..100 %
    // Interruptores IN de cada unidad
    bool   programIn = true, midIn = true, filterIn = true;
};

constexpr int kVintageSections = 11;

inline int designVintage (const VintageSettings& v, double fs, BiquadCoeffs* out)
{
    const double lowF = kVLowFreqs[std::clamp (v.lowFreq, 0, kNumVLowFreqs - 1)];
    const double highF = kVHighFreqs[std::clamp (v.highFreq, 0, kNumVHighFreqs - 1)];
    const double attF = kVAttenFreqs[std::clamp (v.attenFreq, 0, kNumVAttenFreqs - 1)];
    const double airF = kVAirFreqs[std::clamp (v.airFreq, 0, kNumVAirFreqs - 1)];

    // Todas las secciones se disenan "emparejadas con el analogico": la curva coincide con la
    // del circuito pasivo original hasta los 20 kHz, sin el aplastamiento de la bilineal.

    // PROGRAM - graves. Como en el equipo pasivo original: el realce es un shelf con un leve
    // pico de resonancia y su punto de giro mas abajo; la atenuacion es un shelf mas ancho y
    // con el giro mas arriba. Juntos dan la "joroba" en la frecuencia elegida y el hueco encima.
    //  Realce max +13.5 dB, atenuacion max -17.5 dB (valores del equipo clasico).
    out[0] = makeMatchedLowShelf (fs, lowF * 2.0, 0.90, v.lowBoost * 1.35);
    out[1] = makeMatchedLowShelf (fs, lowF * 1.5, 0.45, -v.lowAtten * 1.75);

    // Agudos: campana de hasta +18 dB con ancho variable (SHARP Q 2.5 .. BROAD Q 0.4)
    const double q = 2.5 * std::pow (0.16, v.highBandwidth / 10.0);
    out[2] = makeMatchedBell (fs, highF, q, v.highBoost * 1.8);

    // Atenuacion de agudos (SOFTEN): shelf de hasta -16 dB
    out[3] = makeAnalogHighShelf (fs, attF, 0.5, -v.highAtten * 1.6);

    // Banda de aire: shelf muy suave y abierto
    out[4] = makeAnalogHighShelf (fs, airF, 0.4, v.airGain);

    // MID: campanas anchas y musicales (el realce se ensancha un poco al subirlo, como en los pasivos)
    const double lmF = kVLmFreqs[std::clamp (v.lmFreq, 0, kNumVLmFreqs - 1)];
    const double dipF = kVDipFreqs[std::clamp (v.dipFreq, 0, kNumVDipFreqs - 1)];
    const double hmF = kVHmFreqs[std::clamp (v.hmFreq, 0, kNumVHmFreqs - 1)];
    out[5] = makeMatchedBell (fs, lmF, 0.9 - 0.03 * v.lmBoost, v.lmBoost * 1.0);
    out[6] = makeMatchedBell (fs, dipF, 1.1, -v.dip * 1.0);
    out[7] = makeMatchedBell (fs, hmF, 1.0 - 0.03 * v.hmBoost, v.hmBoost * 1.0);

    // FILTER: 12 dB/oct, Q suave de filtro pasivo
    const double lc = kVLowCuts[std::clamp (v.lowCut, 0, kNumVLowCuts - 1)];
    const double hc = kVHighCuts[std::clamp (v.highCut, 0, kNumVHighCuts - 1)];
    out[8] = (v.filterIn && lc > 0) ? makeMatchedHighPass (fs, lc, 0.62) : BiquadCoeffs {};
    out[9] = (v.filterIn && hc > 0) ? makeMatchedLowPass (fs, hc, 0.62) : BiquadCoeffs {};

    // Acople por transformador del equipo original: cuando se realzan graves no deja pasar DC ni subsonicos
    // (pasa altos de 1er orden hasta 3.5 Hz: -0.13 dB en 20 Hz). Asi el realce nunca amplifica un DC de la senal.
    // Con LOW BOOST en 0 desaparece del todo (el Vintage en cero sigue siendo 100 % transparente).
    out[10] = BiquadCoeffs {};
    if (const double fc = 3.5 * std::clamp (v.lowBoost / 2.0, 0.0, 1.0); fc > 0.01)
    {
        const double K = std::tan (kPi * fc / fs);
        BiquadCoeffs t; t.b0 = 1.0 / (1.0 + K); t.b1 = -t.b0; t.b2 = 0.0; t.a1 = (K - 1.0) / (K + 1.0); t.a2 = 0.0;
        out[10] = t;
    }
    return kVintageSections;
}

// Ajustes tal como suenan: con IN apagado, la unidad PROGRAM o MID queda plana
inline VintageSettings effectiveVintage (VintageSettings v)
{
    if (! v.programIn) { v.lowBoost = v.lowAtten = v.highBoost = v.highAtten = v.airGain = 0.0; }
    if (! v.midIn)     { v.lmBoost = v.dip = v.hmBoost = 0.0; }
    return v;
}

// Respuesta de la curva (lo que dibuja la pantalla): usa exactamente el mismo diseno que el audio
inline double magnitudeOfVintage (const VintageSettings& vIn, double freq, double fs)
{
    const auto v = effectiveVintage (vIn);
    BiquadCoeffs c[kVintageSections];
    const int n = designVintage (v, fs, c);
    double m = 1.0;
    for (int i = 0; i < n; ++i)
        m *= c[i].magnitude (freq, fs);
    return m;
}

// La curva completa para muchas frecuencias a la vez (pantalla): se disena una sola vez. En dB (min -60).
inline void responseOfVintageDb (const VintageSettings& vIn, const double* freqs, int n, double fs, float* outDb)
{
    const auto v = effectiveVintage (vIn);
    BiquadCoeffs c[kVintageSections];
    const int ns = designVintage (v, fs, c);
    for (int i = 0; i < n; ++i)
    {
        double m = 1.0;
        for (int k = 0; k < ns; ++k) m *= c[k].magnitude (freqs[i], fs);
        outDb[i] = (float) (20.0 * std::log10 (std::max (1.0e-3, m)));
    }
}

inline bool sameVintage (const VintageSettings& a, const VintageSettings& b)
{
    return a.lowFreq == b.lowFreq && a.lowBoost == b.lowBoost && a.lowAtten == b.lowAtten && a.highFreq == b.highFreq
        && a.highBoost == b.highBoost && a.highBandwidth == b.highBandwidth && a.attenFreq == b.attenFreq
        && a.highAtten == b.highAtten && a.airFreq == b.airFreq && a.airGain == b.airGain && a.tube == b.tube
        && a.lmFreq == b.lmFreq && a.lmBoost == b.lmBoost && a.dipFreq == b.dipFreq && a.dip == b.dip
        && a.hmFreq == b.hmFreq && a.hmBoost == b.hmBoost && a.lowCut == b.lowCut && a.highCut == b.highCut
        && a.iron == b.iron && a.programIn == b.programIn && a.midIn == b.midIn && a.filterIn == b.filterIn;
}

class VintageEq
{
public:
    static constexpr int kTickSize = 16;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        const double tickRate = fs / kTickSize;
        for (auto& sm : smooth) sm.setTime (tickRate, 0.04);
        for (auto& d : dc) d.prepare (fs);
        for (auto& d : dcIron) d.prepare (fs);
        xfadeStep = 1.0 / (fs * 0.035);   // 35 ms (avanza muestra a muestra)
        ironCoeff = 1.0 - std::exp (-2.0 * kPi * 180.0 / fs);
        reset();
        firstUpdate = true;
    }

    void reset()
    {
        for (auto& chStates : states)
            for (auto& st : chStates) st.reset();
        xfade = 0.0; pendingXfade = false; snapCoeffs = true;
        for (auto& t : tubeState) t.reset();
        for (auto& d : dc) d.reset();
        for (auto& d : dcIron) d.reset();
        ironLp.fill (0.0);
    }

    void setTarget (const VintageSettings& v)
    {
        // Con IN apagado, las ganancias de esa unidad van a 0 suavemente (sin clics)
        const auto e = effectiveVintage (v);
        const double vals[] = { e.lowBoost, e.lowAtten, e.highBoost, e.highBandwidth, e.highAtten, e.airGain, e.tube,
                                e.lmBoost, e.dip, e.hmBoost, e.iron };
        for (int i = 0; i < kNumSmoothed; ++i)
        {
            if (firstUpdate) smooth[(size_t) i].snap (vals[i]);
            smooth[(size_t) i].target = vals[i];
        }
        const bool selectorChanged = v.lowFreq != target.lowFreq || v.highFreq != target.highFreq
            || v.attenFreq != target.attenFreq || v.airFreq != target.airFreq
            || v.lmFreq != target.lmFreq || v.dipFreq != target.dipFreq || v.hmFreq != target.hmFreq
            || v.lowCut != target.lowCut || v.highCut != target.highCut || v.filterIn != target.filterIn;
        if (firstUpdate || selectorChanged) dirty = true;
        if (selectorChanged && ! firstUpdate) pendingXfade = true;   // los selectores cambian con fundido, sin clics
        firstUpdate = false;
        target = v;
    }

    // Sidechain general: 0 = curva completa, 1 = plana
    void setDuck (double d) { duck = std::clamp (d, 0.0, 1.0); }

    void controlTick()
    {
        for (int i = 0; i < kVintageSections; ++i) svfCur[(size_t) i] = svf[(size_t) i];
        interpolating = false;
        // Fundido entre la curva vieja y la nueva cuando se gira un selector
        if (pendingXfade)
        {
            pendingXfade = false;
            coeffsOld = svfCur;
            statesOld = states;
            // Arranca por encima de 1: la curva nueva corre unos ms en silencio (asi su transitorio
            // de arranque se apaga antes de oirse) y despues entra con el fundido.
            xfade = 1.25;
        }

        bool moving = dirty || std::abs (duck - duckApplied) > 1.0e-4;
        for (auto& sm : smooth) { moving = moving || sm.isMoving(); sm.tick(); }
        if (! moving) return;
        dirty = false;
        duckApplied = duck;
        const double cv = 1.0 - duckApplied;   // las ganancias se aplanan con el sidechain general

        current = target;
        current.lowBoost = smooth[0].current * cv;
        current.lowAtten = smooth[1].current * cv;
        current.highBoost = smooth[2].current * cv;
        current.highBandwidth = smooth[3].current;
        current.highAtten = smooth[4].current * cv;
        current.airGain = smooth[5].current * cv;
        current.tube = smooth[6].current;
        current.lmBoost = smooth[7].current * cv;
        current.dip = smooth[8].current * cv;
        current.hmBoost = smooth[9].current * cv;
        current.iron = smooth[10].current;

        designVintage (current, fs, coeffs.data());
        for (int i = 0; i < kVintageSections; ++i)
        {
            const auto& c = coeffs[(size_t) i];
            svf[(size_t) i] = SvfCoeffs::fromBiquad (c);
            // seccion plana: numerador == denominador (la perilla en 0, filtro apagado)
            flat[(size_t) i] = std::abs (c.b0 - 1.0) < 1.0e-12 && std::abs (c.b1 - c.a1) < 1.0e-12 && std::abs (c.b2 - c.a2) < 1.0e-12;
        }
        if (snapCoeffs) { svfCur = svf; snapCoeffs = false; }
        else
        {
            for (int i = 0; i < kVintageSections; ++i) svfInc[(size_t) i] = svfLerpStep (svfCur[(size_t) i], svf[(size_t) i], (double) kTickSize);
            interpolating = true;
        }
        // Iron: hasta +9 dB de drive en el transformador
        ironDrive = std::pow (10.0, (current.iron / 100.0) * 9.0 / 20.0);
        // Tube: hasta +18 dB de drive en la etapa de valvula
        tubeDrive = std::pow (10.0, (current.tube / 100.0) * 18.0 / 20.0);
        // Etapa de valvula de salida: el mismo motor TUBE de las bandas (2do armonico, nivel compensado, ADAA)
        tubeSat.setup (fs, SatType::Tube, (current.tube / 100.0) * 15.0);
    }

    inline double processSample (int ch, double x)
    {
        if (ch == 0 && interpolating)
            for (int i = 0; i < kVintageSections; ++i) svfAdd (svfCur[(size_t) i], svfInc[(size_t) i]);
        double y = x;
        if (current.iron > 0.05) y = ironStep (ch, y);
        for (int i = 0; i < kVintageSections; ++i)
            y = states[(size_t) ch][(size_t) i].process (y, svfCur[(size_t) i]);
        if (xfade > 0.0)
        {
            if (ch == 0) xfade = std::max (0.0, xfade - xfadeStep);   // el canal 0 marca el paso del tiempo
            double yo = x;
            for (int i = 0; i < kVintageSections; ++i)
                yo = statesOld[(size_t) ch][(size_t) i].process (yo, coeffsOld[(size_t) i]);
            // curva en "S" para que el fundido sea imperceptible
            const double xf = std::min (1.0, xfade);
            const double w = xf * xf * (3.0 - 2.0 * xf);
            y = y + w * (yo - y);
        }
        if (current.tube > 0.05) y = glowStep (ch, y);
        return y;
    }

    // Un tramo (dentro de un tic). Con las perillas quietas y sin selector cambiando, cada seccion corre el
    // tramo entero con su estado en registros y L/R intercalados; las secciones planas no se calculan.
    inline void processSpan (double* l, double* r, int n, bool stereo)
    {
        if (interpolating || xfade > 0.0)
        {
            for (int j = 0; j < n; ++j) { l[j] = processSample (0, l[j]); if (stereo) r[j] = processSample (1, r[j]); }
            return;
        }
        if (current.iron > 0.05)
            for (int j = 0; j < n; ++j) { l[j] = ironStep (0, l[j]); if (stereo) r[j] = ironStep (1, r[j]); }
        for (int i = 0; i < kVintageSections; ++i)
        {
            if (flat[(size_t) i]) { states[0][(size_t) i].reset(); states[1][(size_t) i].reset(); continue; }
            const SvfCoeffs c = svfCur[(size_t) i];
            auto& sl = states[0][(size_t) i];
            auto& sr = states[1][(size_t) i];
            double a1 = sl.ic1, a2 = sl.ic2, b1 = sr.ic1, b2 = sr.ic2;
            if (stereo)
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
            else
                for (int j = 0; j < n; ++j)
                {
                    const double x0 = l[j], x3 = x0 - a2;
                    const double x1 = c.a1 * a1 + c.a2 * x3, x2 = a2 + c.a2 * a1 + c.a3 * x3;
                    a1 = 2.0 * x1 - a1; a2 = 2.0 * x2 - a2;
                    l[j] = c.m0 * x0 + c.m1 * x1 + c.m2 * x2;
                }
            sl.ic1 = a1; sl.ic2 = a2; sr.ic1 = b1; sr.ic2 = b2;
        }
        if (current.tube > 0.05)
            for (int j = 0; j < n; ++j) { l[j] = glowStep (0, l[j]); if (stereo) r[j] = glowStep (1, r[j]); }
    }

    double takeSatEnergy (int samplesTimesChannels)
    {
        const double r = satAcc / std::max (1, samplesTimesChannels);
        satAcc = 0.0;
        return r;
    }

private:
    // Transformador de entrada: satura sobre todo los graves (el nucleo se "llena" con la energia
    // de baja frecuencia) y agrega armonicos pares suaves; los agudos casi no se tocan
    inline double ironStep (int ch, double y)
    {
        double& lp = ironLp[(size_t) ch];
        lp += ironCoeff * (y - lp);
        const double sat = saturateNormalised (lp + 0.08 * lp * lp, ironDrive, SatType::Tape);
        const double amt = 0.6 * std::min (1.0, current.iron / 20.0);
        const double d = amt * dcIron[(size_t) ch].process (sat - lp);
        satAcc += d * d;
        return y + d;
    }
    // Etapa de valvula: aporta armonicos pares y cuerpo; el nivel se mantiene.
    // Calibrada como una etapa de tubo real: limpia con senales bajas (a -18 dBFS, GLOW 50 % da
    // ~1 % de distorsion) y cada vez mas color cuanto mas fuerte entra (gain staging analogico)
    inline double glowStep (int ch, double y)
    {
        const double amt = 0.3 * std::min (1.0, current.tube / 25.0);
        const double d = amt * dc[(size_t) ch].process (tubeSat.process (tubeState[(size_t) ch], y));
        satAcc += d * d;
        return y + d;
    }

    double satAcc = 0.0;
    std::array<bool, kVintageSections> flat {};   // secciones planas (no hacen nada): se saltean
    static constexpr int kNumSmoothed = 11;
    double fs = 44100.0, tubeDrive = 1.0, ironDrive = 1.0, ironCoeff = 0.02;
    std::array<double, kMaxChannels> ironLp {};
    std::array<DcBlocker, kMaxChannels> dcIron {};
    Saturator tubeSat;
    std::array<SatChannelState, kMaxChannels> tubeState {};
    VintageSettings target, current;
    double duck = 0.0, duckApplied = 0.0;
    std::array<Smoother, kNumSmoothed> smooth {};
    bool dirty = true, firstUpdate = true;
    std::array<BiquadCoeffs, kVintageSections> coeffs {};
    std::array<SvfCoeffs, kVintageSections> svf {}, coeffsOld {}, svfCur {}, svfInc {};
    bool interpolating = false, snapCoeffs = true;
    std::array<std::array<SvfState, kVintageSections>, kMaxChannels> statesOld {};
    double xfade = 0.0, xfadeStep = 0.01;
    bool pendingXfade = false;
    std::array<std::array<SvfState, kVintageSections>, kMaxChannels> states {};
    std::array<DcBlocker, kMaxChannels> dc {};
};
} // namespace gaux
