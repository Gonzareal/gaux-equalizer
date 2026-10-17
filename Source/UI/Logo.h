// Gaux Equalizer - Logotipos GAUX. La "X" de cuchillas cruzadas es la firma de la marca.
#pragma once

#include "GauxLookAndFeel.h"

namespace gaux
{
namespace Logo
{
    // Ruido determinista para bordes irregulares
    inline float hashNoise (int i, int seed)
    {
        juce::uint32 h = (juce::uint32) i * 374761393u + (juce::uint32) seed * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float) ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
    }

    // Trazo crudo con bordes irregulares (para dibujos "a mano")
    inline juce::Path roughStroke (juce::Point<float> p0, juce::Point<float> p1, float width, int seed)
    {
        const auto dir = p1 - p0;
        const float len = juce::jmax (0.001f, dir.getDistanceFromOrigin());
        const juce::Point<float> n (-dir.y / len, dir.x / len);
        const int steps = 10;
        juce::Array<juce::Point<float>> left, right;
        for (int i = 0; i <= steps; ++i)
        {
            const float t = (float) i / (float) steps;
            const float taper = 0.6f + 0.4f * std::sin (t * juce::MathConstants<float>::pi);
            const auto c = p0 + dir * t + n * (hashNoise (i + 50, seed) - 0.5f) * width * 0.35f;
            left.add (c + n * width * 0.5f * taper * (0.7f + 0.6f * hashNoise (i, seed)));
            right.add (c - n * width * 0.5f * taper * (0.7f + 0.6f * hashNoise (i + 97, seed)));
        }
        juce::Path p;
        p.startNewSubPath (left[0]);
        for (int i = 1; i < left.size(); ++i) p.lineTo (left[i]);
        for (int i = right.size() - 1; i >= 0; --i) p.lineTo (right[i]);
        p.closeSubPath();
        return p;
    }

    inline juce::Path textPath (const juce::String& txt, const juce::Font& f, float x, float baseline)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, txt, x, baseline);
        juce::Path p;
        ga.createPath (p);
        return p;
    }

    // Una cuchilla: ahusada en las puntas, ligeramente curva
    inline juce::Path blade (juce::Point<float> p0, juce::Point<float> p1, float width, float bend)
    {
        const auto dir = p1 - p0;
        const float len = dir.getDistanceFromOrigin();
        const juce::Point<float> n (-dir.y / len, dir.x / len);
        const auto mid = p0 + dir * 0.5f + n * (bend * len);
        juce::Path p;
        p.startNewSubPath (p0);
        p.quadraticTo (mid + n * width, p1);
        p.quadraticTo (mid - n * width * 0.55f, p0);
        p.closeSubPath();
        return p;
    }

    // X estilizada: cuchilla larga que sobresale + cuchilla corta, separadas por un corte
    struct BladeX { juce::Path longBlade, shortBlade; };

    inline BladeX makeX (juce::Rectangle<float> box)
    {
        const float w = box.getWidth(), h = box.getHeight();
        BladeX x;
        x.longBlade = blade ({ box.getX() - w * 0.06f, box.getY() - h * 0.16f },
                             { box.getRight() + w * 0.10f, box.getBottom() + h * 0.12f }, w * 0.30f, -0.04f);
        x.shortBlade = blade ({ box.getRight() - w * 0.02f, box.getY() + h * 0.02f },
                              { box.getX() + w * 0.04f, box.getBottom() - h * 0.02f }, w * 0.25f, 0.03f);
        return x;
    }

    inline void drawX (juce::Graphics& g, const BladeX& x, juce::Colour fillA, juce::Colour fillB, juce::Colour gap, float gapWidth)
    {
        g.setColour (fillB);
        g.fillPath (x.shortBlade);
        g.setColour (gap);
        g.strokePath (x.longBlade, juce::PathStrokeType (gapWidth));
        g.setColour (fillA);
        g.fillPath (x.longBlade);
    }

    //==========================================================================
    // Logo del modo parametrico
    //  - "GAU": tipografia de exhibicion (OFL, incluida en el plugin), en cursiva y plana (sin 3D).
    //  - "X" propia: un trazo grueso "/" que una cuchilla roja "\" corta en dos; las dos mitades
    //    quedan desplazadas a lo largo del corte. La cuchilla sobresale arriba y abajo y deja estela.
    namespace RaveLogo
    {
        constexpr float kShear = 0.20f;   // cursiva

        struct Shape { juce::Path gau; float gauRight = 0; };

        // "GAU" normalizado a alto de mayuscula = 1 (origen arriba a la izquierda)
        inline Shape makeGau (juce::Typeface::Ptr tf)
        {
            Shape r;
            const auto f = juce::Font (juce::FontOptions (tf).withHeight (100.0f).withKerningFactor (-0.01f));
            juce::GlyphArrangement ga;
            ga.addLineOfText (f, "GAU", 0.0f, 0.0f);
            ga.createPath (r.gau);
            const auto b = r.gau.getBounds();
            r.gau.applyTransform (juce::AffineTransform::translation (-b.getX(), -b.getY()).scaled (1.0f / b.getHeight()));
            r.gauRight = r.gau.getBounds().getRight();
            return r;
        }

        inline const Shape& gauFor (juce::Typeface::Ptr tf)
        {
            static std::map<juce::Typeface*, Shape> cache;
            auto it = cache.find (tf.get());
            if (it == cache.end()) it = cache.emplace (tf.get(), makeGau (tf)).first;
            return it->second;
        }

        constexpr float kXGap = 0.10f, kXW = 0.92f;

        // Cuchilla ahusada (afilada en las dos puntas, mas gruesa cerca del final)
        inline juce::Path blade (juce::Point<float> a, juce::Point<float> b, float maxW)
        {
            const auto d = b - a;
            const float len = d.getDistanceFromOrigin();
            const juce::Point<float> n (-d.y / len, d.x / len);
            juce::Path p;
            p.startNewSubPath (a);
            p.cubicTo (a + d * 0.35f + n * maxW * 0.35f, a + d * 0.70f + n * maxW * 0.60f, b);
            p.cubicTo (a + d * 0.80f - n * maxW * 0.55f, a + d * 0.40f - n * maxW * 0.30f, a);
            p.closeSubPath();
            return p;
        }
    }

    inline float raveWidth (float h, juce::Typeface::Ptr tf = nullptr)
    {
        using namespace RaveLogo;
        const auto& s = gauFor (tf != nullptr ? tf : Fonts::get().logo);
        return (s.gauRight + kXGap + kXW + 0.25f + kShear) * h;
    }

    inline void drawRave (juce::Graphics& g, juce::Rectangle<float> area, double timeSec, juce::Typeface::Ptr tf = nullptr)
    {
        using namespace RaveLogo;
        const auto& s = gauFor (tf != nullptr ? tf : Fonts::get().logo);
        const float h = area.getHeight() * 0.80f;   // espacio para que la cuchilla sobresalga
        const float top = area.getY() + area.getHeight() * 0.10f;
        // cursiva: x' = x + (1 - y) * shear, en unidades
        const auto tr = juce::AffineTransform (1.0f, -kShear, kShear, 0.0f, 1.0f, 0.0f).scaled (h).translated (area.getX(), top);

        juce::Path gau (s.gau);
        gau.applyTransform (tr);

        // ---- X: geometria en unidades ----
        const float x0 = s.gauRight + kXGap;
        const float tw = 0.25f;                                   // grosor del trazo "/"
        juce::Path slashBar;                                      // "/"
        slashBar.addQuadrilateral (x0, 1.0f, x0 + tw, 1.0f, x0 + kXW, 0.0f, x0 + kXW - tw, 0.0f);
        const juce::Point<float> c0 (x0 - 0.16f, -0.24f), c1 (x0 + kXW + 0.20f, 1.24f);   // linea del corte "\"
        const auto cut = c1 - c0;
        const auto cutDir = cut / cut.getDistanceFromOrigin();
        const juce::Point<float> cutN (-cutDir.y, cutDir.x);
        const float shift = 0.11f;

        // Mitades del "/" a cada lado del corte, desplazadas en sentidos opuestos
        auto halfPlane = [&] (float side)
        {
            juce::Path hp;
            const auto far = cutN * (side * 4.0f);
            hp.addQuadrilateral (c0.x - cutDir.x * 3, c0.y - cutDir.y * 3, c1.x + cutDir.x * 3, c1.y + cutDir.y * 3,
                                 c1.x + cutDir.x * 3 + far.x, c1.y + cutDir.y * 3 + far.y,
                                 c0.x - cutDir.x * 3 + far.x, c0.y - cutDir.y * 3 + far.y);
            return hp;
        };

        // ---- Brillo de glitch ocasional (como las primeras ideas) ----
        const double ph = std::fmod (timeSec, 5.2);
        const float glitch = ph < 0.14 ? (float) (1.0 - ph / 0.14) : 0.0f;

        // Resplandor rojo suave detras de la X
        {
            const auto xc = juce::Point<float> (x0 + kXW * 0.5f, 0.5f).transformedBy (tr);
            juce::ColourGradient rg (Colours::accent.withAlpha (0.28f), xc.x, xc.y, Colours::accent.withAlpha (0.0f), xc.x + h * 0.9f, xc.y, true);
            rg.addColour (0.5, Colours::accent.withAlpha (0.08f));
            g.setGradientFill (rg);
            g.fillEllipse (juce::Rectangle<float> (h * 1.9f, h * 1.9f).withCentre (xc));
        }

        // GAU: blanco plano con un degradado sutil
        if (glitch > 0.0f)
        {
            g.setColour (juce::Colour (0xff00e5ff).withAlpha (0.55f * glitch));
            g.fillPath (gau, juce::AffineTransform::translation (-h * 0.06f * glitch, 0));
            g.setColour (Colours::accent.withAlpha (0.75f * glitch));
            g.fillPath (gau, juce::AffineTransform::translation (h * 0.06f * glitch, 0));
        }
        g.setGradientFill (juce::ColourGradient (juce::Colours::white, 0, top, juce::Colour (0xffe9dcda), 0, top + h, false));
        g.fillPath (gau);

        // Las dos mitades del "/" (blancas, como las letras)
        for (float side : { -1.0f, 1.0f })
        {
            juce::Graphics::ScopedSaveState ss (g);
            const auto off = cutDir * (shift * side);
            const auto t = juce::AffineTransform::translation (off.x, off.y).followedBy (tr);
            g.reduceClipRegion (halfPlane (side), t);
            g.setGradientFill (juce::ColourGradient (juce::Colours::white, 0, top, juce::Colour (0xffe9dcda), 0, top + h, false));
            g.fillPath (slashBar, t);
        }

        // Cuchilla roja "\" con estela
        auto bl = blade (c0, c1, 0.27f);
        for (int i = 3; i >= 1; --i)
        {
            const auto t = juce::AffineTransform::translation (-0.055f * (float) i, 0.0f).followedBy (tr);
            g.setColour (juce::Colour (0xffff2a6a).withAlpha (0.16f / (float) i));
            g.fillPath (bl, t);
        }
        g.setColour (Colours::ink);
        g.strokePath (bl, juce::PathStrokeType (h * 0.07f), tr);       // separacion limpia con la barra blanca
        {
            const auto bb = bl.getBoundsTransformed (tr);
            juce::ColourGradient cg (juce::Colour (0xffff5a6e), bb.getX(), bb.getY(), juce::Colour (0xffc4002a), bb.getRight(), bb.getBottom(), false);
            cg.addColour (0.55, Colours::accent);
            g.setGradientFill (cg);
            g.fillPath (bl, tr);
        }
        // filo de luz en la cuchilla
        {
            juce::Path edge;
            edge.startNewSubPath (c0 + cutDir * 0.10f);
            edge.lineTo (c1 - cutDir * 0.12f);
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.strokePath (edge, juce::PathStrokeType (juce::jmax (0.8f, h * 0.018f)), tr);
        }
    }

    //==========================================================================
    // X vintage: trazo grueso + trazo fino (contraste de pluma), remates, y una cola
    // caligrafica ("swash") que sale de la X y subraya las letras hacia la izquierda.
    struct VintageX { juce::Path thick, thin, swash; };

    // Trazo ahusado que sigue una curva cubica
    inline juce::Path taperedCurve (juce::Point<float> p0, juce::Point<float> c1, juce::Point<float> c2, juce::Point<float> p1,
                                    float w0, float w1)
    {
        const int n = 40;
        juce::Array<juce::Point<float>> L, R;
        for (int i = 0; i <= n; ++i)
        {
            const float t = (float) i / n, u = 1.0f - t;
            const auto pt = p0 * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + p1 * (t * t * t);
            const auto d = (c1 - p0) * (3 * u * u) + (c2 - c1) * (6 * u * t) + (p1 - c2) * (3 * t * t);
            const float len = juce::jmax (0.001f, d.getDistanceFromOrigin());
            const juce::Point<float> nrm (-d.y / len, d.x / len);
            const float w = (w0 + (w1 - w0) * t) * 0.5f;
            L.add (pt + nrm * w);
            R.add (pt - nrm * w);
        }
        juce::Path p;
        p.startNewSubPath (L[0]);
        for (int i = 1; i < L.size(); ++i) p.lineTo (L[i]);
        for (int i = R.size() - 1; i >= 0; --i) p.lineTo (R[i]);
        p.closeSubPath();
        return p;
    }

    inline VintageX makeVintageX (juce::Rectangle<float> box, float swashEndX)
    {
        const float w = box.getWidth(), h = box.getHeight();
        VintageX x;
        // Trazo grueso (arriba-izq a abajo-der) con remates
        juce::Path thick;
        const float tw = w * 0.30f;
        const auto a0 = juce::Point<float> (box.getX() + w * 0.12f, box.getY());
        const auto a1 = juce::Point<float> (box.getRight() - w * 0.10f, box.getBottom());
        thick.startNewSubPath (a0.x - tw * 0.5f, a0.y);
        thick.lineTo (a0.x + tw * 0.5f, a0.y);
        thick.lineTo (a1.x + tw * 0.5f, a1.y);
        thick.lineTo (a1.x - tw * 0.5f, a1.y);
        thick.closeSubPath();
        thick.addRectangle (a0.x - tw * 0.85f, a0.y, tw * 1.7f, h * 0.06f);          // remate superior
        thick.addRectangle (a1.x - tw * 0.85f, a1.y - h * 0.06f, tw * 1.7f, h * 0.06f); // remate inferior
        x.thick = thick;

        // Trazo fino (arriba-der a abajo-izq) con remate arriba
        const float hw = w * 0.085f;
        const auto b0 = juce::Point<float> (box.getRight() - w * 0.14f, box.getY());
        const auto b1 = juce::Point<float> (box.getX() + w * 0.16f, box.getBottom() - h * 0.04f);
        juce::Path thin;
        thin.startNewSubPath (b0.x - hw * 0.5f, b0.y);
        thin.lineTo (b0.x + hw * 0.5f, b0.y);
        thin.lineTo (b1.x + hw * 0.5f, b1.y);
        thin.lineTo (b1.x - hw * 0.5f, b1.y);
        thin.closeSubPath();
        thin.addRectangle (b0.x - hw * 2.2f, b0.y, hw * 4.4f, h * 0.05f);
        x.thin = thin;

        // Cola: sale de la base del trazo fino, baja y barre hacia la izquierda bajo las letras
        x.swash = taperedCurve (b1, { b1.x - w * 0.15f, b1.y + h * 0.20f },
                                { swashEndX + (b1.x - swashEndX) * 0.55f, box.getBottom() + h * 0.18f },
                                { swashEndX, box.getBottom() + h * 0.07f }, hw * 1.25f, 0.6f);
        return x;
    }

    //==========================================================================
    // Insignia de amplificador: letras cromadas inclinadas, desgastadas, X con esmalte rojo
    inline void drawAmpBadge (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const float h = area.getHeight();
        const auto font = displayFont (h * 1.18f, 0.10f);
        const float baseline = area.getY() + h * 0.95f;
        auto gau = textPath ("GAU", font, area.getX(), baseline);
        gau.applyTransform (juce::AffineTransform::shear (-0.14f, 0.0f).translated (h * 0.13f, 0));
        // corregir el desplazamiento horizontal que introduce la inclinacion
        gau.applyTransform (juce::AffineTransform::translation (area.getX() - gau.getBounds().getX(), 0));
        const float gauRight = gau.getBounds().getRight();
        auto xBox = juce::Rectangle<float> (gauRight + h * 0.06f, area.getY() + h * 0.06f, h * 0.92f, h * 0.89f);
        auto vx = makeVintageX (xBox, area.getX() + h * 0.12f);

        juce::Path all;
        all.addPath (gau);
        all.addPath (vx.thick);
        all.addPath (vx.thin);
        all.addPath (vx.swash);

        // Sombra proyectada
        juce::DropShadow (juce::Colours::black.withAlpha (0.85f), juce::roundToInt (h * 0.12f), { 0, juce::roundToInt (h * 0.06f) }).drawForPath (g, all);

        // Cola y X en esmalte rojo con filo cromado (la cola pasa por debajo de las letras)
        auto enamel = [&] (const juce::Path& p)
        {
            g.setColour (juce::Colour (0xff0d0b0a));
            g.strokePath (p, juce::PathStrokeType (h * 0.05f, juce::PathStrokeType::mitered));
            g.setColour (juce::Colour (0xffd8d2c6));
            g.strokePath (p, juce::PathStrokeType (h * 0.022f, juce::PathStrokeType::mitered));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe0303a), 0, p.getBounds().getY(), juce::Colour (0xff8f1018), 0, p.getBounds().getBottom(), false));
            g.fillPath (p);
        };
        enamel (vx.swash);

        // Letras: borde oscuro + cromo con horizonte reflejado
        g.setColour (juce::Colour (0xff0d0b0a));
        g.strokePath (gau, juce::PathStrokeType (h * 0.05f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        const auto b = gau.getBounds();
        juce::ColourGradient chrome (juce::Colour (0xfff7f4ee), 0, b.getY(), juce::Colour (0xffd9d4ca), 0, b.getBottom(), false);
        chrome.addColour (0.42, juce::Colour (0xffa9a39a));
        chrome.addColour (0.50, juce::Colour (0xff3d3934));
        chrome.addColour (0.58, juce::Colour (0xff8f897e));
        g.setGradientFill (chrome);
        g.fillPath (gau);

        enamel (vx.thin);
        enamel (vx.thick);
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (vx.thick);
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.4f), xBox.getX(), xBox.getY(),
                                                     juce::Colours::white.withAlpha (0.0f), xBox.getCentreX(), xBox.getCentreY(), false));
            g.fillRect (xBox.expanded (h * 0.2f));
        }

        // Desgaste: rayones, picaduras y patina, solo sobre el metal
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (all);
            juce::Random rnd (77);
            auto ab = all.getBounds();
            for (int i = 0; i < 90; ++i)
            {
                const float x0 = ab.getX() + rnd.nextFloat() * ab.getWidth();
                const float y0 = ab.getY() + rnd.nextFloat() * ab.getHeight();
                const float len = 4.0f + rnd.nextFloat() * h * 0.5f;
                const float ang = -0.4f + rnd.nextFloat() * 0.8f;
                g.setColour ((rnd.nextFloat() < 0.6f ? juce::Colours::black : juce::Colours::white).withAlpha (0.15f + rnd.nextFloat() * 0.3f));
                g.drawLine (x0, y0, x0 + std::cos (ang) * len, y0 + std::sin (ang) * len, 0.6f + rnd.nextFloat() * 0.6f);
            }
            for (int i = 0; i < 420; ++i)
            {
                const float r = 0.4f + rnd.nextFloat() * rnd.nextFloat() * 2.4f;
                g.setColour (juce::Colour (0xff1c1712).withAlpha (0.35f + rnd.nextFloat() * 0.5f));
                g.fillEllipse (ab.getX() + rnd.nextFloat() * ab.getWidth(), ab.getY() + rnd.nextFloat() * ab.getHeight(), r, r);
            }
            for (int i = 0; i < 14; ++i)
            {
                const float cx = ab.getX() + rnd.nextFloat() * ab.getWidth(), cy = ab.getY() + rnd.nextFloat() * ab.getHeight();
                const float rr = h * (0.05f + rnd.nextFloat() * 0.12f);
                g.setGradientFill (juce::ColourGradient (juce::Colour (0xff7a4a22).withAlpha (0.45f), cx, cy, juce::Colour (0x007a4a22), cx + rr, cy, true));
                g.fillEllipse (cx - rr, cy - rr, rr * 2, rr * 2);
            }
        }
        // Brillo del bisel superior
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.strokePath (gau, juce::PathStrokeType (1.0f), juce::AffineTransform::translation (0, -1.0f));
    }

    inline float ampBadgeWidth (float h)
    {
        auto gau = textPath ("GAU", displayFont (h * 1.18f, 0.10f), 0, h);
        gau.applyTransform (juce::AffineTransform::shear (-0.14f, 0.0f));
        return gau.getBounds().getWidth() + h * 0.06f + h * 0.92f + h * 0.08f;
    }
}
} // namespace gaux
