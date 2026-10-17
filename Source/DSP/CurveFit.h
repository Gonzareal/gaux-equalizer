// Gaux Equalizer - Ajustador de curvas: convierte una curva objetivo (dB por frecuencia) en bandas reales
//
// Lo usan EQ MATCH (igualar el balance de una referencia) y SKETCH (dibujar la curva con el mouse).
//  1) Busca donde la diferencia entre el objetivo y lo que ya hay es mas grande y pone ahi una banda:
//     shelf si la diferencia esta en un extremo y sigue hacia afuera, campana (con el ancho medido) si no.
//  2) Ajusta TODAS las bandas a la vez (frecuencia, ganancia y Q) con Levenberg-Marquardt, minimizando el
//     error al cuadrado (con pesos: lo que no importa puede pesar poco).
//  3) Repite hasta que el error quede bajo la tolerancia o se acaben las bandas; quita las que no aportan.
// Trabaja con la curva analogica de cada banda (la misma que suena: el Gaux esta emparejado con el analogico).
#pragma once

#include "EqBand.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace gaux
{
struct FitOptions
{
    int maxBands = 8;
    double toleranceDb = 0.4;      // se deja de agregar bandas cuando el error maximo (con peso) baja de esto
    double maxGainDb = 18.0;
    double minQ = 0.3, maxQ = 10.0;
    bool allowShelves = true;
    int iterations = 40;           // pasos de Levenberg-Marquardt por cada banda agregada
};

struct FitResult
{
    std::vector<BandSettings> bands;
    double maxErrorDb = 0.0, rmsErrorDb = 0.0;
};

namespace fitdetail
{
    // Parametros por banda: log(f), ganancia, log(Q)
    struct P { int type; double lf, g, lq; };

    inline BandSettings toBand (const P& p)
    {
        BandSettings b; b.on = true; b.type = p.type;
        b.freq = std::exp (p.lf); b.gainDb = p.g; b.q = std::exp (p.lq);
        return b;
    }
    inline void curveOf (const P& p, const std::vector<double>& f, std::vector<double>& out)
    {
        const auto b = toBand (p);
        out.resize (f.size());
        for (size_t i = 0; i < f.size(); ++i) out[i] = 20.0 * std::log10 (std::max (1.0e-6, analogMagnitudeOfBand (b, f[i])));
    }
    // Resuelve A x = y (A simetrica definida positiva, n pequeno) por eliminacion gaussiana con pivote
    inline bool solve (std::vector<double> A, std::vector<double> y, int n, std::vector<double>& x)
    {
        for (int c = 0; c < n; ++c)
        {
            int piv = c;
            for (int r = c + 1; r < n; ++r) if (std::abs (A[(size_t) (r * n + c)]) > std::abs (A[(size_t) (piv * n + c)])) piv = r;
            if (std::abs (A[(size_t) (piv * n + c)]) < 1.0e-15) return false;
            if (piv != c) { for (int k = 0; k < n; ++k) std::swap (A[(size_t) (c * n + k)], A[(size_t) (piv * n + k)]); std::swap (y[(size_t) c], y[(size_t) piv]); }
            for (int r = c + 1; r < n; ++r)
            {
                const double m = A[(size_t) (r * n + c)] / A[(size_t) (c * n + c)];
                if (m == 0.0) continue;
                for (int k = c; k < n; ++k) A[(size_t) (r * n + k)] -= m * A[(size_t) (c * n + k)];
                y[(size_t) r] -= m * y[(size_t) c];
            }
        }
        x.assign ((size_t) n, 0.0);
        for (int r = n - 1; r >= 0; --r)
        {
            double s = y[(size_t) r];
            for (int k = r + 1; k < n; ++k) s -= A[(size_t) (r * n + k)] * x[(size_t) k];
            x[(size_t) r] = s / A[(size_t) (r * n + r)];
        }
        return true;
    }
}

// freqs: frecuencias de la grilla (Hz, crecientes). target: dB deseados. weight: importancia de cada punto (>= 0).
// fixedDb: lo que ya suena y se queda (por ejemplo, otras bandas); el ajuste busca target - fixedDb.
inline FitResult fitCurve (const std::vector<double>& freqs, const std::vector<double>& target, const std::vector<double>& weight,
                           const FitOptions& opt, const std::vector<double>* fixedDb = nullptr)
{
    using namespace fitdetail;
    const size_t G = freqs.size();
    FitResult res;
    if (G < 4) return res;
    const double fMin = std::max (10.0, freqs.front()), fMax = freqs.back();
    std::vector<double> goal (G);
    for (size_t i = 0; i < G; ++i) goal[i] = target[i] - (fixedDb != nullptr ? (*fixedDb)[i] : 0.0);

    std::vector<P> ps;
    std::vector<std::vector<double>> curves;   // curva de cada banda
    std::vector<double> model (G, 0.0), r (G), tmp;

    auto clampP = [&] (P& p)
    {
        p.lf = std::clamp (p.lf, std::log (fMin), std::log (fMax));
        p.g = std::clamp (p.g, -opt.maxGainDb, opt.maxGainDb);
        const double qmax = p.type == (int) BandType::Bell ? opt.maxQ : 1.4;
        p.lq = std::clamp (p.lq, std::log (opt.minQ), std::log (qmax));
    };
    auto rebuildModel = [&]
    {
        std::fill (model.begin(), model.end(), 0.0);
        for (auto& c : curves) for (size_t i = 0; i < G; ++i) model[i] += c[i];
    };
    auto cost = [&] (const std::vector<double>& m)
    {
        double c = 0;
        for (size_t i = 0; i < G; ++i) { const double e = (goal[i] - m[i]) * weight[i]; c += e * e; }
        return c;
    };

    // Levenberg-Marquardt sobre todas las bandas
    auto refine = [&] (int iters)
    {
        const int nb = (int) ps.size(), n = nb * 3;
        double lambda = 1.0e-2;
        double c0 = cost (model);
        std::vector<double> J ((size_t) n * G), JtJ ((size_t) n * n), Jte ((size_t) n), dx;
        for (int it = 0; it < iters; ++it)
        {
            for (size_t i = 0; i < G; ++i) r[i] = (goal[i] - model[i]) * weight[i];
            // Jacobiano numerico (por banda, solo su curva)
            for (int b = 0; b < nb; ++b)
                for (int k = 0; k < 3; ++k)
                {
                    P q = ps[(size_t) b];
                    const double h = k == 1 ? 0.05 : 0.01;
                    if (k == 0) q.lf += h; else if (k == 1) q.g += h; else q.lq += h;
                    curveOf (q, freqs, tmp);
                    const int col = b * 3 + k;
                    for (size_t i = 0; i < G; ++i) J[(size_t) col * G + i] = (tmp[i] - curves[(size_t) b][i]) / h * weight[i];
                }
            for (int a = 0; a < n; ++a)
            {
                double s = 0; for (size_t i = 0; i < G; ++i) s += J[(size_t) a * G + i] * r[i];
                Jte[(size_t) a] = s;
                for (int bcol = a; bcol < n; ++bcol)
                {
                    double t = 0; for (size_t i = 0; i < G; ++i) t += J[(size_t) a * G + i] * J[(size_t) bcol * G + i];
                    JtJ[(size_t) (a * n + bcol)] = JtJ[(size_t) (bcol * n + a)] = t;
                }
            }
            bool improved = false;
            for (int attempt = 0; attempt < 6 && ! improved; ++attempt)
            {
                auto A = JtJ;
                for (int a = 0; a < n; ++a) A[(size_t) (a * n + a)] += lambda * (JtJ[(size_t) (a * n + a)] + 1.0e-6);
                if (! solve (A, Jte, n, dx)) { lambda *= 10.0; continue; }
                auto trial = ps;
                for (int b = 0; b < nb; ++b)
                {
                    // pasos acotados (estabilidad)
                    trial[(size_t) b].lf += std::clamp (dx[(size_t) (b * 3)], -0.5, 0.5);
                    trial[(size_t) b].g  += std::clamp (dx[(size_t) (b * 3 + 1)], -6.0, 6.0);
                    trial[(size_t) b].lq += std::clamp (dx[(size_t) (b * 3 + 2)], -0.7, 0.7);
                    clampP (trial[(size_t) b]);
                }
                std::vector<std::vector<double>> tc (trial.size());
                std::vector<double> tm (G, 0.0);
                for (size_t b = 0; b < trial.size(); ++b) { curveOf (trial[b], freqs, tc[b]); for (size_t i = 0; i < G; ++i) tm[i] += tc[b][i]; }
                const double c1 = cost (tm);
                if (c1 < c0)
                {
                    ps = trial; curves = tc; model = tm;
                    improved = true;
                    const double rel = (c0 - c1) / std::max (1.0e-12, c0);
                    c0 = c1;
                    lambda = std::max (1.0e-7, lambda * 0.3);
                    if (rel < 1.0e-5) it = iters;   // ya no mejora
                }
                else lambda *= 8.0;
            }
            if (! improved) break;
        }
    };

    auto maxWeightedError = [&] (size_t* where)
    {
        double m = 0; size_t w = 0;
        for (size_t i = 0; i < G; ++i) { const double e = std::abs ((goal[i] - model[i]) * weight[i]); if (e > m) { m = e; w = i; } }
        if (where) *where = w;
        return m;
    };

    for (int added = 0; added < opt.maxBands; ++added)
    {
        size_t k = 0;
        const double err = maxWeightedError (&k);
        if (err < opt.toleranceDb) break;
        // residuo suavizado (1/6 de octava) para medir el ancho de lo que falta
        std::vector<double> rs (G);
        for (size_t i = 0; i < G; ++i)
        {
            double s = 0, w = 0;
            for (size_t j = 0; j < G; ++j)
            {
                const double d = std::log2 (freqs[j] / freqs[i]);
                if (std::abs (d) > 1.0 / 12.0) continue;
                s += (goal[j] - model[j]); w += 1.0;
            }
            rs[i] = s / std::max (1.0, w);
        }
        // pico del residuo con peso
        double best = 0; size_t kp = k;
        for (size_t i = 0; i < G; ++i) { const double v = std::abs (rs[i] * weight[i]); if (v > best) { best = v; kp = i; } }
        const double peak = rs[kp];
        // ancho a mitad de altura
        size_t lo = kp, hi = kp;
        while (lo > 0 && std::abs (rs[lo - 1]) > std::abs (peak) * 0.5 && (rs[lo - 1] > 0) == (peak > 0)) --lo;
        while (hi + 1 < G && std::abs (rs[hi + 1]) > std::abs (peak) * 0.5 && (rs[hi + 1] > 0) == (peak > 0)) ++hi;
        P p;
        const bool atLow = lo == 0, atHigh = hi == G - 1;
        if (opt.allowShelves && (atLow != atHigh))
        {
            // sigue hacia el borde: shelf con la mitad de su efecto en el limite de la zona
            p.type = atLow ? (int) BandType::LowShelf : (int) BandType::HighShelf;
            p.lf = std::log (atLow ? freqs[hi] : freqs[lo]);
            p.g = peak; p.lq = std::log (0.707);
        }
        else
        {
            const double bwOct = std::max (0.1, std::log2 (freqs[hi] / freqs[lo]));
            const double q = 1.0 / (2.0 * std::sinh (std::log (2.0) / 2.0 * bwOct));
            p.type = (int) BandType::Bell;
            p.lf = std::log (freqs[kp]); p.g = peak; p.lq = std::log (q);
        }
        clampP (p);
        ps.push_back (p);
        curves.emplace_back();
        curveOf (p, freqs, curves.back());
        rebuildModel();
        refine (opt.iterations);
    }

    // quitar bandas que casi no hacen nada (y reajustar)
    bool removed = false;
    for (size_t b = 0; b < ps.size();)
    {
        double m = 0; for (double v : curves[b]) m = std::max (m, std::abs (v));
        if (m < 0.2) { ps.erase (ps.begin() + (long) b); curves.erase (curves.begin() + (long) b); removed = true; }
        else ++b;
    }
    if (removed) { rebuildModel(); if (! ps.empty()) refine (opt.iterations / 2); }

    double mx = 0, sq = 0, wsum = 0;
    for (size_t i = 0; i < G; ++i)
    {
        const double e = goal[i] - model[i];
        if (weight[i] > 0.5) mx = std::max (mx, std::abs (e));
        sq += e * e * weight[i]; wsum += weight[i];
    }
    res.maxErrorDb = mx; res.rmsErrorDb = std::sqrt (sq / std::max (1.0e-9, wsum));
    for (auto& p : ps) res.bands.push_back (toBand (p));
    return res;
}

// Grilla logaritmica estandar para ajustar (1/24 de octava)
inline std::vector<double> fitGrid (double fLo = 20.0, double fHi = 20000.0, int perOctave = 24)
{
    std::vector<double> f;
    for (double x = fLo; x <= fHi * 1.0001; x *= std::pow (2.0, 1.0 / perOctave)) f.push_back (x);
    return f;
}
} // namespace gaux
