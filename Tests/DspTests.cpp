#include <complex>
#include <vector>
// Pruebas del motor de audio de Gaux Equalizer (sin JUCE).
// Compilar: g++ -std=c++17 -O2 -I../Source Tests/DspTests.cpp -o dsptests
#include "DSP/EqBand.h"
#include "DSP/VintageEq.h"
#include "DSP/CurveFit.h"
#include "DSP/NaturalPhase.h"
#include <cstdio>
#include <vector>

using namespace gaux;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf ("FALLO: " __VA_ARGS__); std::printf ("\n"); } else { std::printf ("ok   : " __VA_ARGS__); std::printf ("\n"); } } while (0)

static double db (double lin) { return 20.0 * std::log10 (std::max (lin, 1e-12)); }

// Mide la ganancia real de la banda pasando un seno por el procesador
static double measureBand (const BandSettings& s, double freq, double fs, double amp = 0.1)
{
    EqBand band;
    band.prepare (fs);
    band.setTarget (s);
    const int n = (int) fs; // 1 segundo
    double inSq = 0, outSq = 0;
    for (int i = 0; i < n; ++i)
    {
        if (i % EqBand::kTickSize == 0) band.controlTick();
        const double x = amp * std::sin (2 * kPi * freq * i / fs);
        const double y = band.processSample (0, x);
        if (i > n / 2) { inSq += x * x; outSq += y * y; }
    }
    return db (std::sqrt (outSq / inSq));
}

int main()
{
    const double fs = 48000.0;

    BandSettings bell; bell.on = true; bell.type = (int) BandType::Bell; bell.freq = 1000; bell.gainDb = 6; bell.q = 1;
    CHECK (std::abs (measureBand (bell, 1000, fs) - 6.0) < 0.2, "Bell +6 dB a 1 kHz -> %.2f dB", measureBand (bell, 1000, fs));
    CHECK (std::abs (measureBand (bell, 100, fs)) < 0.3, "Bell no afecta 100 Hz -> %.2f dB", measureBand (bell, 100, fs));
    CHECK (std::abs (db (magnitudeOfBand (bell, 1000, fs)) - 6.0) < 0.01, "Curva dibujada coincide con el audio");

    BandSettings lc; lc.on = true; lc.type = (int) BandType::LowCut; lc.freq = 100; lc.q = 0.7071; lc.slope = 3;
    const double lc50 = measureBand (lc, 50, fs);
    CHECK (lc50 < -42.0 && lc50 > -52.0, "Low Cut 48 dB/oct a 100 Hz, una octava abajo -> %.2f dB (esperado ~-48)", lc50);
    CHECK (std::abs (measureBand (lc, 1000, fs)) < 0.2, "Low Cut deja pasar 1 kHz -> %.2f dB", measureBand (lc, 1000, fs));
    const double lc100 = db (magnitudeOfBand (lc, 100, fs));
    CHECK (std::abs (lc100 + 3.0) < 0.3, "Low Cut 48 dB/oct = -3 dB en la frecuencia de corte -> %.2f", lc100);

    BandSettings hs; hs.on = true; hs.type = (int) BandType::HighShelf; hs.freq = 5000; hs.gainDb = -8; hs.q = 0.707;
    CHECK (std::abs (measureBand (hs, 15000, fs) + 8.0) < 0.6, "High Shelf -8 dB -> %.2f dB a 15 kHz", measureBand (hs, 15000, fs));

    BandSettings off = bell; off.on = false;
    CHECK (std::abs (measureBand (off, 1000, fs)) < 0.001, "Banda apagada = transparente");

    // Saturacion: con drive 0 debe ser identica al EQ limpio
    BandSettings satOff = bell; satOff.driveDb = 0;
    CHECK (std::abs (measureBand (satOff, 1000, fs) - 6.0) < 0.2, "Drive 0 = EQ limpio");

    // Con drive alto debe generar armonicos (3er armonico de un seno a 200 Hz)
    for (int t = 0; t < kNumSatTypes; ++t)
    {
        BandSettings sat; sat.on = true; sat.type = (int) BandType::Bell; sat.freq = 200; sat.gainDb = 0; sat.q = 1;
        sat.driveDb = 24; sat.mix = 1; sat.satType = t;
        EqBand band; band.prepare (fs); band.setTarget (sat);
        const int n = (int) fs;
        std::vector<double> out (n);
        double peak = 0; bool finite = true;
        for (int i = 0; i < n; ++i)
        {
            if (i % EqBand::kTickSize == 0) band.controlTick();
            out[i] = band.processSample (0, 0.5 * std::sin (2 * kPi * 200 * i / fs));
            peak = std::max (peak, std::abs (out[i]));
            finite = finite && std::isfinite (out[i]);
        }
        // Correlacion con el 2do y 3er armonico
        double h2 = 0, h3 = 0, c2 = 0, c3 = 0;
        for (int i = n / 2; i < n; ++i)
        {
            h2 += out[i] * std::sin (2 * kPi * 400 * i / fs); c2 += out[i] * std::cos (2 * kPi * 400 * i / fs);
            h3 += out[i] * std::sin (2 * kPi * 600 * i / fs); c3 += out[i] * std::cos (2 * kPi * 600 * i / fs);
        }
        const double harm = std::sqrt (h2 * h2 + c2 * c2) + std::sqrt (h3 * h3 + c3 * c3);
        CHECK (finite && peak < 2.0 && harm > 10.0, "Saturacion %-9s genera armonicos (%.0f), pico %.2f, sin NaN",
               kSatTypeNames[t], harm, peak);
    }

    // Estabilidad: mover la frecuencia y la ganancia muy rapido con ruido
    {
        EqBand band; band.prepare (fs);
        BandSettings s = bell; s.driveDb = 18;
        bool finite = true; double peak = 0; unsigned seed = 1;
        for (int i = 0; i < (int) fs * 3; ++i)
        {
            if (i % 512 == 0)
            {
                s.freq = 20.0 * std::pow (1000.0, ((i / 512) % 37) / 36.0);
                s.gainDb = ((i / 512) % 2) ? 24 : -24;
                s.q = ((i / 512) % 3) ? 10 : 0.2;
                s.type = (i / 4096) % kNumBandTypes;
                band.setTarget (s);
            }
            if (i % EqBand::kTickSize == 0) band.controlTick();
            seed = seed * 1664525u + 1013904223u;
            const double x = ((seed >> 8) / 16777216.0 - 0.5) * 0.5;
            const double y = band.processSample (0, x);
            finite = finite && std::isfinite (y);
            peak = std::max (peak, std::abs (y));
        }
        CHECK (finite && peak < 50.0, "Automatizacion extrema estable (pico %.2f)", peak);
    }

    // EQ dinamica: solo actua cuando la senal supera el umbral
    {
        BandSettings d; d.on = true; d.type = (int) BandType::Bell; d.freq = 1000; d.gainDb = 0; d.q = 1;
        d.dynOn = true; d.thresholdDb = -30; d.rangeDb = -12; d.attackMs = 5; d.releaseMs = 50;
        const double loud = measureBand (d, 1000, fs, 0.5);
        const double quiet = measureBand (d, 1000, fs, 0.005);
        CHECK (loud < -10.5 && loud > -13.0, "Dinamica: senal fuerte comprimida -> %.2f dB (esperado ~-12)", loud);
        CHECK (std::abs (quiet) < 0.3, "Dinamica: senal suave intacta -> %.2f dB", quiet);
    }

    // Canales: Side no toca una senal mono; Left no toca el canal derecho
    {
        BandSettings sideB = bell; sideB.stereo = (int) StereoMode::Side;
        EqBand band; band.prepare (fs); band.setTarget (sideB);
        double maxDiff = 0;
        for (int i = 0; i < (int) fs / 2; ++i)
        {
            if (i % EqBand::kTickSize == 0) band.controlTick();
            const double x = 0.3 * std::sin (2 * kPi * 1000 * i / fs);
            double l = x, r = x;
            band.processFrame (l, r, true);
            maxDiff = std::max ({ maxDiff, std::abs (l - x), std::abs (r - x) });
        }
        CHECK (maxDiff < 1e-9, "Modo Side ignora senal mono (dif %.2e)", maxDiff);

        BandSettings leftB = bell; leftB.stereo = (int) StereoMode::Left;
        EqBand b2; b2.prepare (fs); b2.setTarget (leftB);
        double lSq = 0, rSq = 0, inSq = 0;
        for (int i = 0; i < (int) fs; ++i)
        {
            if (i % EqBand::kTickSize == 0) b2.controlTick();
            const double x = 0.1 * std::sin (2 * kPi * 1000 * i / fs);
            double l = x, r = x;
            b2.processFrame (l, r, true);
            if (i > fs / 2) { lSq += l * l; rSq += r * r; inSq += x * x; }
        }
        CHECK (std::abs (db (std::sqrt (lSq / inSq)) - 6.0) < 0.2 && std::abs (db (std::sqrt (rSq / inSq))) < 0.01,
               "Modo Left: izquierdo +%.2f dB, derecho %.2f dB", db (std::sqrt (lSq / inSq)), db (std::sqrt (rSq / inSq)));
    }

    // Vintage
    // ---- Vintage: comportamiento del equipo pasivo clasico (EQP) ----
    {
        auto at = [&] (const VintageSettings& vv, double f) { return db (magnitudeOfVintage (vv, f, fs)); };
        auto peakIn = [&] (const VintageSettings& vv, double lo, double hi, bool maxi, double& where)
        {
            double best = maxi ? -1e9 : 1e9;
            for (double f = lo; f <= hi; f *= 1.01) { const double g = at (vv, f); if (maxi ? g > best : g < best) { best = g; where = f; } }
            return best;
        };
        VintageSettings b10; b10.lowFreq = 2; b10.lowBoost = 10;
        double w = 0;
        const double bmax = peakIn (b10, 15, 400, true, w);
        CHECK (std::abs (bmax - 13.5) < 1.0 && at (b10, 60) > 10.0 && at (b10, 300) < 1.0,
               "Vintage LOW BOOST 10 en 60 Hz: maximo %+.1f dB (equipo: +13.5), en 60 Hz %+.1f dB, en 300 Hz %+.1f dB", bmax, at (b10, 60), at (b10, 300));
        VintageSettings a10; a10.lowFreq = 2; a10.lowAtten = 10;
        CHECK (at (a10, 20) < -15.0 && at (a10, 20) > -18.5, "Vintage LOW ATTEN 10: %.1f dB en 20 Hz (equipo: -17.5)", at (a10, 20));
        VintageSettings tr; tr.lowFreq = 2; tr.lowBoost = 5; tr.lowAtten = 3;
        double fb = 0, fd = 0;
        const double gb = peakIn (tr, 20, 150, true, fb);
        const double gd = peakIn (tr, fb, 800, false, fd);
        CHECK (fb > 40 && fb < 95 && gb > 2.5 && gb < 5.5 && fd > 120 && fd < 450 && gd < -0.8 && gd > -3.5,
               "Truco de graves (60 Hz, BOOST 5 + ATTEN 3): joroba %+.1f dB en %.0f Hz y hueco %+.1f dB en %.0f Hz", gb, fb, gd, fd);
        VintageSettings hs; hs.highFreq = 4; hs.highBoost = 10; hs.highBandwidth = 0;
        VintageSettings hb = hs; hb.highBandwidth = 10;
        CHECK (std::abs (at (hs, 10000) - 18.0) < 0.3 && at (hs, 5000) < at (hb, 5000) - 6.0,
               "HIGH BOOST 10 en 10 kHz: %+.1f dB; SHARP %+.1f vs BROAD %+.1f dB a 5 kHz", at (hs, 10000), at (hs, 5000), at (hb, 5000));
        // Sin aplastamiento cerca de Nyquist: HIGH 16 kHz BROAD a 48 kHz sigue alto en 20 kHz (como el analogico)
        VintageSettings h16; h16.highFreq = 6; h16.highBoost = 10; h16.highBandwidth = 10;
        CHECK (at (h16, 20000) > 12.0, "HIGH 16 kHz BROAD en 1x: %+.1f dB en 20 kHz (sin aplastamiento)", at (h16, 20000));
        VintageSettings sf; sf.attenFreq = 0; sf.highAtten = 10;
        CHECK (at (sf, 20000) < -14.0 && at (sf, 1000) > -1.0, "SOFTEN 5 kHz a 10: %.1f dB en 20 kHz, %.1f dB en 1 kHz", at (sf, 20000), at (sf, 1000));
        VintageSettings pin; pin.lowBoost = 8; pin.highBoost = 8; pin.programIn = false;
        CHECK (std::abs (at (pin, 60)) < 0.01 && std::abs (at (pin, 10000)) < 0.01, "PROGRAM con IN apagado: la curva queda plana");
    }

    // GLOW (valvula de salida): 2do armonico dominante y nivel estable; selectores sin clics
    {
        auto win = [] (std::vector<double>& o) { const size_t N = o.size(); for (size_t n = 0; n < N; ++n) o[n] *= 0.5 - 0.5 * std::cos (2 * kPi * (double) n / (double) (N - 1)); };
        auto gz = [&] (const std::vector<double>& x, double f) { const double w = 2 * kPi * f / fs; std::complex<double> a = 0;
                                                                  for (size_t n = 0; n < x.size(); ++n) a += x[n] * std::polar (1.0, -w * (double) n); return std::abs (a); };
        bool evenOk = true; double worstLvl = 0;
        for (double glow : { 25.0, 50.0, 100.0 })
        {
            VintageEq ve; ve.prepare (fs); VintageSettings g; g.tube = glow; ve.setTarget (g);
            std::vector<double> o;
            for (int i = 0; i < (int) fs; ++i) { if (i % 16 == 0) ve.controlTick(); const double y = ve.processSample (0, 0.25 * std::sin (2 * kPi * 100 * i / fs)); if (i >= 8000) o.push_back (y); }
            win (o);
            const double f1 = gz (o, 100), h2 = gz (o, 200), h3 = gz (o, 300);
            evenOk = evenOk && h2 > h3;
            worstLvl = std::min (worstLvl, db (f1 / (0.25 * (double) o.size() * 0.25)));
        }
        CHECK (evenOk && worstLvl > -1.5, "GLOW: 2do armonico dominante (calidez de valvula) y nivel estable (peor %.2f dB a -12 dBFS)", worstLvl);

        // Girar cualquier selector con la curva muy cargada: sin clics (energia > 6 kHz con un tono grave)
        auto clickDb = [&] (bool changeSelectors)
        {
            VintageEq ve; ve.prepare (fs);
            VintageSettings v; v.lowBoost = 10; v.highBoost = 10; v.lmBoost = 8; v.dip = 8; v.hmBoost = 8; v.lowCut = 3;
            ve.setTarget (v);
            // detector de clics: pasa altos de 8 kHz y 48 dB/oct (el tono grave no se cuela)
            const auto hpC = makeHighPass (fs, 8000, 0.707);
            BiquadState h1, h2, h3, h4; double worst = 0;
            for (int i = 0; i < (int) fs * 3; ++i)
            {
                if (changeSelectors && i > 0 && i % (int) (fs * 0.2) == 0)
                {
                    const int k = i / (int) (fs * 0.2);
                    v.lowFreq = k % 4; v.highFreq = k % 7; v.attenFreq = k % 3; v.lmFreq = k % 5; v.dipFreq = k % 11; v.hmFreq = k % 5;
                    v.lowCut = k % 11; v.highCut = 10 - k % 11; v.filterIn = (k % 3) != 0; v.airFreq = k % 5;
                    ve.setTarget (v);
                }
                if (i % 16 == 0) ve.controlTick();
                const double y = ve.processSample (0, 0.3 * std::sin (2 * kPi * 80 * i / fs));
                const double hp = h4.process (h3.process (h2.process (h1.process (y, hpC), hpC), hpC), hpC);
                if (i > fs * 0.2) worst = std::max (worst, std::abs (hp));
            }
            return db (worst);
        };
        const double base = clickDb (false), moving = clickDb (true);
        CHECK (moving < -70.0, "Selectores del Vintage con fundido: sin clics (energia de clic %.1f dBFS girando todo; quieto %.1f dBFS)", moving, base);
    }

    VintageSettings flat;
    double maxDev = 0;
    for (double f = 20; f < 20000; f *= 1.1) maxDev = std::max (maxDev, std::abs (db (magnitudeOfVintage (flat, f, fs))));
    CHECK (maxDev < 0.01, "Vintage en cero = plano (desviacion %.4f dB)", maxDev);

    VintageSettings air; air.airFreq = 2; air.airGain = 10;
    CHECK (db (magnitudeOfVintage (air, 18000, fs)) > 7.0 && db (magnitudeOfVintage (air, 1000, fs)) < 1.0,
           "Air 10k +10: abre agudos (%.1f dB a 18k), no toca medios (%.2f dB a 1k)",
           db (magnitudeOfVintage (air, 18000, fs)), db (magnitudeOfVintage (air, 1000, fs)));

    {
        VintageEq ve; ve.prepare (fs * 4); // con oversampling
        VintageSettings t; t.lowBoost = 7; t.highBoost = 6; t.airFreq = 4; t.airGain = 10; t.tube = 100;
        ve.setTarget (t);
        bool finite = true; double peak = 0;
        for (int i = 0; i < (int) fs * 4; ++i)
        {
            if (i % VintageEq::kTickSize == 0) ve.controlTick();
            const double y = ve.processSample (1, 0.8 * std::sin (2 * kPi * 80 * i / (fs * 4)));
            finite = finite && std::isfinite (y); peak = std::max (peak, std::abs (y));
        }
        CHECK (finite && peak < 4.0, "Vintage con tube al 100%% estable a 192 kHz (pico %.2f)", peak);
    }

    // Unidad MID
    {
        VintageSettings m; m.lmFreq = 1; m.lmBoost = 8; m.dipFreq = 6; m.dip = 8; m.hmFreq = 3; m.hmBoost = 6;
        const double a = db (magnitudeOfVintage (m, 300, fs)), b = db (magnitudeOfVintage (m, 2000, fs)), c = db (magnitudeOfVintage (m, 4000, fs));
        const double lo = db (magnitudeOfVintage (m, 30, fs)), hi = db (magnitudeOfVintage (m, 18000, fs));
        CHECK (a > 5.0 && b < -4.0 && c > 2.5 && std::abs (lo) < 1.0 && std::abs (hi) < 1.5,
               "MID: realce 300 Hz %+.1f dB, recorte 2k %+.1f dB, realce 4k %+.1f dB (extremos %+.1f / %+.1f dB)", a, b, c, lo, hi);
        m.midIn = false;
        VintageEq ve; ve.prepare (fs); ve.setTarget (m);
        for (int i = 0; i < 64; ++i) ve.controlTick();
        VintageSettings off = m; off.lmBoost = off.dip = off.hmBoost = 0;
        CHECK (std::abs (db (magnitudeOfVintage (off, 2000, fs))) < 0.01, "MID con IN apagado = plano");
    }
    // Unidad FILTER: 12 dB/oct
    {
        VintageSettings f; f.lowCut = 3; f.highCut = 7;   // 100 Hz y 10 kHz
        const double d1 = db (magnitudeOfVintage (f, 25, fs)), d2 = db (magnitudeOfVintage (f, 12.5, fs));
        const double mid = db (magnitudeOfVintage (f, 1000, fs)), h = db (magnitudeOfVintage (f, 18000, fs));
        CHECK (std::abs ((d1 - d2) - 12.0) < 1.5 && std::abs (mid) < 0.3 && h < -6.0,
               "FILTER 100 Hz / 10 kHz: pendiente %.1f dB/oct, 1k %+.2f dB, 18k %+.1f dB", d1 - d2, mid, h);
        f.filterIn = false;
        CHECK (std::abs (db (magnitudeOfVintage (f, 25, fs))) < 0.01, "FILTER con IN apagado = plano");
    }
    // IRON: satura mas los graves que los agudos
    {
        auto thd = [&] (double freq)
        {
            VintageEq ve; ve.prepare (fs);
            VintageSettings t; t.iron = 100; ve.setTarget (t);
            double err = 0, sig = 0;
            for (int i = 0; i < (int) fs; ++i)
            {
                if (i % VintageEq::kTickSize == 0) ve.controlTick();
                const double x = 0.5 * std::sin (2 * kPi * freq * i / fs);
                const double y = ve.processSample (0, x);
                if (i > fs / 2) { err += (y - x) * (y - x); sig += x * x; }
            }
            return 10 * std::log10 (err / sig);
        };
        const double l = thd (60), h = thd (5000);
        CHECK (l > -40 && l < -12 && l > h + 10, "IRON: distorsion en 60 Hz %.1f dB vs 5 kHz %.1f dB", l, h);
    }

    // ---- Saturaciones de las bandas: cada modo con su caracter ----
    {
        auto goertzel = [] (const std::vector<double>& x, double f, double fsr)
        {
            const double w = 2 * kPi * f / fsr; std::complex<double> acc = 0;
            for (size_t n = 0; n < x.size(); ++n) acc += x[n] * std::polar (1.0, -w * (double) n);
            return 2.0 * std::abs (acc) / (double) x.size();
        };
        auto runSat = [&] (int type, double drive, double freq, double amp, BandType bt, double bandF)
        {
            EqBand b; b.prepare (fs);
            BandSettings st; st.on = true; st.type = (int) bt; st.freq = bandF; st.gainDb = 0; st.q = 1.0;
            st.satType = type; st.driveDb = drive; st.mix = 1.0;
            b.setTarget (st);
            std::vector<double> out;
            for (int i = 0; i < (int) fs / 2 + 8192; ++i)
            {
                if (i % EqBand::kTickSize == 0) b.controlTick();
                const double y = b.processSample (0, amp * std::sin (2 * kPi * freq * i / fs));
                if (i >= 8192) out.push_back (y);
            }
            return out;
        };
        struct H { double lvl, h2, h3, h5, h7; };
        auto harm = [&] (int type, double drive, double amp)
        {
            auto o = runSat (type, drive, 200, amp, BandType::Bell, 200);
            const double f1 = goertzel (o, 200, fs);
            return H { db (f1 / amp), db (goertzel (o, 400, fs) / f1), db (goertzel (o, 600, fs) / f1),
                       db (goertzel (o, 1000, fs) / f1), db (goertzel (o, 1400, fs) / f1) };
        };
        const int TUBE = 0, TAPE = 1, SOFT = 2, HARD = 3, FUZZ = 4;
        const auto tube12 = harm (TUBE, 12, 0.25), tube18 = harm (TUBE, 18, 0.25);
        CHECK (tube12.h2 > tube12.h3 && tube18.h2 > tube18.h3, "TUBE: domina el 2do armonico (drive 12: H2 %.1f / H3 %.1f dB; drive 18: %.1f / %.1f dB)",
               tube12.h2, tube12.h3, tube18.h2, tube18.h3);
        const auto tape6 = harm (TAPE, 6, 0.25), soft6 = harm (SOFT, 6, 0.25), hard6 = harm (HARD, 6, 0.25);
        CHECK (tape6.h2 < -90 && tape6.h3 > soft6.h3 + 6, "TAPE: satura antes y suave, sin pares (H3 %.1f dB vs SOFT %.1f dB)", tape6.h3, soft6.h3);
        CHECK (soft6.h3 < -35, "SOFT: limpio antes de la rodilla (H3 %.1f dB)", soft6.h3);
        CHECK (hard6.h3 < -90, "HARD: totalmente limpio bajo el umbral (H3 %.1f dB)", hard6.h3);
        const auto hard18 = harm (HARD, 18, 0.25), soft18 = harm (SOFT, 18, 0.25), tape18 = harm (TAPE, 18, 0.25);
        CHECK (hard18.h5 > soft18.h5 + 3 && hard18.h7 > tape18.h7 + 1, "HARD: la serie impar mas larga (H5 %.1f vs SOFT %.1f; H7 %.1f vs TAPE %.1f)",
               hard18.h5, soft18.h5, hard18.h7, tape18.h7);
        const auto fuzz18 = harm (FUZZ, 18, 0.25);
        CHECK (fuzz18.h2 > -25 && fuzz18.h3 > -25, "FUZZ: pares e impares fuertes (H2 %.1f, H3 %.1f dB)", fuzz18.h2, fuzz18.h3);
        double worstLvl = 0, worstQuiet = 0;
        for (int t = 0; t < 5; ++t)
        {
            worstLvl = std::min (worstLvl, harm (t, 18, 0.25).lvl);
            worstQuiet = std::max (worstQuiet, harm (t, 30, 0.0158).lvl);
        }
        CHECK (worstLvl > -3.5 && worstQuiet < 7.0, "Nivel compensado: drive 18 cambia como maximo %.1f dB; pasajes suaves a drive 30 suben %.1f dB como maximo", worstLvl, worstQuiet);

        double worstAlias = -200;
        for (int t = 0; t < 5; ++t)
        {
            auto o = runSat (t, 24, 7000, 0.25, BandType::HighShelf, 5000);
            const double f1 = goertzel (o, 7000, fs);
            worstAlias = std::max (worstAlias, db (goertzel (o, 1000, fs) / f1));
        }
        CHECK (worstAlias < -44, "Antialiasing (ADAA): alias en 1 kHz %.1f dB por debajo del tono, en 1x", -worstAlias);

        // Cambiar de modo y mover el drive en plena reproduccion: sin clics; extremos estables
        {
            EqBand b; b.prepare (fs);
            BandSettings st; st.on = true; st.type = (int) BandType::Bell; st.freq = 300; st.q = 0.8; st.driveDb = 20; st.mix = 1.0;
            double maxJump = 0, prev = 0, peak = 0; bool fin = true;
            for (int i = 0; i < (int) fs * 2; ++i)
            {
                if (i % 6000 == 0) { st.satType = (i / 6000) % 5; st.driveDb = (i / 6000) % 2 ? 36.0 : 8.0; b.setTarget (st); }
                if (i % EqBand::kTickSize == 0) b.controlTick();
                const double y = b.processSample (0, 0.9 * std::sin (2 * kPi * 300 * i / fs));
                fin = fin && std::isfinite (y);
                if (i > 2000) maxJump = std::max (maxJump, std::abs (y - prev));
                peak = std::max (peak, std::abs (y));
                prev = y;
            }
            const double natural = 0.9 * 2 * kPi * 300 / fs * 3.0;   // pendiente maxima esperable con armonicos
            CHECK (fin && maxJump < natural * 3 && peak < 3.0, "Cambio de modo/drive en vivo sin clics (salto max %.3f, pico %.2f)", maxJump, peak);
        }
    }

    // ---- Sin clics al cambiar en vivo: modo de saturacion, drive, mix, ganancia, Q ----
    {
        auto clickTest = [&] (int mode)
        {
            double worst = -200;
            for (double freq : { 80.0, 300.0, 1000.0 })
                for (double drive : { 0.0, 8.0, 36.0 })
                {
                    if (mode == 0 && drive == 0.0) continue;
                    EqBand b; b.prepare (fs);
                    BandSettings s; s.on = true; s.type = 0; s.freq = freq * 1.5; s.q = 0.7; s.gainDb = 3; s.driveDb = drive; s.mix = 1; s.satType = 0;
                    b.setTarget (s);
                    const int seg = (int) (fs * 0.3);
                    double p1 = 0, p2 = 0, steady = 1e-12, trans = 0;
                    for (int i = 0; i < seg * 11; ++i)
                    {
                        if (i > 0 && i % seg == 0)
                        {
                            const int k = i / seg;
                            if (mode == 0) s.satType = k % 5;
                            else if (mode == 1) s.driveDb = (k % 2) ? 36.0 : 0.0;
                            else if (mode == 2) s.mix = (k % 2) ? 1.0 : 0.0;
                            else if (mode == 3) s.gainDb = (k % 2) ? 12.0 : -12.0;
                            else s.q = (k % 2) ? 8.0 : 0.5;
                            b.setTarget (s);
                        }
                        if (i % 16 == 0) b.controlTick();
                        const double y = b.processSample (0, 0.4 * std::sin (2 * kPi * freq * i / fs));
                        const double d2 = std::abs (y - 2 * p1 + p2); p2 = p1; p1 = y;
                        if (i < seg) continue;
                        if (i % seg < (int) (fs * 0.05)) trans = std::max (trans, d2); else if (i % seg > (int) (fs * 0.1)) steady = std::max (steady, d2);
                    }
                    worst = std::max (worst, db (trans / steady));
                }
            return worst;
        };
        const double cType = clickTest (0), cDrive = clickTest (1), cMix = clickTest (2), cGain = clickTest (3), cQ = clickTest (4);
        CHECK (cType < 1.5 && cDrive < 3.0 && cMix < 1.5 && cGain < 4.0 && cQ < 3.0,
               "Sin clics en vivo (transitorio sobre lo normal): modo de saturacion %+.1f, drive 0->36 %+.1f, mix 0->100 %+.1f, ganancia -12->+12 %+.1f, Q %+.1f dB",
               cType, cDrive, cMix, cGain, cQ);
    }


    // Barrido de frecuencia: la curva cambia de forma continua (sin saltos de forma) en todos los tipos.
    // Hasta 0.33*fs se mira el salto con pasos de 0.2 %. Mas arriba (cerca de Nyquist todo filtro digital
    // se "comprime") se verifica que el salto se achica al achicar el paso (= curva continua, sin saltos).
    {
        auto sweep = [] (double fs, double fLo, double fHi, double step, double& worst, const char*& wt, double& wf)
        {
            for (int type = 0; type < kNumBandTypes; ++type)
                for (int slope = 0; slope < (type == 3 || type == 4 ? 4 : 1); ++slope)
                    for (double q : { 0.5, 0.707, 2.0, 8.0 })
                        for (double g : { -9.0, 9.0 })
                        {
                            BandSettings s; s.on = true; s.type = type; s.gainDb = g; s.q = q; s.slope = slope;
                            std::vector<double> prev;
                            for (double f0 = fLo; f0 < fHi; f0 *= step)
                            {
                                s.freq = f0; std::vector<double> cur;
                                for (double f = 20.0; f < std::min (20000.0, 0.49 * fs); f *= 1.06)
                                    cur.push_back (std::max (type == 5 ? -6.0 : -40.0, db (magnitudeOfBand (s, f, fs))));   // el fondo del notch es infinito: se mira la forma
                                if (! prev.empty())
                                    for (size_t k = 0; k < cur.size(); ++k)
                                        if (std::abs (cur[k] - prev[k]) > worst) { worst = std::abs (cur[k] - prev[k]); wt = kBandTypeNames[type]; wf = f0; }
                                prev = cur;
                            }
                        }
        };
        double worst = 0, wf = 0; const char* wt = "";
        double hiA = 0, hiB = 0, fa = 0, fb = 0; const char *ta = "", *tb = "";
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            sweep (fs, 20.0, std::min (21000.0, 0.33 * fs), 1.002, worst, wt, wf);
            if (0.33 * fs < 21000.0)
            {
                double a = 0, b = 0;
                sweep (fs, 0.33 * fs, std::min (21000.0, 0.49 * fs), 1.0008, a, ta, fa);
                sweep (fs, 0.33 * fs, std::min (21000.0, 0.49 * fs), 1.0002, b, tb, fb);
                hiA = std::max (hiA, a); hiB = std::max (hiB, b);
            }
        }
        CHECK (worst < 1.5, "Barrido continuo de frecuencia (pasos de 0.2 %%, todos los tipos, pendientes, Q y ganancias): salto max %.3f dB (%s en %.0f Hz)", worst, wt, wf);
        CHECK (hiB < 0.4 * hiA + 0.05, "Cerca de Nyquist la curva tambien es continua: salto %.3f dB con pasos de 0.08 %% -> %.3f dB con 0.02 %%", hiA, hiB);
    }


    // Vintage: girar cualquier perilla cambia la curva de forma continua (sin escalones)
    {
        double worst = 0.0;
        for (double fs : { 44100.0, 48000.0, 96000.0 })
            for (int which = 0; which < 6; ++which)
                for (int sel = 0; sel < 7; ++sel)
                {
                    std::vector<double> prev;
                    for (double k = 0.0; k <= 10.0001; k += 0.02)
                    {
                        VintageSettings v;
                        if (which == 0) { if (sel >= kNumVAttenFreqs) break; v.attenFreq = sel; v.highAtten = k; }
                        else if (which == 1) { if (sel >= kNumVAirFreqs) break; v.airFreq = sel; v.airGain = k; }
                        else if (which == 2) { v.highFreq = sel; v.highBoost = k; }
                        else if (which == 3) { if (sel >= kNumVLowFreqs) break; v.lowFreq = sel; v.lowBoost = k; }
                        else if (which == 4) { if (sel >= kNumVLowFreqs) break; v.lowFreq = sel; v.lowAtten = k; }
                        else { v.highFreq = sel; v.highBoost = 6; v.highBandwidth = k; }
                        std::vector<double> cur;
                        for (double f = 20.0; f < std::min (20000.0, 0.49 * fs); f *= 1.05) cur.push_back (db (magnitudeOfVintage (v, f, fs)));
                        if (! prev.empty()) for (size_t i = 0; i < cur.size(); ++i) worst = std::max (worst, std::abs (cur[i] - prev[i]));
                        prev = cur;
                    }
                }
        CHECK (worst < 0.1, "Vintage: perillas continuas (sin escalones al girar AIR, SOFTEN, BOOST, ATTEN, BANDWIDTH): salto max %.3f dB por 0.02 de perilla", worst);
    }

    // Shelf de agudos y campanas anchas iguales al analogico a 44.1/48 kHz (sin el "aplastamiento" clasico)
    {
        using Cx = std::complex<double>;
        double eH = 0.0, eB = 0.0;
        for (double fs : { 44100.0, 48000.0 })
            for (double q : { 0.3, 0.5, 0.707, 1.0 })
                for (double f0 = 1000.0; f0 <= 16000.0; f0 *= 1.08)
                    for (double g : { -12.0, -6.0, -3.0, 3.0, 6.0, 12.0 })
                    {
                        BandSettings hs; hs.on = true; hs.type = 2; hs.freq = f0; hs.q = q; hs.gainDb = g;
                        BandSettings bl = hs; bl.type = 0; bl.q = q * 0.6;   // campanas anchas (Q 0.18 - 0.6)
                        for (double f = 20.0; f < 20000.0; f *= 1.03)
                        {
                            eH = std::max (eH, std::abs (db (magnitudeOfBand (hs, f, fs)) - analogHighShelfDb (f, f0, q, g)));
                            eB = std::max (eB, std::abs (db (magnitudeOfBand (bl, f, fs)) - analogBellDb (f, f0, bl.q, g)));
                        }
                    }
        CHECK (eH < 1.0 && eB < 2.5, "Shelf de agudos y campanas anchas como el analogico a 44.1/48 kHz (hasta 16 kHz, +-12 dB): shelf %.2f dB, campana ancha %.2f dB", eH, eB);
    }

    // Fase natural: la respuesta compleja analogica tiene la misma curva que la que dibuja la pantalla
    {
        double worst = 0.0; int n = 0;
        for (int t = 0; t < kNumBandTypes; ++t)
            for (int sl = 0; sl < kNumSlopes; ++sl)
                for (double f0 : { 40.0, 700.0, 9000.0 })
                    for (double g : { -9.0, 6.0 })
                    {
                        if ((t == (int) BandType::LowCut || t == (int) BandType::HighCut) && sl == kSlopeBrick) continue;   // (BRICK lineal es otra cosa)
                        BandSettings b; b.on = true; b.type = t; b.slope = sl; b.freq = f0; b.gainDb = g; b.q = 1.3;
                        for (double f = 20.0; f < 20000.0; f *= 1.07)
                        {
                            const double a = analogMagnitudeOfBand (b, f), c = std::abs (analogResponseOfBand (b, f));
                            if (a > 1e-4) { worst = std::max (worst, std::abs (db (a) - db (c))); ++n; }
                        }
                    }
        CHECK (worst < 1e-6, "Fase natural: respuesta analogica compleja = curva analogica (todos los tipos y pendientes, %d puntos): error %.2g dB", n, worst);
    }

    // Ajustador de curvas (EQ MATCH / SKETCH): encuentra las bandas de una curva hecha con bandas conocidas
    {
        const auto f = fitGrid (20, 20000, 24);
        auto mk = [] (int t, double fr, double g, double q) { BandSettings b; b.on = true; b.type = t; b.freq = fr; b.gainDb = g; b.q = q; return b; };
        const std::vector<BandSettings> truth { mk (1, 100, 4, 0.707), mk (0, 500, -6, 2.0), mk (0, 3500, 3, 1.0), mk (2, 9000, -3, 0.707) };
        std::vector<double> tgt (f.size(), 0.0), w (f.size(), 1.0);
        for (size_t i = 0; i < f.size(); ++i) for (auto& b : truth) tgt[i] += db (analogMagnitudeOfBand (b, f[i]));
        FitOptions o; o.maxBands = 8; o.toleranceDb = 0.2;
        const auto r = fitCurve (f, tgt, w, o);
        double mx = 0.0;
        for (size_t i = 0; i < f.size(); ++i)
        {
            double m = 0; for (auto& b : r.bands) m += db (analogMagnitudeOfBand (b, f[i]));
            mx = std::max (mx, std::abs (m - tgt[i]));
        }
        // y con lo que ya suena descontado (fixedDb): si la curva ya esta, no agrega nada
        const auto r2 = fitCurve (f, tgt, w, o, &tgt);
        CHECK (mx < 0.3 && r.bands.size() <= 6 && r2.bands.empty(), "Ajustador: %d bandas, error %.3f dB; con la curva ya hecha agrega %d", (int) r.bands.size(), mx, (int) r2.bands.size());
    }

    std::printf ("\n%s (%d fallos)\n", failures == 0 ? "TODAS LAS PRUEBAS PASARON" : "HAY FALLOS", failures);
    return failures == 0 ? 0 : 1;
}
