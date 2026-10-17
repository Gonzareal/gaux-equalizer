// Gaux Equalizer - SKETCH: dibujar la curva con el mouse ("tinta") y convertirla en bandas reales
//
// El trazo se guarda en la grilla de ajuste (1/24 de octava): cada punto tocado guarda los dB dibujados
// (el ultimo trazo sobre un punto manda, se puede ir y volver). Entre dos posiciones del mouse se interpola,
// asi un movimiento rapido no deja huecos. Al soltar, el ajustador (CurveFit) busca las bandas que, sumadas a
// las que ya existen, dan el trazo. Fuera del trazo la curva actual se respeta (con una transicion suave en
// los bordes, para que el ajustador pueda usar shelves y campanas naturales).
// No corre en el hilo de audio.
#pragma once

#include "CurveFit.h"

namespace gaux
{
class SketchCurve
{
public:
    SketchCurve() : grid (fitGrid (20.0, 20000.0, 24)) { clear(); }

    void clear()
    {
        ink.assign (grid.size(), 0.0);
        has.assign (grid.size(), 0);
        lastIdx = -1;
    }

    const std::vector<double>& freqs() const { return grid; }
    bool touched (size_t i) const { return has[i] != 0; }
    double inkDb (size_t i) const { return ink[i]; }

    // Cuantos puntos de la grilla tiene el trazo
    int count() const { int n = 0; for (auto h : has) n += h; return n; }
    // Rango del trazo (indices de la grilla). false si no hay trazo
    bool range (size_t& lo, size_t& hi) const
    {
        lo = grid.size(); hi = 0;
        for (size_t i = 0; i < grid.size(); ++i) if (has[i]) { lo = std::min (lo, i); hi = std::max (hi, i); }
        return lo <= hi && lo < grid.size();
    }

    // Una posicion del mouse (f en Hz, dB). Une con la anterior del mismo trazo
    void add (double f, double db)
    {
        const int i = indexOf (f);
        if (lastIdx < 0) { put (i, db); }
        else
        {
            const int a = lastIdx, b = i;
            const int n = std::abs (b - a);
            for (int k = 0; k <= n; ++k)
            {
                const double t = n == 0 ? 1.0 : (double) k / n;
                put (a + (b - a) * k / std::max (1, n), lastDb + (db - lastDb) * t);
            }
        }
        lastIdx = i; lastDb = db;
    }
    // El mouse se solto: el proximo add empieza un trazo nuevo (sin unir)
    void endStroke() { lastIdx = -1; }

    // Convierte el trazo en bandas. fixedDb: la curva que ya suena en la grilla (bandas existentes).
    // maxBands: cuantas bandas libres hay. El objetivo es el TOTAL dibujado.
    FitResult fit (const std::vector<double>& fixedDb, int maxBands, double maxGainDb = 24.0) const
    {
        FitResult none;
        size_t lo, hi;
        if (maxBands <= 0 || ! range (lo, hi) || count() < 3) return none;
        const size_t G = grid.size();
        // el trazo, suavizado 1/12 de octava (el pulso de la mano no es la intencion)
        std::vector<double> t (G), w (G);
        for (size_t i = 0; i < G; ++i) t[i] = has[i] ? ink[i] : fixedDb[i];
        std::vector<double> sm = t;
        for (size_t i = lo; i <= hi; ++i)
        {
            if (! has[i]) continue;
            double s = 0, n = 0;
            for (size_t j = (i >= 1 ? i - 1 : 0); j <= std::min (G - 1, i + 1); ++j) if (has[j]) { s += t[j]; n += 1.0; }
            sm[i] = s / std::max (1.0, n);
        }
        // pesos: el trazo manda (1). Fuera, la curva actual pesa 0.5, pero sube de a poco desde el borde del
        // trazo (media octava) para dejar que la curva baje o suba naturalmente
        for (size_t i = 0; i < G; ++i)
        {
            if (has[i]) { w[i] = 1.0; continue; }
            const double d = i < lo ? std::log2 (grid[lo] / grid[i]) : (i > hi ? std::log2 (grid[i] / grid[hi]) : 0.0);
            w[i] = (i > lo && i < hi) ? 0.3 : 0.5 * std::clamp (d / 0.5, 0.06, 1.0);
        }
        FitOptions o;
        o.maxBands = std::min (maxBands, 8);
        o.toleranceDb = 0.5;
        o.maxGainDb = maxGainDb;
        o.maxQ = 8.0;
        o.iterations = 30;
        return fitCurve (grid, sm, w, o, &fixedDb);
    }

private:
    int indexOf (double f) const
    {
        const double x = std::log2 (std::max (1.0, f) / grid.front()) * 24.0;
        return std::clamp ((int) std::lround (x), 0, (int) grid.size() - 1);
    }
    void put (int i, double db) { ink[(size_t) i] = db; has[(size_t) i] = 1; }

    std::vector<double> grid, ink;
    std::vector<char> has;
    int lastIdx = -1;
    double lastDb = 0.0;
};
} // namespace gaux
