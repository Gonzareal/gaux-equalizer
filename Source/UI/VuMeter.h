// Gaux Equalizer - VU meter analogico con aguja (modo Vintage)
#pragma once

#include "GauxLookAndFeel.h"
#include "../PluginProcessor.h"
#include "FrameClock.h"

namespace gaux
{
class VuMeter : public juce::Component
{
public:
    explicit VuMeter (GauxEqProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        clock.paintBegin();
        // Caratula (todo menos la aguja) y vidrio: en cache. Cada cuadro solo dibuja la aguja.
        faceLayer.draw (g, getLocalBounds(), 0, [this] (juce::Graphics& gi) { paintFace (gi); });
        paintNeedle (g);
        glassLayer.draw (g, getLocalBounds(), 0, [this] (juce::Graphics& gi) { paintGlass (gi); });
        clock.paintEnd();
    }

private:
    struct Geo
    {
        juce::Rectangle<float> r, face;
        juce::Point<float> pivot;
        float arcR, a0, a1;
    };
    Geo geo() const
    {
        Geo q;
        q.r = getLocalBounds().toFloat();
        q.face = q.r.reduced (7.0f);
        q.pivot = { q.face.getCentreX(), q.face.getBottom() + q.face.getHeight() * 0.32f };
        q.arcR = q.face.getHeight() * 0.98f;
        q.a0 = juce::degreesToRadians (-48.0f); q.a1 = juce::degreesToRadians (48.0f);
        return q;
    }

    void paintFace (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();

        // Bisel metalico
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4a48), r.getX(), r.getY(), juce::Colour (0xff141414), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 6.0f);
        auto face = r.reduced (7.0f);

        // Cara retroiluminada calida
        juce::ColourGradient light (juce::Colour (0xfff7e7bd), face.getCentreX(), face.getBottom() + face.getHeight() * 0.2f,
                                    juce::Colour (0xffcdb47a), face.getCentreX(), face.getY() - face.getHeight() * 0.6f, true);
        g.setGradientFill (light);
        g.fillRoundedRectangle (face, 3.0f);

        const auto pivot = juce::Point<float> (face.getCentreX(), face.getBottom() + face.getHeight() * 0.32f);
        const float arcR = face.getHeight() * 0.98f;
        const float a0 = juce::degreesToRadians (-48.0f), a1 = juce::degreesToRadians (48.0f);
        auto angleFor = [&] (float vu) { return a0 + (a1 - a0) * vuToPos (vu); };

        g.saveState();
        g.reduceClipRegion (face.toNearestInt());

        // Arco de escala con zona roja
        juce::Path arc;
        arc.addCentredArc (pivot.x, pivot.y, arcR, arcR, 0, a0, angleFor (0.0f), true);
        g.setColour (juce::Colour (0xff2a2418));
        g.strokePath (arc, juce::PathStrokeType (1.4f));
        juce::Path red;
        red.addCentredArc (pivot.x, pivot.y, arcR, arcR, 0, angleFor (0.0f), a1, true);
        g.setColour (juce::Colour (0xffb3261e));
        g.strokePath (red, juce::PathStrokeType (4.0f));

        const float marks[] = { -20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3 };
        g.setFont (engraveFont (face.getHeight() * 0.13f, 0.0f));
        for (float m : marks)
        {
            const float a = angleFor (m);
            const auto p1 = pivot.getPointOnCircumference (arcR, a);
            const auto p2 = pivot.getPointOnCircumference (arcR + face.getHeight() * 0.08f, a);
            g.setColour (m > 0 ? juce::Colour (0xffb3261e) : juce::Colour (0xff2a2418));
            g.drawLine ({ p1, p2 }, 1.4f);
            const auto tp = pivot.getPointOnCircumference (arcR + face.getHeight() * 0.18f, a);
            juce::String lbl = m > 0 ? "+" + juce::String ((int) m) : juce::String ((int) std::abs (m));
            g.drawText (lbl, juce::Rectangle<float> (26, 14).withCentre (tp), juce::Justification::centred);
        }
        g.setColour (juce::Colour (0xff2a2418));
        g.setFont (engraveFont (face.getHeight() * 0.22f, 0.1f));
        g.drawText ("VU", juce::Rectangle<float> (60, 24).withCentre ({ face.getCentreX(), face.getBottom() - face.getHeight() * 0.28f }),
                    juce::Justification::centred);

        g.restoreState();
    }

    void paintNeedle (juce::Graphics& g)
    {
        // Aguja con sombra
        const auto q = geo();
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (q.face.toNearestInt());
        const float a = q.a0 + (q.a1 - q.a0) * vuToPos (needleVu);
        const auto tip = q.pivot.getPointOnCircumference (q.arcR + q.face.getHeight() * 0.1f, a);
        const auto base = q.pivot.getPointOnCircumference (q.face.getHeight() * 0.4f, a);
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.drawLine ({ base.translated (3, 3), tip.translated (3, 3) }, 1.6f);
        g.setColour (juce::Colour (0xff141210));
        g.drawLine ({ base, tip }, 1.4f);
    }

    void paintGlass (juce::Graphics& g)
    {
        const auto face = geo().face;
        // Tapa del pivote
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3a3a), face.getCentreX(), face.getBottom() - 16,
                                                 juce::Colour (0xff0a0a0a), face.getCentreX(), face.getBottom(), false));
        g.fillRect (face.withTop (face.getBottom() - face.getHeight() * 0.12f));

        // Reflejo del vidrio
        juce::Path glass;
        glass.startNewSubPath (face.getX(), face.getY());
        glass.lineTo (face.getRight(), face.getY());
        glass.lineTo (face.getRight(), face.getY() + face.getHeight() * 0.18f);
        glass.quadraticTo (face.getCentreX(), face.getY() + face.getHeight() * 0.42f, face.getX(), face.getY() + face.getHeight() * 0.3f);
        glass.closeSubPath();
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillPath (glass);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (face, 3.0f, 1.2f);
    }

    // Escala VU: posicion proporcional al voltaje (como un medidor real)
    static float vuToPos (float vu)
    {
        const float amp = std::pow (10.0f, vu / 20.0f);
        const float lo = std::pow (10.0f, -20.0f / 20.0f), hi = std::pow (10.0f, 3.0f / 20.0f);
        return juce::jlimit (0.0f, 1.0f, (amp - lo) / (hi - lo));
    }

    void frame (float dt)
    {
        // 0 VU = -18 dBFS RMS; balistica de ~300 ms (en tiempo real, igual a cualquier FPS)
        const float db = juce::Decibels::gainToDecibels (proc.outputRms.load(), -100.0f) + 18.0f;
        const float target = juce::jlimit (-22.0f, 4.0f, db);
        const float coeff = juce::jmin (1.0f, (1.0f - std::exp (-dt / 0.3f)) * 2.2f);
        const float before = needleVu;
        needleVu += (target - needleVu) * coeff;
        needleVu = juce::jlimit (-22.0f, 4.0f, needleVu);
        if (std::abs (vuToPos (needleVu) - vuToPos (before)) > 1.0e-4f) repaint();   // aguja quieta: nada que dibujar
    }

    GauxEqProcessor& proc;
    float needleVu = -22.0f;
    LayerCache faceLayer, glassLayer;
    FrameClock clock { *this, [this] (float dt) { frame (dt); } };
};
} // namespace gaux
