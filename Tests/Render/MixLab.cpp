// Gaux Equalizer - MIXLAB: estudio virtual de mezcla y mastering.
// Genera sesiones sinteticas de muchos generos (electronica, pop, rock, reggaeton, jazz, orquesta, trap,
// metal, folk, podcast, ambient, salsa, drum & bass) y aplica los movimientos tipicos de un ingeniero
// con el modo Parametrico y el Vintage. Mide, con analisis espectral real, si el plugin hace exactamente
// lo que dice (audio = curva de la pantalla), si la curva es la de un EQ analogico ideal (sin
// "aplastamiento" cerca de Nyquist), y el comportamiento dinamico, estereo, de nivel y de CPU.
#include "PluginProcessor.h"
#include <random>
#include <complex>
#include <chrono>
#include <map>
#include <thread>
#include "DSP/CurveFit.h"
#include "DSP/SketchCurve.h"
#include "DSP/NaturalPhase.h"
#include "DSP/SpectrumLearner.h"

using namespace gaux;
using Cx = std::complex<double>;

static int gFails = 0, gWarn = 0, gChecks = 0;
static juce::String gFilter;   // argv[1]: solo correr lo que contenga este texto

//==============================================================================
// Utilidades
struct St
{
    std::vector<double> l, r;
    St() = default;
    explicit St (size_t n) : l (n, 0.0), r (n, 0.0) {}
    size_t size() const { return l.size(); }
    void add (const St& o, double g = 1.0) { for (size_t i = 0; i < size(); ++i) { l[i] += g * o.l[i]; r[i] += g * o.r[i]; } }
    double peak() const { double p = 0; for (size_t i = 0; i < size(); ++i) p = std::max ({ p, std::abs (l[i]), std::abs (r[i]) }); return p; }
    void scaleToPeakDb (double db) { const double g = std::pow (10.0, db / 20.0) / std::max (1e-12, peak()); for (auto* v : { &l, &r }) for (auto& x : *v) x *= g; }
};

static void setP (GauxEqProcessor& p, const juce::String& id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

struct Rng
{
    std::mt19937 g;
    explicit Rng (unsigned s) : g (s) {}
    double uni() { return std::uniform_real_distribution<double> (-1.0, 1.0) (g); }
    double u01() { return std::uniform_real_distribution<double> (0.0, 1.0) (g); }
};

struct Bq
{
    BiquadCoeffs c; BiquadState s;
    Bq() = default;
    explicit Bq (BiquadCoeffs k) : c (k) {}
    double p (double x) { return s.process (x, c); }
};

static double dB (double x) { return 20.0 * std::log10 (std::max (1e-30, x)); }

//==============================================================================
// Instrumentos sinteticos (simples pero con el espectro y la dinamica de los reales)
namespace syn
{
constexpr double tau = 2.0 * kPi;

static double polyBlep (double t, double dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

struct Saw
{
    double ph = 0;
    double next (double f, double fs) { const double dt = f / fs; double v = 2.0 * ph - 1.0 - polyBlep (ph, dt); ph += dt; if (ph >= 1.0) ph -= 1.0; return v; }
};

static void kick (std::vector<double>& v, double fs, double t0, double amp, double f0 = 160, double f1 = 48, double dec = 0.35, Rng* rng = nullptr)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (fs * dec * 4);
    double ph = 0;
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs;
        const double f = f1 + (f0 - f1) * std::exp (-t / 0.035);
        ph += tau * f / fs;
        double y = std::sin (ph) * std::exp (-t / dec) * std::min (1.0, t / 0.0008);
        if (rng != nullptr && t < 0.004) y += 0.5 * rng->uni() * (1.0 - t / 0.004);   // click del parche
        v[s0 + i] += amp * std::tanh (1.4 * y);
    }
}

static void snare (std::vector<double>& v, double fs, double t0, double amp, Rng& rng, double dec = 0.16, double tone = 185)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (fs * dec * 5);
    Bq bp (makeBandPass (fs, 3500, 0.6)), hp (makeHighPass (fs, 400, 0.7));
    double ph = 0;
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs;
        ph += tau * tone * (1.0 + 0.3 * std::exp (-t / 0.01)) / fs;
        const double body = std::sin (ph) * std::exp (-t / 0.06);
        const double wires = hp.p (bp.p (rng.uni())) * 2.2 * std::exp (-t / dec);
        v[s0 + i] += amp * (0.6 * body + wires);
    }
}

static void clap (std::vector<double>& v, double fs, double t0, double amp, Rng& rng)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (fs * 0.4);
    Bq bp (makeBandPass (fs, 1400, 1.2));
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs;
        double e = std::exp (-t / 0.12);
        for (double d : { 0.0, 0.011, 0.023 }) if (t >= d && t < d + 0.008) e += 1.5 * std::exp (-(t - d) / 0.003);
        v[s0 + i] += amp * 3.0 * bp.p (rng.uni()) * e;
    }
}

static void hat (std::vector<double>& v, double fs, double t0, double amp, Rng& rng, double dec = 0.04)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (fs * dec * 6);
    Bq hp1 (makeHighPass (fs, 7000, 0.7)), hp2 (makeHighPass (fs, 7000, 0.7)), pk (makeBell (fs, 10500, 1.5, 6));
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs;
        v[s0 + i] += amp * pk.p (hp2.p (hp1.p (rng.uni()))) * std::exp (-t / dec);
    }
}

static void ride (std::vector<double>& v, double fs, double t0, double amp, Rng& rng)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (fs * 1.5);
    static const double partials[] = { 3150, 4410, 5230, 6870, 8120, 9650 };
    Bq hp (makeHighPass (fs, 4000, 0.7));
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs;
        double y = 0;
        for (double f : partials) y += std::sin (tau * f * t + f) * 0.15;
        y += 0.5 * hp.p (rng.uni());
        v[s0 + i] += amp * y * (0.3 * std::exp (-t / 0.9) + 0.7 * std::exp (-t / 0.05));
    }
}

static void bassNote (std::vector<double>& v, double fs, double t0, double dur, double f, double amp, double cutoff = 700, double res = 0.9)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (dur * fs);
    Saw a, b; Bq lp1 (makeLowPass (fs, cutoff, res)), lp2 (makeLowPass (fs, cutoff * 1.5, 0.6));
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs, env = std::min (1.0, t / 0.004) * std::min (1.0, (dur - t) / 0.01) * (0.7 + 0.3 * std::exp (-t / 0.1));
        const double y = 0.5 * a.next (f, fs) + 0.5 * b.next (f * 1.004, fs);
        v[s0 + i] += amp * env * lp2.p (lp1.p (y));
    }
}

static void sub808 (std::vector<double>& v, double fs, double t0, double dur, double f, double amp, double drive = 1.6)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (dur * fs);
    double ph = 0;
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs, env = std::min (1.0, t / 0.002) * std::exp (-t / (dur * 0.6)) * std::min (1.0, (dur - t) / 0.02);
        ph += tau * f * (1.0 + 0.6 * std::exp (-t / 0.03)) / fs;
        v[s0 + i] += amp * std::tanh (drive * std::sin (ph)) / std::tanh (drive) * env;
    }
}

// Cuerda pulsada (Karplus-Strong): guitarra acustica, contrabajo, pizzicato
static void pluck (std::vector<double>& v, double fs, double t0, double f, double amp, double dur, double bright, Rng& rng)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (dur * fs);
    const int L = std::max (2, (int) std::round (fs / f));
    std::vector<double> d ((size_t) L);
    double lp = 0;
    for (auto& x : d) { lp += bright * (rng.uni() - lp); x = lp; }
    int idx = 0; double prev = 0;
    const double loss = std::pow (0.001, 1.0 / (dur * f));   // -60 dB al final
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double cur = d[(size_t) idx];
        const double nxt = loss * (bright * cur + (1.0 - bright) * prev);
        prev = cur;
        d[(size_t) idx] = nxt;
        idx = (idx + 1) % L;
        v[s0 + i] += amp * cur * std::min (1.0, (dur - i / fs) / 0.02);
    }
}

// Guitarra electrica distorsionada (power chord + amplificador + "caja")
static void distGuitar (std::vector<double>& v, double fs, double t0, double dur, double f, double amp, double gain, unsigned seed)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (dur * fs);
    Saw a, b, c; Rng rng (seed);
    Bq pre (makeHighPass (fs, 150, 0.7)), cab1 (makeHighPass (fs, 90, 0.8)), cab2 (makeBell (fs, 2500, 1.2, 5)), cab3 (makeLowPass (fs, 5200, 0.9)), cab4 (makeLowPass (fs, 6000, 0.7)), mud (makeBell (fs, 400, 1.0, 3));
    const double det = 1.0 + 0.002 * rng.uni();
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs, env = std::min (1.0, t / 0.003) * std::min (1.0, (dur - t) / 0.015);
        double x = 0.4 * a.next (f * det, fs) + 0.3 * b.next (f * 1.4983, fs) + 0.3 * c.next (f * 2.0 * det, fs);
        x = std::tanh (gain * pre.p (x)) ;
        v[s0 + i] += amp * env * mud.p (cab4.p (cab3.p (cab2.p (cab1.p (x)))));
    }
}

static void piano (std::vector<double>& v, double fs, double t0, double f, double amp, double dur)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (dur * fs);
    const double B = 0.0004;
    for (int k = 1; k <= 18; ++k)
    {
        const double fk = k * f * std::sqrt (1.0 + B * k * k);
        if (fk > fs * 0.45) break;
        const double ak = 1.0 / std::pow ((double) k, 1.1), dk = 1.6 / (1.0 + 0.35 * k);
        for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
        {
            const double t = i / fs;
            v[s0 + i] += amp * ak * std::sin (tau * fk * t + k) * std::exp (-t / dk) * std::min (1.0, t / 0.002) * std::min (1.0, (dur - t) / 0.05);
        }
    }
}

static void pad (std::vector<double>& v, double fs, double t0, double dur, double f, double amp, double cutoff = 2000, double atk = 0.6)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (dur * fs);
    std::array<Saw, 5> o; const double dt[] = { 0.993, 0.997, 1.0, 1.004, 1.008 };
    Bq lp (makeLowPass (fs, cutoff, 0.7)), lp2 (makeLowPass (fs, cutoff, 0.7));
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs, env = std::min (1.0, t / atk) * std::min (1.0, (dur - t) / 0.4);
        double y = 0; for (int k = 0; k < 5; ++k) y += o[(size_t) k].next (f * dt[k], fs);
        v[s0 + i] += amp * 0.25 * env * lp2.p (lp.p (y));
    }
}

static void brass (std::vector<double>& v, double fs, double t0, double dur, double f, double amp)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (dur * fs);
    Saw o; Bq f1 (makeBell (fs, 1200, 1.2, 9)), f2 (makeBell (fs, 2600, 2.0, 6)), lp (makeLowPass (fs, 7000, 0.7));
    double bright = 0;
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs, env = std::min (1.0, t / 0.03) * std::min (1.0, (dur - t) / 0.03);
        bright += (env - bright) * 0.0008;
        const double y = std::tanh ((0.5 + 1.5 * bright) * o.next (f * (1.0 + 0.003 * std::sin (tau * 5.5 * t)), fs));
        v[s0 + i] += amp * env * lp.p (f2.p (f1.p (y))) * 0.4;
    }
}

static void conga (std::vector<double>& v, double fs, double t0, double f, double amp, Rng& rng, bool slap)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (fs * 0.5);
    Bq bp (makeBandPass (fs, 3000, 1.0));
    double ph = 0;
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs;
        ph += tau * f * (1.0 + 0.15 * std::exp (-t / 0.02)) / fs;
        double y = std::sin (ph) * std::exp (-t / (slap ? 0.06 : 0.16));
        if (slap) y += 1.5 * bp.p (rng.uni()) * std::exp (-t / 0.015);
        v[s0 + i] += amp * y;
    }
}

// Voz: fuente glotal + formantes (vocales), vibrato, aire, sibilantes ("s") y oclusivas ("p")
struct VoiceNote { double t0, dur, f; int vowel; bool sib; bool plosive; };
static void voice (std::vector<double>& v, double fs, const std::vector<VoiceNote>& notes, double amp, Rng& rng, double sibAmp = 1.0, double tilt = 1800)
{
    static const double formants[5][3] = { { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 350, 2300, 3000 }, { 450, 800, 2830 }, { 325, 700, 2530 } };
    for (auto& nt : notes)
    {
        const size_t s0 = (size_t) (nt.t0 * fs), n = (size_t) (nt.dur * fs);
        Saw src; Bq tl (makeLowPass (fs, tilt, 0.5));
        Bq fm[3] = { Bq (makeBandPass (fs, formants[nt.vowel][0], 6)), Bq (makeBandPass (fs, formants[nt.vowel][1], 8)), Bq (makeBandPass (fs, formants[nt.vowel][2], 10)) };
        Bq pres (makeBell (fs, 3200, 1.0, 4)), airHp (makeHighPass (fs, 6000, 0.7));
        Bq sibBp (makeBandPass (fs, 7000, 1.6)), sibBp2 (makeBandPass (fs, 7500, 1.6)), plos (makeLowPass (fs, 120, 0.7));
        const double sibDur = 0.11, sibStart = std::max (0.0, nt.dur - sibDur - 0.01);
        for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
        {
            const double t = i / fs;
            const double env = std::min (1.0, t / 0.04) * std::min (1.0, (nt.dur - t) / 0.05);
            const double vib = 1.0 + 0.012 * std::sin (tau * 5.4 * t) * std::min (1.0, t / 0.3);
            const double g = tl.p (src.next (nt.f * vib, fs));
            double y = 1.2 * fm[0].p (g) + 0.9 * fm[1].p (g) + 0.5 * fm[2].p (g);
            y = pres.p (y) + 0.03 * airHp.p (rng.uni()) * env;
            y *= env;
            if (nt.sib && t >= sibStart && t < sibStart + sibDur)
            {
                const double ts = t - sibStart, e = std::sin (kPi * ts / sibDur);
                y = y * (1.0 - e) + sibAmp * 0.9 * sibBp2.p (sibBp.p (rng.uni())) * e * 3.0;
            }
            if (nt.plosive && t < 0.03) y += 2.5 * plos.p (rng.uni()) * (1.0 - t / 0.03) * 8.0;
            v[s0 + i] += amp * y;
        }
    }
}

static void panInto (St& dst, const std::vector<double>& m, double pan, double gain)
{
    const double a = (pan + 1.0) * 0.25 * kPi, gl = std::cos (a) * gain * 1.4142, gr = std::sin (a) * gain * 1.4142;
    for (size_t i = 0; i < m.size(); ++i) { dst.l[i] += gl * m[i]; dst.r[i] += gr * m[i]; }
}

// Pequena "sala": dos lineas de retardo cruzadas (da ancho y lados reales)
static void room (St& s, double fs, double mix, unsigned seed)
{
    Rng rng (seed);
    const size_t dl = (size_t) (fs * (0.023 + 0.004 * rng.u01())), dr = (size_t) (fs * (0.031 + 0.004 * rng.u01()));
    std::vector<double> bl (dl, 0.0), br (dr, 0.0); size_t il = 0, ir = 0;
    Bq lpl (makeLowPass (fs, 6000, 0.7)), lpr (makeLowPass (fs, 6000, 0.7));
    for (size_t i = 0; i < s.size(); ++i)
    {
        const double ol = bl[il], or_ = br[ir];
        bl[il] = s.l[i] + 0.45 * lpr.p (or_); br[ir] = s.r[i] + 0.45 * lpl.p (ol);
        il = (il + 1) % dl; ir = (ir + 1) % dr;
        s.l[i] += mix * or_; s.r[i] += mix * ol;
    }
}

static double midiHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }
} // namespace syn

//==============================================================================
// Sesiones (una por genero): pistas sueltas + la mezcla
struct Session
{
    juce::String genre;
    double fs = 48000;
    std::map<juce::String, St> stems;
    St mix;
    void buildMix (double peakDb)
    {
        mix = St (stems.begin()->second.size());
        for (auto& [n, s] : stems) mix.add (s);
        mix.scaleToPeakDb (peakDb);
    }
};

static St mono2 (const std::vector<double>& m, double pan = 0, double g = 1) { St s (m.size()); syn::panInto (s, m, pan, g); return s; }

static Session makeTechHouse (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Tech House"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (1);
    const double beat = 60.0 / 124.0;
    std::vector<double> k (N), b (N), h (N), c (N), p (N), sideHat (N);
    for (double t = 0; t < secs; t += beat) kick (k, fs, t, 0.9, 150, 47, 0.32, &rng);
    for (double t = beat * 0.5; t < secs; t += beat) { bassNote (b, fs, t, beat * 0.42, midiHz (33 + ((int) (t / beat) % 4 == 3 ? 3 : 0)), 0.6, 520, 1.6); hat (h, fs, t, 0.25, rng, 0.05); }
    for (double t = beat * 0.25; t < secs; t += beat * 0.5) hat (sideHat, fs, t, 0.12, rng, 0.02);
    for (double t = beat; t < secs; t += beat * 2) clap (c, fs, t, 0.5, rng);
    pad (p, fs, 0, secs, midiHz (57), 0.25, 1800, 1.5); pad (p, fs, 0, secs, midiHz (60), 0.2, 1800, 1.5);
    s.stems["KICK"] = mono2 (k); s.stems["BASS"] = mono2 (b);
    St hats = mono2 (h, 0.0); syn::panInto (hats, sideHat, 0.6, 1.0); s.stems["HATS"] = hats;
    s.stems["CLAP"] = mono2 (c); room (s.stems["CLAP"], fs, 0.3, 3);
    St pd (N); syn::panInto (pd, p, -0.5, 1); { std::vector<double> p2 (N); pad (p2, fs, 0, secs, midiHz (64), 0.2, 2200, 1.5); syn::panInto (pd, p2, 0.5, 1); } s.stems["PAD"] = pd;
    s.buildMix (-0.5);
    // Bajo "rolling" sostenido (suena encima del kick): para probar el sidechain dinamico
    { std::vector<double> bs (N); for (double t = 0; t < secs; t += beat * 0.5) bassNote (bs, fs, t, beat * 0.48, midiHz (33), 0.6, 520, 1.6); s.stems["BASS_ROLL"] = mono2 (bs); }
    return s;
}

static Session makeDubstep (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Dubstep"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (2);
    const double beat = 60.0 / 140.0;
    std::vector<double> k (N), sn (N), sub (N), gr (N);
    for (double t = 0; t < secs; t += beat * 4) { kick (k, fs, t, 0.95, 170, 50, 0.3, &rng); snare (sn, fs, t + beat * 2, 0.7, rng, 0.2, 200); }
    for (double t = 0; t < secs; t += beat) sub808 (sub, fs, t, beat * 0.9, midiHz (29), 0.7, 1.2);
    // Growl: FM + distorsion + resonancia fija en 1.2 kHz (la que el ingeniero va a "notchear")
    {
        double ph = 0, ph2 = 0; Bq res (makeBell (fs, 1200, 9, 14)), hp (makeHighPass (fs, 80, 0.7));
        for (size_t i = 0; i < N; ++i)
        {
            const double t = i / fs, lfo = 0.5 + 0.5 * std::sin (tau * (2.0 / beat) * t);
            ph2 += tau * midiHz (41) * 2.0 / fs; ph += tau * midiHz (41) / fs;
            const double x = std::sin (ph + (1.0 + 5.0 * lfo) * std::sin (ph2));
            gr[i] = 0.35 * res.p (hp.p (std::tanh (4.0 * x)));
        }
    }
    s.stems["KICK"] = mono2 (k); s.stems["SNARE"] = mono2 (sn); s.stems["SUB"] = mono2 (sub);
    St g (N); syn::panInto (g, gr, -0.3, 1); { std::vector<double> g2 = gr; std::rotate (g2.begin(), g2.begin() + 300, g2.end()); syn::panInto (g, g2, 0.3, 1); } s.stems["GROWL"] = g;
    s.buildMix (-0.3);
    return s;
}

static std::vector<syn::VoiceNote> melody (double t0, double beat, int baseMidi, unsigned seed, double secs)
{
    std::vector<syn::VoiceNote> nts; Rng rng (seed);
    static const int scale[] = { 0, 2, 4, 5, 7, 9, 11, 12 };
    double t = t0; int i = 0;
    while (t < secs - 0.5)
    {
        const double d = beat * (rng.u01() < 0.5 ? 1.0 : 0.5) * 0.95;
        nts.push_back ({ t, d, syn::midiHz (baseMidi + scale[(int) (rng.u01() * 7.99)]), (int) (rng.u01() * 4.99), (i % 3 == 2), (i % 5 == 0) });
        t += d + beat * 0.05; ++i;
    }
    return nts;
}

static Session makePop (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Pop"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (3);
    const double beat = 60.0 / 100.0;
    std::vector<double> vo (N), pi (N), k (N), sn (N), h (N), b (N);
    voice (vo, fs, melody (0.3, beat, 64, 11, secs), 0.5, rng, 1.6);
    for (double t = 0; t < secs; t += beat * 2) for (int m : { 60, 64, 67, 71 }) piano (pi, fs, t, midiHz (m - 12), 0.12, beat * 2);
    for (double t = 0; t < secs; t += beat) { kick (k, fs, t, 0.8, 140, 55, 0.25, &rng); hat (h, fs, t + beat * 0.5, 0.2, rng); bassNote (b, fs, t, beat * 0.9, midiHz (36 + ((int) (t / beat) % 2) * 5), 0.5, 900, 0.8); }
    for (double t = beat; t < secs; t += beat * 2) snare (sn, fs, t, 0.6, rng);
    s.stems["VOZ"] = mono2 (vo); room (s.stems["VOZ"], fs, 0.12, 4);
    St pp (N); syn::panInto (pp, pi, -0.35, 1); { std::vector<double> p2 = pi; std::rotate (p2.begin(), p2.begin() + 600, p2.end()); syn::panInto (pp, p2, 0.35, 0.9); } s.stems["PIANO"] = pp;
    St dr = mono2 (k); dr.add (mono2 (sn)); dr.add (mono2 (h, 0.3)); room (dr, fs, 0.2, 5); s.stems["DRUMS"] = dr;
    s.stems["BAJO"] = mono2 (b);
    s.buildMix (-1.0);
    return s;
}

static Session makeRock (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Rock"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (4);
    const double beat = 60.0 / 120.0;
    std::vector<double> gl (N), gr (N), k (N), sn (N), b (N), vo (N), cy (N);
    static const int riff[] = { 40, 40, 43, 45, 40, 47, 45, 43 };
    for (int i = 0; i * beat < secs; ++i)
    {
        const double t = i * beat, f = midiHz (riff[i % 8]);
        distGuitar (gl, fs, t, beat * 0.95, f, 0.35, 9, 100 + (unsigned) i);
        distGuitar (gr, fs, t + 0.008, beat * 0.95, f, 0.35, 9, 200 + (unsigned) i);
        bassNote (b, fs, t, beat * 0.9, f * 0.5, 0.55, 1400, 0.8);
        kick (k, fs, t, 0.8, 120, 60, 0.18, &rng);
        if (i % 2) snare (sn, fs, t, 0.8, rng, 0.14, 210);
        ride (cy, fs, t, 0.1, rng);
    }
    voice (vo, fs, melody (0.5, beat, 62, 21, secs), 0.45, rng, 1.2, 2400);
    s.stems["GUITARRAS"] = St (N); syn::panInto (s.stems["GUITARRAS"], gl, -0.9, 1); syn::panInto (s.stems["GUITARRAS"], gr, 0.9, 1);
    St dr = mono2 (k); dr.add (mono2 (sn)); dr.add (mono2 (cy, 0.4)); room (dr, fs, 0.25, 6); s.stems["DRUMS"] = dr;
    s.stems["SNARE"] = mono2 (sn);
    s.stems["BAJO"] = mono2 (b); s.stems["VOZ"] = mono2 (vo);
    s.buildMix (-0.5);
    return s;
}

static Session makeReggaeton (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Reggaeton"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (5);
    const double beat = 60.0 / 95.0;
    std::vector<double> k (N), sn (N), b (N), pc (N), vo (N);
    for (double t = 0; t < secs; t += beat)
    {
        kick (k, fs, t, 0.9, 150, 52, 0.25, &rng);
        snare (sn, fs, t + beat * 0.75, 0.55, rng, 0.1, 230);   // dembow: 3+3+2
        snare (sn, fs, t + beat * 0.5 + beat * 0.5 * ((int) (t / beat) % 2), 0.45, rng, 0.08, 230);
        sub808 (b, fs, t, beat * 0.95, midiHz (((int) (t / beat) % 4) < 2 ? 31 : 34), 0.75, 2.2);
        conga (pc, fs, t + beat * 0.25, 330, 0.25, rng, true);
    }
    voice (vo, fs, melody (0.2, beat * 0.5, 60, 31, secs), 0.5, rng, 1.8, 2200);
    s.stems["KICK"] = mono2 (k); s.stems["SNARE"] = mono2 (sn); s.stems["808"] = mono2 (b);
    s.stems["PERC"] = mono2 (pc, 0.4); s.stems["VOZ"] = mono2 (vo); room (s.stems["VOZ"], fs, 0.1, 7);
    s.buildMix (-0.3);
    return s;
}

static Session makeJazz (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Jazz"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (6);
    const double beat = 60.0 / 160.0;
    std::vector<double> ub (N), ri (N), br (N), pi (N);
    static const int walk[] = { 36, 40, 43, 45, 46, 45, 43, 40 };
    for (int i = 0; i * beat < secs; ++i)
    {
        const double t = i * beat;
        pluck (ub, fs, t, midiHz (walk[i % 8]), 0.9, beat * 1.4, 0.35, rng);
        ride (ri, fs, t, 0.12 + 0.05 * (i % 2), rng);
        if (i % 2) hat (br, fs, t, 0.06, rng, 0.12);
        if (i % 4 == 1) for (int m : { 58, 62, 65, 69 }) piano (pi, fs, t, midiHz (m), 0.06, beat * 3);
    }
    s.stems["CONTRABAJO"] = mono2 (ub, -0.1); s.stems["RIDE"] = mono2 (ri, 0.5); s.stems["ESCOBILLAS"] = mono2 (br, 0.2);
    s.stems["PIANO"] = mono2 (pi, -0.4); room (s.stems["PIANO"], fs, 0.3, 8);
    s.buildMix (-14.0);   // jazz grabado con mucho headroom
    return s;
}

static Session makeOrchestra (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Orquesta"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (7);
    std::vector<double> vl (N), vc (N), bra (N), ti (N);
    pad (vl, fs, 0.0, secs, midiHz (67), 0.5, 3500, 2.5); pad (vl, fs, 0.0, secs, midiHz (71), 0.4, 3500, 2.5);
    pad (vc, fs, 0.0, secs, midiHz (43), 0.6, 1200, 2.0);
    brass (bra, fs, secs * 0.5, secs * 0.4, midiHz (55), 0.5);
    for (double t = 1.0; t < secs; t += 2.0) kick (ti, fs, t, 0.5, 110, 90, 0.8, nullptr);
    s.stems["VIOLINES"] = mono2 (vl, -0.5); s.stems["CELLOS"] = mono2 (vc, 0.4); s.stems["METALES"] = mono2 (bra, 0.1); s.stems["TIMBALES"] = mono2 (ti, 0.0);
    for (auto& [n, st] : s.stems) room (st, fs, 0.4, 9);
    s.buildMix (-20.0);   // clasica: muy dinamica y baja
    // Crescendo enorme (pp -> ff): prueba de rango dinamico
    for (size_t i = 0; i < N; ++i) { const double g = std::pow (10.0, (-40.0 + 40.0 * (double) i / N) / 20.0) * 3.0; s.mix.l[i] *= g; s.mix.r[i] *= g; }
    return s;
}

static Session makeTrap (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Trap / Hip-Hop"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (8);
    const double beat = 60.0 / 140.0;
    std::vector<double> b (N), h (N), sn (N), vo (N);
    for (double t = 0; t < secs; t += beat * 2) sub808 (b, fs, t, beat * 1.9, midiHz (30), 0.8, 2.5);
    for (double t = 0; t < secs; t += beat / (std::fmod (t, beat * 4) > beat * 3 ? 6.0 : 2.0)) hat (h, fs, t, 0.2, rng, 0.02);
    for (double t = beat * 2; t < secs; t += beat * 4) { clap (sn, fs, t, 0.5, rng); snare (sn, fs, t, 0.5, rng); }
    // Rap: notas cortas, muchas consonantes y oclusivas, mucho grave de proximidad
    std::vector<syn::VoiceNote> rap; for (double t = 0.2; t < secs - 0.4; t += beat * 0.5) rap.push_back ({ t, beat * 0.45, midiHz (50), (int) (t * 7) % 5, ((int) (t * 3) % 2) == 0, ((int) (t * 5) % 3) == 0 });
    voice (vo, fs, rap, 0.55, rng, 1.4, 1500);
    { Bq prox (makeLowShelf (fs, 180, 0.7, 7)); for (auto& x : vo) x = prox.p (x); }
    s.stems["808"] = mono2 (b); s.stems["HATS"] = mono2 (h, 0.2); s.stems["SNARE"] = mono2 (sn); s.stems["VOZ RAP"] = mono2 (vo);
    s.buildMix (-0.5);
    return s;
}

static Session makeMetal (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Metal"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (9);
    const double sx = 60.0 / 180.0 / 4.0;
    std::vector<double> gl (N), gr (N), k (N);
    int i = 0;
    for (double t = 0; t < secs; t += sx, ++i)
    {
        const bool chug = (i % 8) < 6;
        const double f = syn::midiHz (chug ? 26 : 33);   // afinacion drop
        distGuitar (gl, fs, t, sx * (chug ? 0.6 : 0.95), f, chug ? 0.45 : 0.3, 30, 300 + (unsigned) i);
        distGuitar (gr, fs, t + 0.006, sx * (chug ? 0.6 : 0.95), f, chug ? 0.45 : 0.3, 30, 400 + (unsigned) i);
        kick (k, fs, t, 0.7, 200, 70, 0.08, &rng);
    }
    s.stems["GUITARRAS"] = St (N); syn::panInto (s.stems["GUITARRAS"], gl, -1, 1); syn::panInto (s.stems["GUITARRAS"], gr, 1, 1);
    s.stems["KICK"] = mono2 (k);
    s.buildMix (0.0);
    return s;
}

static Session makeFolk (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Folk / Acustico"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (10);
    const double beat = 60.0 / 90.0;
    std::vector<double> g (N), vo (N);
    static const int chord[] = { 40, 47, 52, 55, 59, 64 };
    for (double t = 0; t < secs; t += beat * 0.5)
        for (int k = 0; k < 6; ++k) pluck (g, fs, t + k * 0.012, syn::midiHz (chord[k]), 0.25, beat * 1.2, 0.6, rng);
    { Bq boom (makeBell (fs, 110, 2.0, 8)); for (auto& x : g) x = boom.p (x); }   // guitarra con "boom" de caja
    voice (vo, fs, melody (0.4, beat, 60, 41, secs), 0.45, rng, 1.0, 1600);
    St gg (N); syn::panInto (gg, g, -0.3, 1); room (gg, fs, 0.2, 10); s.stems["GUITARRA"] = gg; s.stems["VOZ"] = mono2 (vo, 0.1);
    s.buildMix (-3.0);
    return s;
}

static Session makePodcast (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Podcast / Voz hablada"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (11);
    std::vector<double> vo (N);
    std::vector<syn::VoiceNote> sp; for (double t = 0.1; t < secs - 0.3; t += 0.22) sp.push_back ({ t, 0.2, syn::midiHz (48 + (int) (rng.u01() * 5)), (int) (rng.u01() * 4.99), rng.u01() < 0.3, rng.u01() < 0.3 });
    voice (vo, fs, sp, 0.6, rng, 1.2, 1300);
    // Zumbido de corriente de 60 Hz con armonicos (el ingeniero lo quita con notch)
    for (size_t i = 0; i < N; ++i) { const double t = i / fs; vo[i] += 0.02 * std::sin (tau * 60 * t) + 0.012 * std::sin (tau * 120 * t + 1) + 0.006 * std::sin (tau * 180 * t + 2); }
    s.stems["VOZ"] = mono2 (vo);
    s.buildMix (-3.0);
    return s;
}

static Session makeAmbient (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Ambient / Cine"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (12);
    std::vector<double> a (N), b (N), nz (N);
    pad (a, fs, 0, secs, midiHz (45), 0.6, 900, 3.0); pad (b, fs, 0, secs, midiHz (64), 0.4, 5000, 3.0);
    Bq lp (makeLowPass (fs, 3000, 0.5)); for (auto& x : nz) x = 0.05 * lp.p (rng.uni());
    St st (N); syn::panInto (st, a, -0.6, 1); syn::panInto (st, b, 0.6, 1); syn::panInto (st, nz, 0, 1); room (st, fs, 0.6, 12);
    s.stems["PADS"] = st;
    s.buildMix (-6.0);
    return s;
}

static Session makeSalsa (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Salsa / Latin"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (13);
    const double beat = 60.0 / 190.0;
    std::vector<double> cg (N), pi (N), br (N), b (N);
    for (int i = 0; i * beat < secs; ++i)
    {
        const double t = i * beat;
        conga (cg, fs, t, (i % 4 == 3) ? 220 : 330, 0.4, rng, (i % 2) == 1);
        if (i % 2 == 0) for (int m : { 60, 64, 67 }) piano (pi, fs, t + beat * 0.5, midiHz (m + 12 * (i % 4 == 0)), 0.1, beat);
        if (i % 4 == 2) bassNote (b, fs, t, beat * 1.8, midiHz (36 + 7 * ((i / 4) % 2)), 0.5, 700, 0.7);
    }
    brass (br, fs, 1.0, 1.2, midiHz (67), 0.6); brass (br, fs, 3.0, 1.2, midiHz (70), 0.6); brass (br, fs, 5.0, 2.0, midiHz (72), 0.6);
    s.stems["CONGAS"] = mono2 (cg, 0.5); s.stems["PIANO"] = mono2 (pi, -0.3); s.stems["METALES"] = mono2 (br, 0.2); s.stems["BAJO"] = mono2 (b);
    room (s.stems["METALES"], fs, 0.25, 13);
    s.buildMix (-1.0);
    return s;
}

static Session makeDnb (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Drum & Bass"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (14);
    const double beat = 60.0 / 174.0;
    std::vector<double> k (N), sn (N), re (N), h (N);
    for (double t = 0; t < secs; t += beat * 4)
    {
        kick (k, fs, t, 0.85, 180, 55, 0.15, &rng); kick (k, fs, t + beat * 2.5, 0.7, 180, 55, 0.15, &rng);
        snare (sn, fs, t + beat, 0.8, rng, 0.12, 240); snare (sn, fs, t + beat * 3, 0.8, rng, 0.12, 240);
    }
    for (double t = 0; t < secs; t += beat * 0.5) hat (h, fs, t, 0.15, rng, 0.03);
    { Saw a, b; Bq lp (makeLowPass (fs, 400, 2.0)); for (size_t i = 0; i < N; ++i) re[i] = 0.4 * lp.p (a.next (midiHz (31), fs) + b.next (midiHz (31) * 1.012, fs)); }
    s.stems["DRUMS"] = mono2 (k); s.stems["DRUMS"].add (mono2 (sn)); s.stems["DRUMS"].add (mono2 (h, 0.3));
    St r (N); syn::panInto (r, re, -0.2, 1); { std::vector<double> r2 = re; std::rotate (r2.begin(), r2.begin() + 200, r2.end()); syn::panInto (r, r2, 0.2, 1); } s.stems["REESE"] = r;
    s.buildMix (-0.3);
    return s;
}

//==============================================================================
// Analisis
static void fft (std::vector<Cx>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap (a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / (double) len; const Cx wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len) { Cx w (1); for (size_t j = 0; j < len / 2; ++j) { const Cx u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } }
    }
}

struct Spectra { std::vector<double> sxx, syy; std::vector<Cx> sxy; size_t nfft = 0; double fs = 48000; };
static Spectra welch (const std::vector<double>& x, const std::vector<double>& y, double fs, size_t start, size_t nfft = 16384, bool bh = false)
{
    Spectra s; s.nfft = nfft; s.fs = fs;
    s.sxx.assign (nfft / 2 + 1, 0.0); s.syy.assign (nfft / 2 + 1, 0.0); s.sxy.assign (nfft / 2 + 1, Cx (0));
    std::vector<double> win (nfft);
    for (size_t i = 0; i < nfft; ++i)
    {
        const double ph = 2.0 * kPi * i / nfft;
        win[i] = bh ? 0.35875 - 0.48829 * std::cos (ph) + 0.14128 * std::cos (2 * ph) - 0.01168 * std::cos (3 * ph) : 0.5 - 0.5 * std::cos (ph);
    }
    std::vector<Cx> X (nfft), Y (nfft);
    for (size_t off = start; off + nfft <= x.size(); off += nfft / 2)
    {
        for (size_t i = 0; i < nfft; ++i) { X[i] = x[off + i] * win[i]; Y[i] = y[off + i] * win[i]; }
        fft (X); fft (Y);
        for (size_t k = 0; k <= nfft / 2; ++k) { s.sxx[k] += std::norm (X[k]); s.syy[k] += std::norm (Y[k]); s.sxy[k] += std::conj (X[k]) * Y[k]; }
    }
    return s;
}

// Un solo FFT de toda la toma (ventana Tukey): sin sesgo por el retardo de grupo de los cortes empinados
static Spectra fullSpectra (const std::vector<double>& x, const std::vector<double>& y, double fs, size_t start)
{
    const size_t len = x.size() - start; size_t nfft = 1; while (nfft < len) nfft <<= 1;
    Spectra s; s.nfft = nfft; s.fs = fs;
    std::vector<Cx> X (nfft, Cx (0)), Y (nfft, Cx (0));
    const size_t taper = len / 20;
    for (size_t i = 0; i < len; ++i)
    {
        double w = 1.0;
        if (i < taper) w = 0.5 - 0.5 * std::cos (kPi * i / taper);
        else if (i >= len - taper) w = 0.5 - 0.5 * std::cos (kPi * (len - 1 - i) / taper);
        X[i] = x[start + i] * w; Y[i] = y[start + i] * w;
    }
    fft (X); fft (Y);
    s.sxx.resize (nfft / 2 + 1); s.syy.resize (nfft / 2 + 1); s.sxy.resize (nfft / 2 + 1);
    for (size_t k = 0; k <= nfft / 2; ++k) { s.sxx[k] = std::norm (X[k]); s.syy[k] = std::norm (Y[k]); s.sxy[k] = std::conj (X[k]) * Y[k]; }
    return s;
}

//==============================================================================
// Curvas "ideales" analogicas (lo que el ingeniero espera oir: sin aplastamiento cerca de Nyquist)
static Cx anaBell (double f, double f0, double q, double g) { const double A = std::pow (10.0, g / 40.0); const Cx s (0, f / f0); return (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0); }
static Cx anaLS (double f, double f0, double q, double g) { const double A = std::pow (10.0, g / 40.0), sA = std::sqrt (A); const Cx s (0, f / f0); return A * (s * s + (sA / q) * s + A) / (A * s * s + (sA / q) * s + 1.0); }
static Cx anaHS (double f, double f0, double q, double g) { const double A = std::pow (10.0, g / 40.0), sA = std::sqrt (A); const Cx s (0, f / f0); return A * (A * s * s + (sA / q) * s + 1.0) / (s * s + (sA / q) * s + A); }
static Cx anaHP (double f, double f0, double q) { const Cx s (0, f / f0); return s * s / (s * s + s / q + 1.0); }
static Cx anaLP (double f, double f0, double q) { const Cx s (0, f / f0); return 1.0 / (s * s + s / q + 1.0); }
static Cx anaNotch (double f, double f0, double q) { const Cx s (0, f / f0); return (s * s + 1.0) / (s * s + s / q + 1.0); }

static double analogBand (const BandSettings& b, double f, bool linearPhase = false, double brickRes = 0.0)
{
    if (! b.on) return 1.0;
    const double g = safeGain (b.gainDb);
    switch ((BandType) b.type)
    {
        case BandType::Bell: return std::abs (anaBell (f, b.freq, b.q, g));
        case BandType::LowShelf: return std::abs (anaLS (f, b.freq, std::min (b.q, kShelfMaxQ), g));
        case BandType::HighShelf: return std::abs (anaHS (f, b.freq, std::min (b.q, kShelfMaxQ), g));
        case BandType::Notch: return std::abs (anaNotch (f, b.freq, b.q));
        case BandType::AllPass: return 1.0;
        case BandType::TiltShelf: return std::abs (anaHS (f, b.freq, std::min (b.q, kShelfMaxQ), g)) * std::pow (10.0, -g / 40.0);
        case BandType::BandPass:
        {
            // m pasa-banda iguales de 6 dB/oct por lado, con el ancho total a -3 dB = f/Q
            const int m = std::clamp (slopeDbPerOct (b.slope) / 6, 1, 12);
            const double qs = m == 1 ? std::max (0.1, b.q) : std::max (0.1, b.q) / std::sqrt (std::pow (2.0, 1.0 / m) - 1.0);
            const Cx sv (0, f / b.freq);
            return std::pow (std::abs ((sv / qs) / (sv * sv + sv / qs + 1.0)), (double) m);
        }
        case BandType::LowCut: case BandType::HighCut:
        {
            // Butterworth de N polos (calculado aqui de cero, no con las tablas del plugin)
            const bool lc = b.type == (int) BandType::LowCut;
            if (b.slope == kSlopeBrick && linearPhase)
            {
                // pared ideal en los modos lineales
                const double x = lc ? b.freq / f : f / b.freq;
                // (tan empinada como el FIR la puede hacer: ver linearBrickOrder)
                int order = 64;
                if (brickRes > 0)
                {
                    const double span = 16.0 * brickRes / b.freq;
                    const double xx = lc ? 1.0 / (1.0 - std::min (0.95, span)) : 1.0 + span;
                    order = std::clamp ((int) std::floor (std::log (1000.0) / std::log (xx)), 8, 64);
                }
                return 1.0 / std::sqrt (1.0 + std::pow (x, 2.0 * order));
            }
            const int N = slopeDbPerOct (b.slope) / 6;
            const int nb = N / 2; const bool odd = (N % 2) != 0;
            double m = 1; const Cx sv (0, f / b.freq);
            if (odd) m *= std::abs ((lc ? sv : Cx (1.0)) / (sv + 1.0));
            for (int k = 0; k < nb; ++k)
            {
                const double th = 3.14159265358979323846 * (2.0 * k + 1.0 + (odd ? 1.0 : 0.0)) / (2.0 * N);
                double q = (nb == 1 && ! odd) ? b.q : 1.0 / (2.0 * std::cos (th));
                if ((nb > 1 || odd) && k == nb - 1) q *= b.q / 0.7071;
                m *= std::abs (lc ? anaHP (f, b.freq, q) : anaLP (f, b.freq, q));
            }
            return m;
        }
    }
    return 1.0;
}

static double analogVintage (const VintageSettings& vIn, double f)
{
    const auto v = effectiveVintage (vIn);
    const double lowF = kVLowFreqs[std::clamp (v.lowFreq, 0, kNumVLowFreqs - 1)], highF = kVHighFreqs[std::clamp (v.highFreq, 0, kNumVHighFreqs - 1)];
    const double attF = kVAttenFreqs[std::clamp (v.attenFreq, 0, kNumVAttenFreqs - 1)], airF = kVAirFreqs[std::clamp (v.airFreq, 0, kNumVAirFreqs - 1)];
    double m = std::abs (anaLS (f, lowF * 2.0, 0.9, v.lowBoost * 1.35)) * std::abs (anaLS (f, lowF * 1.5, 0.45, -v.lowAtten * 1.75));
    m *= std::abs (anaBell (f, highF, 2.5 * std::pow (0.16, v.highBandwidth / 10.0), v.highBoost * 1.8));
    m *= std::abs (anaHS (f, attF, 0.5, -v.highAtten * 1.6)) * std::abs (anaHS (f, airF, 0.4, v.airGain));
    m *= std::abs (anaBell (f, kVLmFreqs[std::clamp (v.lmFreq, 0, kNumVLmFreqs - 1)], 0.9 - 0.03 * v.lmBoost, v.lmBoost));
    m *= std::abs (anaBell (f, kVDipFreqs[std::clamp (v.dipFreq, 0, kNumVDipFreqs - 1)], 1.1, -v.dip));
    m *= std::abs (anaBell (f, kVHmFreqs[std::clamp (v.hmFreq, 0, kNumVHmFreqs - 1)], 1.0 - 0.03 * v.hmBoost, v.hmBoost));
    const double lc = kVLowCuts[std::clamp (v.lowCut, 0, kNumVLowCuts - 1)], hc = kVHighCuts[std::clamp (v.highCut, 0, kNumVHighCuts - 1)];
    if (v.filterIn && lc > 0) m *= std::abs (anaHP (f, lc, 0.62));
    if (v.filterIn && hc > 0) m *= std::abs (anaLP (f, hc, 0.62));
    { const double fc = 3.5 * std::clamp (v.lowBoost / 2.0, 0.0, 1.0); m *= f / std::sqrt (f * f + fc * fc); }   // acople por transformador
    return m;
}

//==============================================================================
// Movimientos del ingeniero
struct Band
{
    int type = 0; double f = 1000, g = 0, q = 0.707; int slope = 1, st = 0;
    bool dyn = false; double thr = -24, range = -6, atk = 10, rel = 120; bool ext = false, autoThr = false; bool spec = false;
    int sat = 0; double drive = 0, mix = 100;
};
static Band bell (double f, double g, double q, int st = 0) { Band b; b.type = 0; b.f = f; b.g = g; b.q = q; b.st = st; return b; }
static Band lshelf (double f, double g, double q = 0.707, int st = 0) { Band b; b.type = 1; b.f = f; b.g = g; b.q = q; b.st = st; return b; }
static Band hshelf (double f, double g, double q = 0.707, int st = 0) { Band b; b.type = 2; b.f = f; b.g = g; b.q = q; b.st = st; return b; }
static Band lcut (double f, int slope = 1, double q = 0.707, int st = 0) { Band b; b.type = 3; b.f = f; b.slope = slope; b.q = q; b.st = st; return b; }
static Band hcut (double f, int slope = 1, double q = 0.707, int st = 0) { Band b; b.type = 4; b.f = f; b.slope = slope; b.q = q; b.st = st; return b; }
static Band notch (double f, double q) { Band b; b.type = 5; b.f = f; b.q = q; return b; }
static Band bpass (double f, double q, int slope = 0) { Band b; b.type = 6; b.f = f; b.q = q; b.slope = slope; return b; }
static Band tilt (double f, double g, double q = 0.707) { Band b; b.type = 7; b.f = f; b.g = g; b.q = q; return b; }
static Band apass (double f, double q) { Band b; b.type = 8; b.f = f; b.q = q; return b; }
static Band dynb (Band b, double thr, double range, double atk = 10, double rel = 120, bool ext = false) { b.dyn = true; b.thr = thr; b.range = range; b.atk = atk; b.rel = rel; b.ext = ext; return b; }
static Band sat (Band b, int type, double drive, double mix = 100) { b.sat = type; b.drive = drive; b.mix = mix; return b; }

static void applyBand (GauxEqProcessor& p, int i, const Band& b)
{
    using namespace ParamID;
    setP (p, band (i, "on"), 1); setP (p, band (i, "type"), (float) b.type); setP (p, band (i, "freq"), (float) b.f);
    setP (p, band (i, "gain"), (float) b.g); setP (p, band (i, "q"), (float) b.q); setP (p, band (i, "slope"), (float) b.slope);
    setP (p, band (i, "stereo"), (float) b.st); setP (p, band (i, "dyn"), b.dyn ? 1.0f : 0.0f); setP (p, band (i, "thr"), (float) b.thr);
    setP (p, band (i, "range"), (float) b.range); setP (p, band (i, "atk"), (float) b.atk); setP (p, band (i, "rel"), (float) b.rel);
    setP (p, band (i, "ext"), b.ext ? 1.0f : 0.0f); setP (p, band (i, "autoThr"), b.autoThr ? 1.0f : 0.0f);
    setP (p, band (i, "satType"), (float) b.sat); setP (p, band (i, "drive"), (float) b.drive); setP (p, band (i, "mix"), (float) b.mix);
    setP (p, band (i, "spec"), b.spec ? 1.0f : 0.0f);
}

struct Vin { std::vector<std::pair<juce::String, float>> kv; };

struct Move
{
    juce::String track, name;
    std::vector<Band> bands;
    bool vintage = false; Vin vin;
    int quality = 0;
    int phase = 0;      // 0 fase minima, 1 lineal, 2 lineal HQ, 3 natural
    double output = 0;
    bool autoGain = false;
    char domain = 'L';              // 'L', 'M' (mid), 'S' (side)
    const St* sidechain = nullptr;  // pista que entra por el sidechain
    bool scGeneral = false;
    double scThr = -30, scDuck = 60;
    // Comprobacion especifica (opcional): devuelve texto y marca fallo
    std::function<bool (const St& in, const St& out, double fs, juce::String& msg)> check;
};

static bool isLinear (const Move& m)
{
    if (m.scGeneral) return false;
    for (auto& b : m.bands) if (b.dyn || b.drive > 0.01) return false;
    for (auto& [k, v] : m.vin.kv) if ((k == ParamID::vTube || k == ParamID::vIron) && v > 0.01f) return false;
    return ! m.autoGain || true;
}

struct RunResult { bool linear = false, natural = false; double brickRes = 0; double detMax = -200, thrSeen = 0; std::vector<double> dynTrace; St out; double cpuPct = 0; double latency = 0; double efs = 48000; std::array<BandSettings, kNumBands> bs {}; VintageSettings vs; bool vintage = false; double outGain = 1; };

static RunResult runMove (const Move& m, const St& in, double fs, int block = 512, bool randomBlocks = false)
{
    RunResult r;
    auto p = std::make_unique<GauxEqProcessor>();
    const bool needSc = m.sidechain != nullptr;
    if (needSc) p->enableAllBuses();
    setP (*p, ParamID::mode, m.vintage ? 1.0f : 0.0f);
    setP (*p, ParamID::quality, (float) m.quality);
    setP (*p, ParamID::phase, (float) m.phase);
    setP (*p, ParamID::output, (float) m.output);
    setP (*p, ParamID::autoGain, m.autoGain ? 1.0f : 0.0f);
    setP (*p, ParamID::scOn, m.scGeneral ? 1.0f : 0.0f);
    setP (*p, ParamID::scThr, (float) m.scThr); setP (*p, ParamID::scDuck, (float) m.scDuck);
    for (size_t i = 0; i < m.bands.size(); ++i) applyBand (*p, (int) i, m.bands[i]);
    for (auto& [k, v] : m.vin.kv) setP (*p, k, v);
    p->prepareToPlay (fs, block);
    const int nch = needSc ? 4 : 2;
    juce::AudioBuffer<float> buf (nch, block); juce::MidiBuffer midi;
    r.out = St (in.size());
    std::mt19937 g (77);
    double secs = 0;
    for (size_t off = 0; off < in.size();)
    {
        const int n = (int) std::min<size_t> (randomBlocks ? 1 + g() % (unsigned) block : (unsigned) block, in.size() - off);
        buf.setSize (nch, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            buf.setSample (0, i, (float) in.l[off + i]); buf.setSample (1, i, (float) in.r[off + i]);
            if (needSc) { buf.setSample (2, i, (float) m.sidechain->l[off + i]); buf.setSample (3, i, (float) m.sidechain->r[off + i]); }
        }
        const auto t0 = std::chrono::high_resolution_clock::now();
        p->processBlock (buf, midi);
        secs += std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
        for (int i = 0; i < n; ++i) { r.out.l[off + i] = buf.getSample (0, i); r.out.r[off + i] = buf.getSample (1, i); }
        double dmin = 0; for (size_t b = 0; b < m.bands.size(); ++b) if (m.bands[b].dyn) { const double g = p->dynGainDb[b].load(); if (std::abs (g) > std::abs (dmin)) dmin = g; r.detMax = std::max (r.detMax, (double) p->detectorDb[b].load()); r.thrSeen = p->thresholdDb[b].load(); }
        r.dynTrace.push_back (dmin);
        off += (size_t) n;
    }
    r.cpuPct = 100.0 * secs / (in.size() / fs);
    r.latency = p->getLatencySamples();
    r.efs = p->engineSampleRate.load();
    for (int b = 0; b < kNumBands; ++b) r.bs[(size_t) b] = p->params.band (b);
    r.vs = p->params.vintage();
    r.vintage = m.vintage;
    r.linear = m.phase > 0 && ! m.vintage;
    r.natural = m.phase == 3 && ! m.vintage;
    r.brickRes = (m.phase == 1 || m.phase == 2) && ! m.vintage ? fs / linearPhaseLength (m.phase, fs) : 0.0;
    r.outGain = std::pow (10.0, (p->params.output->load() + p->autoGainDb.load()) / 20.0);
    return r;
}

static std::vector<double> domainOf (const St& s, char d)
{
    std::vector<double> v (s.size());
    for (size_t i = 0; i < s.size(); ++i) v[i] = d == 'L' ? s.l[i] : d == 'R' ? s.r[i] : d == 'M' ? 0.5 * (s.l[i] + s.r[i]) : 0.5 * (s.l[i] - s.r[i]);
    return v;
}

// Curva que dibuja la pantalla para el dominio pedido
static double displayCurve (const RunResult& r, double f, char d)
{
    if (r.vintage) return magnitudeOfVintage (r.vs, f, r.efs) * r.outGain;
    double m = r.outGain;
    for (auto& b : r.bs)
    {
        if (! b.on) continue;
        const bool applies = b.stereo == 0 || (b.stereo == 1 && d == 'L') || (b.stereo == 2 && d == 'R') || (b.stereo == 3 && d == 'M') || (b.stereo == 4 && d == 'S');
        if (applies) m *= r.linear ? analogMagnitudeOfBand (b, f, ! r.natural, r.brickRes) : magnitudeOfBand (b, f, r.efs);
    }
    return m;
}
static double idealCurve (const RunResult& r, double f, char d)
{
    if (r.vintage) return analogVintage (r.vs, f) * r.outGain;
    double m = r.outGain;
    for (auto& b : r.bs)
    {
        if (! b.on) continue;
        const bool applies = b.stereo == 0 || (b.stereo == 1 && d == 'L') || (b.stereo == 2 && d == 'R') || (b.stereo == 3 && d == 'M') || (b.stereo == 4 && d == 'S');
        if (applies) m *= analogBand (b, f, r.linear && ! r.natural, r.brickRes);
    }
    return m;
}

struct CurveCheck { double maxDev = 0, fAt = 0, minCoh = 1, cohAt = 0; int pts = 0, skipped = 0; };

// Audio real vs pantalla (1/24 de octava, solo donde hay senal y donde el analizador puede resolver la curva)
// Audio real vs pantalla: respuesta al impulso REAL del plugin con los mismos ajustes (metodo exacto,
// sin fugas espectrales ni sesgos del analizador). Mide en 'L' (canal izquierdo), 'M' (centro) o 'S' (lados).
static RunResult runMove (const Move& m, const St& in, double fs, int block, bool randomBlocks);
static CurveCheck audioVsDisplay (const Move& m, const RunResult& r, double fs, char d, double fmax)
{
    const size_t pre = (size_t) (0.1 * fs), N = (size_t) 1 << 18;
    St in (pre + N);
    const double a = 0.25;
    if (d == 'S') { in.l[pre] = a; in.r[pre] = -a; } else { in.l[pre] = a; in.r[pre] = d == 'L' ? 0.0 : a; }
    Move mm = m; mm.autoGain = false; mm.check = nullptr;
    const auto rr = runMove (mm, in, fs, 512, false);
    std::vector<Cx> Y (N);
    for (size_t i = 0; i < N; ++i)
    {
        const double l = rr.out.l[pre + i], rv = rr.out.r[pre + i];
        Y[i] = d == 'L' ? l : d == 'M' ? 0.5 * (l + rv) : 0.5 * (l - rv);
    }
    fft (Y);
    const double ref = d == 'L' ? a : a;   // amplitud de la entrada en ese dominio
    CurveCheck c;
    const double gainAdj = r.outGain / rr.outGain;   // (Auto Gain apagado en la medicion del impulso)
    for (double f = 20.0; f <= fmax; f *= std::pow (2.0, 1.0 / 48.0))
    {
        const size_t k = (size_t) std::llround (f * N / fs);
        const double meas = std::abs (Y[k]) / ref * gainAdj;
        const double ex = displayCurve (r, k * fs / N, d);
        if (ex < 0.03) continue;   // fondo de un corte o notch (debajo de -30 dB)
        if (r.linear && ! r.natural && ex < 0.1) continue;   // (fase lineal: debajo de -20 dB manda el piso del FIR con ventana)
        // debajo de -20 dB (falda de un corte) se compara la diferencia en amplitud, no en dB: una milesima de
        // amplitud ahi es inaudible pero en dB se ve grande
        const double lim = r.linear && ! r.natural ? 0.25 : 0.1;   // (fase lineal: la falda del FIR desde -12 dB)
        const double dev = ex >= lim ? dB (meas) - dB (ex) : (meas - ex >= 0 ? 1.0 : -1.0) * dB (1.0 + std::abs (meas - ex) / lim);
        ++c.pts;
        if (std::abs (dev) > std::abs (c.maxDev)) { c.maxDev = dev; c.fAt = f; }
    }
    return c;
}

static CurveCheck displayVsIdeal (const RunResult& r, double fs, char d, double fmax)
{
    CurveCheck c;
    for (double f = 20.0; f <= fmax; f *= std::pow (2.0, 1.0 / 48.0))
    {
        const double a = displayCurve (r, f, d) / r.outGain, b = idealCurve (r, f, d) / r.outGain;
        if (f > fmax) break;
        if (b < 0.18 && a < 0.18) continue;   // por debajo de -15 dB (zona ya cortada) no se compara la forma
        const double dev = dB (a) - dB (b);
        ++c.pts;
        if (std::abs (dev) > std::abs (c.maxDev)) { c.maxDev = dev; c.fAt = f; }
    }
    juce::ignoreUnused (fs);
    return c;
}

// Loudness aproximado (ponderacion K, estilo LUFS integrado sin compuerta)
static double lufs (const St& s, double fs, size_t start)
{
    Bq hs[2] = { Bq (makeHighShelf (fs, 1680, 0.71, 4.0)), Bq (makeHighShelf (fs, 1680, 0.71, 4.0)) };
    Bq hp[2] = { Bq (makeHighPass (fs, 38, 0.5)), Bq (makeHighPass (fs, 38, 0.5)) };
    double e = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const double l = hp[0].p (hs[0].p (s.l[i])), r = hp[1].p (hs[1].p (s.r[i]));
        if (i >= start) e += l * l + r * r;
    }
    return -0.691 + 10.0 * std::log10 (std::max (1e-30, e / (s.size() - start)));
}

// Nivel por tramos de tiempo en una banda de frecuencia (para EQ dinamica)
static std::vector<double> bandEnvelopeDb (const std::vector<double>& x, double fs, double f1, double f2, double frameSec = 0.01)
{
    Bq h1 (makeHighPass (fs, f1, 0.7071)), h2 (makeHighPass (fs, f1, 0.7071)), l1 (makeLowPass (fs, f2, 0.7071)), l2 (makeLowPass (fs, f2, 0.7071));
    const size_t fr = (size_t) (frameSec * fs);
    std::vector<double> env; double acc = 0; size_t cnt = 0;
    for (double v : x)
    {
        const double y = l2.p (l1.p (h2.p (h1.p (v))));
        acc += y * y; if (++cnt == fr) { env.push_back (10.0 * std::log10 (std::max (1e-30, acc / fr))); acc = 0; cnt = 0; }
    }
    return env;
}

//==============================================================================
// Ejecucion y reporte
static const RunResult* gLast = nullptr;
static void report (const juce::String& line, bool ok, bool warnOnly = false)
{
    ++gChecks;
    if (! ok) { if (warnOnly) ++gWarn; else ++gFails; }
    std::cout << (ok ? "  ok   " : (warnOnly ? "  AVISO " : "  FALLA ")) << line << std::endl;
}

static void runSessionMove (const Session& s, const St& in, const Move& m)
{
    const double fs = s.fs;
    const auto r = runMove (m, in, fs);
    gLast = &r;
    const double fmax = std::min (19500.0, fs * 0.45);
    bool finite = true;
    for (size_t i = 0; i < r.out.size(); ++i) finite = finite && std::isfinite (r.out.l[i]) && std::isfinite (r.out.r[i]);

    juce::String head = "[" + s.genre + "] " + m.track + " - " + m.name + " (" + (m.vintage ? "Vintage" : "Param") + ", "
                      + juce::String (fs / 1000.0, 1) + " kHz" + (m.quality > 0 ? ", HQ " + juce::String (1 << m.quality) + "x" : juce::String()) + ")";
    std::cout << head << std::endl;
    if (! finite) { report ("salida con NaN/Inf", false); return; }

    const size_t st = (size_t) (0.4 * fs);
    const double lin = lufs (in, fs, st), lout = lufs (r.out, fs, st);
    const double pin = dB (in.peak()), pout = dB (r.out.peak());
    juce::String lvl = "nivel " + juce::String (lin, 1) + " -> " + juce::String (lout, 1) + " LUFS, pico " + juce::String (pin, 1) + " -> " + juce::String (pout, 1) + " dBFS, CPU " + juce::String (r.cpuPct, 2) + " %";

    if (isLinear (m))
    {
        const auto a = audioVsDisplay (m, r, fs, m.domain, fmax);
        const double tol = m.phase == 1 ? 0.5 : m.phase == 2 ? 0.15 : m.phase == 3 ? 0.05 : m.quality > 0 ? 0.25 : 0.02;   // (fase lineal: el FIR resuelve hasta ~fs/largo Hz)
        report ("audio = pantalla (respuesta al impulso real): desvio max " + juce::String (a.maxDev, 3) + " dB en " + juce::String (a.fAt, 0) + " Hz (" + juce::String (a.pts) + " frecuencias)",
                std::abs (a.maxDev) <= tol && a.pts > 10);
        // Curva vs EQ analogico ideal: hasta 12 kHz +-0.5 dB, 12-16 kHz +-1 dB, arriba de 16 kHz +-2 dB (limite fisico de un biquad a 44.1/48 kHz)
        CurveCheck b1 = displayVsIdeal (r, fs, m.domain, 12000), b2 = displayVsIdeal (r, fs, m.domain, 16000), b3 = displayVsIdeal (r, fs, m.domain, fmax);
        // Un high cut muy cerca de Nyquist sin HQ (p. ej. 18 kHz a 44.1 kHz) no puede ser identico al analogico
        // con filtros de 2do orden: se tolera +-0.8 / +-1.25 dB (con HQ desaparece)
        bool nearNyq = false;
        if (m.quality == 0 && ! m.vintage) for (auto& b : m.bands) nearNyq = nearNyq || (b.type == 4 && b.f > 0.33 * fs);
        double t12 = nearNyq ? 0.8 : 0.5, t16 = nearNyq ? 1.25 : 1.0;
        // Cortes empinados (fase minima, sin HQ): en un flanco de 48 dB/oct, 0.6 dB = mover el corte menos
        // del 1 %. La tolerancia equivale a un corrimiento de frecuencia de 1 % (y 1 dB para las secciones de
        // primer orden de 6 y 18 dB/oct, su limite fisico frente a Nyquist).
        if (m.quality == 0 && m.phase == 0 && ! m.vintage)
            for (auto& b : m.bands)
                if (b.type == 3 || b.type == 4)
                {
                    const int db = slopeDbPerOct (b.slope);
                    const double steep = (db / 6) % 2 != 0 ? 1.0 : std::max (0.5, db * std::log2 (1.01));
                    t12 = std::max (t12, steep); t16 = std::max (t16, 1.6 * steep);
                }
        report ("curva = EQ analogico ideal: <12k " + juce::String (b1.maxDev, 2) + " dB, <16k " + juce::String (b2.maxDev, 2) + " dB (en " + juce::String (b2.fAt, 0) + " Hz)"
                + (nearNyq ? juce::String (" [high cut cerca de Nyquist sin HQ: tolerancia +-") + juce::String (t12, 2) + "/" + juce::String (t16, 2) + "]" : juce::String()),
                std::abs (b1.maxDev) <= t12 && std::abs (b2.maxDev) <= t16);
        if (std::abs (b3.maxDev) > 2.0)
            report ("arriba de 16 kHz difiere del analogico " + juce::String (b3.maxDev, 1) + " dB en " + juce::String (b3.fAt, 0) + " Hz (limite fisico sin HQ a " + juce::String (fs / 1000, 1) + " kHz; con HQ desaparece)", false, true);
    }
    report (lvl, pout < 12.0 && finite);
    bool anyDyn = false; for (auto& b : m.bands) anyDyn = anyDyn || b.dyn;
    if (anyDyn)
    {
        double mx = 0, lim = 0; for (auto& b : m.bands) if (b.dyn) lim = std::abs (b.range) > std::abs (lim) ? b.range : lim;
        for (double g : r.dynTrace) if (std::abs (g) > std::abs (mx)) mx = g;
        report ("EQ dinamica (motor): llega a " + juce::String (mx, 1) + " dB con rango " + juce::String (lim, 1) + " dB (nunca se pasa del rango); detector max " + juce::String (r.detMax, 1) + " dB, umbral " + juce::String (r.thrSeen, 1) + " dB", std::abs (mx) <= std::abs (lim) + 0.01 && std::abs (mx) > 0.5);
    }
    if (m.check)
    {
        juce::String msg;
        const bool ok = m.check (in, r.out, fs, msg);
        report (msg, ok);
    }
}

//==============================================================================
static bool want (const juce::String& s) { return gFilter.isEmpty() || s.containsIgnoreCase (gFilter); }

static void genreTests()
{
    const double secs = 7.0;

    // ---------------- TECH HOUSE ----------------
    if (want ("tech"))
    {
        auto s = makeTechHouse (48000, secs);
        Move m;
        m = {}; m.track = "KICK"; m.name = "low cut 25 Hz 24 dB + punch 55 Hz +3 + caja 300 Hz -4 Q2 + click 4 kHz +2";
        m.bands = { lcut (25, 1), bell (55, 3, 1.2), bell (300, -4, 2), bell (4000, 2, 1) }; runSessionMove (s, s.stems["KICK"], m);
        m = {}; m.track = "HATS"; m.name = "low cut 400 Hz 12 dB + aire shelf 12 kHz +3";
        m.bands = { lcut (400, 0), hshelf (12000, 3, 0.7) }; runSessionMove (s, s.stems["HATS"], m);
        m = {}; m.track = "BAJO ROLLING"; m.name = "graves se apartan cuando pega el kick (dinamica con sidechain del kick)";
        m.bands = { dynb (lshelf (120, 0, 0.7), -10, -9, 2, 90, true) }; m.sidechain = &s.stems["KICK"];
        m.check = [&] (const St& in, const St& out, double fs, juce::String& msg)
        {
            // Energia de graves del bajo cuando suena el kick (primeros 40 ms) vs cuando no
            const double beat = 60.0 / 124.0; const auto ei = bandEnvelopeDb (domainOf (in, 'M'), fs, 30, 150), eo = bandEnvelopeDb (domainOf (out, 'M'), fs, 30, 150);
            double onDiff = 0, offDiff = 0; int on = 0, off = 0;
            for (size_t k = 50; k < ei.size(); ++k)
            {
                const double t = k * 0.01, ph = std::fmod (t, beat);
                if (ei[k] < -70) continue;
                if (ph > 0.01 && ph < 0.06) { onDiff += eo[k] - ei[k]; ++on; } else if (ph > beat * 0.6) { offDiff += eo[k] - ei[k]; ++off; }
            }
            onDiff /= std::max (1, on); offDiff /= std::max (1, off);
            msg = "con el kick los graves del bajo bajan " + juce::String (-onDiff, 1) + " dB; sin kick " + juce::String (-offDiff, 1) + " dB";
            return onDiff < -4.0 && offDiff > -1.5;
        };
        runSessionMove (s, s.stems["BASS_ROLL"], m);
        m = {}; m.track = "MASTER"; m.name = "sub limpio 20 Hz 48 dB + graves en mono (Side low cut 120 Hz) + brillo 10 kHz +1.5";
        m.bands = { lcut (20, 3), lcut (120, 1, 0.707, 4), hshelf (10000, 1.5, 0.7), bell (250, -1, 0.8) }; m.domain = 'M';
        m.check = [&] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto si = welch (domainOf (in, 'S'), domainOf (out, 'S'), fs, 20000);
            double ei = 0, eo = 0; for (size_t k = 1; k < 16384 * 60 / (size_t) fs; ++k) { ei += si.sxx[k]; eo += si.syy[k]; }
            double mi = 0, mo = 0; { const auto sm = welch (domainOf (in, 'M'), domainOf (out, 'M'), fs, 20000); for (size_t k = (size_t) (16384 * 40 / fs); k < 16384 * 80 / (size_t) fs; ++k) { mi += sm.sxx[k]; mo += sm.syy[k]; } }
            msg = "lados bajo 60 Hz: " + juce::String (10 * std::log10 (eo / std::max (1e-30, ei)), 1) + " dB (debe ser mono); centro 40-80 Hz: " + juce::String (10 * std::log10 (mo / mi), 2) + " dB";
            return 10 * std::log10 (eo / std::max (1e-30, ei)) < -12 && std::abs (10 * std::log10 (mo / mi)) < 0.6;
        };
        runSessionMove (s, s.mix, m);
        m = {}; m.track = "MASTER"; m.name = "lo mismo en HQ 4x (para comparar)"; m.quality = 2;
        m.bands = { lcut (20, 3), hshelf (10000, 1.5, 0.7), bell (250, -1, 0.8) }; runSessionMove (s, s.mix, m);
        m = {}; m.track = "KICK"; m.vintage = true; m.name = "truco Pultec 60 Hz (boost 5 + atten 3) + high 5k boost 2";
        m.vin.kv = { { ParamID::vLowFreq, 2 }, { ParamID::vLowBoost, 5 }, { ParamID::vLowAtten, 3 }, { ParamID::vHighFreq, 2 }, { ParamID::vHighBoost, 2 } };
        runSessionMove (s, s.stems["KICK"], m);
        m = {}; m.track = "MASTER"; m.vintage = true; m.name = "glue vintage: low 30 boost 2, high 12k boost 2 bw 8, soften 20k 1";
        m.vin.kv = { { ParamID::vLowFreq, 1 }, { ParamID::vLowBoost, 2 }, { ParamID::vHighFreq, 5 }, { ParamID::vHighBoost, 2 }, { ParamID::vHighBw, 8 }, { ParamID::vAttenFreq, 2 }, { ParamID::vHighAtten, 1 } };
        m.domain = 'M'; runSessionMove (s, s.mix, m);
        m = {}; m.track = "MASTER"; m.vintage = true; m.name = "color: lo anterior + GLOW 20 % + IRON 15 %";
        m.vin.kv = { { ParamID::vLowFreq, 1 }, { ParamID::vLowBoost, 2 }, { ParamID::vHighFreq, 5 }, { ParamID::vHighBoost, 2 }, { ParamID::vHighBw, 8 }, { ParamID::vTube, 20 }, { ParamID::vIron, 15 } };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const double d = lufs (out, fs, 20000) - lufs (in, fs, 20000);
            msg = "el color no cambia el volumen de forma brusca: " + juce::String (d, 2) + " dB (esperado ~ +1 por la curva)";
            return d > -0.5 && d < 3.0;
        };
        runSessionMove (s, s.mix, m);
    }

    // ---------------- DUBSTEP ----------------
    if (want ("dubstep"))
    {
        auto s = makeDubstep (48000, secs);
        Move m;
        m = {}; m.track = "GROWL"; m.name = "notch resonancia 1.2 kHz Q 8 + low cut 150 Hz 24 dB (deja lugar al sub) + presencia 3k +3";
        m.bands = { notch (1200, 8), lcut (150, 1), bell (3000, 3, 1.2) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto S = welch (domainOf (in, 'M'), domainOf (out, 'M'), fs, 20000);
            double a = 0, b = 0; for (size_t i = (size_t) (1170.0 * 16384 / fs); i <= (size_t) (1230.0 * 16384 / fs); ++i) { a += S.sxx[i]; b += S.syy[i]; }
            msg = "zona resonante 1170-1230 Hz reducida " + juce::String (-10 * std::log10 (b / a), 1) + " dB";
            return 10 * std::log10 (b / a) < -10;
        };
        runSessionMove (s, s.stems["GROWL"], m);
        m = {}; m.track = "SUB"; m.name = "high cut 120 Hz 24 dB + low cut 25 Hz";
        m.bands = { hcut (120, 1), lcut (25, 1) }; runSessionMove (s, s.stems["SUB"], m);
        m = {}; m.track = "GROWL"; m.name = "saturacion FUZZ 18 dB en 2 kHz (agresividad) - nivel estable";
        m.bands = { sat (bell (2000, 2, 1.0), 4, 18, 70) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const double d = lufs (out, fs, 20000) - lufs (in, fs, 20000);
            msg = "volumen con saturacion: " + juce::String (d, 2) + " dB (la saturacion no debe disparar el nivel)";
            return std::abs (d) < 4.0;
        };
        runSessionMove (s, s.stems["GROWL"], m);
        m = {}; m.track = "MASTER"; m.name = "master en HQ 2x: low cut 22 Hz 36 dB + shelf 80 Hz +1 + 2.5k -1 + aire 14k +1.5"; m.quality = 1;
        m.bands = { lcut (22, 2), lshelf (80, 1, 0.7), bell (2500, -1, 1.0), hshelf (14000, 1.5, 0.7) }; runSessionMove (s, s.mix, m);
    }

    // ---------------- POP ----------------
    if (want ("pop"))
    {
        auto s = makePop (44100, secs);
        Move m;
        m = {}; m.track = "VOZ"; m.name = "low cut 90 Hz 24 dB + barro 250 -3 Q1.4 + presencia 3k +2 + aire 12k +3 (44.1 kHz)";
        m.bands = { lcut (90, 1), bell (250, -3, 1.4), bell (3000, 2, 1.0), hshelf (12000, 3, 0.7) }; runSessionMove (s, s.stems["VOZ"], m);
        m = {}; m.track = "VOZ"; m.name = "aire tipo 'Pro' en 16 kHz bell +4 Q 0.7 (44.1 kHz, sin HQ)";
        m.bands = { bell (16000, 4, 0.7) }; runSessionMove (s, s.stems["VOZ"], m);
        m = {}; m.track = "VOZ"; m.name = "de-esser dinamico 7 kHz (-8 dB, ataque 1 ms)";
        m.bands = { dynb (bell (7200, 0, 2.0), -38, -9, 1, 60) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto ei = bandEnvelopeDb (domainOf (in, 'M'), fs, 5000, 10000), eo = bandEnvelopeDb (domainOf (out, 'M'), fs, 5000, 10000);
            const auto vi = bandEnvelopeDb (domainOf (in, 'M'), fs, 200, 2000), vo = bandEnvelopeDb (domainOf (out, 'M'), fs, 200, 2000);
            double sib = 0, body = 0; int ns = 0, nb = 0;
            const double hiMax = *std::max_element (ei.begin() + 40, ei.end());
            for (size_t k = 40; k < ei.size(); ++k)
            {
                if (ei[k] > hiMax - 6) { sib += eo[k] - ei[k]; ++ns; }
                if (ei[k] < hiMax - 30 && vi[k] > -60) { body += vo[k] - vi[k]; ++nb; }
            }
            sib /= std::max (1, ns); body /= std::max (1, nb);
            msg = "sibilantes bajan " + juce::String (-sib, 1) + " dB (" + juce::String (ns) + " tramos), cuerpo de la voz cambia " + juce::String (body, 2) + " dB";
            return sib < -4.0 && std::abs (body) < 0.5;
        };
        runSessionMove (s, s.stems["VOZ"], m);
        m = {}; m.track = "PIANO"; m.name = "shelf 120 Hz -2 + 2.5k +1.5 Q 0.9";
        m.bands = { lshelf (120, -2, 0.7), bell (2500, 1.5, 0.9) }; runSessionMove (s, s.stems["PIANO"], m);
        m = {}; m.track = "MIX BUS"; m.name = "M/S: presencia en el centro (Mid 2k +1.5) y ancho (Side shelf 8k +2)";
        m.bands = { bell (2000, 1.5, 0.9, 3), hshelf (8000, 2, 0.7, 4) }; m.domain = 'M'; runSessionMove (s, s.mix, m);
        m.domain = 'S'; m.name = "M/S (lado Side)"; runSessionMove (s, s.mix, m);
        m = {}; m.track = "VOZ"; m.vintage = true; m.name = "Pultec vocal: high 10k boost 4 bw 6, soften 20k 1, dip 300 2, HPF 80";
        m.vin.kv = { { ParamID::vHighFreq, 4 }, { ParamID::vHighBoost, 4 }, { ParamID::vHighBw, 6 }, { ParamID::vAttenFreq, 2 }, { ParamID::vHighAtten, 1 }, { ParamID::vDipFreq, 1 }, { ParamID::vDip, 2 }, { ParamID::vLowCut, 2 } };
        runSessionMove (s, s.stems["VOZ"], m);
        m = {}; m.track = "MIX BUS"; m.name = "Auto Gain: curva sonriente (+3 graves, +3 aire) debe quedar igual de fuerte"; m.autoGain = true;
        m.bands = { lshelf (90, 3, 0.7), hshelf (9000, 3, 0.7) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const double d = lufs (out, fs, 20000) - lufs (in, fs, 20000);
            msg = "diferencia de loudness con Auto Gain: " + juce::String (d, 2) + " dB";
            return std::abs (d) < 1.0;
        };
        runSessionMove (s, s.mix, m);
        m = {}; m.track = "VOZ"; m.name = "Auto Gain: low cut 120 Hz (no debe subir el volumen)"; m.autoGain = true;
        m.bands = { lcut (120, 1) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const double d = lufs (out, fs, 20000) - lufs (in, fs, 20000);
            msg = "diferencia de loudness con Auto Gain: " + juce::String (d, 2) + " dB";
            return std::abs (d) < 1.0;
        };
        runSessionMove (s, s.stems["VOZ"], m);
    }

    // ---------------- ROCK ----------------
    if (want ("rock"))
    {
        auto s = makeRock (44100, secs);
        Move m;
        m = {}; m.track = "GUITARRAS"; m.name = "low cut 100 Hz + 400 Hz -3 Q1.5 + 3k +2 + high cut 9 kHz 12 dB";
        m.bands = { lcut (100, 1), bell (400, -3, 1.5), bell (3000, 2, 1.0), hcut (9000, 0) }; runSessionMove (s, s.stems["GUITARRAS"], m);
        m = {}; m.track = "SNARE"; m.name = "cuerpo 200 +3, anillo 900 Hz -4 Q5, crack 5k shelf +2";
        m.bands = { bell (200, 3, 1.2), bell (900, -4, 5), hshelf (5000, 2, 0.7) }; runSessionMove (s, s.stems["SNARE"], m);
        m = {}; m.track = "BAJO"; m.name = "low cut 35, 80 Hz +3, growl 800 Hz +3 Q1.2, high cut 5k";
        m.bands = { lcut (35, 1), bell (80, 3, 1.0), bell (800, 3, 1.2), hcut (5000, 1) }; runSessionMove (s, s.stems["BAJO"], m);
        m = {}; m.track = "GUITARRAS"; m.vintage = true; m.name = "MID: dip 500 4, mid-high 3k +3, FILTER high cut 8k";
        m.vin.kv = { { ParamID::vDipFreq, 2 }, { ParamID::vDip, 4 }, { ParamID::vHmFreq, 2 }, { ParamID::vHmBoost, 3 }, { ParamID::vHighCut, 6 } };
        runSessionMove (s, s.stems["GUITARRAS"], m);
        m = {}; m.track = "MASTER"; m.name = "master rock 44.1: low cut 30 Hz 24 dB, 3.5k -1 Q2, shelf 15k +2";
        m.bands = { lcut (30, 1), bell (3500, -1, 2), hshelf (15000, 2, 0.7) }; runSessionMove (s, s.mix, m);
    }

    // ---------------- REGGAETON ----------------
    if (want ("reggaeton"))
    {
        auto s = makeReggaeton (48000, secs);
        Move m;
        m = {}; m.track = "808"; m.name = "low cut 28 Hz 24 dB + 55 Hz +2 + 180 Hz -2 Q1.5";
        m.bands = { lcut (28, 1), bell (55, 2, 1.0), bell (180, -2, 1.5) }; runSessionMove (s, s.stems["808"], m);
        m = {}; m.track = "808"; m.name = "808 que se oye en celular: TUBE drive 9 en shelf 120 Hz";
        m.bands = { sat (lshelf (120, 0, 0.7), 0, 9, 100) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto S = welch (domainOf (in, 'M'), domainOf (out, 'M'), fs, 20000);
            double hi = 0, ho = 0; for (size_t k = (size_t) (300.0 * 16384 / fs); k < (size_t) (2000.0 * 16384 / fs); ++k) { hi += S.sxx[k]; ho += S.syy[k]; }
            const double d = lufs (out, fs, 20000) - lufs (in, fs, 20000);
            msg = "armonicos 300 Hz-2 kHz (los que oye un celular): " + juce::String (10 * std::log10 (ho / hi), 1) + " dB; loudness " + juce::String (d, 2) + " dB";
            return 10 * std::log10 (ho / hi) > 0.5 && std::abs (d) < 2.0;
        };
        runSessionMove (s, s.stems["808"], m);
        m = {}; m.track = "VOZ"; m.name = "low cut 100 Hz + 2.8k +2 + aire 10k +2 + de-esser 6.5 kHz";
        m.bands = { lcut (100, 1), bell (2800, 2, 1.0), hshelf (10000, 2, 0.7), dynb (bell (6500, 0, 2.5), -36, -8, 1, 70) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto ei = bandEnvelopeDb (domainOf (in, 'M'), fs, 5000, 9000), eo = bandEnvelopeDb (domainOf (out, 'M'), fs, 5000, 9000);
            const double hiMax = *std::max_element (ei.begin() + 40, ei.end());
            double sib = 0; int ns = 0; for (size_t k = 40; k < ei.size(); ++k) if (ei[k] > hiMax - 6) { sib += eo[k] - ei[k]; ++ns; }
            sib /= std::max (1, ns);
            msg = "sibilantes (con el shelf de aire +2 incluido): " + juce::String (sib, 1) + " dB";
            return sib < -2.0;
        };
        runSessionMove (s, s.stems["VOZ"], m);
        m = {}; m.track = "MASTER"; m.name = "Side low cut 150 Hz + shelf dinamico 80 Hz (controla los picos del 808)";
        m.bands = { lcut (150, 1, 0.707, 4), dynb (lshelf (80, 0, 0.7), -14, -4, 5, 150) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto ei = bandEnvelopeDb (domainOf (in, 'M'), fs, 30, 90), eo = bandEnvelopeDb (domainOf (out, 'M'), fs, 30, 90);
            double mx = 0; for (size_t k = 40; k < ei.size(); ++k) mx = std::min (mx, eo[k] - ei[k]);
            msg = "reduccion maxima de graves en los picos del 808: " + juce::String (-mx, 1) + " dB (rango -4)";
            return mx < -1.0 && mx > -4.6;
        };
        runSessionMove (s, s.mix, m);
        m = {}; m.track = "MIX"; m.name = "sidechain general: kick aplana la curva del bajo"; m.scGeneral = true; m.sidechain = &s.stems["KICK"]; m.scThr = -12; m.scDuck = 100;
        m.bands = { lshelf (100, 4, 0.7) };
        m.check = [&] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto ei = bandEnvelopeDb (domainOf (in, 'M'), fs, 30, 90), eo = bandEnvelopeDb (domainOf (out, 'M'), fs, 30, 90);
            const double beat = 60.0 / 95.0; double on = 0, off = 0; int a = 0, b = 0;
            for (size_t k = 50; k < ei.size(); ++k) { const double ph = std::fmod (k * 0.01, beat); if (ph > 0.01 && ph < 0.05) { on += eo[k] - ei[k]; ++a; } else if (ph > beat * 0.7) { off += eo[k] - ei[k]; ++b; } }
            on /= std::max (1, a); off /= std::max (1, b);
            msg = "realce de graves con kick sonando " + juce::String (on, 1) + " dB, sin kick " + juce::String (off, 1) + " dB";
            return on < off - 1.0;
        };
        runSessionMove (s, s.stems["808"], m);
    }

    // ---------------- JAZZ ----------------
    if (want ("jazz"))
    {
        auto s = makeJazz (44100, secs);
        Move m;
        m = {}; m.track = "CONTRABAJO"; m.name = "low cut 40 Hz 12 dB, 200 -2 Q1, definicion 700 +2 (senal a -14 dBFS)";
        m.bands = { lcut (40, 0), bell (200, -2, 1.0), bell (700, 2, 1.0) }; runSessionMove (s, s.stems["CONTRABAJO"], m);
        m = {}; m.track = "RIDE"; m.name = "4k -2 Q2, aire shelf 11k +1.5";
        m.bands = { bell (4000, -2, 2), hshelf (11000, 1.5, 0.7) }; runSessionMove (s, s.stems["RIDE"], m);
        m = {}; m.track = "MASTER"; m.vintage = true; m.name = "Pultec sutil: low 100 boost 1.5, high 10k 1.5 bw 7";
        m.vin.kv = { { ParamID::vLowFreq, 3 }, { ParamID::vLowBoost, 1.5f }, { ParamID::vHighFreq, 4 }, { ParamID::vHighBoost, 1.5f }, { ParamID::vHighBw, 7 } };
        m.domain = 'M'; runSessionMove (s, s.mix, m);
    }

    // ---------------- ORQUESTA ----------------
    if (want ("orquesta"))
    {
        auto s = makeOrchestra (96000, secs);
        Move m;
        m = {}; m.track = "MASTER (pp -> ff, 96 kHz)"; m.name = "low cut 20 Hz 12 dB, 300 -1 Q0.5, shelf 12k +1 Q0.5";
        m.bands = { lcut (20, 0), bell (300, -1, 0.5), hshelf (12000, 1, 0.5) };
        m.domain = 'M';
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            // En el pasaje muy suave (-60 dBFS) el EQ debe ser igual de preciso que en el fuerte (sin ruido propio)
            juce::ignoreUnused (fs);
            double ei = 0, eo = 0; for (size_t i = 20000; i < 100000; ++i) { ei += in.l[i] * in.l[i]; eo += out.l[i] * out.l[i]; }
            msg = "pasaje pianissimo (" + juce::String (10 * std::log10 (ei / 80000), 1) + " dBFS): salida " + juce::String (10 * std::log10 (eo / 80000), 1) + " dBFS";
            return std::isfinite (eo) && eo > 0 && std::abs (10 * std::log10 (eo / ei)) < 1.5;
        };
        runSessionMove (s, s.mix, m);
        m = {}; m.track = "VIOLINES"; m.name = "aspereza 2.8k -2 Q2 + aire 16k +2 (96 kHz)";
        m.bands = { bell (2800, -2, 2), bell (16000, 2, 0.8) }; runSessionMove (s, s.stems["VIOLINES"], m);
    }

    // ---------------- TRAP ----------------
    if (want ("trap"))
    {
        auto s = makeTrap (48000, secs);
        Move m;
        m = {}; m.track = "VOZ RAP"; m.name = "low cut 100 Hz 24 dB + proximidad dinamica 200 Hz (-5) + 4k +3";
        m.bands = { lcut (100, 1), dynb (bell (200, 0, 1.0), -26, -5, 5, 100), bell (4000, 3, 1.0) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto ei = bandEnvelopeDb (domainOf (in, 'M'), fs, 140, 280), eo = bandEnvelopeDb (domainOf (out, 'M'), fs, 140, 280);
            const double mx = *std::max_element (ei.begin() + 40, ei.end()); double loud = 0, soft = 0; int nl = 0, ns = 0;
            for (size_t k = 40; k < ei.size(); ++k) { if (ei[k] > mx - 4) { loud += eo[k] - ei[k]; ++nl; } else if (ei[k] < mx - 20 && ei[k] > -70) { soft += eo[k] - ei[k]; ++ns; } }
            loud /= std::max (1, nl); soft /= std::max (1, ns);
            msg = "zona de proximidad: en las partes fuertes " + juce::String (loud, 1) + " dB, en las suaves " + juce::String (soft, 1) + " dB";
            return loud < soft - 1.5;
        };
        runSessionMove (s, s.stems["VOZ RAP"], m);
        m = {}; m.track = "808"; m.vintage = true; m.name = "Pultec en 808: low 30 boost 4 atten 2, FILTER low cut 50";
        m.vin.kv = { { ParamID::vLowFreq, 1 }, { ParamID::vLowBoost, 4 }, { ParamID::vLowAtten, 2 }, { ParamID::vLowCut, 1 } };
        runSessionMove (s, s.stems["808"], m);
        m = {}; m.track = "HATS"; m.name = "low cut 600 Hz 36 dB + high shelf 9k -2 (menos filo)";
        m.bands = { lcut (600, 2), hshelf (9000, -2, 0.7) }; runSessionMove (s, s.stems["HATS"], m);
    }

    // ---------------- METAL ----------------
    if (want ("metal"))
    {
        auto s = makeMetal (44100, secs);
        Move m;
        m = {}; m.track = "GUITARRAS"; m.name = "low cut 110 Hz 24 dB + fizz 6.5k -3 Q3 + chug dinamico 180 Hz (-4)";
        m.bands = { lcut (110, 1), bell (6500, -3, 3), dynb (bell (180, 0, 1.2), -24, -4, 3, 60) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto ei = bandEnvelopeDb (domainOf (in, 'L'), fs, 140, 230), eo = bandEnvelopeDb (domainOf (out, 'L'), fs, 140, 230);
            const double mx = *std::max_element (ei.begin() + 40, ei.end()); double loud = 0, soft = 0; int nl = 0, ns = 0;
            for (size_t k = 40; k < ei.size(); ++k) { if (ei[k] > mx - 3) { loud += eo[k] - ei[k]; ++nl; } else if (ei[k] < mx - 15 && ei[k] > -70) { soft += eo[k] - ei[k]; ++ns; } }
            loud /= std::max (1, nl); soft /= std::max (1, ns);
            msg = "chugs: en los golpes fuertes " + juce::String (loud, 1) + " dB, en lo suave " + juce::String (soft, 1) + " dB";
            return loud < soft - 1.5;
        };
        runSessionMove (s, s.stems["GUITARRAS"], m);
        m = {}; m.track = "KICK"; m.name = "click 4k +5 Q2, caja 400 -4 Q2, low cut 40 Hz (senal caliente)";
        m.bands = { bell (4000, 5, 2), bell (400, -4, 2), lcut (40, 1) }; runSessionMove (s, s.stems["KICK"], m);
        m = {}; m.track = "MASTER (+3 dBFS de entrada)"; m.name = "entra pasado de 0 dBFS (bus sumado): lineal y sin clip interno";
        St hot = s.mix; for (auto* v : { &hot.l, &hot.r }) for (auto& x : *v) x *= 1.41;
        m.bands = { bell (100, -1.5, 1), hshelf (12000, 1, 0.7) }; runSessionMove (s, hot, m);
    }

    // ---------------- FOLK ----------------
    if (want ("folk"))
    {
        auto s = makeFolk (48000, secs);
        Move m;
        m = {}; m.track = "GUITARRA"; m.name = "low cut 80 Hz, boom 110 Hz -5 Q2, brillo 10k +2";
        m.bands = { lcut (80, 0), bell (110, -5, 2), hshelf (10000, 2, 0.7) }; runSessionMove (s, s.stems["GUITARRA"], m);
        m = {}; m.track = "VOZ"; m.vintage = true; m.name = "Pultec voz calida: low 100 boost 2 atten 1, high 8k 3 bw 4, GLOW 30 %";
        m.vin.kv = { { ParamID::vLowFreq, 3 }, { ParamID::vLowBoost, 2 }, { ParamID::vLowAtten, 1 }, { ParamID::vHighFreq, 3 }, { ParamID::vHighBoost, 3 }, { ParamID::vHighBw, 4 }, { ParamID::vTube, 30 } };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const double d = lufs (out, fs, 20000) - lufs (in, fs, 20000);
            msg = "volumen: " + juce::String (d, 2) + " dB";
            return d > -1 && d < 4;
        };
        runSessionMove (s, s.stems["VOZ"], m);
    }

    // ---------------- PODCAST ----------------
    if (want ("podcast"))
    {
        auto s = makePodcast (48000, secs);
        Move m;
        m = {}; m.track = "VOZ"; m.name = "quitar zumbido: notch 60/120/180 Hz Q 12 + low cut 70 Hz 24 dB";
        m.bands = { notch (60, 12), notch (120, 12), notch (180, 12), lcut (70, 1) };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto S = welch (domainOf (in, 'M'), domainOf (out, 'M'), fs, 20000);
            // Amplitud exacta del zumbido (detector sincronico: ignora la voz)
            auto lockin = [&] (const St& x, double f) { Cx acc (0); for (size_t i = 48000; i < x.size(); ++i) acc += x.l[i] * std::polar (1.0, -2 * kPi * f * i / fs); return std::abs (acc); };
            const double r60 = dB (lockin (out, 60) / lockin (in, 60)), r120 = dB (lockin (out, 120) / lockin (in, 120));
            double vi = 0, vo = 0; for (size_t k = (size_t) (300.0 * 16384 / fs); k < (size_t) (4000.0 * 16384 / fs); ++k) { vi += S.sxx[k]; vo += S.syy[k]; }
            msg = "zumbido 60 Hz " + juce::String (r60, 1) + " dB, 120 Hz " + juce::String (r120, 1) + " dB; voz 300 Hz-4 kHz " + juce::String (10 * std::log10 (vo / vi), 2) + " dB";
            return r60 < -25 && r120 < -25 && std::abs (10 * std::log10 (vo / vi)) < 0.3;
        };
        runSessionMove (s, s.stems["VOZ"], m);
        m = {}; m.track = "VOZ"; m.name = "radio: 150 Hz +2, 3k +3 Q1, de-esser 6k, high cut 15k";
        m.bands = { bell (150, 2, 1), bell (3000, 3, 1), dynb (bell (6000, 0, 2), -36, -6, 1, 60), hcut (15000, 1) };
        runSessionMove (s, s.stems["VOZ"], m);
    }

    // ---------------- AMBIENT ----------------
    if (want ("ambient"))
    {
        auto s = makeAmbient (48000, secs);
        Move m;
        m = {}; m.track = "PADS"; m.name = "Q extremas: campana super ancha 800 Hz -2 Q0.1 + resonancia 2.4k +6 Q 18";
        m.bands = { bell (800, -2, 0.1), bell (2400, 6, 18) }; runSessionMove (s, s.stems["PADS"], m);
        m = {}; m.track = "PADS"; m.name = "ancho: Side shelf 3k +3, Mid 300 -2";
        m.bands = { hshelf (3000, 3, 0.7, 4), bell (300, -2, 0.8, 3) }; m.domain = 'S';
        runSessionMove (s, s.stems["PADS"], m);
        m = {}; m.track = "PADS"; m.name = "compatibilidad mono: solo Side shelf 3k +6";
        m.bands = { hshelf (3000, 6, 0.7, 4) }; m.domain = 'S';
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            juce::ignoreUnused (fs);
            double d = 0, pk = 0; for (size_t i = 0; i < in.size(); ++i) { d = std::max (d, std::abs ((out.l[i] + out.r[i]) - (in.l[i] + in.r[i]))); pk = std::max (pk, std::abs (in.l[i] + in.r[i])); }
            msg = "la suma mono (L+R) queda intacta: diferencia " + juce::String (dB (d / pk), 1) + " dB";
            return dB (d / pk) < -90;
        };
        runSessionMove (s, s.stems["PADS"], m);
    }

    // ---------------- SALSA ----------------
    if (want ("salsa"))
    {
        auto s = makeSalsa (48000, secs);
        Move m;
        m = {}; m.track = "METALES"; m.name = "honk 1.2k -2 Q1, aspereza shelf 6k -2, low cut 120";
        m.bands = { bell (1200, -2, 1), hshelf (6000, -2, 0.7), lcut (120, 0) }; runSessionMove (s, s.stems["METALES"], m);
        m = {}; m.track = "CONGAS"; m.name = "tono 220 +2, slap 5k +2 Q1.5";
        m.bands = { bell (220, 2, 1.2), bell (5000, 2, 1.5) }; runSessionMove (s, s.stems["CONGAS"], m);
        m = {}; m.track = "PIANO"; m.vintage = true; m.name = "Pultec piano: mid-low 500 +2, high 5k 2 bw 5, FILTER low cut 80";
        m.vin.kv = { { ParamID::vLmFreq, 2 }, { ParamID::vLmBoost, 2 }, { ParamID::vHighFreq, 2 }, { ParamID::vHighBoost, 2 }, { ParamID::vLowCut, 2 } };
        runSessionMove (s, s.stems["PIANO"], m);
    }

    // ---------------- DNB ----------------
    if (want ("dnb") || want ("drum"))
    {
        auto s = makeDnb (44100, secs);
        Move m;
        m = {}; m.track = "REESE"; m.name = "low cut 35 Hz 48 dB, Mid 60 Hz +2, Side low cut 200";
        m.bands = { lcut (35, 3), bell (60, 2, 1, 3), lcut (200, 1, 0.707, 4) }; m.domain = 'M'; runSessionMove (s, s.stems["REESE"], m);
        m = {}; m.track = "DRUMS"; m.name = "high cut 48 dB a 18 kHz + low cut 48 dB a 30 Hz (44.1 kHz)";
        m.bands = { hcut (18000, 3), lcut (30, 3) }; runSessionMove (s, s.stems["DRUMS"], m);
    }
}

//==============================================================================
// Transparencia, latencia, bypass, tamanos de bloque, sample rates
static void transparencyTests()
{
    if (! want ("transp")) return;
    std::cout << "\n== TRANSPARENCIA Y CONFIABILIDAD ==" << std::endl;
    auto pop = makePop (48000, 4.0);
    const St& x = pop.mix;
    const double fs = 48000;

    // 1) Todo apagado = bit-exacto
    {
        Move m; const auto r = runMove (m, x, fs);
        double d = 0; for (size_t i = 0; i < x.size(); ++i) d = std::max (d, std::abs (r.out.l[i] - (double) (float) x.l[i]));
        report ("Parametrico sin bandas: salida identica a la entrada (dif " + juce::String (d) + ")", d == 0.0);
    }
    {
        Move m; m.bands = { bell (1000, 0, 1), lshelf (100, 0), hshelf (8000, 0) };
        const auto r = runMove (m, x, fs);
        double d = 0; for (size_t i = 0; i < x.size(); ++i) d = std::max (d, std::abs (r.out.l[i] - (double) (float) x.l[i]));
        report ("Parametrico con bandas encendidas en 0 dB: diferencia " + juce::String (dB (d / x.peak()), 1) + " dB", dB (d / x.peak()) < -100);
    }
    {
        Move m; m.vintage = true; const auto r = runMove (m, x, fs);
        double d = 0; for (size_t i = 0; i < x.size(); ++i) d = std::max (d, std::abs (r.out.l[i] - (double) (float) x.l[i]));
        report ("Vintage con todo en 0: diferencia " + juce::String (dB (d / x.peak()), 1) + " dB", dB (d / x.peak()) < -90);
    }
    // 2) HQ plano: respuesta 20 Hz-20 kHz
    for (int q : { 1, 2 })
        for (double sr : { 44100.0, 48000.0 })
        {
            auto s = makePop (sr, 4.0);
            Move m; m.quality = q; const auto r = runMove (m, s.mix, sr);
            const auto S = welch (domainOf (s.mix, 'L'), domainOf (r.out, 'L'), sr, 20000);
            double worst = 0, fw = 0;
            for (double f = 30; f < 20000; f *= 1.05)
            {
                const size_t k = (size_t) (f * 16384 / sr);
                if (S.sxx[k] < 1e-6 * *std::max_element (S.sxx.begin(), S.sxx.end())) continue;
                const double g = dB (std::abs (S.sxy[k]) / S.sxx[k]);
                if (std::abs (g) > std::abs (worst)) { worst = g; fw = f; }
            }
            report ("HQ " + juce::String (1 << q) + "x a " + juce::String (sr / 1000, 1) + " kHz sin bandas: respuesta plana, desvio max " + juce::String (worst, 2) + " dB en " + juce::String (fw, 0) + " Hz, latencia " + juce::String (r.latency, 0) + " muestras", std::abs (worst) < 0.2);
        }
    // 3) Bypass en HQ: la senal sin procesar debe salir alineada con la latencia que se reporta al DAW
    for (int q : { 0, 1, 2 })
    {
        GauxEqProcessor p; setP (p, ParamID::quality, (float) q); setP (p, ParamID::band (0, "on"), 1); setP (p, ParamID::band (0, "gain"), 6);
        setP (p, ParamID::bypass, 1);
        p.prepareToPlay (fs, 512);
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer midi;
        int imp = -1;
        for (int blk = 0; blk < 20; ++blk)
        {
            b.clear(); if (blk == 10) { b.setSample (0, 100, 1.0f); b.setSample (1, 100, 1.0f); }
            p.processBlock (b, midi);
            for (int i = 0; i < 512; ++i) if (std::abs (b.getSample (0, i)) > 0.5f && imp < 0) imp = blk * 512 + i - (10 * 512 + 100);
        }
        report ("Bypass con calidad " + juce::String (q == 0 ? "Normal" : "HQ " + juce::String (1 << q) + "x") + ": la senal sale con " + juce::String (imp) + " muestras de retraso (el DAW compensa " + juce::String (p.getLatencySamples()) + ")", imp == p.getLatencySamples());
    }
    // 4) Activado vs bypass alineados (sin efecto 'flanger' al prender/apagar en HQ)
    {
        GauxEqProcessor p; setP (p, ParamID::quality, 1); p.prepareToPlay (fs, 512);
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer midi;
        // Impulso sin bypass: donde sale
        int impOn = -1;
        for (int blk = 0; blk < 6; ++blk) { b.clear(); if (blk == 3) { b.setSample (0, 0, 1.0f); } p.processBlock (b, midi); for (int i = 0; i < 512; ++i) if (impOn < 0 && std::abs (b.getSample (0, i)) > 0.2f) impOn = blk * 512 + i - 3 * 512; }
        report ("HQ 2x activo: el impulso sale en " + juce::String (impOn) + " muestras (latencia reportada " + juce::String (p.getLatencySamples()) + ")", std::abs (impOn - p.getLatencySamples()) <= 1);
    }
    // 5) Bloques gigantes (render offline) y bloques irregulares = mismo resultado
    {
        Move m; m.bands = { lcut (40, 1), bell (300, -3, 1.5), hshelf (10000, 2), dynb (bell (6000, 0, 2), -30, -6, 2, 80) }; m.quality = 1;
        const auto a = runMove (m, x, fs, 512, false), b = runMove (m, x, fs, 512, true);
        double d = 0; for (size_t i = 0; i < x.size(); ++i) d = std::max (d, std::abs (a.out.l[i] - b.out.l[i]));
        report ("Bloques irregulares (1-512) vs fijos: diferencia " + juce::String (dB (d / x.peak()), 1) + " dB", dB (d / x.peak()) < -60);
        // Bloque mas grande que el preparado (algunos DAWs al exportar)
        GauxEqProcessor p; setP (p, ParamID::band (0, "on"), 1); setP (p, ParamID::band (0, "gain"), 6); setP (p, ParamID::bypass, 1);
        p.prepareToPlay (fs, 256);
        juce::AudioBuffer<float> big (2, 4096); juce::MidiBuffer midi;
        for (int i = 0; i < 4096; ++i) { big.setSample (0, i, (float) x.l[(size_t) i + 48000]); big.setSample (1, i, (float) x.r[(size_t) i + 48000]); }
        for (int k = 0; k < 3; ++k)
        {
            for (int i = 0; i < 4096; ++i) { big.setSample (0, i, (float) x.l[(size_t) i + 48000 + 4096 * (size_t) k]); big.setSample (1, i, (float) x.r[(size_t) i + 48000 + 4096 * (size_t) k]); }
            p.processBlock (big, midi);
        }
        double dd = 0; for (int i = 0; i < 4096; ++i) dd = std::max (dd, std::abs (big.getSample (0, i) - (double) (float) x.l[(size_t) i + 48000 + 4096 * 2]));
        report ("Bloque de 4096 con el DAW preparado a 256 + bypass: el bypass funciona (dif " + juce::String (dB (dd), 1) + " dB)", dB (dd) < -100);
    }
    // 6) Independencia del sample rate: misma curva a 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz
    {
        BandSettings b1; b1.on = true; b1.type = 0; b1.freq = 12000; b1.gainDb = 6; b1.q = 1.0;
        BandSettings b2; b2.on = true; b2.type = 2; b2.freq = 8000; b2.gainDb = 4; b2.q = 0.7;
        BandSettings b3; b3.on = true; b3.type = 4; b3.freq = 15000; b3.slope = 1; b3.q = 0.707;
        juce::String row; double worst = 0;
        for (auto* bs : { &b1, &b3 })
            for (double f : { 1000.0, 8000.0, 12000.0, 14000.0, 16000.0 })
            {
                double lo = 1e9, hi = -1e9;
                for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
                { const double g = dB (magnitudeOfBand (*bs, f, sr)); lo = std::min (lo, g); hi = std::max (hi, g); }
                worst = std::max (worst, hi - lo);
            }
        report ("Campana 12 kHz y high cut 15 kHz suenan igual en cualquier sample rate (sin HQ): diferencia max entre 44.1 y 192 kHz hasta 16 kHz " + juce::String (worst, 2) + " dB (el high cut a 15 kHz esta a 0.34 x 44.1 kHz)", worst < 1.25);
        double ws = 0;
        for (double f : { 1000.0, 8000.0, 12000.0, 16000.0 })
        { double lo = 1e9, hi = -1e9; for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 }) { const double g = dB (magnitudeOfBand (b2, f, sr)); lo = std::min (lo, g); hi = std::max (hi, g); } ws = std::max (ws, hi - lo); }
        report ("High shelf 8 kHz +4: diferencia entre sample rates hasta 16 kHz " + juce::String (ws, 2) + " dB", ws < 0.6);
    }
    // 7) Silencio: la cola se apaga a cero (sin ruido ni denormales)
    {
        Move m; m.bands = { bell (60, 12, 18), lcut (20, 3), hshelf (10000, 6) };
        St in (48000 * 6); for (size_t i = 0; i < 48000; ++i) { in.l[i] = in.r[i] = 0.5 * std::sin (2 * kPi * 60 * i / 48000.0); }
        const auto r = runMove (m, in, fs);
        double tail = 0; for (size_t i = 48000 * 5; i < in.size(); ++i) tail = std::max (tail, std::abs (r.out.l[i]));
        report ("Despues de 4 s de silencio la salida queda en silencio (" + (tail == 0 ? juce::String ("cero exacto") : juce::String (dB (tail), 0) + " dBFS") + ")", tail < 1e-7);
    }
    // 8) DC en la entrada
    {
        Move m; m.bands = { sat (bell (200, 3, 1), 0, 20) };
        St in (48000 * 3); for (size_t i = 0; i < in.size(); ++i) in.l[i] = in.r[i] = 0.3 + 0.2 * std::sin (2 * kPi * 200 * i / 48000.0);
        const auto r = runMove (m, in, fs);
        double mi = 0, mo = 0; for (size_t i = 48000; i < in.size(); ++i) { mi += in.l[i]; mo += r.out.l[i]; }
        report ("Senal con DC + saturacion: el DC de salida (" + juce::String (mo / (in.size() - 48000), 3) + ") no crece respecto a la entrada (" + juce::String (mi / (in.size() - 48000), 3) + ")", std::abs (mo) <= std::abs (mi) * 1.05);
    }
}

//==============================================================================
// Rendimiento de una sesion real: muchas instancias
static void cpuTests()
{
    if (! want ("cpu")) return;
    std::cout << "\n== CPU EN SESION REAL ==" << std::endl;
    auto s = makeTechHouse (48000, 4.0);
    struct Case { const char* name; Move m; };
    std::vector<Case> cases;
    { Move m; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) }; cases.push_back ({ "4 bandas tipicas (Normal)", m }); }
    { Move m; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) }; m.quality = 1; cases.push_back ({ "4 bandas tipicas (HQ 2x)", m }); }
    { Move m; m.bands = { lcut (30, 1), dynb (bell (300, 0, 1.5), -30, -4), bell (3000, 2, 1), dynb (bell (7000, 0, 2), -30, -6, 1, 60) }; cases.push_back ({ "4 bandas, 2 dinamicas (Normal)", m }); }
    { Move m; m.vintage = true; m.vin.kv = { { ParamID::vLowBoost, 3 }, { ParamID::vHighBoost, 2 } }; cases.push_back ({ "Vintage sin color (Normal)", m }); }
    { Move m; m.vintage = true; m.vin.kv = { { ParamID::vLowBoost, 3 }, { ParamID::vHighBoost, 2 }, { ParamID::vTube, 30 }, { ParamID::vIron, 30 } }; cases.push_back ({ "Vintage con GLOW + IRON (Normal)", m }); }
    for (auto& c : cases)
    {
        const auto r = runMove (c.m, s.mix, 48000);
        report (juce::String (c.name) + ": " + juce::String (r.cpuPct, 2) + " % de un nucleo (en un Mac M5 es bastante menos) -> ~" + juce::String ((int) (60.0 / std::max (0.01, r.cpuPct))) + " instancias en el 60 % de un nucleo", r.cpuPct < 3.0, true);
    }
}


//==============================================================================
// EN VIVO: automatizacion (barridos de filtro de build-up), cambios durante la reproduccion, saturacion
static St renderLive (double fs, const St& in, int block, const std::function<void (GauxEqProcessor&)>& setup,
                      const std::function<void (GauxEqProcessor&, double t)>& automate, int* latencyOut = nullptr)
{
    GauxEqProcessor p;
    setup (p);
    p.prepareToPlay (fs, block);
    juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi;
    St out (in.size());
    for (size_t off = 0; off < in.size(); off += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, in.size() - off);
        if (automate) automate (p, off / fs);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i) { buf.setSample (0, i, (float) in.l[off + i]); buf.setSample (1, i, (float) in.r[off + i]); }
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i) { out.l[off + i] = buf.getSample (0, i); out.r[off + i] = buf.getSample (1, i); }
    }
    if (latencyOut) *latencyOut = p.getLatencySamples();
    return out;
}

// Clic: segunda diferencia maxima justo despues de un evento vs la maxima en estado estable (misma senal)
static double clickDb (const std::vector<double>& y, size_t ev, double fs)
{
    auto d2max = [&] (size_t a, size_t b) { double m = 0; for (size_t i = std::max<size_t> (a, 2); i < b && i < y.size(); ++i) m = std::max (m, std::abs (y[i] - 2 * y[i - 1] + y[i - 2])); return m; };
    const double tr = d2max (ev, ev + (size_t) (0.06 * fs));
    const double st = std::max (d2max (ev - (size_t) (0.25 * fs), ev - (size_t) (0.02 * fs)), d2max (ev + (size_t) (0.15 * fs), ev + (size_t) (0.4 * fs)));
    return dB (tr / std::max (1e-12, st));
}

// Filtros nuevos: Band Pass, Tilt, All Pass y todas las pendientes (6 a 96 dB/oct y Brickwall)
static void newFilterTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("filtros nuevos pendientes").contains (gFilter)) return;
    std::cout << "\n== FILTROS NUEVOS ==" << std::endl;
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        auto pop = makePop (fs, 3.0);
        for (int q : { 0, 1 })
        {
            for (int sl : { 4, 0, 5, 1, 2, 3, 6, 7, 8 })
            {
                Move m; m.track = "MIX"; m.quality = q;
                m.name = juce::String ("low cut 40 Hz + high cut 9 kHz ") + kSlopeNames[sl];
                m.bands = { lcut (40, sl), hcut (9000, sl) };
                runSessionMove (pop, pop.mix, m);
                Move b; b.track = "MIX"; b.quality = q; b.name = juce::String ("band pass 1.2 kHz Q 1 ") + kSlopeNames[sl];
                b.bands = { bpass (1200, 1.0, sl) };
                runSessionMove (pop, pop.mix, b);
            }
            Move t; t.track = "MIX"; t.quality = q; t.name = "tilt +4 dB en 900 Hz + all pass 300 Hz + tilt -3 dB en 6 kHz";
            t.bands = { tilt (900, 4), apass (300, 0.7), tilt (6000, -3, 1.2) };
            runSessionMove (pop, pop.mix, t);
        }
        // en fase lineal (la pared del Brickwall es ideal)
        for (int ph : { 1, 2 })
        {
            Move m; m.track = "MASTER"; m.phase = ph; m.name = ph == 1 ? "fase lineal: brickwall 30 Hz + 18 kHz, band pass, tilt" : "lo mismo en Lineal HQ";
            m.bands = { lcut (30, 8), hcut (18000, 8), tilt (2000, 2), bpass (3000, 4.0, 1) };
            runSessionMove (pop, pop.mix, m);
        }
    }
}

// ============================================================================
// DINAMICA ESPECTRAL
// ============================================================================
static double toneDb (const std::vector<double>& x, size_t a, size_t b, double f, double fs)
{
    // amplitud de un tono (Goertzel con ventana de Hann)
    double re = 0, im = 0, wsum = 0;
    for (size_t i = a; i < b; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * (i - a) / (b - a));
        re += w * x[i] * std::cos (2.0 * 3.14159265358979 * f * i / fs);
        im -= w * x[i] * std::sin (2.0 * 3.14159265358979 * f * i / fs);
        wsum += w;
    }
    return 20.0 * std::log10 (2.0 * std::sqrt (re * re + im * im) / wsum + 1e-15);
}
// energia en una banda de frecuencias excluyendo una zona (dB), por FFT de bloques
static double bandNoiseDb (const std::vector<double>& x, size_t a, size_t b, double f0, double f1, double fs, double exLo = 0, double exHi = 0)
{
    const size_t n = 8192; double e = 0; int cnt = 0;
    for (size_t s0 = a; s0 + n <= b; s0 += n / 2)
    {
        std::vector<Cx> v (n);
        for (size_t i = 0; i < n; ++i) v[i] = x[s0 + i] * (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * i / n));
        fft (v);
        for (size_t k = 1; k < n / 2; ++k)
        {
            const double f = k * fs / n;
            if (f < f0 || f > f1 || (f > exLo && f < exHi)) continue;
            e += std::norm (v[k]);
        }
        ++cnt;
    }
    return 10.0 * std::log10 (e / std::max (1, cnt) + 1e-30);
}

static void spectralTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("espectral spectral").contains (gFilter)) return;
    std::cout << "\n== DINAMICA ESPECTRAL ==" << std::endl;
    using namespace ParamID;
    const double fs = 48000;
    auto specBand = [&] (GauxEqProcessor& p, int i, Band b, bool spectral, bool autoThr = false)
    {
        applyBand (p, i, b);
        setP (p, band (i, "spec"), spectral ? 1.0f : 0.0f);
        setP (p, band (i, "autoThr"), autoThr ? 1.0f : 0.0f);
    };
    // senal: ruido blanco -40 dBFS RMS + tono "resonancia"
    auto makeSig = [&] (double toneF, double toneDbFs, double secs, unsigned seed, double noiseRms = 0.01)
    {
        Rng rr (seed); St x ((size_t) (fs * secs));
        const double A = std::pow (10.0, toneDbFs / 20.0);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = A * std::sin (2.0 * 3.14159265358979 * toneF * i / fs);
            x.l[i] = noiseRms * 1.7320508 * rr.uni() + t; x.r[i] = noiseRms * 1.7320508 * rr.uni() + t;
        }
        return x;
    };

    // 1) Sin accion: sale identica, solo retrasada la latencia informada
    {
        auto x = makeSig (1000, -20, 2.0, 5);
        int lat = 0;
        const auto y = renderLive (fs, x, 512, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (2000, 0, 1.0), 0.0, -12), true); }, {}, &lat);
        double md = 0;
        for (size_t i = (size_t) fs / 2; i < x.size(); ++i) md = std::max (md, std::abs (y.l[i] - x.l[i - (size_t) lat]));
        report ("Espectral sin accion: la senal sale identica (diferencia " + juce::String (dB (md), 0) + " dB) con la latencia informada (" + juce::String (lat)
                + " muestras = " + juce::String (lat / fs * 1000.0, 1) + " ms)", dB (md) < -100 && lat > 0 && lat <= 4096);
    }
    // 2) Selectiva: un tono de -12 dB dentro de una campana ancha se doma; el ruido alrededor casi no cambia.
    //    Comparado con la dinamica normal, que baja toda la banda.
    {
        auto x = makeSig (2000, -12, 3.0, 7);
        const auto ySpec = renderLive (fs, x, 512, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (2000, 0, 0.7), -24, -12, 5, 80), true); }, {});
        const auto yNorm = renderLive (fs, x, 512, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (2000, 0, 0.7), -24, -12, 5, 80), false); }, {});
        int lat = 0; renderLive (fs, x, 512, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (2000, 0, 0.7), -24, -12, 5, 80), true); }, {}, &lat);
        const size_t a = (size_t) (fs * 1.5), b = (size_t) (fs * 2.9);
        const double t0 = toneDb (x.l, a, b, 2000, fs);
        const double tS = toneDb (ySpec.l, a + (size_t) lat, b + (size_t) lat, 2000, fs), tN = toneDb (yNorm.l, a, b, 2000, fs);
        const double n0 = bandNoiseDb (x.l, a, b, 1000, 4000, fs, 1800, 2200);
        const double nS = bandNoiseDb (ySpec.l, a + (size_t) lat, b + (size_t) lat, 1000, 4000, fs, 1800, 2200), nN = bandNoiseDb (yNorm.l, a, b, 1000, 4000, fs, 1800, 2200);
        report ("Espectral selectiva: tono de 2 kHz " + juce::String (tS - t0, 1) + " dB, ruido alrededor (1-4 kHz) " + juce::String (nS - n0, 2)
                + " dB  |  dinamica normal: tono " + juce::String (tN - t0, 1) + " dB, ruido " + juce::String (nN - n0, 1) + " dB",
                tS - t0 < -9.0 && std::abs (nS - n0) < 1.0 && nN - n0 < -3.0);
    }
    // 3) AUTO: doma lo que sobresale de sus vecinas sin tocar un espectro parejo
    {
        auto x = makeSig (3150, -26, 3.0, 9);
        int lat = 0;
        const auto y = renderLive (fs, x, 512, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (3000, 0, 0.5), -24, -12, 5, 100), true, true); }, {}, &lat);
        const size_t a = (size_t) (fs * 1.5), b = (size_t) (fs * 2.9);
        const double dt = toneDb (y.l, a + lat, b + lat, 3150, fs) - toneDb (x.l, a, b, 3150, fs);
        const double dn = bandNoiseDb (y.l, a + lat, b + lat, 1000, 8000, fs, 2900, 3400) - bandNoiseDb (x.l, a, b, 1000, 8000, fs, 2900, 3400);
        // ruido solo (sin resonancia): AUTO no debe hacer casi nada
        auto xn = makeSig (3150, -200, 3.0, 11);
        const auto yn = renderLive (fs, xn, 512, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (3000, 0, 0.5), -24, -12, 5, 100), true, true); }, {});
        const double dflat = bandNoiseDb (yn.l, a + lat, b + lat, 1000, 8000, fs) - bandNoiseDb (xn.l, a, b, 1000, 8000, fs);
        report ("Espectral AUTO (supresor de resonancias): resonancia de 3.15 kHz " + juce::String (dt, 1) + " dB, ruido alrededor " + juce::String (dn, 2)
                + " dB; con un espectro parejo solo cambia " + juce::String (dflat, 2) + " dB", dt < -6.0 && std::abs (dn) < 1.0 && std::abs (dflat) < 1.5);
    }
    // 4) Attack / release: el tono aparece y desaparece
    {
        St x ((size_t) (fs * 3.0));
        Rng rr (3);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / fs;
            const double on = (t > 1.0 && t < 2.0) ? 0.25 : 0.0;
            x.l[i] = x.r[i] = 0.01 * 1.7320508 * rr.uni() + on * std::sin (2.0 * 3.14159265358979 * 1500 * i / fs);
        }
        int lat = 0;
        const auto y = renderLive (fs, x, 256, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (1500, 0, 1.0), -30, -12, 10, 150), true); }, {}, &lat);
        auto toneAt = [&] (const St& s, double t0, double t1, size_t off) { return toneDb (s.l, (size_t) (t0 * fs) + off, (size_t) (t1 * fs) + off, 1500, fs); };
        const double steady = toneAt (y, 1.5, 1.95, lat) - toneAt (x, 1.5, 1.95, 0);
        const double early = toneAt (y, 1.0, 1.05, lat) - toneAt (x, 1.0, 1.05, 0);   // primeros 50 ms: ya actua (attack 10 ms + cuadro)
        // despues de que el tono se va, el ruido de esa zona vuelve a su nivel en ~release
        const double after = bandNoiseDb (y.l, (size_t) (2.4 * fs) + lat, (size_t) (2.9 * fs) + lat, 1200, 1800, fs) - bandNoiseDb (x.l, (size_t) (2.4 * fs), (size_t) (2.9 * fs), 1200, 1800, fs);
        report ("Espectral attack/release: en regimen " + juce::String (steady, 1) + " dB, primeros 50 ms " + juce::String (early, 1) + " dB, 400 ms despues de soltar "
                + juce::String (after, 2) + " dB", steady < -9.0 && early < -4.0 && std::abs (after) < 1.0);
    }
    // 5) Side: un tono solo en los lados se doma; el centro no se toca
    {
        St x ((size_t) (fs * 3.0)); Rng rr (21);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double side = 0.2 * std::sin (2.0 * 3.14159265358979 * 5000 * i / fs), mid = 0.2 * std::sin (2.0 * 3.14159265358979 * 4000 * i / fs);
            const double n = 0.005 * rr.uni();
            x.l[i] = mid + side + n; x.r[i] = mid - side + n;
        }
        int lat = 0;
        const auto y = renderLive (fs, x, 512, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (4500, 0, 0.6, 4), -30, -12, 5, 100), true); }, {}, &lat);
        const auto mi = domainOf (x, 'M'), si = domainOf (x, 'S'), mo = domainOf (y, 'M'), so = domainOf (y, 'S');
        const size_t a = (size_t) (fs * 1.5), b = (size_t) (fs * 2.9);
        const double ds = toneDb (so, a + lat, b + lat, 5000, fs) - toneDb (si, a, b, 5000, fs), dm = toneDb (mo, a + lat, b + lat, 4000, fs) - toneDb (mi, a, b, 4000, fs);
        report ("Espectral en Side: tono de los lados " + juce::String (ds, 1) + " dB, tono del centro " + juce::String (dm, 2) + " dB", ds < -9.0 && std::abs (dm) < 0.3);
    }
    // 6) Sidechain externo: actua solo cuando suena la otra pista, y solo en sus frecuencias
    {
        auto x = makeSig (800, -20, 3.0, 31);
        x.add (makeSig (2500, -20, 3.0, 32, 0.0));
        St sc (x.size());
        for (size_t i = 0; i < sc.size(); ++i) { const double t = i / fs; const double v = t > 1.5 ? 0.3 * std::sin (2.0 * 3.14159265358979 * 800 * i / fs) : 0.0; sc.l[i] = sc.r[i] = v; }
        Move m; m.sidechain = &sc;
        auto b0 = dynb (bell (1200, 0, 0.5), -30, -12, 5, 100, true);
        m.bands = { b0 };
        // (runMove con sidechain)
        GauxEqProcessor::allowSleep = false;
        auto p = std::make_unique<GauxEqProcessor>();
        p->enableAllBuses();
        specBand (*p, 0, b0, true);
        p->prepareToPlay (fs, 512);
        juce::AudioBuffer<float> buf (4, 512); juce::MidiBuffer midi; St y (x.size());
        for (size_t off = 0; off + 512 <= x.size(); off += 512)
        {
            for (int i = 0; i < 512; ++i) { buf.setSample (0, i, (float) x.l[off + i]); buf.setSample (1, i, (float) x.r[off + i]); buf.setSample (2, i, (float) sc.l[off + i]); buf.setSample (3, i, (float) sc.r[off + i]); }
            p->processBlock (buf, midi);
            for (int i = 0; i < 512; ++i) { y.l[off + i] = buf.getSample (0, i); y.r[off + i] = buf.getSample (1, i); }
        }
        GauxEqProcessor::allowSleep = true;
        const int lat = p->getLatencySamples();
        const double before = toneDb (y.l, (size_t) (0.7 * fs) + lat, (size_t) (1.4 * fs) + lat, 800, fs) - toneDb (x.l, (size_t) (0.7 * fs), (size_t) (1.4 * fs), 800, fs);
        const double during = toneDb (y.l, (size_t) (2.0 * fs) + lat, (size_t) (2.9 * fs) + lat, 800, fs) - toneDb (x.l, (size_t) (2.0 * fs), (size_t) (2.9 * fs), 800, fs);
        const double other = toneDb (y.l, (size_t) (2.0 * fs) + lat, (size_t) (2.9 * fs) + lat, 2500, fs) - toneDb (x.l, (size_t) (2.0 * fs), (size_t) (2.9 * fs), 2500, fs);
        report ("Espectral con sidechain externo: 800 Hz sin sidechain " + juce::String (before, 2) + " dB, con sidechain " + juce::String (during, 1)
                + " dB; 2.5 kHz (no esta en el sidechain) " + juce::String (other, 2) + " dB", std::abs (before) < 0.5 && during < -8.0 && std::abs (other) < 1.0);
    }
    // 7) Prender y apagar el modo espectral en plena reproduccion: sin clics, latencia informada al momento
    {
        auto x = makeSig (440, -14, 4.0, 41);
        int latEnd = 0;
        std::vector<int> lats;
        const auto y = renderLive (fs, x, 256, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (440, 0, 1.0), -20, -6), false); },
                                   [&] (GauxEqProcessor& p, double t) { setP (p, band (0, "spec"), (t > 1.0 && t < 2.5) ? 1.0f : 0.0f); lats.push_back (p.getLatencySamples()); }, &latEnd);
        double c = 0; for (double tEv : { 1.0, 2.5 }) c = std::max (c, clickDb (y.l, (size_t) (tEv * fs), fs));
        int maxLat = 0; for (int l : lats) maxLat = std::max (maxLat, l);
        report ("Espectral prender/apagar en vivo: transitorio " + juce::String (c, 1) + " dB, latencia " + juce::String (maxLat) + " -> " + juce::String (latEnd) + " muestras",
                c < 6.0 && maxLat > 0 && latEnd == 0);
    }
    // 8) Golpes (kick + hats): sin accion es transparente; con accion fuerte, sin pre-eco audible
    {
        St x ((size_t) (fs * 3.0)); Rng rr (51);
        std::vector<double> m (x.size(), 0.0);
        for (double t = 0.25; t < 2.9; t += 0.5) syn::kick (m, fs, t, 0.9);
        for (double t = 0.5; t < 2.9; t += 0.25) syn::hat (m, fs, t, 0.3, rr);
        for (size_t i = 0; i < x.size(); ++i) x.l[i] = x.r[i] = m[i];
        int lat = 0;
        const auto y = renderLive (fs, x, 512, [&] (GauxEqProcessor& p) { specBand (p, 0, dynb (bell (80, 0, 0.8), -40, -15, 1, 80), true); }, {}, &lat);
        // pre-eco: energia en los 20 ms antes de cada golpe de kick vs el golpe
        double worst = -200;
        for (double t = 0.75; t < 2.8; t += 0.5)
        {
            const size_t k = (size_t) (t * fs) + lat;
            double pre = 0, hit = 0;
            for (size_t i = k - (size_t) (0.02 * fs); i < k - (size_t) (0.004 * fs); ++i) pre += y.l[i] * y.l[i];
            for (size_t i = k; i < k + (size_t) (0.02 * fs); ++i) hit += y.l[i] * y.l[i];
            double preIn = 0, hitIn = 0;
            for (size_t i = k - lat - (size_t) (0.02 * fs); i < k - lat - (size_t) (0.004 * fs); ++i) preIn += x.l[i] * x.l[i];
            for (size_t i = k - lat; i < k - lat + (size_t) (0.02 * fs); ++i) hitIn += x.l[i] * x.l[i];
            // cuanto sube la energia ANTES del golpe (respecto del golpe) comparado con la senal original
            worst = std::max (worst, 10.0 * std::log10 ((pre + 1e-20) / (hit + 1e-20)) - 10.0 * std::log10 ((preIn + 1e-20) / (hitIn + 1e-20)));
        }
        const double kickChange = toneDb (y.l, (size_t) (0.25 * fs) + lat, (size_t) (2.8 * fs) + lat, 60, fs) - toneDb (x.l, (size_t) (0.25 * fs), (size_t) (2.8 * fs), 60, fs);
        report ("Espectral en golpes (kick domado " + juce::String (kickChange, 1) + " dB en 60 Hz): energia agregada antes de cada golpe (pre-eco) " + juce::String (worst, 1) + " dB",
                kickChange < -3.0 && worst < 3.0);
    }
    // 9) CPU
    {
        auto s = makeTechHouse (fs, 4.0);
        Move m; m.bands = { lcut (30, 1), dynb (bell (300, 0, 1.0), -30, -6), bell (3000, 2, 1), hshelf (10000, 2) };
        const auto r0 = runMove (m, s.mix, fs, 256);
        GauxEqProcessor probe; juce::ignoreUnused (probe);
        // misma mezcla con la banda 2 espectral
        auto p = std::make_unique<GauxEqProcessor>();
        for (size_t i = 0; i < m.bands.size(); ++i) applyBand (*p, (int) i, m.bands[i]);
        setP (*p, band (1, "spec"), 1);
        p->prepareToPlay (fs, 256);
        juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer midi;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (size_t off = 0; off + 256 <= s.mix.size(); off += 256)
        {
            for (int i = 0; i < 256; ++i) { buf.setSample (0, i, (float) s.mix.l[off + i]); buf.setSample (1, i, (float) s.mix.r[off + i]); }
            p->processBlock (buf, midi);
        }
        const double pct = (juce::Time::getMillisecondCounterHiRes() - t0) / (s.mix.size() / fs * 1000.0) * 100.0;
        report ("CPU con una banda espectral: " + juce::String (pct, 2) + " % de un nucleo (sin espectral " + juce::String (r0.cpuPct, 2) + " %)", pct < 3.0, true);
    }
}

// ============================================================================
// EQ MATCH (ajustador de curvas + espectro de largo plazo)
// ============================================================================
static std::vector<double> curveOfBands (const std::vector<BandSettings>& bs, const std::vector<double>& f)
{
    std::vector<double> c (f.size(), 0.0);
    for (auto& b : bs) for (size_t i = 0; i < f.size(); ++i) c[i] += 20.0 * std::log10 (analogMagnitudeOfBand (b, f[i]));
    return c;
}
static std::vector<double> learnDb (const St& s, double fs, const std::vector<double>& f, double smooth)
{
    LongTermSpectrum L; L.prepare (fs);
    std::vector<float> m (s.size());
    for (size_t i = 0; i < s.size(); ++i) m[i] = (float) (0.5 * (s.l[i] + s.r[i]));
    L.add (m.data(), (int) m.size());
    return L.toDb (f, smooth);
}
static St processWith (const St& x, double fs, const std::vector<BandSettings>& bs)
{
    return renderLive (fs, x, 512, [&] (GauxEqProcessor& p)
    {
        for (size_t i = 0; i < bs.size(); ++i)
        {
            using namespace ParamID;
            setP (p, band ((int) i, "on"), 1); setP (p, band ((int) i, "type"), (float) bs[i].type); setP (p, band ((int) i, "freq"), (float) bs[i].freq);
            setP (p, band ((int) i, "gain"), (float) bs[i].gainDb); setP (p, band ((int) i, "q"), (float) bs[i].q);
        }
    }, {});
}


// Error de la fase natural (diseno): total = digital * FIR vs analogico retrasado D
struct NatErr { double magDb = 0, phDeg = 0, magHf = 0, phHf = 0; };
static NatErr naturalDesignError (const NaturalSpec& sp, const NaturalSizes& z, const std::vector<Cx>& osResp = {})
{
    std::array<std::vector<float>, 4> irs;
    const bool cross = designNaturalPhase (sp, osResp, z, irs);
    NatErr e;
    for (double f = 20; f <= 20000; f *= std::pow (2.0, 1.0 / 24.0))
    {
        Mat2 hd, ha; naturalResponses (sp, f, hd, ha);
        if (! osResp.empty())
        {
            // interpolar la respuesta del oversampling (bins)
            const double kf = f * z.fft / sp.fs; const int k0 = (int) kf; const double t = kf - k0;
            hd = hd.scaled (osResp[(size_t) k0] * (1 - t) + osResp[(size_t) std::min ((int) osResp.size() - 1, k0 + 1)] * t);
        }
        auto dtft = [&] (const std::vector<float>& h) { Cx s = 0; for (size_t n = 0; n < h.size(); ++n) s += (double) h[n] * std::polar (1.0, -2 * M_PI * f * n / sp.fs); return s; };
        Mat2 C { dtft (irs[0]), cross ? dtft (irs[2]) : Cx (0), cross ? dtft (irs[3]) : Cx (0), dtft (irs[1]) };
        const Mat2 tot = C * hd;
        const Cx dl = std::polar (1.0, -2 * M_PI * f * z.delay / sp.fs);
        const Mat2 want = ha.scaled (dl);
        for (auto [g, w] : { std::pair<Cx, Cx> { tot.a, want.a }, { tot.d, want.d }, { tot.b, want.b }, { tot.c, want.c } })
        {
            if (std::abs (w) < 0.05 && std::abs (g) < 0.05) continue;   // (casi nada de senal)
            const double md = std::abs (20 * std::log10 (std::abs (g) + 1e-12) - 20 * std::log10 (std::abs (w) + 1e-12));
            const double pd = std::abs (std::arg (g / w)) * 180 / M_PI;
            // error vectorial relativo (dB) como medida unica
            if (std::abs (w) >= 0.05)
            {
                if (f <= 16000) { e.magDb = std::max (e.magDb, md); e.phDeg = std::max (e.phDeg, pd); }
                else { e.magHf = std::max (e.magHf, md); e.phHf = std::max (e.phHf, pd); }
            }
        }
    }
    return e;
}


// Respuesta al impulso real del plugin (columna 'inCh' de la matriz 2x2): devuelve L y R de salida
static std::pair<std::vector<double>, std::vector<double>> pluginImpulse (GauxEqProcessor& p, double fs, int inCh, int& lat, int lenSamples)
{
    p.prepareToPlay (fs, 512);
    lat = p.getLatencySamples();
    std::vector<double> hl, hr; juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer midi;
    // primero un poco de silencio (motores listos), despues el impulso en la muestra 100 del bloque
    for (int k = 0; k < (int) (fs / 512) + 1; ++k) { b.clear(); p.processBlock (b, midi); }   // 1 s: bandas ya encendidas del todo
    for (int off = 0; off < lenSamples + lat + 1024; off += 512)
    {
        b.clear(); if (off == 0) b.setSample (inCh, 100, 1.0f);
        p.processBlock (b, midi);
        for (int i = 0; i < 512; ++i) { hl.push_back (b.getSample (0, i)); hr.push_back (b.getSample (1, i)); }
    }
    // (se deja todo lo que sale desde el impulso: lo de antes del centro tambien cuenta; el retraso se descuenta en frecuencia)
    hl.erase (hl.begin(), hl.begin() + 100); hr.erase (hr.begin(), hr.begin() + 100);
    return { hl, hr };
}

static Cx dtftAt (const std::vector<double>& h, double f, double fs)
{
    // (rotacion incremental: exacta y rapida)
    const Cx w = std::polar (1.0, -2 * M_PI * f / fs);
    Cx z = 1.0, s = 0.0;
    for (size_t n = 0; n < h.size(); ++n) { s += h[n] * z; z *= w; if ((n & 1023) == 1023) z /= std::abs (z); }
    return s;
}

static void naturalProcessTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("natural fase").contains (gFilter)) return;
    std::cout << "\n== FASE NATURAL (el plugin completo) ==" << std::endl;
    using namespace ParamID;
    auto toBs = [] (const Band& b) { BandSettings s; s.on = true; s.type = b.type; s.freq = b.f; s.gainDb = b.g; s.q = b.q; s.slope = b.slope; s.stereo = b.st; return s; };
    struct Cfg { const char* name; double fs; int quality; std::vector<Band> bands; };
    std::vector<Cfg> dbg {
        { "solo campana 3k", 44100, 0, { bell (3000, 2, 1) } },
        { "solo lcut 30", 44100, 0, { lcut (30, 1) } },
        { "solo bell 60", 44100, 0, { bell (60, 4, 2) } },
        { "solo shelf 10k", 44100, 0, { hshelf (10000, 3) } },
        { "solo hcut 18k", 44100, 0, { hcut (18000, 1) } },
        { "nada", 44100, 0, { } },
        { "HQ2 shelf 12k", 48000, 1, { hshelf (12000, 6) } },
        { "HQ2 nada", 48000, 1, { } },
    };
    const std::vector<Cfg> cfgs0 {
        { "mezcla tipica", 44100, 0, { lcut (30, 1), bell (60, 4, 2), bell (3000, 2, 1), hshelf (10000, 3), hcut (18000, 1) } },
        { "agudos fuertes (campana 15 kHz +8 Q 2, corte 19 kHz 48 dB)", 48000, 0, { bell (15000, 8, 2), hcut (19000, 3) } },
        { "HQ 2x: misma mezcla", 48000, 1, { lcut (30, 1), bell (60, 4, 2), bell (3000, 2, 1), hshelf (10000, 3), hcut (18000, 1) } },
        { "HQ 4x: shelf 12 kHz +6, campana 8 kHz -5", 44100, 2, { hshelf (12000, 6), bell (8000, -5, 1.5) } },
        { "96 kHz: campana 16 kHz +6, tilt 1 kHz", 96000, 0, { bell (16000, 6, 1), tilt (1000, 4) } },
    };
    const auto& cfgs = std::getenv ("NAT_DBG") ? dbg : cfgs0;
    for (auto& c : cfgs)
    {
        double worstNat[2] = { 0, 0 }, worstMin[2] = { 0, 0 };
        int latNat = 0;
        for (int ph : { 3, 0 })
        {
            GauxEqProcessor p;
            setP (p, phase, (float) ph); setP (p, quality, (float) c.quality);
            for (size_t i = 0; i < c.bands.size(); ++i) applyBand (p, (int) i, c.bands[i]);
            int lat = 0;
            const auto [hl, hr] = pluginImpulse (p, c.fs, 0, lat, (int) c.fs);
            std::vector<double> h (hl.size());
            for (size_t i = 0; i < h.size(); ++i) h[i] = hl[i] + hr[i] * 0;   // columna L -> L
            // (sin bandas M/S: L->R es cero)
            if (ph == 3) latNat = lat;
            for (double f = 20; f <= 20000; f *= std::pow (2.0, 1.0 / 12.0))
            {
                Cx want = 1.0; for (auto& b : c.bands) want *= analogResponseOfBand (toBs (b), f);
                if (std::abs (want) < 0.1) continue;   // (donde casi no pasa senal la fase no importa)
                const Cx got = dtftAt (h, f, c.fs) * std::polar (1.0, 2 * M_PI * f * lat / c.fs);
                const double md = std::abs (20 * std::log10 (std::abs (got) / std::abs (want)));
                const double pd = std::abs (std::arg (got / want)) * 180 / M_PI;
                if (ph == 3 && std::getenv ("NAT_DBG") && f > 400 && f < 450)
                {
                    NaturalSpec ns; ns.fs = c.fs; ns.osFactor = 1 << c.quality;
                    for (size_t i = 0; i < c.bands.size(); ++i) ns.bands[i] = toBs (c.bands[i]);
                    const auto z = naturalSizes (c.fs);
                    std::array<std::vector<float>, 4> irs; designNaturalPhase (ns, {}, z, irs);
                    std::vector<double> ir (irs[0].begin(), irs[0].end());
                    Mat2 hd, ha; naturalResponses (ns, f, hd, ha);
                    const Cx cf = dtftAt (ir, f, c.fs) * std::polar (1.0, 2 * M_PI * f * z.delay / c.fs);
                    std::printf ("      %.0f Hz: medido %.4f dB, modelo digital %.4f dB, C %.4f dB, analogico %.4f dB\n", f, 20 * std::log10 (std::abs (got)),
                                 20 * std::log10 (std::abs (hd.a)), 20 * std::log10 (std::abs (cf)), 20 * std::log10 (std::abs (ha.a)));
                }
                if (ph == 0 && std::getenv ("NAT_DBG"))
                {
                    NaturalSpec ns; ns.fs = c.fs; ns.osFactor = 1 << c.quality;
                    for (size_t i = 0; i < c.bands.size(); ++i) ns.bands[i] = toBs (c.bands[i]);
                    Mat2 hd, ha; naturalResponses (ns, f, hd, ha);
                    if (c.quality > 0)
                    {
                        static std::vector<Cx> osr; static int osf = 0;
                        if (osf != (1 << c.quality)) { osf = 1 << c.quality; osr = gaux::measureOversampling (osf, 1 << 16); }
                        hd = hd.scaled (osr[(size_t) std::round (f * 65536 / c.fs)]);
                    }
                    const double e1 = 20 * std::log10 (std::abs (got) / std::abs (hd.a)), e2 = std::arg (got / hd.a) * 180 / M_PI;
                    if (std::abs (e1) > 0.01 || std::abs (e2) > 0.2) std::printf ("      min vs modelo digital %.0f Hz: %.3f dB %.2f grados\n", f, e1, e2);
                }
                double* w = ph == 3 ? worstNat : worstMin;
                if (ph == 3 && std::getenv ("NAT_VERBOSE") && (md > 0.05 || pd > 1.0)) std::printf ("      %.0f Hz: %.3f dB %.2f grados\n", f, md, pd);
                w[0] = std::max (w[0], md); w[1] = std::max (w[1], pd);
            }
        }
        report ("Natural, " + juce::String (c.name) + " (" + juce::String (c.fs / 1000, 1) + " kHz" + (c.quality ? ", HQ " + juce::String (1 << c.quality) + "x" : juce::String()) + "): latencia "
                + juce::String (latNat) + " muestras (" + juce::String (1000.0 * latNat / c.fs, 1) + " ms); frente al analogico ideal: curva " + juce::String (worstNat[0], 2) + " dB, fase "
                + juce::String (worstNat[1], 1) + " grados  (fase minima: " + juce::String (worstMin[0], 2) + " dB, " + juce::String (worstMin[1], 1) + " grados)",
                worstNat[0] < 0.08 && worstNat[1] < 1.0 && latNat <= (c.quality > 0 ? 300 : c.fs > 52000 ? 130 : 66));
    }
    // M/S y L/R: la matriz 2x2 completa (L->L, R->L, L->R, R->R)
    {
        const double fs = 48000;
        const std::vector<Band> bands { hshelf (9000, 5, 0.707, 4), bell (12000, -6, 1.5, 3), bell (14000, 4, 1, 1), hcut (17000, 1, 0.707, 2) };
        auto mat = [&] (double f)
        {
            Mat2 t;
            for (auto& b : bands) t = Mat2::ofDomain (analogResponseOfBand (toBs (b), f), b.st) * t;
            return t;
        };
        double wm = 0, wp = 0;
        int latMs = 0;
        std::vector<double> cols[2][2];
        for (int ch = 0; ch < 2; ++ch)
        {
            GauxEqProcessor p;
            setP (p, phase, 3.0f);
            for (size_t i = 0; i < bands.size(); ++i) applyBand (p, (int) i, bands[i]);
            int lat = 0;
            auto [hl, hr] = pluginImpulse (p, fs, ch, lat, 48000);
            cols[0][ch] = hl; cols[1][ch] = hr; latMs = lat;
        }
        for (double f = 20; f <= 20000; f *= std::pow (2.0, 1.0 / 12.0))
        {
            const Mat2 w = mat (f);
            const Cx want[2][2] = { { w.a, w.b }, { w.c, w.d } };
            for (int o = 0; o < 2; ++o) for (int i = 0; i < 2; ++i)
            {
                if (std::abs (want[o][i]) < 0.1) continue;
                const Cx got = dtftAt (cols[o][i], f, fs) * std::polar (1.0, 2 * M_PI * f * latMs / fs);
                wm = std::max (wm, std::abs (20 * std::log10 (std::abs (got) / std::abs (want[o][i]))));
                wp = std::max (wp, std::abs (std::arg (got / want[o][i])) * 180 / M_PI);
            }
        }
        report ("Natural con M/S y L/R (matriz 2x2 completa): curva " + juce::String (wm, 2) + " dB, fase " + juce::String (wp, 1) + " grados frente al analogico", wm < 0.1 && wp < 1.5);
    }
    // Cambios en plena reproduccion: el motor se arma en segundo plano y el paso es sin clic
    {
        const double fs = 48000;
        St tone ((size_t) (fs * 6));
        for (size_t i = 0; i < tone.size(); ++i) { const double t = i / fs; tone.l[i] = tone.r[i] = 0.35 * std::sin (2 * kPi * 220 * t) + 0.12 * std::sin (2 * kPi * 3100 * t) + 0.05 * std::sin (2 * kPi * 11000 * t); }
        struct Sw { const char* name; std::function<void (GauxEqProcessor&)> setup, change; };
        const Sw sws[] = {
            { "fase minima -> natural", [] (GauxEqProcessor&) {}, [] (GauxEqProcessor& p) { setP (p, phase, 3); } },
            { "natural -> fase minima", [] (GauxEqProcessor& p) { setP (p, phase, 3); }, [] (GauxEqProcessor& p) { setP (p, phase, 0); } },
            { "natural: Normal -> HQ 2x", [] (GauxEqProcessor& p) { setP (p, phase, 3); }, [] (GauxEqProcessor& p) { setP (p, quality, 1); } },
            { "natural -> lineal", [] (GauxEqProcessor& p) { setP (p, phase, 3); }, [] (GauxEqProcessor& p) { setP (p, phase, 1); } },
            { "natural: shelf 6 kHz +4 -> -6 de golpe", [] (GauxEqProcessor& p) { setP (p, phase, 3); }, [] (GauxEqProcessor& p) { setP (p, band (1, "gain"), -6); } },
        };
        for (auto& sw : sws)
        {
            GauxEqProcessor p;
            applyBand (p, 0, bell (220, 6, 1)); applyBand (p, 1, hshelf (6000, 4));
            sw.setup (p);
            p.prepareToPlay (fs, 256);
            std::vector<double> y; juce::AudioBuffer<float> b (2, 256); juce::MidiBuffer midi;
            size_t evAt = 0; bool asked = false; int lat0 = p.getLatencySamples(), lat1 = lat0; bool finite = true;
            for (size_t off = 0; off + 256 <= tone.size(); off += 256)
            {
                if (! asked && off >= (size_t) fs) { sw.change (p); asked = true; }
                if (asked && (off / 256) % 4 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
                const int before = p.getLatencySamples(); const bool natB = p.naturalActiveNow.load(), linB = p.linearActiveNow.load();
                for (int i = 0; i < 256; ++i) { b.setSample (0, i, (float) tone.l[off + (size_t) i]); b.setSample (1, i, (float) tone.r[off + (size_t) i]); }
                p.processBlock (b, midi);
                if (asked && evAt == 0 && (before != p.getLatencySamples() || natB != p.naturalActiveNow.load() || linB != p.linearActiveNow.load())) evAt = off;
                for (int i = 0; i < 256; ++i) { y.push_back (b.getSample (0, i)); finite = finite && std::isfinite (b.getSample (0, i)); }
                lat1 = p.getLatencySamples();
            }
            if (juce::String (sw.name).contains ("de golpe")) evAt = (size_t) fs;   // (no cambia la latencia: el evento es el cambio)
            const double c = evAt > 0 ? clickDb (y, evAt, fs) : 99.0;
            report ("Natural en vivo, " + juce::String (sw.name) + ": " + (evAt > 0 ? "hecho" : "NO HECHO") + ", transitorio " + juce::String (c, 1) + " dB, latencia "
                    + juce::String (lat0) + " -> " + juce::String (lat1) + " muestras", evAt > 0 && c < 6.0 && finite);
        }
    }
    // En fase natural la dinamica, la saturacion y el sidechain siguen funcionando (en lineal se pausan)
    {
        auto pop = makePop (48000, 4.0);
        Move m; m.track = "MASTER"; m.phase = 3; m.name = "fase natural: corte 30 Hz, 60 Hz +4, 3 kHz +2, shelf 10 kHz +3";
        m.bands = { lcut (30, 1), bell (60, 4, 2), bell (3000, 2, 1), hshelf (10000, 3, 0.7) };
        runSessionMove (pop, pop.mix, m);
        Move d; d.track = "MASTER"; d.phase = 3; d.name = "fase natural con EQ dinamica en 250 Hz";
        d.bands = { dynb (bell (250, 0, 1.2), -30, -6, 5, 80), hshelf (9000, 2) };
        runSessionMove (pop, pop.mix, d);
        Move ms; ms.track = "MASTER"; ms.phase = 3; ms.domain = 'S'; ms.name = "fase natural M/S: Side shelf 8 kHz +3, Mid 300 Hz -2";
        ms.bands = { hshelf (8000, 3, 0.7, 4), bell (300, -2, 1, 3) };
        runSessionMove (pop, pop.mix, ms);
        // CPU: natural frente a fase minima (misma mezcla)
        Move mn = m; mn.phase = 0;
        const auto rn = runMove (m, pop.mix, 48000), r0 = runMove (mn, pop.mix, 48000);
        report ("CPU fase natural " + juce::String (rn.cpuPct, 2) + " % frente a fase minima " + juce::String (r0.cpuPct, 2) + " % (latencia " + juce::String ((int) rn.latency) + " muestras)",
                rn.cpuPct < r0.cpuPct + 1.5);
    }
}

static void naturalDesignTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("natural natdesign").contains (gFilter)) return;
    std::cout << "\n== FASE NATURAL (diseno) ==" << std::endl;
    auto mk = [] (int t, double fr, double g, double q, int slope = 1, int st = 0) { BandSettings b; b.on = true; b.type = t; b.freq = fr; b.gainDb = g; b.q = q; b.slope = slope; b.stereo = st; return b; };
    struct Case { const char* name; std::vector<BandSettings> bands; bool zone = false; };
    std::vector<Case> cases {
        { "campana +6 dB 12 kHz Q 1", { mk (0, 12000, 6, 1.0) } },
        { "campana -9 dB 16 kHz Q 3", { mk (0, 16000, -9, 3.0) } },
        { "shelf agudos +6 dB 10 kHz", { mk (2, 10000, 6, 0.707) } },
        { "corte agudos 15 kHz 24 dB/oct", { mk (4, 15000, 0, 0.707, 1) } },
        { "corte agudos 18 kHz 48 dB/oct", { mk (4, 18000, 0, 0.707, 3) } },
        { "corte graves 30 Hz 48 dB/oct", { mk (3, 30, 0, 0.707, 3) } },
        { "campana graves 60 Hz +6 Q 4", { mk (0, 60, 6, 4.0) } },
        { "mezcla: 5 bandas", { mk (3, 30, 0, 0.707, 1), mk (1, 100, 3, 0.707), mk (0, 400, -3, 1.4), mk (0, 4000, 2, 1.0), mk (2, 12000, 4, 0.707) } },
        { "M/S: lados +6 en 14 kHz, medio -4 en 9 kHz", { mk (0, 14000, 6, 1.0, 1, 4), mk (0, 9000, -4, 2.0, 1, 3) } },
        { "L/R + estereo", { mk (0, 11000, 5, 1.0, 1, 1), mk (2, 13000, -6, 0.707, 1, 2), mk (0, 3000, 3, 1.0) } },
        { "ZONE 200 Hz - 8 kHz con shelf", { mk (2, 6000, 6, 0.707) }, true },
    };
    if (std::getenv ("NAT_OS"))
    {
        for (int f : { 2, 4 })
        {
            const auto r = gaux::measureOversampling (f, 2048);
            std::printf ("os %dx:", f);
            for (double hz : { 1000.0, 5000.0, 10000.0, 16000.0, 20000.0 }) { const int k = (int) std::round (hz * 2048 / 48000); std::printf ("  %.0f Hz %.3f dB %.1f grados", hz, 20 * std::log10 (std::abs (r[(size_t) k])), std::arg (r[(size_t) k]) * 180 / M_PI); }
            std::printf ("\n");
            for (int D : { 32, 64, 128, 256, 512 })
            {
                NaturalSpec sp; sp.fs = 48000; sp.osFactor = f;
                auto z = naturalSizes (48000); z.delay = D; z.length = std::max (256, 4 * D);
                const auto osr = gaux::measureOversampling (f, z.fft);
                const auto e = naturalDesignError (sp, z, osr);
                std::printf ("   D %d L %d: %.3f dB %.2f grados | >16k %.3f dB %.2f grados\n", D, z.length, e.magDb, e.phDeg, e.magHf, e.phHf);
            }
        }
    }
    if (std::getenv ("NAT_DBG"))
    {
        // el motor (convolucion) entrega exactamente el FIR disenado?
        gaux::LinearSpec ls; ls.fs = 44100; ls.natural = true; ls.length = naturalSizes (44100).length;
        ls.bands[0] = mk (2, 10000, 3, 0.707);
        NaturalSpec ns; ns.fs = 44100; ns.bands = ls.bands;
        std::array<std::vector<float>, 4> irs; designNaturalPhase (ns, {}, naturalSizes (44100), irs);
        gaux::LinearPhaseEngine e; e.prepare (ls, 512);
        std::vector<float> l (512, 0.0f), r (512, 0.0f); l[0] = r[0] = 1.0f;
        e.process (l.data(), r.data(), 512);
        const int lat = e.getLatency();
        double err = 0; int pk = 0;
        for (int n = 0; n < 256; ++n) { err = std::max (err, (double) std::abs (l[(size_t) (n + lat - naturalSizes (44100).delay)] - irs[0][(size_t) n])); if (std::abs (l[(size_t) n]) > std::abs (l[(size_t) pk])) pk = n; }
        std::printf ("motor vs FIR: error max %g, latencia %d, pico de salida en %d, pico del FIR en ~%d\n", err, lat, pk, naturalSizes (44100).delay);
    }
    const char* dEnv = std::getenv ("NAT_D"); const char* lEnv = std::getenv ("NAT_L");
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        auto z = naturalSizes (fs);
        if (dEnv) z.delay = std::atoi (dEnv);
        if (lEnv) z.length = std::atoi (lEnv);
        double worstM = 0, worstP = 0, worstMh = 0, worstPh = 0;
        for (auto& c : cases)
        {
            NaturalSpec sp; sp.fs = fs;
            for (size_t i = 0; i < c.bands.size(); ++i) sp.bands[i] = c.bands[i];
            sp.zoneOn = c.zone; sp.zoneLo = 200; sp.zoneHi = 8000;
            const auto e = naturalDesignError (sp, z);
            if (std::getenv ("NAT_VERBOSE")) std::printf ("   %6.0f  %-44s mag %.3f dB  fase %.2f deg  | >16k: %.2f dB %.1f deg\n", fs, c.name, e.magDb, e.phDeg, e.magHf, e.phHf);
            worstM = std::max (worstM, e.magDb); worstP = std::max (worstP, e.phDeg); worstMh = std::max (worstMh, e.magHf); worstPh = std::max (worstPh, e.phHf);
        }
        report ("Fase natural a " + juce::String (fs / 1000.0, 1) + " kHz (latencia " + juce::String (z.delay + z.partition) + " muestras = "
                + juce::String ((z.delay + z.partition) * 1000.0 / fs, 1) + " ms): igual al analogico en curva y fase hasta 16 kHz (peor " + juce::String (worstM, 2)
                + " dB / " + juce::String (worstP, 1) + " grados); 16-20 kHz: " + juce::String (worstMh, 2) + " dB / " + juce::String (worstPh, 1) + " grados",
                worstM < 0.05 && worstP < 0.5 && worstMh < 0.3 && worstPh < 2.0);
    }
}

static void sketchTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("sketch dibujar ajuste fit").contains (gFilter)) return;
    std::cout << "\n== SKETCH (dibujar la curva) ==" << std::endl;
    auto mk = [] (int t, double fr, double g, double q) { BandSettings b; b.on = true; b.type = t; b.freq = fr; b.gainDb = g; b.q = q; return b; };
    // simula la mano: puntos cada ~1/8 de octava con un temblor de +-0.3 dB
    auto drawCurve = [] (SketchCurve& sk, const std::vector<BandSettings>& truth, double f0, double f1, double stepOct, juce::Random& rnd, bool backwards = false)
    {
        std::vector<double> fs;
        for (double f = f0; f <= f1 * 1.0001; f *= std::pow (2.0, stepOct)) fs.push_back (f);
        if (backwards) std::reverse (fs.begin(), fs.end());
        for (double f : fs)
        {
            double db = 0; for (auto& b : truth) db += 20.0 * std::log10 (analogMagnitudeOfBand (b, f));
            sk.add (f, db + (rnd.nextDouble() - 0.5) * 0.6);
        }
        sk.endStroke();
    };
    juce::Random rnd (7);
    {
        // 1) curva completa (20 Hz - 20 kHz) hecha con 4 bandas conocidas
        SketchCurve sk;
        const std::vector<BandSettings> truth { mk (1, 120, 4, 0.707), mk (0, 400, -5, 1.4), mk (0, 3000, 3, 2.0), mk (2, 10000, -4, 0.707) };
        drawCurve (sk, truth, 20, 20000, 1.0 / 8.0, rnd);
        std::vector<double> fixed (sk.freqs().size(), 0.0);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const auto r = sk.fit (fixed, 24);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const auto got = curveOfBands (r.bands, sk.freqs()), want = curveOfBands (truth, sk.freqs());
        double mx = 0; for (size_t i = 0; i < got.size(); ++i) mx = std::max (mx, std::abs (got[i] - want[i]));
        report ("SKETCH: curva dibujada a mano (con temblor) -> " + juce::String ((int) r.bands.size()) + " bandas, error " + juce::String (mx, 2)
                + " dB, calculo " + juce::String (ms, 0) + " ms", mx < 1.0 && r.bands.size() <= 6 && ms < 400);
    }
    {
        // 2) solo un tramo (campana en 2 kHz, dibujada de 800 Hz a 5 kHz): fuera del trazo la curva no cambia
        SketchCurve sk;
        const std::vector<BandSettings> truth { mk (0, 2000, 6, 1.5) };
        drawCurve (sk, truth, 800, 5000, 1.0 / 10.0, rnd, true);   // de derecha a izquierda
        std::vector<double> fixed (sk.freqs().size(), 0.0);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const auto r = sk.fit (fixed, 24);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const auto got = curveOfBands (r.bands, sk.freqs()), want = curveOfBands (truth, sk.freqs());
        double inErr = 0, outErr = 0;
        for (size_t i = 0; i < got.size(); ++i)
        {
            const double f = sk.freqs()[i];
            if (f >= 800 && f <= 5000) inErr = std::max (inErr, std::abs (got[i] - want[i]));
            else if (f < 300 || f > 12000) outErr = std::max (outErr, std::abs (got[i]));
        }
        report ("SKETCH: un tramo (campana +6 dB en 2 kHz): " + juce::String ((int) r.bands.size()) + " banda(s), error en el trazo " + juce::String (inErr, 2)
                + " dB, lejos del trazo " + juce::String (outErr, 2) + " dB, " + juce::String (ms, 0) + " ms", inErr < 0.8 && outErr < 0.5 && r.bands.size() <= 2);
    }
    {
        // 3) sobre bandas existentes: el trazo es el TOTAL; las bandas nuevas completan lo que falta
        SketchCurve sk;
        const std::vector<BandSettings> existing { mk (1, 80, 3, 0.707), mk (0, 1000, -4, 1.0) };
        const auto fixed = curveOfBands (existing, sk.freqs());
        // el usuario dibuja de 400 Hz a 3 kHz una linea plana en 0 dB (quiere quitar el hueco de 1 kHz)
        for (double f = 400; f <= 3000; f *= std::pow (2.0, 1.0 / 12.0)) sk.add (f, 0.0);
        sk.endStroke();
        const auto r = sk.fit (fixed, 22);
        auto all = existing; for (auto& b : r.bands) all.push_back (b);
        const auto tot = curveOfBands (all, sk.freqs());
        double inErr = 0, lowErr = 0;
        for (size_t i = 0; i < tot.size(); ++i)
        {
            const double f = sk.freqs()[i];
            if (f >= 400 && f <= 3000) inErr = std::max (inErr, std::abs (tot[i]));
            if (f <= 60) lowErr = std::max (lowErr, std::abs (tot[i] - fixed[i]));
        }
        report ("SKETCH sobre bandas existentes: el hueco de 1 kHz queda plano (error " + juce::String (inErr, 2) + " dB), los graves no cambian ("
                + juce::String (lowErr, 2) + " dB)", inErr < 0.8 && lowErr < 0.5);
    }
    {
        // 4) varios trazos: el ultimo manda donde se cruzan; sin bandas libres no hace nada; un toque no hace nada
        SketchCurve sk;
        for (double f = 100; f <= 1000; f *= 1.05) sk.add (f, 6.0);
        sk.endStroke();
        for (double f = 300; f <= 600; f *= 1.05) sk.add (f, -6.0);
        sk.endStroke();
        size_t lo, hi; sk.range (lo, hi);
        bool ok = std::abs (sk.inkDb (lo) - 6.0) < 1e-9;
        for (size_t i = 0; i < sk.freqs().size(); ++i) { const double f = sk.freqs()[i]; if (f > 320 && f < 580) ok = ok && std::abs (sk.inkDb (i) + 6.0) < 1e-9; }
        std::vector<double> fixed (sk.freqs().size(), 0.0);
        ok = ok && sk.fit (fixed, 0).bands.empty();
        SketchCurve tap; tap.add (1000, 6); tap.endStroke();
        ok = ok && tap.fit (fixed, 24).bands.empty();
        // un movimiento muy rapido (dos posiciones a 3 octavas) no deja huecos
        SketchCurve fast; fast.add (100, 0); fast.add (800, 6); fast.endStroke();
        ok = ok && fast.count() == 3 * 24 + 1;
        report ("SKETCH: varios trazos (el ultimo manda), sin bandas libres / un toque no hacen nada, movimiento rapido sin huecos", ok);
    }
}

static void matchTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("match ajuste fit").contains (gFilter)) return;
    std::cout << "\n== EQ MATCH ==" << std::endl;
    const double fs = 48000;
    const auto f = fitGrid (20, 20000, 24);
    // 1) La referencia es la misma mezcla con un EQ conocido: Match debe descubrir ese EQ
    {
        auto src = makePop (fs, 12.0).mix;
        std::vector<BandSettings> truth;
        auto mk = [] (int t, double fr, double g, double q) { BandSettings b; b.on = true; b.type = t; b.freq = fr; b.gainDb = g; b.q = q; return b; };
        truth = { mk (1, 90, 3, 0.707), mk (0, 350, -3, 1.2), mk (0, 2500, 2, 1.0), mk (2, 9000, 4, 0.707) };
        const auto ref = processWith (src, fs, truth);
        const auto sDb = learnDb (src, fs, f, 1.0 / 3.0), rDb = learnDb (ref, fs, f, 1.0 / 3.0);
        std::vector<double> w;
        const auto tgt = matchTarget (f, sDb, rDb, 1.0, 12.0, w);
        FitOptions o; o.maxBands = 6; o.maxQ = matchMaxQ (1.0 / 3.0);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const auto fit = fitCurve (f, tgt, w, o);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const auto got = curveOfBands (fit.bands, f), want = curveOfBands (truth, f);
        double mx = 0, off = 0, cnt = 0;
        for (size_t i = 0; i < f.size(); ++i) if (f[i] >= 60 && f[i] <= 12000) { off += want[i] - got[i]; ++cnt; }
        off /= cnt;   // (el volumen general no cuenta)
        for (size_t i = 0; i < f.size(); ++i) if (f[i] >= 50 && f[i] <= 12000) mx = std::max (mx, std::abs (got[i] + off - want[i]));
        report ("EQ Match descubre el EQ de la referencia (4 bandas conocidas): " + juce::String ((int) fit.bands.size()) + " bandas, error de la curva "
                + juce::String (mx, 2) + " dB (50 Hz - 12 kHz), calculo " + juce::String (ms, 0) + " ms", mx < 1.0 && ms < 2000);
        // aplicado: la mezcla queda con el balance de la referencia
        const auto out = processWith (src, fs, fit.bands);
        const auto oDb = learnDb (out, fs, f, 1.0 / 3.0);
        double mo = 0, o2 = 0; cnt = 0;
        for (size_t i = 0; i < f.size(); ++i) if (f[i] >= 60 && f[i] <= 12000) { o2 += rDb[i] - oDb[i]; ++cnt; }
        o2 /= cnt;
        for (size_t i = 0; i < f.size(); ++i) if (f[i] >= 50 && f[i] <= 12000) mo = std::max (mo, std::abs (oDb[i] + o2 - rDb[i]));
        report ("EQ Match aplicado: el espectro de la mezcla queda igual al de la referencia (1/3 oct): diferencia maxima " + juce::String (mo, 2) + " dB", mo < 1.0);
    }
    // 2) Dos temas distintos (tech house -> balance de un pop): sin respuesta "verdadera"; debe acercarse mucho
    {
        auto src = makeTechHouse (fs, 12.0).mix, ref = makePop (fs, 12.0).mix;
        const auto sDb = learnDb (src, fs, f, 1.0 / 3.0), rDb = learnDb (ref, fs, f, 1.0 / 3.0);
        std::vector<double> w;
        const auto tgt = matchTarget (f, sDb, rDb, 1.0, 12.0, w);
        FitOptions o; o.maxBands = 12; o.toleranceDb = 0.5; o.maxQ = matchMaxQ (1.0 / 3.0);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const auto fit = fitCurve (f, tgt, w, o);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const auto out = processWith (src, fs, fit.bands);
        const auto oDb = learnDb (out, fs, f, 1.0 / 3.0);
        std::vector<double> sDbS (f.size()), oDbS (f.size());
        // se compara con el detalle del match (1/3 de octava sobre la diferencia), como lo pide el usuario
        std::vector<double> dBefore (f.size()), dAfter (f.size());
        for (size_t i = 0; i < f.size(); ++i) { dBefore[i] = rDb[i] - sDb[i]; dAfter[i] = rDb[i] - oDb[i]; }
        dBefore = smoothCurveOct (f, dBefore, 1.0 / 3.0); dAfter = smoothCurveOct (f, dAfter, 1.0 / 3.0);
        for (size_t i = 0; i < f.size(); ++i) { sDbS[i] = rDb[i] - dBefore[i]; oDbS[i] = rDb[i] - dAfter[i]; }
        double before = 0, after = 0, b0 = 0, a0 = 0, cnt = 0;
        for (size_t i = 0; i < f.size(); ++i) if (f[i] >= 60 && f[i] <= 12000) { b0 += rDb[i] - sDbS[i]; a0 += rDb[i] - oDbS[i]; ++cnt; }
        b0 /= cnt; a0 /= cnt;
        for (size_t i = 0; i < f.size(); ++i) if (f[i] >= 60 && f[i] <= 12000)
        {
            const double lim = 12.0;   // (lo que excede +-12 dB no se pide)
            before = std::max (before, std::min (lim, std::abs (sDbS[i] + b0 - rDb[i])));
            const double ai = std::max (0.0, std::abs (oDbS[i] + a0 - rDb[i]) - std::max (0.0, std::abs (rDb[i] - sDbS[i] - b0) - lim));
            if (ai > after && std::getenv ("GAUX_DIAG")) std::printf ("   peor hasta ahora en %.0f Hz: %.2f (objetivo %.2f, peso %.2f)\n", f[i], ai, tgt[i], w[i]);
            after = std::max (after, ai);
        }
        if (std::getenv ("GAUX_DIAG"))
        {
            const auto got = curveOfBands (fit.bands, f);
            for (size_t i = 0; i < f.size(); i += 6)
                std::printf ("   %7.0f Hz  objetivo %+6.2f  ajuste %+6.2f  peso %.2f  resultado-ref %+6.2f\n", f[i], tgt[i], got[i], w[i], oDbS[i] + a0 - rDb[i]);
            std::printf ("   ajuste: error max %.2f rms %.2f\n", fit.maxErrorDb, fit.rmsErrorDb);
        }
        report ("EQ Match entre dos temas (tech house -> balance de pop): diferencia de balance " + juce::String (before, 1) + " dB -> " + juce::String (after, 1)
                + " dB con " + juce::String ((int) fit.bands.size()) + " bandas (" + juce::String (ms, 0) + " ms)", after < 2.0 && after < before * 0.35);
    }
}

static void liveTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("vivo").contains (gFilter)) return;
    std::cout << "\n== EN VIVO: AUTOMATIZACION, CAMBIOS DURANTE LA REPRODUCCION Y SATURACION ==" << std::endl;
    const double fs = 48000.0;
    auto th = makeTechHouse (fs, 8.0);
    const St& mix = th.mix;
    using namespace ParamID;

    // 1) Barridos de build-up: la automatizacion a 512 muestras suena igual que a 32 (sin "zipper" ni clics)
    struct Sweep { const char* name; Band b; double f0, f1; bool gainSweep; };
    const Sweep sweeps[] = {
        { "low cut 12 dB resonante (Q 4) 20 Hz -> 2 kHz -> 20 Hz", lcut (20, 0, 4.0), 20, 2000, false },
        { "high cut 24 dB 20 kHz -> 200 Hz -> 20 kHz (filter-down clasico)", hcut (20000, 1), 20000, 200, false },
        { "high cut 12 dB resonante (Q 6) 18 kHz -> 300 Hz -> 18 kHz", hcut (18000, 0, 6.0), 18000, 300, false },
        { "campana +12 dB Q 10 buscando resonancias 200 Hz -> 8 kHz -> 200 Hz", bell (200, 12, 10), 200, 8000, false },
        { "campana 1 kHz Q 1: ganancia -18 -> +18 dB -> -18", bell (1000, -18, 1), -18, 18, true },
    };
    for (auto& sw : sweeps)
    {
        const double T = 8.0;
        auto setup = [&] (GauxEqProcessor& p) { applyBand (p, 0, sw.b); };
        auto autom = [&] (GauxEqProcessor& p, double t)
        {
            const double x = t < T / 2 ? t / (T / 2) : 2.0 - t / (T / 2);   // ida y vuelta
            if (sw.gainSweep) setP (p, band (0, "gain"), (float) (sw.f0 + (sw.f1 - sw.f0) * x));
            else setP (p, band (0, "freq"), (float) (sw.f0 * std::pow (sw.f1 / sw.f0, x)));
        };
        const St a = renderLive (fs, mix, 512, setup, autom), b = renderLive (fs, mix, 32, setup, autom);
        double err = 0, ref = 0, pk = 0; bool fin = true;
        for (size_t i = (size_t) (0.05 * fs); i < a.size(); ++i)
        {
            const double d = a.l[i] - b.l[i]; err += d * d; ref += b.l[i] * b.l[i];
            pk = std::max ({ pk, std::abs (a.l[i]), std::abs (a.r[i]) }); fin = fin && std::isfinite (a.l[i]) && std::isfinite (a.r[i]);
        }
        const double diffDb = 10 * std::log10 (err / std::max (1e-30, ref));
        const double maxBoost = sw.b.type == 0 ? std::max (sw.b.g, sw.gainSweep ? sw.f1 : 0.0) : (sw.b.slope == 0 ? dB (sw.b.q) : 0.0);
        report (juce::String ("Barrido ") + sw.name + ": automatizacion a 512 vs 32 muestras " + juce::String (diffDb, 1) + " dB, pico " + juce::String (dB (pk), 1) + " dBFS (entrada " + juce::String (dB (mix.peak()), 1) + ")",
                fin && diffDb < -30.0 && dB (pk) < dB (mix.peak()) + maxBoost + 3.0);
    }

    // 2) Cambios durante la reproduccion (botones y selectores): sin clics
    {
        St tone (fs * 3.0 > 0 ? (size_t) (fs * 3.0) : 0);
        for (size_t i = 0; i < tone.size(); ++i)
        {
            const double t = i / fs;
            const double v = 0.35 * std::sin (2 * kPi * 220 * t) + 0.12 * std::sin (2 * kPi * 3100 * t) + 0.06 * std::sin (2 * kPi * 9000 * t);
            tone.l[i] = v; tone.r[i] = 0.8 * v + 0.05 * std::sin (2 * kPi * 450 * t);
        }
        const size_t ev = (size_t) (1.5 * fs);
        struct Ev { const char* name; std::function<void (GauxEqProcessor&)> setup, change; double tol; bool warnOnly; };
        auto baseP = [] (GauxEqProcessor& p) { applyBand (p, 0, bell (220, 6, 1)); applyBand (p, 1, hshelf (6000, 4)); applyBand (p, 2, lcut (40, 2)); };
        auto baseV = [] (GauxEqProcessor& p) { setP (p, vLowFreq, 3); setP (p, vLowBoost, 5); setP (p, vHighFreq, 3); setP (p, vHighBoost, 4); setP (p, vLmBoost, 3); };
        const Ev evs[] = {
            { "Bypass ON", [&] (auto& p) { baseP (p); }, [] (auto& p) { setP (p, bypass, 1); }, 6, false },
            { "Bypass ON en HQ 4x", [&] (auto& p) { baseP (p); setP (p, quality, 2); }, [] (auto& p) { setP (p, bypass, 1); }, 6, false },
            { "Encender una banda +6 dB", [&] (auto& p) { baseP (p); setP (p, band (0, "on"), 0); }, [] (auto& p) { setP (p, band (0, "on"), 1); }, 6, false },
            { "Cambiar tipo Bell -> High Shelf", [&] (auto& p) { baseP (p); }, [] (auto& p) { setP (p, band (0, "type"), 2); }, 6, false },
            { "Cambiar pendiente 24 -> 48 dB", [&] (auto& p) { baseP (p); }, [] (auto& p) { setP (p, band (2, "slope"), 3); }, 6, false },
            { "Cambiar canal Stereo -> Side", [&] (auto& p) { baseP (p); }, [] (auto& p) { setP (p, band (1, "stereo"), 4); }, 6, false },
            { "Output -12 -> +6 dB de golpe", [&] (auto& p) { baseP (p); setP (p, output, -12); }, [] (auto& p) { setP (p, output, 6); }, 6, false },
            { "Gain Scale 100 -> 200 %", [&] (auto& p) { baseP (p); }, [] (auto& p) { setP (p, gainScale, 200); }, 6, false },
            { "Auto Gain ON", [&] (auto& p) { baseP (p); }, [] (auto& p) { setP (p, autoGain, 1); }, 6, false },
            { "Parametrico -> Vintage", [&] (auto& p) { baseP (p); baseV (p); }, [] (auto& p) { setP (p, mode, 1); }, 6, false },
            { "Vintage -> Parametrico", [&] (auto& p) { baseP (p); baseV (p); setP (p, mode, 1); }, [] (auto& p) { setP (p, mode, 0); }, 6, false },
            { "Vintage: selector LOW FREQ 100 -> 30", [&] (auto& p) { baseV (p); setP (p, mode, 1); }, [] (auto& p) { setP (p, vLowFreq, 1); }, 6, false },
            { "Vintage: PROGRAM IN off", [&] (auto& p) { baseV (p); setP (p, mode, 1); }, [] (auto& p) { setP (p, vProgramIn, 0); }, 6, false },
            { "Vintage: GLOW 0 -> 60 %", [&] (auto& p) { baseV (p); setP (p, mode, 1); }, [] (auto& p) { setP (p, vTube, 60); }, 6, false },
            { "ZONE (rango) ON", [&] (auto& p) { baseP (p); setP (p, rangeLo, 150); setP (p, rangeHi, 5000); }, [] (auto& p) { setP (p, rangeOn, 1); }, 6, false },
            { "Saturacion: drive 0 -> 18 (TUBE)", [&] (auto& p) { baseP (p); }, [] (auto& p) { setP (p, band (0, "drive"), 18); }, 6, false },
            { "Dinamica ON (umbral bajo, actua al instante)", [&] (auto& p) { baseP (p); setP (p, band (1, "thr"), -50); setP (p, band (1, "range"), -9); setP (p, band (1, "atk"), 1); }, [] (auto& p) { setP (p, band (1, "dyn"), 1); }, 6, false },
            { "Calidad Normal -> HQ 4x (cambia la latencia: el DAW la recompensa)", [&] (auto& p) { baseP (p); }, [] (auto& p) { setP (p, quality, 2); }, 6, false },
            { "Calidad HQ 4x -> Normal", [&] (auto& p) { baseP (p); setP (p, quality, 2); }, [] (auto& p) { setP (p, quality, 0); }, 6, false },
        };
        for (auto& e : evs)
        {
            bool done = false;
            const St y = renderLive (fs, tone, 256, e.setup, [&] (GauxEqProcessor& p, double t) { if (! done && t * fs >= (double) ev) { e.change (p); done = true; } });
            const double c = std::max (clickDb (y.l, ev, fs), clickDb (y.r, ev, fs));
            report (juce::String ("Cambio en vivo: ") + e.name + ": transitorio " + juce::String (c, 1) + " dB sobre lo normal", c < e.tol, e.warnOnly);
        }
    }

    // 3) Saturacion: aliasing (componentes que no son armonicos) en Normal / HQ 2x / HQ 4x
    {
        const double sr = 44100.0, f0 = 4410.0 * 1.0137;   // sinusoide aguda: sus armonicos pasan Nyquist
        St x ((size_t) (sr * 1.5));
        for (size_t i = 0; i < x.size(); ++i) x.l[i] = x.r[i] = 0.5 * std::sin (2 * kPi * f0 * i / sr);
        const char* satN[] = { "TUBE", "TAPE", "SOFT", "HARD", "FUZZ" };
        for (int st : { 3, 4, 0 })
        {
            juce::String row; double aN = 0, a4 = 0;
            for (int q : { 0, 1, 2 })
            {
                Band b = bell (f0, 0, 0.5); b.sat = st; b.drive = 24;
                const St y = renderLive (sr, x, 512, [&] (GauxEqProcessor& p) { applyBand (p, 0, b); setP (p, quality, (float) q); }, nullptr);
                const size_t N = 32768, st0 = (size_t) (0.4 * sr);
                std::vector<Cx> Y (N); for (size_t i = 0; i < N; ++i) Y[i] = y.l[st0 + i] * (0.5 - 0.5 * std::cos (2 * kPi * i / N));
                fft (Y);
                double harm = 0, alias = 0, fund = 0;
                for (size_t k = 2; k < N / 2; ++k)
                {
                    const double f = k * sr / N, e = std::norm (Y[k]);
                    const double h = f / f0, dist = std::abs (h - std::round (h)) * f0;   // distancia al armonico mas cercano
                    if (std::abs (f - f0) < 30) fund += e;
                    else if (dist < 30 && std::round (h) >= 2) harm += e;
                    else if (f > 20 && f < 19000) alias += e;   // lo que se escucha (arriba de 19 kHz no cuenta)
                }
                const double ad = 10 * std::log10 (alias / fund);
                if (std::getenv ("GAUX_ALIAS_DUMP"))
                {
                    std::vector<std::pair<double, double>> pk;
                    for (size_t k = 3; k < N / 2 - 1; ++k) { const double e = std::norm (Y[k]); if (e > std::norm (Y[k - 1]) && e > std::norm (Y[k + 1])) pk.push_back ({ e, k * sr / N }); }
                    std::sort (pk.rbegin(), pk.rend());
                    std::cout << "    [" << satN[st] << " q" << q << "] fund " << 10 * std::log10 (fund) << " picos:";
                    for (size_t j = 0; j < 12 && j < pk.size(); ++j) std::cout << " " << juce::String (pk[j].second, 0) << "Hz/" << juce::String (10 * std::log10 (pk[j].first / fund), 1);
                    std::cout << std::endl;
                }
                row += juce::String (q == 0 ? " Normal " : q == 1 ? ", HQ 2x " : ", HQ 4x ") + juce::String (ad, 1) + " dB";
                if (q == 0) aN = ad; if (q == 2) a4 = ad;
            }
            report (juce::String ("Aliasing ") + satN[st] + " drive 24 en 4.5 kHz a 44.1 kHz (aliasing audible, bajo la fundamental):" + row, a4 < -55 && aN < -25 && a4 < aN);
        }
    }

    // 4) El DAW re-prepara el plugin (cambia buffer o sample rate): los ajustes y el sonido se mantienen
    {
        GauxEqProcessor p; applyBand (p, 0, bell (1000, 6, 1)); applyBand (p, 1, lcut (80, 2));
        p.prepareToPlay (44100, 512);
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer midi;
        for (int k = 0; k < 20; ++k) { for (int i = 0; i < 512; ++i) { b.setSample (0, i, (float) mix.l[(size_t) (k * 512 + i)]); b.setSample (1, i, (float) mix.r[(size_t) (k * 512 + i)]); } p.processBlock (b, midi); }
        p.releaseResources(); p.prepareToPlay (96000, 128);
        bool fin = true; double pk = 0;
        juce::AudioBuffer<float> c (2, 128);
        for (int k = 0; k < 400; ++k) { for (int i = 0; i < 128; ++i) { c.setSample (0, i, (float) mix.l[(size_t) (k * 128 + i)]); c.setSample (1, i, (float) mix.r[(size_t) (k * 128 + i)]); } p.processBlock (c, midi);
            for (int i = 0; i < 128; ++i) { fin = fin && std::isfinite (c.getSample (0, i)); pk = std::max (pk, (double) std::abs (c.getSample (0, i))); } }
        const double g = dB (magnitudeOfBand (p.params.band (0), 1000, p.engineSampleRate.load()));
        report ("Re-preparar en vivo (44.1 kHz/512 -> 96 kHz/128): ajustes intactos (1 kHz = " + juce::String (g, 2) + " dB), audio finito, pico " + juce::String (dB (pk), 1) + " dBFS",
                fin && std::abs (g - 6.0) < 0.05 && dB (pk) < 12);
    }

    // 5) EQ dinamica hacia arriba (rango positivo): realza solo cuando la senal pasa el umbral
    {
        auto pop = makePop (48000, 6.0);
        Move m; m.bands = { dynb (bell (3000, 0, 1.0), -30, 6, 5, 120) };
        const auto r = runMove (m, pop.stems["VOZ"], 48000);
        double mx = -100, mn = 100; for (size_t i = 20; i < r.dynTrace.size(); ++i) { mx = std::max (mx, r.dynTrace[i]); mn = std::min (mn, r.dynTrace[i]); }
        report ("EQ dinamica hacia arriba (rango +6 dB en 3 kHz): se mueve entre " + juce::String (mn, 1) + " y " + juce::String (mx, 1) + " dB (nunca pasa de +6)", mx > 2.0 && mx <= 6.01 && mn >= -0.01);
        Move m2; m2.bands = { dynb (bell (7000, 0, 2.0), -60, -8, 1, 80) }; m2.bands[0].autoThr = true;
        const auto r2 = runMove (m2, pop.stems["VOZ"], 48000);
        double mn2 = 0, avg = 0; int cnt = 0; for (size_t i = 20; i < r2.dynTrace.size(); ++i) { mn2 = std::min (mn2, r2.dynTrace[i]); avg += r2.dynTrace[i]; ++cnt; }
        report ("De-esser con THRESHOLD AUTO: actua solo en las eses (minimo " + juce::String (mn2, 1) + " dB, promedio " + juce::String (avg / std::max (1, cnt), 1) + " dB)", mn2 < -3.0 && avg / std::max (1, cnt) > -4.0);
    }

    // 6) Gain staging analogico: el color del Vintage (IRON + GLOW) depende del nivel de entrada
    {
        const double sr = 48000.0;
        auto thdAt = [&] (double levelDb, float iron, float tube, double f0)
        {
            St x ((size_t) (sr * 1.2));
            const double a = std::pow (10.0, levelDb / 20.0);
            for (size_t i = 0; i < x.size(); ++i) x.l[i] = x.r[i] = a * std::sin (2 * kPi * f0 * i / sr);
            const St y = renderLive (sr, x, 512, [&] (GauxEqProcessor& p) { setP (p, ParamID::mode, 1); setP (p, ParamID::vIron, iron); setP (p, ParamID::vTube, tube); }, nullptr);
            const size_t N = 32768, s0 = (size_t) (0.3 * sr);
            std::vector<Cx> Y (N); for (size_t i = 0; i < N; ++i) Y[i] = y.l[s0 + i] * (0.35875 - 0.48829 * std::cos (2 * kPi * i / N) + 0.14128 * std::cos (4 * kPi * i / N) - 0.01168 * std::cos (6 * kPi * i / N));
            fft (Y);
            auto e = [&] (double f) { double v = 0; const long k = std::lround (f * N / sr); for (long j = k - 6; j <= k + 6; ++j) v += std::norm (Y[(size_t) j]); return v; };
            double h = 0; for (int k = 2; k <= 9; ++k) if (k * f0 < 0.45 * sr) h += e (k * f0);
            return 10 * std::log10 (std::max (1e-30, h) / e (f0));
        };
        const double lo = thdAt (-24, 50, 50, 100), hi = thdAt (-6, 50, 50, 100), hot = thdAt (0, 50, 50, 100);
        report ("Vintage IRON 50 % + GLOW 50 % (100 Hz): distorsion a -24 dBFS " + juce::String (lo, 1) + " dB, a -6 dBFS " + juce::String (hi, 1) + " dB, a 0 dBFS " + juce::String (hot, 1) + " dB (sube con el nivel, como el hardware)",
                lo < -40 && hi > lo + 10 && hot >= hi - 0.5 && hot < -15);
        const double off = thdAt (-6, 0, 0, 100);
        report ("Vintage sin IRON ni GLOW: limpio a -6 dBFS (" + juce::String (off, 1) + " dB)", off < -90);
    }

    // 7) Vintage igual en cualquier sample rate (sin HQ): realce de 16k, AIR 20k, atten 10k
    {
        VintageSettings v; v.highFreq = 6; v.highBoost = 5; v.highBandwidth = 4; v.airFreq = 3; v.airGain = 4; v.attenFreq = 1; v.highAtten = 3; v.lowFreq = 2; v.lowBoost = 5; v.lowAtten = 3;
        double worst = 0, fw = 0;
        for (double f : { 30.0, 60.0, 200.0, 1000.0, 4000.0, 8000.0, 10000.0, 12000.0, 14000.0, 16000.0 })
        {
            double lo2 = 1e9, hi2 = -1e9;
            for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) { const double g = dB (magnitudeOfVintage (v, f, sr)); lo2 = std::min (lo2, g); hi2 = std::max (hi2, g); }
            if (hi2 - lo2 > worst) { worst = hi2 - lo2; fw = f; }
        }
        report ("Vintage suena igual a 44.1 / 48 / 88.2 / 96 / 192 kHz: diferencia max " + juce::String (worst, 2) + " dB (en " + juce::String (fw, 0) + " Hz, hasta 16 kHz)", worst < 1.0);
    }

    // 8) Precision extrema a 192 kHz: graves muy bajos con Q alta (sub 22 Hz, Q 10) + aire 18 kHz
    {
        const double sr = 192000.0;
        auto orch = makeOrchestra (sr, 1.0);
        Move m; m.bands = { bell (22, 6, 10), lshelf (30, 4, 1.5), hshelf (18000, 3, 0.7), bell (40, -8, 18) };
        const auto r = runMove (m, orch.mix, sr);
        const auto a = audioVsDisplay (m, r, sr, 'L', 20000);
        report ("192 kHz con campana 22 Hz Q 10, shelf 30 Hz, notch-campana 40 Hz Q 18: audio = pantalla, desvio " + juce::String (a.maxDev, 4) + " dB", std::abs (a.maxDev) < 0.02 && a.pts > 20);
    }

    // 10) Modo "dormido" en silencio: al volver el audio suena exactamente igual que si nunca hubiera dormido
    {
        auto th = makeTechHouse (48000, 2.0);
        St x ((size_t) (48000 * 7.0));
        for (size_t i = 0; i < th.mix.size(); ++i) { x.l[i] = th.mix.l[i]; x.r[i] = th.mix.r[i]; x.l[i + 5 * 48000] = th.mix.l[i]; x.r[i + 5 * 48000] = th.mix.r[i]; }
        Move m; m.bands = { lcut (30, 2), bell (60, 4, 4), bell (3000, 2, 1), hshelf (10000, 2) };
        GauxEqProcessor::allowSleep = false; const auto ref = runMove (m, x, 48000, 256);
        GauxEqProcessor::allowSleep = true;  const auto slp = runMove (m, x, 48000, 256);
        double d = 0; for (size_t i = 0; i < x.size(); ++i) d = std::max ({ d, std::abs (ref.out.l[i] - slp.out.l[i]), std::abs (ref.out.r[i] - slp.out.r[i]) });
        report ("Pista que se calla 3 s y vuelve: con el modo dormido suena igual (diferencia " + juce::String (juce::roundToInt (dB (d))) + " dBFS) y en el silencio gasta " + juce::String (slp.cpuPct, 3) + " % (antes " + juce::String (ref.cpuPct, 3) + " %)",
                dB (d) < -130.0);
    }

    // 9) GAIN SCALE y OUTPUT exactos
    {
        auto pop = makePop (44100, 2.0);
        Move m; m.bands = { bell (300, -4, 1), hshelf (8000, 5, 0.7), lshelf (100, 3) };
        const St y0 = renderLive (44100, pop.mix, 512, [&] (GauxEqProcessor& p) { for (size_t i = 0; i < m.bands.size(); ++i) applyBand (p, (int) i, m.bands[i]); setP (p, ParamID::gainScale, 0); }, nullptr);
        double d = 0; for (size_t i = 4410; i < y0.size(); ++i) d = std::max (d, std::abs (y0.l[i] - pop.mix.l[i]));
        report ("GAIN SCALE 0 %: la curva queda plana (diferencia con la entrada " + juce::String (dB (d), 1) + " dB)", dB (d) < -90);
        const St y1 = renderLive (44100, pop.mix, 512, [&] (GauxEqProcessor& p) { setP (p, ParamID::output, -6); }, nullptr);
        double e1 = 0, e0 = 0; for (size_t i = 22050; i < y1.size(); ++i) { e1 += y1.l[i] * y1.l[i]; e0 += pop.mix.l[i] * pop.mix.l[i]; }
        const double g = 10 * std::log10 (e1 / e0);
        report ("OUTPUT -6 dB: la salida baja exactamente " + juce::String (g, 3) + " dB", std::abs (g + 6.0) < 0.01);
    }
}


//==============================================================================
// PRESETS EN MATERIAL REAL: cada preset de fabrica sobre la pista para la que fue pensado
static double gBandShare = 1.0;   // parte de la energia de la entrada que cae en la banda medida
static double bandChangeDb (const St& in, const St& out, double fs, double f1, double f2, char d = 'M')
{
    const auto S = welch (domainOf (in, d), domainOf (out, d), fs, (size_t) (0.3 * fs), 16384, true);   // ventana Blackman-Harris: sin fugas
    double a = 0, b = 0;
    double tot = 0;
    for (size_t k = 1; k < S.sxx.size(); ++k) { const double f = k * fs / S.nfft; tot += S.sxx[k]; if (f >= f1 && f < f2) { a += S.sxx[k]; b += S.syy[k]; } }
    gBandShare = a / std::max (1e-30, tot);
    return 10 * std::log10 (std::max (1e-30, b) / std::max (1e-30, a));
}

static void presetTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("presets").contains (gFilter)) return;
    std::cout << "\n== PRESETS DE FABRICA EN MATERIAL REAL ==" << std::endl;
    const double secs = 8.0;
    auto th = makeTechHouse (48000, secs); auto pop = makePop (44100, secs); auto rock = makeRock (44100, secs);
    auto dub = makeDubstep (48000, secs); auto sal = makeSalsa (48000, secs);
    St drumBus = th.stems["KICK"]; drumBus.add (th.stems["CLAP"]); drumBus.add (th.stems["HATS"]);
    St synthPad = th.stems["PAD"];

    struct Pick { const St* x; double fs; const char* what; };
    auto pick = [&] (const juce::String& cat, const juce::String& name) -> Pick
    {
        if (cat == "Voces")     return { &pop.stems["VOZ"], 44100, "voz pop" };
        if (cat == "Baterias")
        {
            if (name.contains ("Kick Dubstep")) return { &dub.stems["KICK"], 48000, "kick dubstep" };
            if (name.contains ("Kick"))  return { &th.stems["KICK"], 48000, "kick tech house" };
            if (name.contains ("Snare")) return { &rock.stems["SNARE"], 44100, "snare rock" };
            if (name.contains ("Hats"))  return { &th.stems["HATS"], 48000, "hats" };
            if (name.contains ("Percusion")) return { &sal.stems["CONGAS"], 48000, "congas" };
            return { &drumBus, 48000, "bus de bateria" };
        }
        if (cat == "Bajo")      return name.contains ("Dubstep") ? Pick { &dub.stems["GROWL"], 48000, "growl" } : Pick { &th.stems["BASS"], 48000, "bajo house" };
        if (cat == "Sintes")    return { &synthPad, 48000, "pad/synth" };
        if (cat == "Buses")     return name.contains ("Voces") ? Pick { &pop.stems["VOZ"], 44100, "voces" } : name.contains ("Instrumental") ? Pick { &rock.mix, 44100, "mezcla rock" } : Pick { &th.mix, 48000, "mezcla tech house" };
        if (cat == "Mastering") return name.contains ("Dubstep") ? Pick { &dub.mix, 48000, "master dubstep" } : Pick { &th.mix, 48000, "master tech house" };
        // Vintage
        if (name.contains ("Kick"))  return { &th.stems["KICK"], 48000, "kick" };
        if (name.contains ("Voz"))   return { &pop.stems["VOZ"], 44100, "voz" };
        if (name.contains ("Snare")) return { &rock.stems["SNARE"], 44100, "snare" };
        if (name.contains ("Bajo"))  return { &th.stems["BASS"], 48000, "bajo" };
        if (name.contains ("Synth")) return { &synthPad, 48000, "synth" };
        return { &th.mix, 48000, "mezcla" };
    };

    int tested = 0, bad = 0;
    for (int mode : { 0, 1 })
    {
        GauxEqProcessor lister; setP (lister, ParamID::mode, (float) mode);
        const auto list = lister.presets.entries();
        for (int idx = 0; idx < (int) list.size(); ++idx)
        {
            if (list[(size_t) idx].user) continue;
            const auto& e = list[(size_t) idx];
            const auto pk = pick (e.category, e.name);
            GauxEqProcessor p; setP (p, ParamID::mode, (float) mode); p.presets.load (idx);
            p.prepareToPlay (pk.fs, 512);
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
            const St& x = *pk.x; St y (x.size()); bool fin = true;
            for (size_t off = 0; off < x.size(); off += 512)
            {
                const int n = (int) std::min<size_t> (512, x.size() - off); buf.setSize (2, n, false, false, true);
                for (int i = 0; i < n; ++i) { buf.setSample (0, i, (float) x.l[off + i]); buf.setSample (1, i, (float) x.r[off + i]); }
                p.processBlock (buf, midi);
                for (int i = 0; i < n; ++i) { y.l[off + i] = buf.getSample (0, i); y.r[off + i] = buf.getSample (1, i); fin = fin && std::isfinite (y.l[off + i]); }
            }
            const double dl = lufs (y, pk.fs, (size_t) pk.fs) - lufs (x, pk.fs, (size_t) pk.fs);
            const double dp = dB (y.peak()) - dB (x.peak());
            const bool ag = p.params.autoGain->load() > 0.5f;
            // Que haga lo que dice su nombre
            juce::String what; bool intent = true;
            auto need = [&] (const char* desc, double v, bool ok) { what += juce::String (" | ") + desc + " " + juce::String (v, 1) + " dB"; intent = intent && ok; };
            const juce::String n = e.name;
            if (n.contains ("De-Esser"))            { const double v = bandChangeDb (x, y, pk.fs, 5500, 11000); need ("6-10 kHz", v, v < -1.0); }
            if (n.contains ("Graves en Mono"))      { const double v = bandChangeDb (x, y, pk.fs, 20, 90, 'S'); need ("lados <90 Hz", v, v < -15.0); }
            if (n.contains ("Aire"))                { const double v = bandChangeDb (x, y, pk.fs, 12000, 19000); need ("aire 12-19 kHz", v, v > 0.8); }
            if (n.contains ("Sub Profundo"))        { const double v = bandChangeDb (x, y, pk.fs, 40, 65); need ("40-65 Hz", v, v > 1.5); }
            if (n.contains ("Hats"))                { const double v = bandChangeDb (x, y, pk.fs, 30, 250); need ((juce::String ("<250 Hz (") + juce::String (juce::roundToInt (10 * std::log10 (gBandShare))) + " dB de la pista)").toRawUTF8(), v, v < -10.0 || gBandShare < 1e-4); }
            if (n.contains ("Lo-Fi") || n.contains ("Telefono")) { const double a = bandChangeDb (x, y, pk.fs, 30, 200), b = bandChangeDb (x, y, pk.fs, 7000, 16000); need ("graves", a, a < -8.0); need ("agudos", b, b < -8.0); }
            if (n.contains ("Truco de Graves"))     { const double v = bandChangeDb (x, y, pk.fs, 40, 80); need ("40-80 Hz", v, v > 2.0); }
            if (n.contains ("Sub Limpio"))          { const double v = bandChangeDb (x, y, pk.fs, 15, 24); need ("<24 Hz (rumble)", v, v < -6.0); }
            if (n.contains ("Brillante"))           { const double v = bandChangeDb (x, y, pk.fs, 10000, 18000); need ("brillo 10-18 kHz", v, v > 1.5); }
            if (n.contains ("Ancho"))               { const double v = bandChangeDb (x, y, pk.fs, 5000, 16000, 'S'); need ("lados agudos", v, v > 0.7); }
            const bool lofi = n.contains ("Lo-Fi") || n.contains ("Telefono");
            const bool lvlOk = ag ? std::abs (dl) < 1.5 : (dl < 3.5 && dl > (lofi ? -16.0 : -7.0));   // un preset no deberia "ganar" solo por sonar mas fuerte
            const bool ok = fin && lvlOk && dp < 6.0 && intent;
            ++tested; if (! ok) ++bad;
            report (juce::String (mode ? "Vintage " : "Param.  ") + e.name + " en " + pk.what + ": volumen " + juce::String (dl, 1) + " LU" + (ag ? " (Auto Gain)" : "") + ", pico " + juce::String (dp, 1) + " dB" + what, ok);
        }
    }
    std::cout << "  -> " << tested << " presets en su material, " << bad << " con problemas" << std::endl;
}


//==============================================================================
// ELECTRONICA A FONDO: techno (peak time, hard/industrial, acid, minimal, melodico), trance (uplifting, psy),
// big room, hardstyle, progressive, deep house, future bass, UK garage e IDM/glitch.
namespace el
{
using namespace syn;

// Supersaw estereo de 7 voces (leads de trance / big room / future bass)
static void supersaw (St& d, double fs, double t0, double dur, double f, double amp, double cutoff, double atk = 0.005, double rel = 0.08)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) ((dur + rel) * fs);
    static const double det[] = { -0.11, -0.065, -0.025, 0.0, 0.025, 0.065, 0.11 };
    static const double pan[] = { -0.9, 0.7, -0.4, 0.0, 0.4, -0.7, 0.9 };
    std::array<Saw, 7> o; for (int k = 0; k < 7; ++k) o[(size_t) k].ph = 0.137 * k;
    Bq lpl (makeLowPass (fs, cutoff, 0.7)), lpr (makeLowPass (fs, cutoff, 0.7));
    for (size_t i = 0; i < n && s0 + i < d.size(); ++i)
    {
        const double t = i / fs;
        const double env = std::min (1.0, t / atk) * (t < dur ? 1.0 : std::exp (-(t - dur) / (rel * 0.3)));
        double l = 0, r = 0;
        for (int k = 0; k < 7; ++k)
        {
            const double v = o[(size_t) k].next (f * std::pow (2.0, det[k] / 12.0), fs);
            const double a = (pan[k] + 1.0) * 0.25 * kPi;
            l += std::cos (a) * v; r += std::sin (a) * v;
        }
        d.l[s0 + i] += amp * 0.22 * env * lpl.p (l); d.r[s0 + i] += amp * 0.22 * env * lpr.p (r);
    }
}

// Linea acid tipo 303: sierra -> pasa bajos resonante con envolvente (filtro TPT), acentos y saturacion
static void acid (std::vector<double>& v, double fs, double t0, double dur, double f, double amp, double cut0, double envAmt, double res, bool accent)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (dur * fs);
    Saw o; double ic1 = 0, ic2 = 0;
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs;
        const double fc = std::min (0.42 * fs, cut0 * (1.0 + (accent ? 1.6 : 1.0) * envAmt * std::exp (-t / 0.12)));
        const double g = std::tan (kPi * fc / fs), k = 1.0 / res;
        const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double x = o.next (f, fs);
        const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        const double env = std::min (1.0, t / 0.002) * std::min (1.0, (dur - t) / 0.004) * (accent ? 1.3 : 1.0);
        v[s0 + i] += amp * std::tanh (1.8 * v2 * env);
    }
}

// Kick distorsionado (hard techno / hardstyle): pitch alto, saturacion fuerte y cola larga
static void hardKick (std::vector<double>& v, double fs, double t0, double amp, double drive = 7.0, double tail = 0.45, double f1 = 52)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (fs * tail * 3);
    double ph = 0;
    for (size_t i = 0; i < n && s0 + i < v.size(); ++i)
    {
        const double t = i / fs;
        const double f = f1 + 260.0 * std::exp (-t / 0.022);
        ph += tau * f / fs;
        const double y = std::sin (ph) * std::exp (-t / tail) * std::min (1.0, t / 0.0005);
        v[s0 + i] += amp * std::tanh (drive * y + 0.15 * drive * y * y);   // asimetrico: genera DC y pares
    }
}

// "Rumble" de techno: el kick pasado por una reverb oscura y saturada (la masa de graves entre golpes)
static std::vector<double> rumble (const std::vector<double>& k, double fs)
{
    std::vector<double> o (k.size(), 0.0);
    const size_t d1 = (size_t) (fs * 0.037), d2 = (size_t) (fs * 0.053), d3 = (size_t) (fs * 0.071);
    std::vector<double> b1 (d1), b2 (d2), b3 (d3); size_t i1 = 0, i2 = 0, i3 = 0;
    Bq lp1 (makeLowPass (fs, 160, 0.7)), lp2 (makeLowPass (fs, 160, 0.7)), hp (makeHighPass (fs, 35, 0.7));
    for (size_t i = 0; i < k.size(); ++i)
    {
        const double x = k[i];
        const double y = 0.33 * (b1[i1] + b2[i2] + b3[i3]);
        b1[i1] = x + 0.82 * y; b2[i2] = x + 0.8 * b2[i2] * 0.0 + 0.82 * y; b3[i3] = x + 0.82 * y;
        i1 = (i1 + 1) % d1; i2 = (i2 + 1) % d2; i3 = (i3 + 1) % d3;
        o[i] = 0.9 * std::tanh (2.5 * hp.p (lp2.p (lp1.p (y))));
    }
    // el rumble empieza despues del golpe (ducking natural del productor: aca no, eso lo hace el EQ)
    return o;
}

static void noiseRiser (St& d, double fs, double t0, double dur, double amp, Rng& rng)
{
    const size_t s0 = (size_t) (t0 * fs), n = (size_t) (dur * fs);
    BiquadState sl, sr;
    for (size_t i = 0; i < n && s0 + i < d.size(); ++i)
    {
        const double x = (double) i / n;
        const auto c = makeHighPass (fs, 200.0 * std::pow (40.0, x), 0.8);   // el riser se abre hacia arriba
        d.l[s0 + i] += amp * x * sl.process (rng.uni(), c);
        d.r[s0 + i] += amp * x * sr.process (rng.uni(), c);
    }
}
} // namespace el

static Session makeTechno (double fs, double secs, double bpm, const char* name, bool hard)
{
    using namespace syn; using namespace el;
    Session s; s.genre = name; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (31 + (unsigned) bpm);
    const double beat = 60.0 / bpm;
    std::vector<double> k (N), h (N), c (N), ride (N), stab (N);
    for (double t = 0; t < secs; t += beat) { if (hard) hardKick (k, fs, t, 0.9, 6.0, 0.3, 48); else kick (k, fs, t, 0.95, 170, 50, 0.2, &rng); }
    for (double t = beat * 0.5; t < secs; t += beat) hat (h, fs, t, 0.28, rng, hard ? 0.06 : 0.035);
    for (double t = beat * 0.25; t < secs; t += beat * 0.5) hat (ride, fs, t, 0.08, rng, 0.02);
    for (double t = beat; t < secs; t += beat * 2) clap (c, fs, t, hard ? 0.35 : 0.45, rng);
    for (double t = beat * 0.75; t < secs; t += beat * 2) bassNote (stab, fs, t, beat * 0.2, midiHz (48), 0.25, 1800, 2.5);
    auto rb = rumble (k, fs);
    s.stems["KICK"] = mono2 (k); s.stems["RUMBLE"] = mono2 (rb);
    St hats = mono2 (h, 0.1); syn::panInto (hats, ride, -0.4, 1.0); s.stems["HATS"] = hats;
    s.stems["CLAP"] = mono2 (c); room (s.stems["CLAP"], fs, 0.35, 33);
    St st (N); syn::panInto (st, stab, 0.3, 1.0); room (st, fs, 0.4, 34); s.stems["STAB"] = st;
    s.buildMix (hard ? 6.0 : -0.3);   // hard techno: bus de bateria ya "caliente" (+6 dBFS)
    return s;
}

static Session makeAcid (double fs, double secs)
{
    using namespace syn; using namespace el;
    Session s; s.genre = "Acid Techno"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (41);
    const double beat = 60.0 / 132.0, st = beat / 4;
    std::vector<double> k (N), a (N), h (N);
    for (double t = 0; t < secs; t += beat) kick (k, fs, t, 0.9, 160, 50, 0.25, &rng);
    for (double t = beat * 0.5; t < secs; t += beat) hat (h, fs, t, 0.2, rng, 0.04);
    static const int notes[] = { 36, 36, 48, 36, 39, 36, 46, 48, 36, 43, 36, 36, 48, 41, 36, 39 };
    int idx = 0;
    for (double t = 0; t < secs; t += st, ++idx)
        acid (a, fs, t, st * 0.85, midiHz (notes[idx % 16] + 12), 0.5, 380 + 300 * std::sin (t * 0.9), 6.0, 7.0, idx % 3 == 0);
    s.stems["KICK"] = mono2 (k); s.stems["303"] = mono2 (a); s.stems["HATS"] = mono2 (h, 0.2);
    room (s.stems["303"], fs, 0.2, 42);
    s.buildMix (-0.3);
    return s;
}

static Session makeMinimal (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Minimal / Microhouse"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (51);
    const double beat = 60.0 / 126.0;
    std::vector<double> k (N), cl (N), b (N);
    for (double t = 0; t < secs; t += beat) if (std::fmod (t, beat * 8) < beat * 6) kick (k, fs, t, 0.6, 140, 52, 0.18, &rng);   // huecos de silencio
    for (double t = beat * 0.75; t < secs; t += beat * 1.5) hat (cl, fs, t, 0.12, rng, 0.012);
    for (double t = beat * 0.5; t < secs; t += beat * 2) bassNote (b, fs, t, beat * 0.25, midiHz (36), 0.35, 300, 1.2);
    s.stems["KICK"] = mono2 (k); s.stems["CLICKS"] = mono2 (cl, 0.5); s.stems["BAJO"] = mono2 (b);
    s.buildMix (-14.0);   // mezcla muy dinamica y baja (mucho headroom)
    return s;
}

static Session makeMelodicTechno (double fs, double secs)
{
    using namespace syn; using namespace el;
    Session s; s.genre = "Melodic Techno"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (61);
    const double beat = 60.0 / 122.0;
    std::vector<double> k (N), b (N), arp (N);
    for (double t = 0; t < secs; t += beat) kick (k, fs, t, 0.9, 150, 47, 0.3, &rng);
    for (double t = beat * 0.25; t < secs; t += beat * 0.25) if (std::fmod (t, beat) > beat * 0.2) bassNote (b, fs, t, beat * 0.2, midiHz (33), 0.4, 450, 1.4);
    static const int an[] = { 57, 60, 64, 67, 69, 67, 64, 60 }; int i = 0;
    for (double t = 0; t < secs; t += beat * 0.5, ++i) pluck (arp, fs, t, midiHz (an[i % 8]), 0.3, beat * 0.45, 0.7, rng);
    St pads (N); supersaw (pads, fs, 0.0, secs - 0.2, midiHz (57), 0.5, 2500, 0.8, 0.5); supersaw (pads, fs, 0.0, secs - 0.2, midiHz (64), 0.4, 2500, 0.8, 0.5);
    room (pads, fs, 0.5, 62);
    St ar (N); syn::panInto (ar, arp, -0.5, 1.0); { auto a2 = arp; std::rotate (a2.begin(), a2.begin() + (size_t) (beat * 0.75 * fs), a2.end()); syn::panInto (ar, a2, 0.5, 0.6); }
    s.stems["KICK"] = mono2 (k); s.stems["BAJO"] = mono2 (b); s.stems["PADS"] = pads; s.stems["ARP"] = ar;
    s.buildMix (-0.5);
    return s;
}

static Session makeTrance (double fs, double secs)
{
    using namespace syn; using namespace el;
    Session s; s.genre = "Trance Uplifting"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (71);
    const double beat = 60.0 / 138.0;
    std::vector<double> k (N), b (N), h (N);
    for (double t = 0; t < secs; t += beat) kick (k, fs, t, 0.95, 180, 52, 0.22, &rng);
    for (double t = beat * 0.5; t < secs; t += beat) { bassNote (b, fs, t, beat * 0.4, midiHz (38), 0.45, 900, 1.2); hat (h, fs, t, 0.25, rng, 0.05); }
    St lead (N);
    static const int ch[][3] = { { 62, 65, 69 }, { 58, 62, 65 }, { 60, 64, 67 }, { 57, 60, 64 } };
    for (int bar = 0; bar * beat * 4 < secs; ++bar) for (int v = 0; v < 3; ++v) supersaw (lead, fs, bar * beat * 4, beat * 4 - 0.05, midiHz (ch[bar % 4][v] + 12), 0.45, 9000, 0.004, 0.1);
    room (lead, fs, 0.4, 72);
    St riser (N); noiseRiser (riser, fs, 0.0, secs, 0.25, rng);
    s.stems["KICK"] = mono2 (k); s.stems["BAJO"] = mono2 (b); s.stems["HATS"] = mono2 (h, 0.15); s.stems["SUPERSAW"] = lead; s.stems["RISER"] = riser;
    s.buildMix (-0.3);
    return s;
}

static Session makePsy (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Psytrance"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (81);
    const double beat = 60.0 / 145.0;
    std::vector<double> k (N), b (N), h (N);
    for (double t = 0; t < secs; t += beat) kick (k, fs, t, 0.95, 200, 55, 0.12, &rng);
    for (double t = 0; t < secs; t += beat)
        for (int q = 1; q < 4; ++q) bassNote (b, fs, t + q * beat / 4, beat / 4 * 0.85, midiHz (40), 0.55, 650, 1.6);   // "rolling bass" entre golpes
    for (double t = beat * 0.5; t < secs; t += beat) hat (h, fs, t, 0.22, rng, 0.03);
    s.stems["KICK"] = mono2 (k); s.stems["BAJO"] = mono2 (b); s.stems["HATS"] = mono2 (h, -0.2);
    s.buildMix (-0.3);
    return s;
}

static Session makeBigRoom (double fs, double secs)
{
    using namespace syn; using namespace el;
    Session s; s.genre = "Big Room"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (91);
    const double beat = 60.0 / 128.0;
    std::vector<double> k (N);
    for (double t = 0; t < secs; t += beat) { kick (k, fs, t, 1.0, 220, 50, 0.45, &rng); }
    St lead (N);
    for (double t = beat * 0.5; t < secs; t += beat) supersaw (lead, fs, t, beat * 0.3, midiHz (70), 0.9, 12000, 0.002, 0.05);
    room (lead, fs, 0.5, 92);
    St kk = mono2 (k); for (auto* ch : { &kk.l, &kk.r }) for (auto& x : *ch) x = std::tanh (1.6 * x) * 1.1;   // kick ya clipeado
    s.stems["KICK"] = kk; s.stems["LEAD"] = lead;
    s.buildMix (6.0);   // master que llega a +6 dBFS (el clasico "ya viene clipeado")
    return s;
}

static Session makeHardstyle (double fs, double secs)
{
    using namespace syn; using namespace el;
    Session s; s.genre = "Hardstyle"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (101);
    const double beat = 60.0 / 150.0;
    std::vector<double> k (N), rb (N);
    for (double t = 0; t < secs; t += beat) hardKick (k, fs, t, 0.95, 14.0, 0.32, 55);
    // "reverse bass" en los contratiempos: nota que sube de volumen hacia el siguiente kick
    for (double t = beat * 0.5; t < secs; t += beat)
    {
        const size_t s0 = (size_t) (t * fs), n = (size_t) (beat * 0.5 * fs); Saw o;
        for (size_t i = 0; i < n && s0 + i < N; ++i) { const double x = (double) i / n; rb[s0 + i] += 0.6 * std::tanh (4.0 * x * o.next (midiHz (40), fs)); }
    }
    St screech (N);
    { Saw a; Bq bp (makeBandPass (fs, 2500, 3.0)); std::vector<double> sc (N); for (size_t i = 0; i < N; ++i) { const double t = i / fs; sc[i] = 0.5 * std::tanh (12.0 * bp.p (a.next (midiHz (64) * (1.0 + 0.04 * std::sin (tau * 6 * t)), fs))); } syn::panInto (screech, sc, 0.0, 1.0); room (screech, fs, 0.3, 102); }
    s.stems["KICK"] = mono2 (k); s.stems["REVERSE BASS"] = mono2 (rb); s.stems["SCREECH"] = screech;
    s.buildMix (3.0);
    for (auto* n : { "KICK", "REVERSE BASS", "SCREECH" }) s.stems[n].scaleToPeakDb (-1.0);   // pistas sueltas a -1 dBFS
    return s;
}

static Session makeFutureBass (double fs, double secs)
{
    using namespace syn; using namespace el;
    Session s; s.genre = "Future Bass"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (111);
    const double beat = 60.0 / 150.0;
    std::vector<double> k (N), sn (N);
    for (double t = 0; t < secs; t += beat * 2) { kick (k, fs, t, 0.9, 150, 48, 0.3, &rng); snare (sn, fs, t + beat, 0.7, rng, 0.2, 200); }
    St chords (N);
    static const int ch[][4] = { { 62, 65, 69, 72 }, { 60, 64, 67, 71 } };
    for (int bar = 0; bar * beat * 4 < secs; ++bar) for (int v = 0; v < 4; ++v) supersaw (chords, fs, bar * beat * 4, beat * 4 - 0.1, midiHz (ch[bar % 2][v]), 0.4, 7000, 0.01, 0.2);
    // pumping tipico (volumen con la forma del sidechain)
    for (size_t i = 0; i < N; ++i) { const double ph = std::fmod (i / fs, beat) / beat; const double g = std::min (1.0, 0.15 + ph * 2.2); chords.l[i] *= g; chords.r[i] *= g; }
    std::vector<double> vo (N);
    std::vector<VoiceNote> vn; for (double t = 0.2; t < secs - 0.5; t += beat) vn.push_back ({ t, beat * 0.4, midiHz (69 + (int) (t / beat) % 3 * 2), (int) (t / beat) % 5, false, false });
    voice (vo, fs, vn, 0.45, rng, 0.6, 2600);
    s.stems["KICK"] = mono2 (k); s.stems["SNARE"] = mono2 (sn); s.stems["CHORDS"] = chords; s.stems["VOCAL CHOPS"] = mono2 (vo, 0.1);
    room (s.stems["VOCAL CHOPS"], fs, 0.3, 112);
    s.buildMix (-0.3);
    return s;
}

static Session makeDeepHouse (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "Deep / Progressive House"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (121);
    const double beat = 60.0 / 122.0;
    std::vector<double> k (N), b (N), keys (N), h (N);
    for (double t = 0; t < secs; t += beat) kick (k, fs, t, 0.8, 130, 48, 0.3, &rng);
    for (double t = beat * 0.5; t < secs; t += beat) { bassNote (b, fs, t, beat * 0.35, midiHz (33), 0.45, 350, 0.9); hat (h, fs, t, 0.15, rng, 0.06); }
    static const int ch[] = { 57, 60, 64, 67 };
    for (double t = 0; t < secs; t += beat * 2) for (int n : ch) piano (keys, fs, t + beat * 0.25, midiHz (n), 0.18, beat * 1.5);
    s.stems["KICK"] = mono2 (k); s.stems["BAJO"] = mono2 (b); s.stems["KEYS"] = mono2 (keys, -0.2); s.stems["HATS"] = mono2 (h, 0.3);
    room (s.stems["KEYS"], fs, 0.4, 122);
    s.buildMix (-1.0);
    return s;
}

static Session makeUkg (double fs, double secs)
{
    using namespace syn;
    Session s; s.genre = "UK Garage / 2-Step"; s.fs = fs;
    const size_t N = (size_t) (fs * secs); Rng rng (131);
    const double beat = 60.0 / 132.0, sw = beat * 0.58;   // swing
    std::vector<double> k (N), sn (N), h (N), sub (N);
    for (double t = 0; t < secs; t += beat * 2) { kick (k, fs, t, 0.85, 160, 50, 0.2, &rng); kick (k, fs, t + beat * 1.5 + sw - beat * 0.5, 0.6, 160, 50, 0.15, &rng); snare (sn, fs, t + beat, 0.7, rng, 0.12, 210); }
    for (double t = 0; t < secs; t += beat) { hat (h, fs, t + sw * 0.5, 0.18, rng, 0.03); hat (h, fs, t + beat * 0.5 + sw * 0.3, 0.12, rng, 0.02); }
    for (double t = 0; t < secs; t += beat * 2) sub808 (sub, fs, t + beat * 0.5, beat * 1.2, midiHz (31), 0.7, 1.4);
    s.stems["KICK"] = mono2 (k); s.stems["SNARE"] = mono2 (sn); s.stems["HATS"] = mono2 (h, 0.25); s.stems["SUB"] = mono2 (sub);
    s.buildMix (-0.5);
    return s;
}

// Energia de una banda (dB) en los primeros ms despues de cada golpe vs el resto del pulso
static void hitVsGap (const St& in, const St& out, double fs, double bpm, double f1, double f2, double hitA, double hitB, double gapA, double gapB, double& onDiff, double& offDiff)
{
    const double beat = 60.0 / bpm;
    const auto ei = bandEnvelopeDb (domainOf (in, 'M'), fs, f1, f2, 0.002), eo = bandEnvelopeDb (domainOf (out, 'M'), fs, f1, f2, 0.002);
    double on = 0, off = 0; int non = 0, noff = 0;
    for (size_t k = 250; k < ei.size(); ++k)
    {
        const double t = k * 0.002, ph = std::fmod (t, beat) / beat;
        if (ei[k] < -70) continue;
        if (ph >= hitA && ph < hitB) { on += eo[k] - ei[k]; ++non; }
        else if (ph >= gapA && ph < gapB) { off += eo[k] - ei[k]; ++noff; }
    }
    onDiff = on / std::max (1, non); offDiff = off / std::max (1, noff);
}

static void electroTests()
{
    const bool all = gFilter.isEmpty() || gFilter.containsIgnoreCase ("electro");
    auto wantE = [&] (const char* k) { return all || juce::String (k).containsIgnoreCase (gFilter); };
    const double secs = 7.0;
    std::cout << "\n== ELECTRONICA A FONDO ==" << std::endl;
    Move m;

    // ---------------- TECHNO PEAK TIME (130) ----------------
    if (wantE ("techno peak"))
    {
        auto s = makeTechno (48000, secs, 130, "Techno Peak Time", false);
        m = {}; m.track = "KICK"; m.name = "low cut 28 Hz 48 dB + golpe 58 Hz +3 Q 1.8 + caja 220 Hz -6 Q 3 + click 3.5 kHz +2";
        m.bands = { lcut (28, 3), bell (58, 3, 1.8), bell (220, -6, 3), bell (3500, 2, 1.2) }; runSessionMove (s, s.stems["KICK"], m);
        m = {}; m.track = "RUMBLE"; m.name = "rumble que se aparta del kick: shelf dinamico 110 Hz -12 dB con sidechain del kick (ataque 0.5 ms, release 70 ms)";
        // umbral como lo pondria un ingeniero: unos 12 dB debajo del pico del kick (el golpe aparta casi todo el rango)
        const double kpk = dB (s.stems["KICK"].peak());
        m.bands = { dynb (lshelf (110, 0, 0.7), kpk - 12, -12, 0.5, 70, true), lcut (30, 2) }; m.sidechain = &s.stems["KICK"];
        m.check = [&] (const St& in, const St& out, double fs, juce::String& msg)
        {
            double on, off; hitVsGap (in, out, fs, 130, 35, 110, 0.0, 0.12, 0.75, 0.98, on, off);   // los primeros 55 ms del golpe
            msg = "con el kick el rumble baja " + juce::String (-on, 1) + " dB; al final del pulso " + juce::String (-off, 1) + " dB (debe volver)";
            return on < -4.0 && off > -3.0;
        };
        runSessionMove (s, s.stems["RUMBLE"], m);
        m = {}; m.track = "MASTER"; m.name = "low cut 22 Hz 24 dB + graves en mono (Side low cut 150 Hz 24 dB) + 300 Hz -1 Q 0.8 + aire 12 kHz +1.5";
        m.bands = { lcut (22, 1), lcut (150, 1, 0.707, 4), bell (300, -1, 0.8), hshelf (12000, 1.5, 0.7) }; m.domain = 'M'; runSessionMove (s, s.mix, m);
    }

    // ---------------- HARD / INDUSTRIAL TECHNO (150, bus a +6 dBFS) ----------------
    if (wantE ("hard techno industrial"))
    {
        auto s = makeTechno (48000, secs, 150, "Hard / Industrial Techno", true);
        m = {}; m.track = "KICK (distorsionado, +6 dBFS)"; m.name = "FUZZ drive 24 en 90 Hz + HARD drive 18 en 2.5 kHz (HQ 4x) + low cut 30 Hz";
        { Band a = bell (90, 2, 1.0); a.sat = 4; a.drive = 24; Band b = bell (2500, 0, 1.5); b.sat = 3; b.drive = 18; m.bands = { a, b, lcut (30, 2) }; }
        m.quality = 2;
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            double dcIn = 0, dcOut = 0; for (size_t i = (size_t) fs; i < in.size(); ++i) { dcIn += in.l[i]; dcOut += out.l[i]; }
            dcIn /= (double) (in.size() - (size_t) fs); dcOut /= (double) (in.size() - (size_t) fs);
            const double d = lufs (out, fs, (size_t) fs) - lufs (in, fs, (size_t) fs);
            msg = "saturacion extrema sobre kick asimetrico: DC " + juce::String (dcIn, 4) + " -> " + juce::String (dcOut, 4) + ", volumen " + juce::String (d, 1) + " LU";
            return std::abs (dcOut) <= std::abs (dcIn) + 0.002 && d > -3.0 && d < 6.0;
        };
        runSessionMove (s, s.stems["KICK"], m);
        m = {}; m.track = "MASTER (+6 dBFS)"; m.name = "las 16 bandas encendidas (cortes, campanas, shelfs, M/S, 4 saturando, 3 dinamicas) en HQ 4x";
        m.bands = { lcut (25, 3), bell (55, 2, 1.5), bell (120, -2, 2), bell (250, -3, 1.2), bell (500, -1, 1), bell (900, 1, 2), bell (1800, -2, 3), bell (3200, 2, 1),
                    bell (5000, -2, 4), hshelf (9000, 2, 0.7), hshelf (14000, 1, 0.5), lcut (140, 1, 0.707, 4), hcut (19000, 1), notch (1000, 8), bell (7000, 0, 2), lshelf (80, 1) };
        m.bands[1].sat = 0; m.bands[1].drive = 9; m.bands[7].sat = 1; m.bands[7].drive = 6; m.bands[9].sat = 2; m.bands[9].drive = 6; m.bands[15].sat = 3; m.bands[15].drive = 4;
        m.bands[3] = dynb (m.bands[3], -24, -4, 5, 120); m.bands[8] = dynb (m.bands[8], -30, -5, 1, 60); m.bands[14] = dynb (m.bands[14], -30, -4, 2, 80);
        m.quality = 2;
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const double d = lufs (out, fs, (size_t) fs) - lufs (in, fs, (size_t) fs);
            msg = "16 bandas a la vez con la senal caliente: volumen " + juce::String (d, 1) + " LU, pico " + juce::String (dB (out.peak()), 1) + " dBFS";
            return std::abs (d) < 6.0 && dB (out.peak()) < 14.0;
        };
        runSessionMove (s, s.mix, m);
    }

    // ---------------- ACID (303 resonante) ----------------
    if (wantE ("acid techno"))
    {
        auto s = makeAcid (44100, secs);
        m = {}; m.track = "303"; m.name = "low cut 120 Hz + 'squelch' 1.2 kHz +4 Q 4 + quitar zumbido 3 kHz -3 Q 6 (44.1 kHz)";
        m.bands = { lcut (120, 1), bell (1200, 4, 4), bell (3000, -3, 6) }; runSessionMove (s, s.stems["303"], m);
        m = {}; m.track = "303"; m.name = "TAPE drive 15 en la campana del squelch (el 303 'quemado')";
        { Band b = bell (1200, 3, 2); b.sat = 1; b.drive = 15; m.bands = { b }; }
        runSessionMove (s, s.stems["303"], m);
    }

    // ---------------- MINIMAL (mucho silencio y headroom) ----------------
    if (wantE ("minimal microhouse"))
    {
        auto s = makeMinimal (44100, secs);
        m = {}; m.track = "MIX (-14 dBFS, con silencios)"; m.name = "de-click dinamico 4 kHz con THRESHOLD AUTO + low cut 30 Hz + 60 Hz +2";
        { Band d = dynb (bell (4000, 0, 2), -40, -6, 1, 80); d.autoThr = true; m.bands = { d, lcut (30, 1), bell (60, 2, 1) }; }
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            // en los huecos no aparece ruido ni "respiracion" y el primer golpe despues del silencio no se deforma
            double worst = -300; size_t zeros = 0;
            for (size_t i = (size_t) fs; i < in.size(); ++i)
            {
                zeros = std::abs (in.l[i]) < 1e-12 ? zeros + 1 : 0;
                if (zeros > (size_t) (0.25 * fs)) worst = std::max (worst, dB (std::abs (out.l[i])));   // 250 ms despues del ultimo sonido
            }
            const double d = lufs (out, fs, (size_t) fs) - lufs (in, fs, (size_t) fs);
            msg = "en los silencios (250 ms despues del ultimo sonido) la salida queda en " + juce::String (worst, 0) + " dBFS; volumen " + juce::String (d, 1) + " LU";
            return worst < -100 && std::abs (d) < 3.0;
        };
        runSessionMove (s, s.mix, m);
    }

    // ---------------- MELODIC TECHNO (anchos, M/S) ----------------
    if (wantE ("melodic techno"))
    {
        auto s = makeMelodicTechno (48000, secs);
        m = {}; m.track = "PADS"; m.name = "solo los lados: Side shelf 5 kHz +4 y Side low cut 250 Hz (el centro no se toca)";
        m.bands = { hshelf (5000, 4, 0.7, 4), lcut (250, 1, 0.707, 4) }; m.domain = 'S';
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const auto S = welch (domainOf (in, 'M'), domainOf (out, 'M'), fs, (size_t) (0.4 * fs));
            double w = 0; for (size_t k = 10; k < S.sxx.size(); ++k) if (S.sxx[k] > 1e-12 * *std::max_element (S.sxx.begin(), S.sxx.end())) w = std::max (w, std::abs (dB (std::abs (S.sxy[k]) / S.sxx[k])));
            msg = "el centro (Mid) queda intacto: desvio max " + juce::String (w, 3) + " dB";
            return w < 0.05;
        };
        runSessionMove (s, s.stems["PADS"], m);
        m = {}; m.track = "ARP"; m.name = "L y R distinto (L 2 kHz +3, R 4 kHz +3): compatibilidad mono";
        m.bands = { bell (2000, 3, 1.5, 1), bell (4000, 3, 1.5, 2) };
        runSessionMove (s, s.stems["ARP"], m);
        m = {}; m.track = "MASTER"; m.vintage = true; m.name = "Vintage: low 30 boost 2 atten 1 + high 12k 2 bw 7 + AIR 40k 3 + IRON 15";
        m.vin.kv = { { ParamID::vLowFreq, 1 }, { ParamID::vLowBoost, 2 }, { ParamID::vLowAtten, 1 }, { ParamID::vHighFreq, 5 }, { ParamID::vHighBoost, 2 }, { ParamID::vHighBw, 7 }, { ParamID::vAirFreq, 4 }, { ParamID::vAirGain, 3 }, { ParamID::vIron, 15 } };
        runSessionMove (s, s.mix, m);
    }

    // ---------------- TRANCE UPLIFTING (138) ----------------
    if (wantE ("trance uplifting"))
    {
        auto s = makeTrance (44100, secs);
        m = {}; m.track = "SUPERSAW"; m.name = "low cut 200 Hz 24 dB + 400 Hz -3 + brillo shelf 8 kHz +3 + high cut 18 kHz 12 dB (44.1 kHz)";
        m.bands = { lcut (200, 1), bell (400, -3, 1), hshelf (8000, 3, 0.7), hcut (18000, 0) }; runSessionMove (s, s.stems["SUPERSAW"], m);
        m = {}; m.track = "SUPERSAW"; m.name = "SC general con el kick (100 %): el realce de 3 kHz +6 se aparta en cada golpe y vuelve";
        m.bands = { bell (3000, 6, 1.0) }; m.scGeneral = true; m.sidechain = &s.stems["KICK"]; m.scThr = dB (s.stems["KICK"].peak()) - 8; m.scDuck = 100;
        m.check = [&] (const St& in, const St& out, double fs, juce::String& msg)
        {
            double on, off; hitVsGap (in, out, fs, 138, 2200, 4000, 0.02, 0.08, 0.75, 0.98, on, off);
            msg = "realce de 3 kHz justo despues del kick " + juce::String (on, 1) + " dB (debe ~0), antes del siguiente " + juce::String (off, 1) + " dB (debe volver a ~+6 con la curva)";
            return on < 2.5 && off > 3.5;
        };
        runSessionMove (s, s.stems["SUPERSAW"], m);
        m = {}; m.track = "MASTER"; m.name = "master trance 44.1 kHz: low cut 25 Hz 36 dB, 60 Hz +1, 3 kHz -0.7 Q 1, aire 16 kHz campana +2 Q 0.5";
        m.bands = { lcut (25, 2), bell (60, 1, 1), bell (3000, -0.7, 1), bell (16000, 2, 0.5) }; m.domain = 'M'; runSessionMove (s, s.mix, m);
    }

    // ---------------- PSYTRANCE (145, bajo rodante) ----------------
    if (wantE ("psytrance psy"))
    {
        auto s = makePsy (48000, secs);
        m = {}; m.track = "BAJO RODANTE"; m.name = "graves del bajo se apartan del kick a 145 BPM (dinamica 70 Hz -10, ataque 0.3 ms, release 25 ms)";
        const double kpk = dB (s.stems["KICK"].peak());
        m.bands = { dynb (bell (70, 0, 1.0), kpk - 12, -10, 0.3, 25, true) }; m.sidechain = &s.stems["KICK"];
        m.check = [&] (const St& in, const St& out, double fs, juce::String& msg)
        {
            double on, off; hitVsGap (in, out, fs, 145, 40, 110, 0.25, 0.30, 0.40, 0.98, on, off);
            msg = "primera nota del bajo (todavia suena la cola del kick) " + juce::String (on, 1) + " dB; resto de las notas " + juce::String (off, 1) + " dB (release rapido: pasan enteras)";
            return on < -1.0 && off > -1.5;
        };
        runSessionMove (s, s.stems["BAJO"], m);
        m = {}; m.track = "KICK"; m.name = "kick psy seco: low cut 35 Hz 48 dB + 50 Hz +2 Q 2 + 150 Hz -4 Q 2 + 6 kHz +2";
        m.bands = { lcut (35, 3), bell (50, 2, 2), bell (150, -4, 2), bell (6000, 2, 1) }; runSessionMove (s, s.stems["KICK"], m);
    }

    // ---------------- BIG ROOM (master a +6 dBFS) ----------------
    if (wantE ("big room"))
    {
        auto s = makeBigRoom (44100, secs);
        m = {}; m.track = "MASTER (+6 dBFS)"; m.name = "master caliente: low cut 30 Hz 24 dB + 50 Hz +2 + 2.5 kHz -1.5 + shelf 10 kHz +2 (HQ 2x)";
        m.bands = { lcut (30, 1), bell (50, 2, 1.2), bell (2500, -1.5, 1), hshelf (10000, 2, 0.7) }; m.quality = 1; runSessionMove (s, s.mix, m);
        m = {}; m.track = "MASTER (+6 dBFS)"; m.vintage = true; m.name = "Vintage con entrada a +6 dBFS: truco 60 Hz + high 10k + GLOW 40 + IRON 30";
        m.vin.kv = { { ParamID::vLowFreq, 2 }, { ParamID::vLowBoost, 4 }, { ParamID::vLowAtten, 2 }, { ParamID::vHighFreq, 4 }, { ParamID::vHighBoost, 3 }, { ParamID::vTube, 40 }, { ParamID::vIron, 30 } };
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            double dc = 0; for (size_t i = (size_t) fs; i < out.size(); ++i) dc += out.l[i]; dc /= (double) (out.size() - (size_t) fs);
            const double d = lufs (out, fs, (size_t) fs) - lufs (in, fs, (size_t) fs);
            double dcIn = 0; for (size_t i = (size_t) fs; i < in.size(); ++i) dcIn += in.l[i]; dcIn /= (double) (in.size() - (size_t) fs);
            msg = "DC entrada " + juce::String (dcIn, 4) + " | color del Vintage con la senal pasada de 0 dBFS: volumen " + juce::String (d, 1) + " LU, pico " + juce::String (dB (out.peak()), 1) + " dBFS, DC " + juce::String (dc, 4);
            return d > -2.0 && d < 5.0 && dB (out.peak()) < 14.0 && std::abs (dc) < 0.005;
        };
        runSessionMove (s, s.mix, m);
        m = {}; m.track = "LEAD"; m.name = "supersaw de drop: low cut 300 Hz + presencia 4 kHz +3 + aire shelf 12 kHz +2";
        m.bands = { lcut (300, 1), bell (4000, 3, 1), hshelf (12000, 2, 0.7) }; runSessionMove (s, s.stems["LEAD"], m);
    }

    // ---------------- HARDSTYLE (kick saturado + reverse bass) ----------------
    if (wantE ("hardstyle"))
    {
        auto s = makeHardstyle (44100, secs);
        m = {}; m.track = "KICK"; m.name = "low cut 40 Hz 48 dB + 'punch' 120 Hz +3 + 300 Hz -4 Q 2 + HARD drive 30 en 2 kHz (44.1 kHz, Normal)";
        { Band h = bell (2000, 0, 1.2); h.sat = 3; h.drive = 30; m.bands = { lcut (40, 3), bell (120, 3, 1.5), bell (300, -4, 2), h }; }
        runSessionMove (s, s.stems["KICK"], m);
        m = {}; m.track = "SCREECH"; m.name = "FUZZ drive 36 en 2.5 kHz con HQ 4x + notch del pito 5 kHz Q 10";
        { Band f = bell (2500, 0, 1.0); f.sat = 4; f.drive = 36; m.bands = { f, notch (5000, 10) }; }
        m.quality = 2;
        runSessionMove (s, s.stems["SCREECH"], m);
        m = {}; m.track = "MASTER (+3 dBFS)"; m.name = "master hardstyle: low cut 30 Hz 48 dB, 3.2 kHz -2 Q 2 dinamico, shelf 9 kHz +1";
        m.bands = { lcut (30, 3), dynb (bell (3200, 0, 2), -20, -4, 2, 80), hshelf (9000, 1, 0.7) }; runSessionMove (s, s.mix, m);
    }

    // ---------------- FUTURE BASS ----------------
    if (wantE ("future bass"))
    {
        auto s = makeFutureBass (48000, secs);
        m = {}; m.track = "CHORDS"; m.name = "acordes brillantes: low cut 150 Hz + 600 Hz -3 + shelf 10 kHz +6 Q 1.5 (resonante) + Side shelf 4k +3";
        m.bands = { lcut (150, 1), bell (600, -3, 1), hshelf (10000, 6, 1.5), hshelf (4000, 3, 0.7, 4) }; m.domain = 'S'; runSessionMove (s, s.stems["CHORDS"], m);
        m = {}; m.track = "VOCAL CHOPS"; m.name = "low cut 200 Hz 24 dB + 'formante' 1.8 kHz +4 Q 3 + aire 14 kHz +3 + de-esser auto";
        { Band d = dynb (bell (7000, 0, 2), -40, -6, 1, 60); d.autoThr = true; m.bands = { lcut (200, 1), bell (1800, 4, 3), hshelf (14000, 3, 0.7), d }; }
        runSessionMove (s, s.stems["VOCAL CHOPS"], m);
    }

    // ---------------- DEEP / PROGRESSIVE HOUSE ----------------
    if (wantE ("deep progressive house"))
    {
        auto s = makeDeepHouse (44100, secs);
        m = {}; m.track = "KEYS"; m.vintage = true; m.name = "Vintage en teclados: low cut 100, mid-low 300 +2, dip 1k 2, high 5k 2 bw 5, GLOW 25";
        m.vin.kv = { { ParamID::vLowCut, 3 }, { ParamID::vLmFreq, 1 }, { ParamID::vLmBoost, 2 }, { ParamID::vDipFreq, 4 }, { ParamID::vDip, 2 }, { ParamID::vHighFreq, 2 }, { ParamID::vHighBoost, 2 }, { ParamID::vHighBw, 5 }, { ParamID::vTube, 25 } };
        runSessionMove (s, s.stems["KEYS"], m);
        m = {}; m.track = "BAJO"; m.name = "bajo deep: low cut 30 Hz + 80 Hz +2 + 250 Hz -3 Q 1.5 + high cut 2 kHz 24 dB";
        m.bands = { lcut (30, 1), bell (80, 2, 1), bell (250, -3, 1.5), hcut (2000, 1) }; runSessionMove (s, s.stems["BAJO"], m);
    }

    // ---------------- UK GARAGE / 2-STEP ----------------
    if (wantE ("uk garage 2-step ukg"))
    {
        auto s = makeUkg (44100, secs);
        m = {}; m.track = "SUB"; m.name = "sub de garage: low cut 25 Hz 48 dB + 45 Hz +2 + TUBE drive 10 en shelf 100 Hz (para que se oiga en parlantes chicos)";
        { Band t = lshelf (100, 1, 0.7); t.sat = 0; t.drive = 10; m.bands = { lcut (25, 3), bell (45, 2, 1.2), t }; }
        m.check = [] (const St& in, const St& out, double fs, juce::String& msg)
        {
            const double h = bandChangeDb (in, out, fs, 120, 400);
            msg = "armonicos del sub entre 120 y 400 Hz: " + juce::String (h, 1) + " dB (debe subir para oirse en el celular)";
            return h > 2.0;
        };
        runSessionMove (s, s.stems["SUB"], m);
        m = {}; m.track = "MASTER"; m.name = "master garage: low cut 20 Hz, Side low cut 120 Hz, 4 kHz +1, shelf 11 kHz +1.5";
        m.bands = { lcut (20, 1), lcut (120, 1, 0.707, 4), bell (4000, 1, 0.9), hshelf (11000, 1.5, 0.7) }; m.domain = 'M'; runSessionMove (s, s.mix, m);
    }

    // ---------------- ESTRES EXTREMO PROPIO DE LA ELECTRONICA ----------------
    if (all || gFilter.containsIgnoreCase ("estres") || gFilter.containsIgnoreCase ("acid") || gFilter.containsIgnoreCase ("glitch") || gFilter.containsIgnoreCase ("riser"))
    {
        std::cout << "-- estres --" << std::endl;
        using namespace ParamID;
        // 1) Filtro "303" con LFO rapido (8 Hz = 1/16 a 120 BPM), como LFO Tool o el LFO de Ableton sobre la frecuencia:
        //    a) la banda sigue el LFO casi completo (no se queda a mitad de camino)
        //    b) la automatizacion por bloques grandes (512) no agrega "zipper" respecto de bloques chicos (16)
        //    c) con resonancia alta y senal real (acid) nunca se dispara
        {
            const double fs = 44100;
            // a) seguimiento: ruido blanco + campana +15 dB Q 8 que el LFO mueve entre 300 Hz y 3 kHz
            for (double lfoHz : { 2.0, 8.0 })
            {
                St nz ((size_t) (fs * 3.0)); Rng rr (9); for (size_t i = 0; i < nz.size(); ++i) nz.l[i] = nz.r[i] = 0.1 * rr.uni();
                const St y = renderLive (fs, nz, 256, [&] (GauxEqProcessor& p) { applyBand (p, 0, bell (1000, 15, 8)); },
                                         [&] (GauxEqProcessor& p, double t) { setP (p, band (0, "freq"), (float) (1000.0 * std::pow (10.0, 0.5 + 0.5 * std::sin (2 * kPi * lfoHz * t)))); });
                const size_t W = 1024, H = 128; double lo = 1e9, hi = 0;
                for (size_t st0 = (size_t) (0.5 * fs); st0 + W < y.size(); st0 += H)
                {
                    std::vector<Cx> X (W), Y (W);
                    for (size_t i = 0; i < W; ++i) { const double w = 0.5 - 0.5 * std::cos (2 * kPi * i / W); X[i] = nz.l[st0 + i] * w; Y[i] = y.l[st0 + i] * w; }
                    fft (X); fft (Y);
                    double best = 0, bf = 0;
                    for (size_t k = 4; k < W / 2 - 2; ++k)
                    {
                        double ry = 0, rx = 0; for (size_t j = k - 2; j <= k + 2; ++j) { ry += std::norm (Y[j]); rx += std::norm (X[j]); }
                        const double f = k * fs / W; if (f < 600 || f > 15000) continue;
                        if (ry / rx > best) { best = ry / rx; bf = f; }
                    }
                    lo = std::min (lo, bf); hi = std::max (hi, bf);
                }
                const double depth = std::log10 (hi / lo);   // 1.0 = recorre la decada entera
                report ("LFO de " + juce::String (lfoHz, 0) + " Hz sobre la frecuencia (1 kHz - 10 kHz): la banda recorre " + juce::String (lo, 0) + " - " + juce::String (hi, 0) + " Hz ("
                        + juce::String (depth * 100.0, 0) + " % del barrido)", depth > (lfoHz > 4 ? 0.8 : 0.9));
            }
            // b) zipper: tonos puros + LFO de 8 Hz; ruido lejos de los tonos con bloques de 512 vs 16
            for (int type : { 0, 4 })
            {
                St x ((size_t) (fs * 3.5));
                for (size_t i = 0; i < x.size(); ++i) { const double t = i / fs; x.l[i] = x.r[i] = 0.3 * std::sin (2 * kPi * 200 * t) + 0.3 * std::sin (2 * kPi * 1000 * t) + 0.2 * std::sin (2 * kPi * 5000 * t); }
                Band bd = type == 0 ? bell (300, 12, 2) : hcut (300, 0, 6.0);
                auto setup = [&] (GauxEqProcessor& p) { applyBand (p, 0, bd); };
                auto autom = [&] (GauxEqProcessor& p, double t) { setP (p, band (0, "freq"), (float) (300.0 * std::pow (10.0, 0.5 + 0.5 * std::sin (2 * kPi * 8.0 * t)))); };
                auto noiseDb = [&] (const St& y)
                {
                    const size_t N = 65536; std::vector<Cx> Y (N);
                    for (size_t i = 0; i < N; ++i) Y[i] = y.l[(size_t) (0.5 * fs) + i] * (0.5 - 0.5 * std::cos (2 * kPi * i / N));
                    fft (Y); double e = 0, t = 0;
                    for (size_t k = 1; k < N / 2; ++k)
                    {
                        const double f = k * fs / N; t += std::norm (Y[k]); bool nearTone = false;
                        for (double ft : { 200.0, 1000.0, 5000.0 }) for (int h = 1; h <= 4; ++h) if (std::abs (f - ft * h) < 300) nearTone = true;
                        if (f > 1400 && f < 4500 && ! nearTone) e += std::norm (Y[k]);
                    }
                    return 10 * std::log10 (e / t);
                };
                const double n16 = noiseDb (renderLive (fs, x, 16, setup, autom)), n512 = noiseDb (renderLive (fs, x, 512, setup, autom));
                report (juce::String ("Sin zipper con LFO de 8 Hz (") + (type == 0 ? "campana +12 Q 2" : "high cut Q 6") + "): ruido de modulacion con bloques de 512 " + juce::String (n512, 1)
                        + " dB vs 16 muestras " + juce::String (n16, 1) + " dB (exceso " + juce::String (n512 - n16, 1) + " dB)", n512 - n16 < 3.0);
            }
            // c) acid real con LFO 8 Hz y resonancias altas: nunca se dispara
            auto s = makeAcid (44100, 6.0);
            for (int type : { 4, 3, 0 })
            {
                Band b = type == 0 ? bell (1000, 15, 12) : type == 4 ? hcut (1000, 0, 10.0) : lcut (1000, 0, 10.0);
                const St a = renderLive (fs, s.mix, 256, [&] (GauxEqProcessor& p) { applyBand (p, 0, b); },
                                         [&] (GauxEqProcessor& p, double t) { setP (p, band (0, "freq"), (float) (300.0 * std::pow (20.0, 0.5 + 0.5 * std::sin (2 * kPi * 8.0 * t)))); });
                double pk = 0; bool fin = true; for (size_t i = 0; i < a.size(); ++i) { pk = std::max (pk, std::abs (a.l[i])); fin = fin && std::isfinite (a.l[i]); }
                const double maxBoost = type == 0 ? 15.0 : dB (10.0);
                report (juce::String ("Acid con LFO de 8 Hz en ") + (type == 4 ? "high cut Q 10" : type == 3 ? "low cut Q 10" : "campana +15 dB Q 12") + ": finito " + (fin ? "si" : "NO")
                        + ", pico " + juce::String (dB (pk), 1) + " dBFS (entrada " + juce::String (dB (s.mix.peak()), 1) + ", maximo posible de la curva +" + juce::String (maxBoost, 0) + " dB)",
                        fin && dB (pk) < dB (s.mix.peak()) + maxBoost + 1.0);
            }
        }
        // 2) Riser de trance: high cut 24 dB de 200 Hz a 20 kHz en 6 s + low cut que sube al final, todo automatizado
        {
            auto s = makeTrance (44100, 6.0);
            const double fs = 44100;
            auto setup = [&] (GauxEqProcessor& p) { applyBand (p, 0, hcut (200, 1, 1.2)); applyBand (p, 1, lcut (20, 1)); };
            auto autom = [&] (GauxEqProcessor& p, double t) { const double x = t / 6.0; setP (p, band (0, "freq"), (float) (200.0 * std::pow (100.0, x))); setP (p, band (1, "freq"), (float) (20.0 * std::pow (50.0, std::max (0.0, x - 0.6) / 0.4))); };
            const St a = renderLive (fs, s.mix, 512, setup, autom), c = renderLive (fs, s.mix, 32, setup, autom);
            double err = 0, ref = 0; for (size_t i = (size_t) (0.1 * fs); i < a.size(); ++i) { const double d = a.l[i] - c.l[i]; err += d * d; ref += c.l[i] * c.l[i]; }
            const double diff = 10 * std::log10 (err / std::max (1e-30, ref));
            report ("Build-up de trance (high cut 200 Hz -> 20 kHz + low cut 20 Hz -> 1 kHz): automatizacion a 512 vs 32 muestras " + juce::String (diff, 1) + " dB", diff < -30.0);
        }
        // 3) IDM / glitch: cada 1/32 (cada ~58 ms) cambia tipo, pendiente, canal, saturacion, modo y bypass. Al terminar el
        //    caos, el plugin tiene que sonar exactamente como uno recien abierto con los mismos ajustes (sin memoria rara)
        {
            auto s = makeTechno (48000, 6.0, 130, "IDM", false);
            const double fs = 48000; std::mt19937 g (5);
            auto chaos = [&] (GauxEqProcessor& p, double t)
            {
                if (t > 4.0) return;   // los ultimos 2 s, quieto
                if (std::fmod (t, 0.058) > 0.0107 * 0.5) return;
                const int b = (int) (g() % 6);
                setP (p, band (b, "on"), 1); setP (p, band (b, "type"), (float) (g() % 6)); setP (p, band (b, "slope"), (float) (g() % 4));
                setP (p, band (b, "stereo"), (float) (g() % 5)); setP (p, band (b, "satType"), (float) (g() % 5)); setP (p, band (b, "drive"), (float) (g() % 30));
                setP (p, band (b, "freq"), (float) (40.0 * std::pow (400.0, (g() % 1000) / 1000.0))); setP (p, band (b, "gain"), (float) ((int) (g() % 37) - 18));
                setP (p, band (b, "q"), (float) (0.3 + (g() % 100) / 10.0)); setP (p, band (b, "dyn"), (float) (g() % 2));
                setP (p, mode, (float) (g() % 4 == 0)); setP (p, bypass, (float) (g() % 9 == 0)); setP (p, vLowBoost, (float) (g() % 10)); setP (p, vTube, (float) (g() % 60));
            };
            GauxEqProcessor* lastP = nullptr;
            std::vector<std::pair<juce::String, float>> finalState;
            const St a = renderLive (fs, s.mix, 128, [] (GauxEqProcessor&) {}, [&] (GauxEqProcessor& p, double t)
            {
                chaos (p, t); lastP = &p;
                if (t > 4.0 && finalState.empty())
                    for (auto* prm : p.getParameters()) if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm)) finalState.push_back ({ rp->getParameterID(), rp->convertFrom0to1 (rp->getValue()) });
            });
            bool fin = true; double pk = 0; for (size_t i = 0; i < a.size(); ++i) { fin = fin && std::isfinite (a.l[i]) && std::isfinite (a.r[i]); pk = std::max (pk, std::abs (a.l[i])); }
            const St b = renderLive (fs, s.mix, 128, [&] (GauxEqProcessor& p) { for (auto& [id, v] : finalState) setP (p, id, v); }, nullptr);
            double err = 0, ref = 0; for (size_t i = (size_t) (5.0 * fs); i < a.size(); ++i) { const double d = a.l[i] - b.l[i]; err += d * d; ref += b.l[i] * b.l[i]; }
            const double diff = 10 * std::log10 (err / std::max (1e-30, ref));
            report ("IDM/glitch: 70 cambios bruscos de tipo, pendiente, canal, saturacion, modo y bypass: finito " + juce::String (fin ? "si" : "NO") + ", pico " + juce::String (dB (pk), 1)
                    + " dBFS; 1 s despues suena igual que una instancia nueva (diferencia " + juce::String (diff, 1) + " dB)", fin && dB (pk) < 24.0 && diff < -60.0);
        }
        // 4) DJ en vivo con buffer minimo (16 muestras a 44.1 kHz) y HQ 4x: mismo sonido que con 512
        {
            auto s = makeTrance (44100, 3.0);
            Move mm; mm.quality = 2; mm.bands = { lcut (30, 2), bell (3000, 3, 1), hshelf (10000, 2, 0.7) };
            const auto r1 = runMove (mm, s.mix, 44100, 16), r2 = runMove (mm, s.mix, 44100, 512);
            double d = 0; for (size_t i = 0; i < r1.out.size(); ++i) d = std::max (d, std::abs (r1.out.l[i] - r2.out.l[i]));
            report ("Buffer de 16 muestras (tocando en vivo) vs 512 en HQ 4x: diferencia " + juce::String (dB (d), 1) + " dB, CPU " + juce::String (r1.cpuPct, 2) + " % vs " + juce::String (r2.cpuPct, 2) + " %",
                    dB (d) < -80.0 && r1.cpuPct < 25.0);
        }
    }
}


//==============================================================================
// BENCH: costo por instancia (para usar el plugin en todos los canales)
static void benchTests()
{
    if (gFilter.startsWith ("prof"))
    {
        // perfilado: un solo caso, 4 s de audio
        auto s2 = makeTechHouse (48000, 4.0);
        Move m;
        if (gFilter == "prof4") m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) };
        if (gFilter == "profv") { m.vintage = true; m.vin.kv = { { ParamID::vLowBoost, 3 }, { ParamID::vHighBoost, 2 } }; }
        GauxEqProcessor::forceUiForTests = false;
        runMove (m, s2.mix, 48000, 256);
        return;
    }
    if (gFilter != "bench" && gFilter != "cpu") return;
    GauxEqProcessor::forceUiForTests = false;   // como en un DAW con la ventana del plugin cerrada
    std::cout << "\n== COSTO POR INSTANCIA (48 kHz, buffer 256, ventana cerrada) ==" << std::endl;
    auto s = makeTechHouse (48000, 6.0);
    St silence (s.mix.size());
    struct Case { const char* name; Move m; const St* in; };
    std::vector<Case> cases;
    { Move m; cases.push_back ({ "insertado sin bandas (pasando audio)", m, &s.mix }); }
    { Move m; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) }; cases.push_back ({ "4 bandas tipicas", m, &s.mix }); }
    { Move m; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) }; cases.push_back ({ "4 bandas tipicas, pista en silencio", m, &silence }); }
    { Move m; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) }; m.scGeneral = true; m.sidechain = &s.stems["KICK"]; m.scThr = -20; cases.push_back ({ "4 bandas + sidechain general", m, &s.mix }); }
    { Move m; m.bands = { lcut (30, 1), bell (120, 2, 1), bell (300, -3, 1.5), bell (800, -1, 2), bell (3000, 2, 1), bell (6000, -2, 3), hshelf (10000, 2), hcut (18000, 1) }; cases.push_back ({ "8 bandas", m, &s.mix }); }
    { Move m; m.bands = { lcut (30, 1), dynb (bell (300, 0, 1.5), -30, -4), bell (3000, 2, 1), dynb (bell (7000, 0, 2), -30, -6, 1, 60) }; cases.push_back ({ "4 bandas, 2 dinamicas", m, &s.mix }); }
    { Move m; Band t = bell (100, 2, 1); t.drive = 8; m.bands = { lcut (30, 1), t, bell (3000, 2, 1), hshelf (10000, 2) }; cases.push_back ({ "4 bandas, 1 saturando", m, &s.mix }); }
    { Move m; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) }; m.autoGain = true; cases.push_back ({ "4 bandas con Auto Gain", m, &s.mix }); }
    { Move m; m.vintage = true; m.vin.kv = { { ParamID::vLowBoost, 3 }, { ParamID::vHighBoost, 2 } }; cases.push_back ({ "Vintage", m, &s.mix }); }
    { Move m; m.vintage = true; m.vin.kv = { { ParamID::vLowBoost, 3 }, { ParamID::vHighBoost, 2 }, { ParamID::vTube, 30 }, { ParamID::vIron, 30 } }; cases.push_back ({ "Vintage con GLOW + IRON", m, &s.mix }); }
    { Move m; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) }; m.quality = 2; cases.push_back ({ "4 bandas en HQ 4x", m, &s.mix }); }
    { Move m; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) }; m.phase = 3; cases.push_back ({ "4 bandas en fase NATURAL", m, &s.mix }); }
    { Move m; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2), bell (500, -2, 1), bell (900, 1, 1), bell (1500, -1, 2), bell (6000, 1, 1),
                          bell (60, 1, 1), bell (200, 1, 1), bell (4500, -1, 3), bell (8000, 1, 1), hcut (19000, 1), bell (2200, 1, 1), bell (700, -1, 1), bell (12000, 1, 1),
                          bell (90, 1, 2), bell (400, 1, 1), bell (1200, -1, 1), bell (2600, 1, 1), bell (3700, 1, 1), bell (5200, -1, 1), bell (7300, 1, 1), bell (15000, 1, 1) };
      cases.push_back ({ "24 bandas", m, &s.mix }); }
    double total = 0;
    for (auto& c : cases)
    {
        double best = 1e9;
        for (int rep = 0; rep < 5; ++rep) best = std::min (best, runMove (c.m, *c.in, 48000, 256).cpuPct);
        std::cout << "  " << juce::String (c.name).paddedRight (' ', 40) << juce::String (best, 3) << " % de un nucleo -> " << (int) (100.0 / std::max (0.001, best)) << " instancias por nucleo" << std::endl;
        total += best;
    }
    std::cout << "  SUMA " << juce::String (total, 3) << std::endl;
}



//==============================================================================
// ESTRES MAXIMO v0.21: todos los generos con las funciones nuevas al limite, tormentas de parametros, NaN,
// sesiones viejas, hilos en paralelo y una sesion larga
static Band spectral (Band b) { b.spec = true; return b; }

static void stressTests()
{
    if (gFilter == "dbgbrick")
    {
        const std::vector<std::pair<const char*, std::vector<Band>>> cs {
            { "brick 25", { lcut (25, 8) } }, { "brick25+lc32 6dB", { lcut (25, 8), lcut (32, 4) } }, { "brick25+lc20 12dB", { lcut (25, 8), lcut (20, 0) } },
            { "3 cortes", { lcut (25, 8), lcut (32, 4), lcut (20, 0) } }, { "3 cortes + tilt + allpass", { lcut (25, 8), lcut (32, 4), lcut (20, 0), tilt (900, 1.5), apass (120, 0.8) } },
            { "lc20 12dB", { lcut (20, 0) } }, { "lc32 6dB", { lcut (32, 4) } } };
        for (auto& [n, b] : cs) for (int ph : { 0, 3 })
        {
            Move m; m.bands = b; m.phase = ph;
            St dummy ((size_t) 48000);
            const auto r = runMove (m, dummy, 48000);
            const auto c = audioVsDisplay (m, r, 48000, 'L', 19000);
            std::printf ("%-28s fase %d: desvio %.3f dB en %.1f Hz\n", n, ph, c.maxDev, c.fAt);
        }
        return;
    }
    if (gFilter.isNotEmpty() && ! juce::String ("estres stress maximo hilos").contains (gFilter)) return;
    std::cout << "\n== ESTRES MAXIMO (v0.21) ==" << std::endl;
    const bool onlyThreads = gFilter == "hilos";   // (para el detector de carreras entre hilos: solo la prueba 6)
    using namespace ParamID;
    const double secs = 6.0;
    struct G { const char* track; std::function<Session()> make; };
    const std::vector<G> genres {
        { "BASS",  [&] { return makeTechHouse (48000, secs); } }, { "BASS", [&] { return makeDubstep (44100, secs); } },
        { "VOX",   [&] { return makePop (44100, secs); } },       { "GTR", [&] { return makeRock (48000, secs); } },
        { "VOX",   [&] { return makeReggaeton (48000, secs); } }, { "PIANO", [&] { return makeJazz (96000, secs); } },
        { "STR",   [&] { return makeOrchestra (96000, secs); } }, { "808", [&] { return makeTrap (44100, secs); } },
        { "GTR",   [&] { return makeMetal (48000, secs); } },     { "GTR", [&] { return makeFolk (44100, secs); } },
        { "VOX",   [&] { return makePodcast (48000, secs); } },   { "PAD", [&] { return makeAmbient (48000, secs); } },
        { "PIANO", [&] { return makeSalsa (44100, secs); } },     { "BASS", [&] { return makeDnb (48000, secs); } },
        { "KICK",  [&] { return makeTechno (48000, secs, 130, "Techno Peak Time", false); } }, { "KICK", [&] { return makeTechno (48000, secs, 150, "Hard Techno", true); } },
        { "ACID",  [&] { return makeAcid (48000, secs); } },      { "KICK", [&] { return makeMinimal (48000, secs); } },
        { "LEAD",  [&] { return makeMelodicTechno (48000, secs); } }, { "LEAD", [&] { return makeTrance (48000, secs); } },
        { "BASS",  [&] { return makePsy (48000, secs); } },       { "LEAD", [&] { return makeBigRoom (48000, secs); } },
        { "KICK",  [&] { return makeHardstyle (48000, secs); } }, { "CHORDS", [&] { return makeFutureBass (44100, secs); } },
        { "BASS",  [&] { return makeDeepHouse (48000, secs); } }, { "BASS", [&] { return makeUkg (48000, secs); } },
    };
    int gi = 0;
    if (! onlyThreads) for (auto& g : genres)
    {
        const auto s = g.make();
        const St* stem = nullptr; juce::String name;
        for (auto& [n, st] : s.stems) if (n.containsIgnoreCase (g.track)) { stem = &st; name = n; break; }
        if (stem == nullptr) { stem = &s.stems.begin()->second; name = s.stems.begin()->first; }
        const int phaseSel[] = { 0, 3, 2 };   // (brick a 25 Hz en fase lineal: la resolucion de LIN HQ)
        // 1) Las 24 bandas con todos los tipos y pendientes nuevas (sin dinamica: audio = pantalla)
        {
            Move m; m.track = "MASTER"; m.phase = phaseSel[gi % 3]; m.quality = (gi % 4 == 1 && m.phase != 1) ? 1 : 0;
            m.name = juce::String ("24 bandas: brick 25 Hz, band pass, tilt, all pass, 6 a 96 dB/oct (fase ") + (m.phase == 0 ? "MIN" : m.phase == 3 ? "NAT" : "LIN HQ") + ")";
            m.bands = { lcut (25, 8), lcut (32, 4), hcut (19000, 7), hcut (17500, 5), tilt (900, 1.5), apass (120, 0.8), bell (60, 1.5, 1.2), lshelf (90, 1.0),
                        bell (180, -1.5, 2.0), bell (250, -1.0, 4.0), bell (400, -2.0, 1.5), notch (520, 12.0), bell (800, 1.0, 1.0), bell (1200, -1.0, 3.0), bell (2000, 1.5, 1.0),
                        bell (3150, -2.0, 5.0), bell (4500, 1.0, 1.0), bell (6300, -1.5, 3.0), hshelf (8000, 1.5), bell (10000, 1.0, 2.0), bell (12500, -1.0, 4.0),
                        hshelf (14000, 1.0, 0.5), bell (300, -1.0, 1.0), lcut (20, 0) };
            runSessionMove (s, s.mix, m);
        }
        // 2) En la pista: dinamica espectral (resonancias), dinamica normal, saturacion y un band pass de efecto
        {
            Move m; m.track = name; m.phase = gi % 2 == 0 ? 3 : 0;
            m.name = juce::String ("espectral AUTO 2-6 kHz + dinamica 200 Hz + TAPE + band pass FX (fase ") + (m.phase == 3 ? "NAT" : "MIN") + ")";
            Band sp = spectral (dynb (bell (3500, 0, 0.6), -30, -9, 3, 90)); sp.autoThr = true;
            m.bands = { lcut (30, 3), sp, dynb (bell (200, 0, 1.2), -28, -5, 8, 120), sat (bell (900, 2, 1.0), 1, 9, 70), bpass (1500, 0.7, 0) };
            m.bands.back().g = 0;
            const auto r = runMove (m, *stem, s.fs);
            bool finite = true; for (size_t i = 0; i < r.out.size(); ++i) finite = finite && std::isfinite (r.out.l[i]) && std::isfinite (r.out.r[i]);
            const double pin = dB (stem->peak()), pout = dB (r.out.peak());
            std::cout << "[" << s.genre << "] " << name << " - " << m.name << std::endl;
            report ("finito, pico " + juce::String (pin, 1) + " -> " + juce::String (pout, 1) + " dBFS, CPU " + juce::String (r.cpuPct, 2) + " %", finite && pout < pin + 12.0);
        }
        ++gi;
    }

    // 3) Tormenta de parametros: todo al azar (tipos, pendientes, espectral, fase, calidad, canal, bypass...)
    if (! onlyThreads)
    //    con bloques de 1 a 4096 muestras y de 44.1 a 192 kHz. Nunca NaN, nunca se dispara, y despues se recupera.
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        GauxEqProcessor p;
        p.prepareToPlay (fs, 4096);
        std::mt19937 rg ((unsigned) fs);
        auto params = p.getParameters();
        Rng rr (3);
        bool finite = true; double peak = 0; long long n = 0;
        juce::AudioBuffer<float> buf (2, 4096); juce::MidiBuffer midi;
        const long long total = (long long) (fs * 12.0);
        while (n < total)
        {
            const int bs = (int) (1 + rg() % 4096);
            for (int k = 0; k < 6; ++k)
            {
                auto* prm = params[(int) (rg() % (unsigned) params.size())];
                float v = (float) (rg() % 10001) / 10000.0f;
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                {
                    const auto id = rp->getParameterID();
                    if (id == bypass || id == output || id == gainScale) continue;   // (subir el volumen a proposito no es un error)
                    if (id.endsWith ("_gain")) v = rp->convertTo0to1 ((float) (rg() % 1201) / 100.0f - 6.0f);   // +-6 dB por banda
                }
                prm->setValueNotifyingHost (v);
            }
            for (int i = 0; i < bs; ++i)
            {
                const double t = (double) (n + i) / fs;
                const double kick = std::fmod (t, 0.5) < 0.12 ? 0.8 * std::sin (2 * kPi * (50 + 120 * std::exp (-std::fmod (t, 0.5) * 30)) * std::fmod (t, 0.5)) : 0.0;
                const double v = 0.15 * rr.uni() + kick;
                buf.setSample (0, i, (float) v); buf.setSample (1, i, (float) (0.9 * v));
            }
            juce::AudioBuffer<float> part (buf.getArrayOfWritePointers(), 2, bs);
            p.processBlock (part, midi);
            for (int i = 0; i < bs; ++i) { const float a = part.getSample (0, i), b = part.getSample (1, i); finite = finite && std::isfinite (a) && std::isfinite (b); peak = std::max (peak, (double) std::max (std::abs (a), std::abs (b))); }
            n += bs;
        }
        // despues de la tormenta: todo a cero (EQ plano) y la salida tiene que volver a ser la entrada
        for (auto* prm : params) prm->setValueNotifyingHost (prm->getDefaultValue());
        double err = 0, ref = 0;
        for (int blk = 0; blk < (int) (fs * 3.0 / 512); ++blk)
        {
            for (int i = 0; i < 512; ++i) { const float v = (float) (0.2 * std::sin (2 * kPi * 1000.0 * (blk * 512 + i) / fs)); buf.setSample (0, i, v); buf.setSample (1, i, v); }
            juce::AudioBuffer<float> part (buf.getArrayOfWritePointers(), 2, 512);
            p.processBlock (part, midi);
            if (blk > (int) (fs * 2.0 / 512)) for (int i = 0; i < 512; ++i) { err = std::max (err, (double) std::abs (std::abs (part.getSample (0, i)) - 0.0)); ref = 0.2; }
        }
        const double outDb = dB (err) - dB (ref);
        report ("Tormenta de parametros a " + juce::String (fs / 1000, 1) + " kHz (12 s, bloques de 1 a 4096): finito " + juce::String (finite ? "si" : "NO") + ", pico " + juce::String (dB (peak), 1)
                + " dBFS; al volver a plano el tono sale a " + juce::String (outDb, 2) + " dB de la entrada", finite && dB (peak) < 36.0 && std::abs (outDb) < 0.3);
    }

    // 4) Entrada con NaN / infinito (un plugin anterior roto en la cadena): el Gaux no se queda "muerto" para siempre
    if (! onlyThreads)
    for (int ph : { 0, 3, 1 })
    {
        GauxEqProcessor p;
        setP (p, phase, (float) ph);
        applyBand (p, 0, bell (1000, 6, 1)); applyBand (p, 1, spectral (dynb (bell (3000, 0, 1), -30, -6)));
        p.prepareToPlay (48000, 512);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        bool after = true; double lvl = 0;
        for (int blk = 0; blk < 300; ++blk)
        {
            for (int i = 0; i < 512; ++i) { const float v = (float) (0.2 * std::sin (2 * kPi * 440.0 * (blk * 512 + i) / 48000.0)); buf.setSample (0, i, v); buf.setSample (1, i, v); }
            if (blk == 20) { buf.setSample (0, 100, std::numeric_limits<float>::quiet_NaN()); buf.setSample (1, 200, std::numeric_limits<float>::infinity()); }
            p.processBlock (buf, midi);
            if (blk > 200) for (int i = 0; i < 512; ++i) { after = after && std::isfinite (buf.getSample (0, i)) && std::isfinite (buf.getSample (1, i)); lvl = std::max (lvl, (double) std::abs (buf.getSample (0, i))); }
        }
        report (juce::String ("Entrada con NaN e infinito (fase ") + (ph == 0 ? "MIN" : ph == 3 ? "NAT" : "LIN") + "): 2 s despues la salida es normal (" + (after ? "finita" : "NaN") + ", nivel " + juce::String (dB (lvl), 1) + " dBFS)",
                after && lvl > 0.1 && lvl < 1.0);
    }

    // 5) Sesion guardada con la v0.20 (16 bandas, sin espectral): abre igual
    if (! onlyThreads)
    {
        GauxEqProcessor a;
        applyBand (a, 0, bell (300, -4, 1.5)); applyBand (a, 15, hshelf (9000, 3));
        juce::MemoryBlock mb; a.getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        // quitar lo que la v0.20 no tenia
        juce::Array<juce::XmlElement*> drop;
        for (auto* e : xml->getChildIterator())
        {
            const auto id = e->getStringAttribute ("id");
            const int bn = id.startsWith ("b") ? id.substring (1).upToFirstOccurrenceOf ("_", false, false).getIntValue() : 0;
            if (bn > 16 || id.endsWith ("_spec")) drop.add (e);
        }
        for (auto* e : drop) xml->removeChildElement (e, true);
        xml->removeAttribute ("hostNorm");
        juce::MemoryBlock old; juce::AudioProcessor::copyXmlToBinary (*xml, old);
        GauxEqProcessor b;
        b.setStateInformation (old.getData(), (int) old.getSize());
        bool same = true;
        for (int k = 0; k < kNumBands; ++k)
        {
            const auto x = a.params.band (k), y = b.params.band (k);
            same = same && x.on == y.on && (! x.on || (std::abs (x.freq - y.freq) < 0.01 && std::abs (x.gainDb - y.gainDb) < 0.01 && x.type == y.type));
        }
        same = same && ! b.params.band (20).on && b.params.bands[0].spec->load() < 0.5f;
        report ("Sesion de la v0.20 (16 bandas, sin espectral ni fase natural) abre identica en la v0.21: " + juce::String (same ? "si" : "NO"), same);
    }

    // 6) Hilos en paralelo: el audio suena mientras la interfaz guarda/carga el estado, pega curvas, carga presets
    //    y deshace (como un usuario apurado con el DAW guardando solo)
    {
        GauxEqProcessor p;
        p.prepareToPlay (48000, 256);
        std::atomic<bool> stop { false }, bad { false };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer midi; Rng rr (9);
            while (! stop.load())
            {
                for (int i = 0; i < 256; ++i) { const float v = (float) (0.2 * rr.uni()); buf.setSample (0, i, v); buf.setSample (1, i, v); }
                p.processBlock (buf, midi);
                for (int i = 0; i < 256; ++i) if (! std::isfinite (buf.getSample (0, i))) bad = true;
            }
        });
        GauxEqProcessor src; applyBand (src, 2, bell (700, -3, 2)); applyBand (src, 5, spectral (dynb (bell (5000, 0, 1), -30, -6))); applyBand (src, 9, hcut (16000, 7));
        const auto curve = src.curveToText();
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        int ops = 0;
        std::mt19937 rg (5);
        while (juce::Time::getMillisecondCounterHiRes() - t0 < 4000.0)
        {
            switch (rg() % 6)
            {
                case 0: { juce::MemoryBlock mb; p.getStateInformation (mb); p.setStateInformation (mb.getData(), (int) mb.getSize()); break; }
                case 1: p.curveFromText (curve); break;
                case 2: p.presets.load ((int) (rg() % 20)); break;
                case 3: p.history.undo(); break;
                case 4: setP (p, phase, (float) (rg() % 4)); break;
                case 5: setP (p, quality, (float) (rg() % 3)); break;
            }
            ++ops;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (2);   // (temporizadores: arman los motores de fase lineal/natural)
        }
        stop = true; audio.join();
        const bool engines = p.dspResets.load() == 0;
        report ("Hilos en paralelo (audio + " + juce::String (ops) + " operaciones de la interfaz en 4 s: guardar/cargar, pegar curva, presets, deshacer, fase, calidad): sin cuelgues ni NaN",
                ! bad.load() && ops > 100 && engines);
    }

    // 8) Cambiar la frecuencia de muestreo en plena sesion (el DAW re-prepara el plugin) en cada modo de fase y con
    if (! onlyThreads)
    //    espectral: el retraso real del impulso es siempre la latencia informada, y el bypass queda alineado
    for (int ph : { 0, 3, 1 })
    {
        GauxEqProcessor p;
        setP (p, phase, (float) ph);
        applyBand (p, 0, bell (1000, 4, 1)); applyBand (p, 1, spectral (dynb (bell (5000, 0, 1), -30, -6)));
        bool ok = true; juce::String row;
        for (double fs : { 44100.0, 192000.0, 48000.0, 96000.0 })
        {
            for (int bs : { 512, 128 })
            {
                p.prepareToPlay (fs, bs);
                juce::AudioBuffer<float> buf (2, bs); juce::MidiBuffer midi;
                for (int k = 0; k < (int) (fs / bs); ++k) { buf.clear(); p.processBlock (buf, midi); }   // (motores listos)
                const int lat = p.getLatencySamples();
                int at = -1; float pk = 0;
                for (int k = 0; k < (lat + 4 * bs) / bs + 2; ++k)
                {
                    buf.clear(); if (k == 0) { buf.setSample (0, 5, 1.0f); buf.setSample (1, 5, 1.0f); }
                    p.processBlock (buf, midi);
                    for (int i = 0; i < bs; ++i) if (std::abs (buf.getSample (0, i)) > pk) { pk = std::abs (buf.getSample (0, i)); at = k * bs + i - 5; }
                }
                // (una campana de +4 dB ancha: el pico del impulso cae en la latencia +- 1 muestra)
                const bool good = std::abs (at - lat) <= 1;
                ok = ok && good;
                row << juce::String (fs / 1000, 1) << "k/" << bs << ":" << lat << (good ? "" : "(!" + juce::String (at) + ")") << " ";
            }
        }
        report (juce::String ("Re-preparar en vivo (44.1 -> 192 -> 48 -> 96 kHz, buffers 512 y 128), fase ") + (ph == 0 ? "MIN" : ph == 3 ? "NAT" : "LIN")
                + " + espectral: el impulso sale justo en la latencia informada [" + row.trim() + "]", ok);
    }

    // 9) Dinamica espectral a 44.1, 96 y 192 kHz: recorta la resonancia lo pedido y no toca el resto
    if (! onlyThreads)
    for (double fs : { 44100.0, 96000.0, 192000.0 })
    {
        St x ((size_t) (fs * 3.0)); Rng rr (4);
        for (size_t i = 0; i < x.size(); ++i) { const double v = 0.02 * rr.uni() + 0.3 * std::sin (2 * kPi * 3000.0 * i / fs); x.l[i] = v; x.r[i] = v; }
        Move m; m.bands = { spectral (dynb (bell (3000, 0, 1.0), -30, -9, 3, 80)) };
        const auto r = runMove (m, x, fs);
        const int lat = (int) r.latency;
        const size_t a0 = (size_t) (1.5 * fs) + (size_t) lat, a1 = (size_t) (2.9 * fs) + (size_t) lat;
        const double tone = toneDb (r.out.l, a0, std::min (a1, r.out.size()), 3000, fs) - toneDb (x.l, (size_t) (1.5 * fs), (size_t) (2.9 * fs), 3000, fs);
        const double side = toneDb (r.out.l, a0, std::min (a1, r.out.size()), 700, fs) - toneDb (x.l, (size_t) (1.5 * fs), (size_t) (2.9 * fs), 700, fs);
        report ("Espectral a " + juce::String (fs / 1000, 1) + " kHz: resonancia " + juce::String (tone, 1) + " dB (rango -9), lejos de ella " + juce::String (side, 2) + " dB, latencia "
                + juce::String (lat) + " muestras (" + juce::String (1000.0 * lat / fs, 1) + " ms)", tone < -7.0 && tone > -9.5 && std::abs (side) < 1.0);
    }

    // 10) El DAW manda bloques mas grandes que los anunciados (al exportar) en natural + espectral + HQ: igual que en tiempo real
    if (! onlyThreads)
    {
        auto s = makePop (48000, 4.0);
        auto render = [&] (int prepared, int sent)
        {
            GauxEqProcessor p;
            setP (p, phase, 3); setP (p, quality, 1);
            applyBand (p, 0, hshelf (8000, 3)); applyBand (p, 1, spectral (dynb (bell (2500, 0, 1), -30, -5)));
            p.prepareToPlay (48000, prepared);
            St out (s.mix.size()); juce::AudioBuffer<float> buf (2, sent); juce::MidiBuffer midi;
            for (size_t off = 0; off + (size_t) sent <= s.mix.size(); off += (size_t) sent)
            {
                for (int i = 0; i < sent; ++i) { buf.setSample (0, i, (float) s.mix.l[off + i]); buf.setSample (1, i, (float) s.mix.r[off + i]); }
                p.processBlock (buf, midi);
                for (int i = 0; i < sent; ++i) { out.l[off + i] = buf.getSample (0, i); out.r[off + i] = buf.getSample (1, i); }
            }
            return out;
        };
        const auto a = render (256, 256), b = render (256, 4096);
        double d = 0; for (size_t i = 48000; i < 3 * 48000; ++i) d = std::max (d, std::abs (a.l[i] - b.l[i]));
        report ("Exportar con bloques de 4096 (preparado para 256) en natural + espectral + HQ 2x: igual que en tiempo real (diferencia " + juce::String (dB (d), 0) + " dB)", dB (d) < -90.0);
    }

    // 7) Sesion larga: 3 minutos de master con fase natural + espectral + dinamica + saturacion: estable y sin derivas
    if (! onlyThreads)
    {
        auto s = makeTechHouse (48000, 30.0);
        GauxEqProcessor p;
        setP (p, phase, 3);
        applyBand (p, 0, lcut (28, 3)); applyBand (p, 1, spectral (dynb (bell (3000, 0, 0.8), -30, -4))); applyBand (p, 2, dynb (bell (120, 0, 1.0), -20, -3));
        applyBand (p, 3, sat (hshelf (9000, 2), 1, 6, 50));
        p.prepareToPlay (48000, 512);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        std::vector<double> lvl;
        double cpuFirst = 0, cpuLast = 0;
        for (int rep = 0; rep < 6; ++rep)
        {
            double acc = 0; const auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t off = 0; off + 512 <= s.mix.size(); off += 512)
            {
                for (int i = 0; i < 512; ++i) { buf.setSample (0, i, (float) s.mix.l[off + i]); buf.setSample (1, i, (float) s.mix.r[off + i]); }
                p.processBlock (buf, midi);
                for (int i = 0; i < 512; ++i) acc += (double) buf.getSample (0, i) * buf.getSample (0, i);
            }
            const double sec = std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
            lvl.push_back (10 * std::log10 (acc / s.mix.size() + 1e-30));
            if (rep == 0) cpuFirst = sec; cpuLast = sec;
        }
        double drift = 0; for (double v : lvl) drift = std::max (drift, std::abs (v - lvl[1]));
        report ("Sesion larga (3 min, fase natural + espectral + dinamica + saturacion): nivel estable (+-" + juce::String (drift, 3) + " dB entre pasadas), CPU sin crecer ("
                + juce::String (cpuFirst * 1000 / 30, 2) + " -> " + juce::String (cpuLast * 1000 / 30, 2) + " ms por segundo de audio)", drift < 0.05 && cpuLast < cpuFirst * 1.5 + 0.05);
    }
}

//==============================================================================
// FASE LINEAL
static void linearPhaseTests()
{
    if (gFilter == "dbglin")
    {
        for (int len : { 16384, 32768, 65536 })
        {
            gaux::LinearSpec sp; sp.fs = 48000; sp.length = len; sp.bands[0].on = true; sp.bands[0].type = 0; sp.bands[0].freq = 1000; sp.bands[0].gainDb = 6; sp.bands[0].q = 1;
            juce::AudioBuffer<float> a, b; bool nc = false; gaux::designLinearPhase (sp, a, b, nc);
            int pk = 0; for (int i = 0; i < len; ++i) if (std::abs (a.getSample (0, i)) > std::abs (a.getSample (0, pk))) pk = i;
            gaux::LinearPhaseEngine e; e.prepare (sp, 512);
            std::vector<float> l (512), r (512); int opk = -1; float best = 0;
            for (int blk = 0; blk < len / 512 + 4; ++blk)
            {
                std::fill (l.begin(), l.end(), 0.0f); std::fill (r.begin(), r.end(), 0.0f); if (blk == 0) l[0] = r[0] = 1.0f;
                e.process (l.data(), r.data(), 512);
                for (int i = 0; i < 512; ++i) if (std::abs (l[(size_t) i]) > best) { best = std::abs (l[(size_t) i]); opk = blk * 512 + i; }
            }
            std::cout << "len " << len << " pico del IR en " << pk << " (" << a.getSample (0, pk) << "), salida pico en " << opk << " (" << best << ")" << std::endl;
        }
        return;
    }
    if (gFilter.isNotEmpty() && ! juce::String ("lineal linear fase").contains (gFilter)) return;
    std::cout << "\n== FASE LINEAL ==" << std::endl;
    using namespace ParamID;
    // 1) Curvas: lo que suena = la curva analogica que dibuja la pantalla
    {
        auto pop = makePop (44100, 4.0);
        Move m; m.track = "MASTER"; m.name = "fase lineal: low cut 30 Hz 24 dB, 60 Hz +4 Q 2, 300 Hz -3, 3 kHz +2, shelf 10 kHz +2"; m.phase = 1;
        m.bands = { lcut (30, 1), bell (60, 4, 2), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2, 0.7) };
        runSessionMove (pop, pop.mix, m);
        m.phase = 2; m.name = "lo mismo en fase lineal HQ"; runSessionMove (pop, pop.mix, m);
        Move ms; ms.track = "MASTER"; ms.name = "fase lineal M/S: Side shelf 8 kHz +3, Mid 300 Hz -2, Side low cut 120 Hz"; ms.phase = 1; ms.domain = 'S';
        ms.bands = { hshelf (8000, 3, 0.7, 4), bell (300, -2, 1, 3), lcut (120, 1, 0.707, 4) };
        runSessionMove (pop, pop.mix, ms);
        ms.domain = 'M'; ms.name = "fase lineal M/S (lado Mid)"; runSessionMove (pop, pop.mix, ms);
    }
    // 2) Fase realmente lineal: respuesta al impulso simetrica, retraso = latencia informada; bypass alineado
    for (int res : { 1, 2 })
    {
        const double fs = 48000;
        GauxEqProcessor p;
        setP (p, phase, (float) res);
        applyBand (p, 0, lcut (40, 2)); applyBand (p, 1, bell (200, 5, 2)); applyBand (p, 2, hshelf (6000, -4, 0.7));
        p.prepareToPlay (fs, 512);
        const int lat = p.getLatencySamples();
        const int total = lat * 2 + 48000;
        std::vector<double> h; juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer midi;
        for (int off = 0; off < total; off += 512)
        {
            b.clear(); if (off == 0) { b.setSample (0, 100, 0.5f); b.setSample (1, 100, 0.5f); }
            p.processBlock (b, midi);
            for (int i = 0; i < 512; ++i) h.push_back (b.getSample (0, i));
        }
        int pk = 0; for (size_t i = 0; i < h.size(); ++i) if (std::abs (h[i]) > std::abs (h[(size_t) pk])) pk = (int) i;
        double asym = 0, en = 0;
        for (int k = 1; k < lat && pk + k < (int) h.size() && pk - k >= 0; ++k) { asym += std::pow (h[(size_t) (pk + k)] - h[(size_t) (pk - k)], 2); en += h[(size_t) (pk + k)] * h[(size_t) (pk + k)] + h[(size_t) (pk - k)] * h[(size_t) (pk - k)]; }
        const double symDb = 10 * std::log10 (asym / std::max (1e-30, en));
        report (juce::String (res == 1 ? "Lineal" : "Lineal HQ") + ": latencia " + juce::String (lat) + " muestras (" + juce::String (1000.0 * lat / fs, 0) + " ms), el impulso sale en " + juce::String (pk - 100)
                + ", simetria de la respuesta (fase lineal) " + juce::String (symDb, 0) + " dB", pk - 100 == lat && symDb < -60);
        // bypass alineado con esa latencia
        setP (p, bypass, 1);
        for (int k = 0; k < 20; ++k) { b.clear(); p.processBlock (b, midi); }
        int imp = -1;
        for (int blk = 0; blk < (lat + 2048) / 512 + 2 && imp < 0; ++blk)
        {
            b.clear(); if (blk == 0) { b.setSample (0, 10, 1.0f); b.setSample (1, 10, 1.0f); }
            p.processBlock (b, midi);
            for (int i = 0; i < 512; ++i) if (imp < 0 && std::abs (b.getSample (0, i)) > 0.5f) imp = blk * 512 + i - 10;
        }
        report ("   bypass en fase lineal: la senal original sale con el mismo retraso (" + juce::String (imp) + " muestras)", imp == lat);
    }
    // 3) Pasar de fase minima a lineal en plena reproduccion: sin clic (el motor se arma en segundo plano)
    {
        const double fs = 48000;
        GauxEqProcessor p;
        applyBand (p, 0, bell (220, 6, 1)); applyBand (p, 1, hshelf (6000, 4));
        p.prepareToPlay (fs, 256);
        St tone ((size_t) (fs * 6));
        for (size_t i = 0; i < tone.size(); ++i) { const double t = i / fs; tone.l[i] = tone.r[i] = 0.35 * std::sin (2 * kPi * 220 * t) + 0.12 * std::sin (2 * kPi * 3100 * t); }
        std::vector<double> y; juce::AudioBuffer<float> b (2, 256); juce::MidiBuffer midi;
        size_t switchAt = 0; bool asked = false;
        for (size_t off = 0; off + 256 <= tone.size(); off += 256)
        {
            if (! asked && off >= (size_t) fs) { setP (p, phase, 1); asked = true; }
            if (asked && ! p.linearActiveNow.load() && (off / 256) % 4 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
            const bool before = p.linearActiveNow.load();
            for (int i = 0; i < 256; ++i) { b.setSample (0, i, (float) tone.l[off + (size_t) i]); b.setSample (1, i, (float) tone.r[off + (size_t) i]); }
            p.processBlock (b, midi);
            if (! before && p.linearActiveNow.load()) switchAt = off;
            for (int i = 0; i < 256; ++i) y.push_back (b.getSample (0, i));
        }
        double c = 99;
        if (switchAt > 0) c = clickDb (y, switchAt, fs);
        report ("Fase minima -> lineal en plena reproduccion: cambio " + juce::String (switchAt > 0 ? "hecho" : "NO HECHO") + ", transitorio " + juce::String (c, 1) + " dB (el DAW recompensa la latencia nueva)", switchAt > 0 && c < 6.0);
    }
    // 3b) Mover una banda en plena reproduccion en fase lineal: la salida pasa suave de la curva vieja a la
    //     nueva, sin huecos ni golpes (el filtro nuevo debe tener el historial del audio desde el primer instante)
    for (int res : { 1, 2 })
    {
        const double fs = 48000;
        Rng rr (77); St x ((size_t) (fs * 4.0));
        for (size_t i = 0; i < x.size(); ++i) { x.l[i] = 0.25 * rr.uni(); x.r[i] = 0.25 * rr.uni(); }
        auto render = [&] (float gainAfter)
        {
            return renderLive (fs, x, 256, [&] (GauxEqProcessor& p)
            {
                setP (p, ParamID::phase, (float) res);
                setP (p, ParamID::band (0, "on"), 1); setP (p, ParamID::band (0, "gain"), 0.0f); setP (p, ParamID::band (0, "freq"), 1000); setP (p, ParamID::band (0, "q"), 0.7f);
            }, [&] (GauxEqProcessor& p, double t) { if (t >= 1.5) setP (p, ParamID::band (0, "gain"), gainAfter); });
        };
        const auto y = render (-6.0f);
        // referencia: la curva nueva desde el principio. Pasado el cambio, la salida debe ser identica a ella
        const auto ref = renderLive (fs, x, 256, [&] (GauxEqProcessor& p)
        {
            setP (p, ParamID::phase, (float) res);
            setP (p, ParamID::band (0, "on"), 1); setP (p, ParamID::band (0, "gain"), -6.0f); setP (p, ParamID::band (0, "freq"), 1000); setP (p, ParamID::band (0, "q"), 0.7f);
        }, {});
        double md = 0, pk = 1e-9;
        for (size_t i = (size_t) (fs * 3.2); i < x.size(); ++i) { md = std::max (md, std::abs (y.l[i] - ref.l[i])); pk = std::max (pk, std::abs (ref.l[i])); }
        const double settle = 20.0 * std::log10 (md / pk + 1e-15);
        // nivel en ventanas de 10 ms: comparado con el nivel estable antes (curva plana) y despues (-6 dB en 1 kHz)
        auto rmsAt = [&] (size_t a, size_t n) { double e = 0; for (size_t i = a; i < a + n; ++i) e += y.l[i] * y.l[i]; return 10.0 * std::log10 (e / n + 1e-20); };
        const size_t w = (size_t) (fs * 0.01);
        const double before = rmsAt ((size_t) (fs * 1.0), (size_t) (fs * 0.4)), after = rmsAt ((size_t) (fs * 3.3), (size_t) (fs * 0.5));
        double lo = 1e9, hi = -1e9;
        for (size_t a = (size_t) (fs * 1.45); a + w < (size_t) (fs * 3.3); a += w) { const double r = rmsAt (a, w); lo = std::min (lo, r); hi = std::max (hi, r); }
        const double dip = std::min (before, after) - lo, bump = hi - std::max (before, after);
        report (juce::String (res == 1 ? "Lineal" : "Lineal HQ") + ": bajar una banda -6 dB en plena reproduccion: hueco " + juce::String (dip, 1) + " dB, golpe "
                + juce::String (bump, 1) + " dB; despues del cambio = curva nueva exacta (diferencia " + juce::String (settle, 0) + " dB)", dip < 1.5 && bump < 1.5 && settle < -90);
    }
    // 4) CPU
    {
        auto s = makeTechHouse (48000, 4.0);
        for (int res : { 1, 2 })
        {
            Move m; m.phase = res; m.bands = { lcut (30, 1), bell (300, -3, 1.5), bell (3000, 2, 1), hshelf (10000, 2) };
            const auto r = runMove (m, s.mix, 48000, 256);
            Move ms = m; ms.bands.push_back (hshelf (8000, 2, 0.7, 4));
            const auto r2 = runMove (ms, s.mix, 48000, 256);
            report (juce::String (res == 1 ? "Lineal" : "Lineal HQ") + " (48 kHz): " + juce::String (r.cpuPct, 2) + " % de un nucleo; con bandas M/S " + juce::String (r2.cpuPct, 2) + " %", r.cpuPct < 8.0 && r2.cpuPct < 15.0, true);
        }
    }
}

// Medidor de true peak y comparacion A/B
static void meterAbTests()
{
    if (gFilter.isNotEmpty() && ! juce::String ("medidor truepeak ab vivo").contains (gFilter)) return;
    std::cout << "\n== TRUE PEAK y A/B ==" << std::endl;
    juce::MidiBuffer midi;
    // 1) Senoide a fs/4 con fase de 45 grados: las muestras caen en 0.707 del pico real
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        GauxEqProcessor p; GauxEqProcessor::allowSleep = false;
        p.prepareToPlay (fs, 256);
        juce::AudioBuffer<float> b (2, 256); float sp = 0;
        for (int k = 0; k < 40; ++k)
        {
            for (int i = 0; i < 256; ++i)
            {
                const double t = k * 256 + i;
                const float v = (float) (0.5 * std::sin (juce::MathConstants<double>::halfPi * t + juce::MathConstants<double>::pi / 4));
                b.setSample (0, i, v); b.setSample (1, i, v); sp = std::max (sp, std::abs (v));
            }
            p.processBlock (b, midi);
        }
        GauxEqProcessor::allowSleep = true;
        const double tp = 20.0 * std::log10 (std::max (1e-9f, p.truePeak.load()));
        const double smp = 20.0 * std::log10 (sp);
        report ("True peak a " + juce::String (fs / 1000.0, 1) + " kHz: senoide fs/4 desfasada, muestras " + juce::String (smp, 2) + " dBFS, true peak " + juce::String (tp, 2) + " dBTP (real -6.02)",
                std::abs (tp + 6.02) < 0.3);
    }
    // 2) Senal de musica: el true peak nunca es menor que el pico de muestra y la diferencia es razonable
    {
        auto s = makeTechHouse (48000, 3.0); s.mix.scaleToPeakDb (-0.3);
        GauxEqProcessor p; p.prepareToPlay (48000, 512);
        juce::AudioBuffer<float> b (2, 512); float sp = 0;
        for (size_t k = 0; (k + 1) * 512 <= s.mix.size(); ++k)
        {
            for (int i = 0; i < 512; ++i) { b.setSample (0, i, (float) s.mix.l[k * 512 + (size_t) i]); b.setSample (1, i, (float) s.mix.r[k * 512 + (size_t) i]); }
            p.processBlock (b, midi);
            for (int i = 0; i < 512; ++i) sp = std::max ({ sp, std::abs (b.getSample (0, i)), std::abs (b.getSample (1, i)) });
        }
        const double tp = 20.0 * std::log10 (p.truePeak.load()), smp = 20.0 * std::log10 (sp);
        report ("True peak en tech house a -0.3 dBFS: muestra " + juce::String (smp, 2) + " / true peak " + juce::String (tp, 2) + " dBTP", tp >= smp - 0.05 && tp - smp < 3.0);
        p.resetTruePeak(); b.clear(); p.processBlock (b, midi);
        report ("True peak: reiniciar con un clic vuelve a cero", p.truePeak.load() < 1.0e-4f);
    }
    // 3) A/B: cambiar, copiar y recuperar con la sesion
    {
        using namespace ParamID;
        GauxEqProcessor p; p.prepareToPlay (48000, 256);
        setP (p, band (0, "on"), 1); setP (p, band (0, "gain"), 4);
        p.ab.switchTo (1);   // B empieza como copia de A
        const bool bCopy = std::abs (p.params.band (0).gainDb - 4.0) < 0.01;
        setP (p, band (0, "gain"), -3);
        p.ab.switchTo (0);
        const bool backA = std::abs (p.params.band (0).gainDb - 4.0) < 0.01;
        p.ab.switchTo (1);
        const bool backB = std::abs (p.params.band (0).gainDb + 3.0) < 0.01;
        juce::MemoryBlock mb; p.getStateInformation (mb);
        GauxEqProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
        const bool restored = q.ab.active() == 1 && std::abs (q.params.band (0).gainDb + 3.0) < 0.01;
        q.ab.switchTo (0);
        const bool restoredA = std::abs (q.params.band (0).gainDb - 4.0) < 0.01;
        q.ab.copyToOther(); q.ab.switchTo (1);
        const bool copied = std::abs (q.params.band (0).gainDb - 4.0) < 0.01;
        setP (p, bypass, 1); p.ab.switchTo (0);
        const bool bypassKept = p.apvts.getRawParameterValue (bypass)->load() > 0.5f;
        report (juce::String ("A/B: B nace como copia ") + (bCopy ? "si" : "NO") + ", A vuelve " + (backA ? "si" : "NO") + ", B vuelve " + (backB ? "si" : "NO")
                + ", se guarda con la sesion " + (restored && restoredA ? "si" : "NO") + ", copiar A->B " + (copied ? "si" : "NO") + ", el bypass no cambia " + (bypassKept ? "si" : "NO"),
                bCopy && backA && backB && restored && restoredA && copied && bypassKept);
    }
}

// ============================================================================
// HUELLA DE REGRESION: el audio exacto de cientos de ajustes (presets + aleatorios) se guarda con una
// version y se compara con la siguiente. Lo que ya funcionaba debe seguir sonando igual.
//   GauxMixLab golden-write <dir>     GauxMixLab golden-check <dir>
// ============================================================================
struct GoldenCfg { double fs; int block; std::vector<std::pair<juce::String, float>> kv; };

static std::vector<GoldenCfg> goldenConfigs()
{
    std::vector<GoldenCfg> out;
    auto capture = [] (GauxEqProcessor& p)
    {
        std::vector<std::pair<juce::String, float>> kv;
        for (auto* prm : p.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                kv.push_back ({ rp->getParameterID(), rp->convertFrom0to1 (rp->getValue()) });
        return kv;
    };
    // 1) Todos los presets de fabrica (con sus valores, no su indice: la lista puede crecer)
    for (int mode : { 0, 1 })
    {
        GauxEqProcessor lister; setP (lister, ParamID::mode, (float) mode);
        const auto list = lister.presets.entries();
        for (int idx = 0; idx < (int) list.size(); ++idx)
        {
            if (list[(size_t) idx].user) continue;
            GauxEqProcessor p; setP (p, ParamID::mode, (float) mode); p.presets.load (idx);
            out.push_back ({ 48000.0, 512, capture (p) });
        }
    }
    // 2) Ajustes aleatorios de todos los parametros (semilla fija)
    juce::Random r (20261016);
    const double rates[] = { 44100, 48000, 96000 };
    const int blocks[] = { 64, 256, 512, 1024 };
    for (int k = 0; k < 220; ++k)
    {
        GauxEqProcessor p;
        for (auto* prm : p.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            {
                const auto id = rp->getParameterID();
                if (id == ParamID::bypass || id == ParamID::analyzer) continue;
                float v = r.nextFloat();
                if (id == ParamID::phase) v = k % 25 == 0 ? 0.34f : 0.0f;    // pocas en fase lineal (lentas) (indice 1 de 4)
                if (id == ParamID::quality) v = r.nextFloat() * 0.75f;
                if (id.endsWith ("_on")) v = r.nextFloat() < 0.4f ? 1.0f : 0.0f;
                rp->setValueNotifyingHost (v);
            }
        out.push_back ({ rates[k % 3], blocks[(k / 3) % 4], capture (p) });
    }
    return out;
}

static St goldenInput (double fs)
{
    auto s = makeTechHouse (fs, 0.7);
    s.mix.scaleToPeakDb (-3.0);
    return s.mix;
}

static std::vector<float> goldenRender (const GoldenCfg& c, const St& in)
{
    GauxEqProcessor p;
    GauxEqProcessor::allowSleep = true;
    for (auto& [id, v] : c.kv)
        if (auto* rp = p.apvts.getParameter (id)) rp->setValueNotifyingHost (rp->convertTo0to1 (v));
    p.prepareToPlay (c.fs, c.block);
    juce::AudioBuffer<float> buf (2, c.block); juce::MidiBuffer midi;
    std::vector<float> y; y.reserve (in.size() * 2);
    for (size_t off = 0; off < in.size(); off += (size_t) c.block)
    {
        const int n = (int) std::min<size_t> ((size_t) c.block, in.size() - off);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i) { buf.setSample (0, i, (float) in.l[off + i]); buf.setSample (1, i, (float) in.r[off + i]); }
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i) { y.push_back (buf.getSample (0, i)); y.push_back (buf.getSample (1, i)); }
    }
    return y;
}

static int goldenMain (bool write, const juce::File& dir)
{
    dir.createDirectory();
    const auto cfgs = goldenConfigs();
    std::map<double, St> inputs;
    int fails = 0; double worst = -300; int worstIdx = -1;
    for (size_t k = 0; k < cfgs.size(); ++k)
    {
        const auto& c = cfgs[k];
        auto cfgFile = dir.getChildFile ("cfg_" + juce::String ((int) k) + ".txt");
        auto binFile = dir.getChildFile ("out_" + juce::String ((int) k) + ".bin");
        GoldenCfg use = c;
        if (! write)
        {
            // la configuracion guardada manda (asi los valores son exactamente los de la version anterior)
            juce::StringArray lines; lines.addLines (cfgFile.loadFileAsString());
            use.kv.clear();
            use.fs = lines[0].getDoubleValue(); use.block = lines[1].getIntValue();
            for (int i = 2; i < lines.size(); ++i)
                if (lines[i].contains ("=")) use.kv.push_back ({ lines[i].upToFirstOccurrenceOf ("=", false, false), lines[i].fromFirstOccurrenceOf ("=", false, false).getFloatValue() });
        }
        if (inputs.find (use.fs) == inputs.end()) inputs[use.fs] = goldenInput (use.fs);
        const auto y = goldenRender (use, inputs[use.fs]);
        if (write)
        {
            juce::String t; t << use.fs << "\n" << use.block << "\n";
            for (auto& [id, v] : use.kv) t << id << "=" << juce::String (v, 9) << "\n";
            cfgFile.replaceWithText (t);
            binFile.replaceWithData (y.data(), y.size() * sizeof (float));
        }
        else
        {
            juce::MemoryBlock mb; binFile.loadFileAsData (mb);
            const auto* ref = (const float*) mb.getData();
            const size_t n = mb.getSize() / sizeof (float);
            double maxDiff = 0, peak = 1e-9;
            bool sizeOk = n == y.size();
            for (size_t i = 0; i < std::min (n, y.size()); ++i) { maxDiff = std::max (maxDiff, (double) std::abs (y[i] - ref[i])); peak = std::max (peak, (double) std::abs (ref[i])); }
            const double rel = 20.0 * std::log10 (std::max (1e-15, maxDiff / peak));
            if (rel > worst) { worst = rel; worstIdx = (int) k; }
            if (! sizeOk || rel > -90.0) { ++fails; std::printf ("  CAMBIO en ajuste %d: diferencia %.1f dB respecto al pico%s\n", (int) k, rel, sizeOk ? "" : " (largo distinto)"); }
        }
    }
    if (write) std::printf ("Huella guardada: %d ajustes\n", (int) cfgs.size());
    else std::printf ("Huella de regresion: %d ajustes, %d cambiaron; mayor diferencia %.1f dB respecto al pico (ajuste %d)%s\n",
                      (int) cfgs.size(), fails, worst, worstIdx, fails == 0 ? "  -> TODO SUENA IGUAL" : "");
    return fails == 0 ? 0 : 1;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 1 && juce::String (argv[1]) == "fftbench")
    {
        for (int order : { 10, 11, 12 })
        {
            juce::dsp::FFT f (order); const int n = 1 << order;
            std::vector<float> d ((size_t) (2 * n)); Rng rr (1); for (auto& v : d) v = (float) rr.uni();
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            for (int k = 0; k < 2000; ++k) { f.performRealOnlyForwardTransform (d.data(), true); f.performRealOnlyInverseTransform (d.data()); }
            std::printf ("FFT JUCE %d puntos: ida+vuelta %.2f us\n", n, (juce::Time::getMillisecondCounterHiRes() - t0) * 1000.0 / 2000);
            gaux::RealFFT pf (order);
            const double t1 = juce::Time::getMillisecondCounterHiRes();
            for (int k = 0; k < 2000; ++k) { pf.performRealOnlyForwardTransform (d.data(), true); pf.performRealOnlyInverseTransform (d.data()); }
            std::printf ("FFT PFFFT %d puntos: ida+vuelta %.2f us\n", n, (juce::Time::getMillisecondCounterHiRes() - t1) * 1000.0 / 2000);
            std::vector<float> a ((size_t) (2 * n)), b ((size_t) (2 * n)); for (int i = 0; i < n; ++i) a[(size_t) i] = b[(size_t) i] = (float) rr.uni();
            f.performRealOnlyForwardTransform (a.data(), true); pf.performRealOnlyForwardTransform (b.data(), true);
            double md = 0, mx = 0; for (int i = 0; i < n + 2; ++i) { md = std::max (md, (double) std::abs (a[(size_t) i] - b[(size_t) i])); mx = std::max (mx, (double) std::abs (a[(size_t) i])); }
            std::vector<float> c ((size_t) (2 * n)); for (int i = 0; i < n; ++i) c[(size_t) i] = (float) std::sin (i * 0.37); auto c0 = c;
            pf.performRealOnlyForwardTransform (c.data(), true); pf.performRealOnlyInverseTransform (c.data());
            double rt = 0; for (int i = 0; i < n; ++i) rt = std::max (rt, (double) std::abs (c[(size_t) i] - c0[(size_t) i]));
            std::printf ("   espectro igual a JUCE: diferencia %.1f dB; ida y vuelta exacta: %.1f dB\n", 20 * std::log10 (md / mx + 1e-20), 20 * std::log10 (rt + 1e-20));
        }
        return 0;
    }
    if (argc > 3 && juce::String (argv[1]) == "golden-diag")
    {
        // renderizar un ajuste guardado dos veces y decir donde empiezan a diferir
        const juce::File dir (argv[2]); const int k = juce::String (argv[3]).getIntValue();
        juce::StringArray lines; lines.addLines (dir.getChildFile ("cfg_" + juce::String (k) + ".txt").loadFileAsString());
        GoldenCfg use; use.fs = lines[0].getDoubleValue(); use.block = lines[1].getIntValue();
        for (int i = 2; i < lines.size(); ++i) if (lines[i].contains ("=")) use.kv.push_back ({ lines[i].upToFirstOccurrenceOf ("=", false, false), lines[i].fromFirstOccurrenceOf ("=", false, false).getFloatValue() });
        for (auto& [id, v] : use.kv) if (id == "phase" || id == "quality" || id == "mode" || id == "scOn") std::printf ("%s=%g ", id.toRawUTF8(), v);
        std::printf ("fs=%g block=%d\n", use.fs, use.block);
        const auto in = goldenInput (use.fs);
        const auto a = goldenRender (use, in), b = goldenRender (use, in);
        size_t first = 0; double md = 0;
        for (size_t i = 0; i < a.size(); ++i) { const double d = std::abs (a[i] - b[i]); if (d > 1e-6 && first == 0) first = i; md = std::max (md, d); }
        std::printf ("max diff %g, primera diferencia en muestra %zu (%.3f s)\n", md, first / 2, first / 2 / use.fs);
        return 0;
    }
    if (argc > 2 && (juce::String (argv[1]) == "golden-write" || juce::String (argv[1]) == "golden-check"))
        return goldenMain (juce::String (argv[1]) == "golden-write", juce::File (argv[2]));
    if (argc > 1) gFilter = argv[1];
    GauxEqProcessor::forceUiForTests = true;   // las pruebas leen los medidores de la dinamica
    std::cout << "=== MIXLAB: Gaux Equalizer en sesiones reales ===" << std::endl;
    genreTests();
    electroTests();
    transparencyTests();
    liveTests();
    linearPhaseTests();
    newFilterTests();
    spectralTests();
    matchTests();
    sketchTests();
    naturalDesignTests();
    naturalProcessTests();
    meterAbTests();
    presetTests();
    cpuTests();
    benchTests();
    stressTests();
    std::cout << "\nRESUMEN: " << gChecks << " comprobaciones, " << gFails << " fallas, " << gWarn << " avisos" << std::endl;
    return gFails == 0 ? 0 : 1;
}
