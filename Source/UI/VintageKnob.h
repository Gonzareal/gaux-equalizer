// Gaux Equalizer - Perillas fotorrealistas del modo Vintage (diseno propio "GX")
//  - Perilla continua: base de baquelita moleteada + tapa de aluminio cepillado.
//  - Selector: misma base con una aleta de aluminio que senala la posicion.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "GauxLookAndFeel.h"

namespace gaux
{
namespace KnobRender
{
    // Brillo anisotropico del aluminio cepillado (simula un degradado conico)
    inline void brushedCap (juce::Graphics& g, juce::Rectangle<float> cap)
    {
        const auto c = cap.getCentre();
        const float r = cap.getWidth() * 0.5f;

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffd9dcdf), c.x - r * 0.4f, c.y - r * 0.5f,
                                                 juce::Colour (0xff8b8f95), c.x + r * 0.6f, c.y + r * 0.8f, true));
        g.fillEllipse (cap);

        // Sectores con brillo variable alrededor del centro
        const int segs = 120;
        for (int i = 0; i < segs; ++i)
        {
            const float a0 = juce::MathConstants<float>::twoPi * (float) i / (float) segs;
            const float a1 = a0 + juce::MathConstants<float>::twoPi / (float) segs + 0.01f;
            const float am = (a0 + a1) * 0.5f;
            // Dos lobulos de luz (arriba-izquierda y abajo-derecha) como en metal real
            const float l = std::pow (std::abs (std::cos (am - 2.4f)), 6.0f);
            const float d = std::pow (std::abs (std::sin (am - 2.4f)), 8.0f);
            juce::Path seg;
            seg.addPieSegment (cap, a0, a1, 0.0f);
            g.setColour (juce::Colours::white.withAlpha (0.28f * l));
            g.fillPath (seg);
            g.setColour (juce::Colours::black.withAlpha (0.16f * d));
            g.fillPath (seg);
        }

        // Anillos concentricos del cepillado
        juce::Random rnd (7);
        for (float rr = r * 0.08f; rr < r; rr += 0.9f)
        {
            g.setColour ((rnd.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.015f + rnd.nextFloat() * 0.035f));
            g.drawEllipse (juce::Rectangle<float> (rr * 2, rr * 2).withCentre (c), 0.7f);
        }

        // Bisel del borde
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.7f), c.x, cap.getY(),
                                                 juce::Colours::black.withAlpha (0.5f), c.x, cap.getBottom(), false));
        g.drawEllipse (cap.reduced (0.6f), 1.2f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawEllipse (cap.expanded (0.6f), 0.9f);
    }

    // Tapa de baquelita negra brillante (estilo amplificador)
    inline void blackCap (juce::Graphics& g, juce::Rectangle<float> cap)
    {
        const auto c = cap.getCentre();
        const float r = cap.getWidth() * 0.5f;
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3b3735), c.x - r * 0.35f, c.y - r * 0.45f,
                                                 juce::Colour (0xff050404), c.x + r * 0.7f, c.y + r * 0.8f, true));
        g.fillEllipse (cap);
        // Reflejo especular suave arriba a la izquierda
        auto spec = juce::Rectangle<float> (r * 1.1f, r * 0.7f).withCentre (c.translated (-r * 0.22f, -r * 0.42f));
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.38f), spec.getCentreX(), spec.getCentreY(),
                                                 juce::Colours::white.withAlpha (0.0f), spec.getRight(), spec.getCentreY(), true));
        g.fillEllipse (spec);
        // Anillo de luz inferior (rebote)
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.0f), c.x, c.y, juce::Colours::white.withAlpha (0.12f), c.x, cap.getBottom(), false));
        g.drawEllipse (cap.reduced (1.2f), 1.2f);
        g.setColour (juce::Colours::black);
        g.drawEllipse (cap.expanded (0.5f), 1.0f);
    }

    // Perilla de baquelita negra con falda ancha y tapa estriada (estatica: el indice se dibuja aparte)
    inline void bakeliteSkirt (juce::Graphics& g, juce::Rectangle<float> base)
    {
        const auto c = base.getCentre();
        const float R = base.getWidth() * 0.5f;
        juce::Path disc; disc.addEllipse (base);
        juce::DropShadow (juce::Colours::black.withAlpha (0.8f), juce::roundToInt (R * 0.32f), { 0, juce::roundToInt (R * 0.14f) }).drawForPath (g, disc);

        // Falda: cono bajo, brillo suave arriba
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff34302e), c.x - R * 0.3f, base.getY(),
                                                 juce::Colour (0xff040303), c.x + R * 0.3f, base.getBottom(), false));
        g.fillEllipse (base);
        for (int i = 0; i < 90; ++i)
        {
            const float a0 = juce::MathConstants<float>::twoPi * (float) i / 90.0f, a1 = a0 + juce::MathConstants<float>::twoPi / 90.0f + 0.01f;
            const float am = (a0 + a1) * 0.5f;
            const float l = std::pow (std::abs (std::cos (am - 2.5f)), 10.0f);
            juce::Path seg; seg.addPieSegment (base, a0, a1, 0.62f);
            g.setColour (juce::Colours::white.withAlpha (0.10f * l));
            g.fillPath (seg);
        }
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.30f), c.x, base.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), c.x, c.y, false));
        g.drawEllipse (base.reduced (0.8f), 1.2f);
        g.setColour (juce::Colours::black);
        g.drawEllipse (base, 1.0f);

        // Escalon entre falda y tapa
        const float capR = R * 0.64f;
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillEllipse (juce::Rectangle<float> (capR * 2 + 5, capR * 2 + 5).withCentre (c.translated (0, 2.0f)));

        // Tapa estriada (contorno ondulado)
        juce::Path cap;
        const int flutes = 18;
        for (int i = 0; i <= 360; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / 360.0f;
            const float rr = capR * (1.0f - 0.045f * std::pow (std::abs (std::sin (a * flutes * 0.5f)), 0.6f));
            const auto p = c.getPointOnCircumference (rr, a);
            if (i == 0) cap.startNewSubPath (p); else cap.lineTo (p);
        }
        cap.closeSubPath();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3d3937), c.x - capR * 0.4f, c.y - capR * 0.5f,
                                                 juce::Colour (0xff060505), c.x + capR * 0.6f, c.y + capR * 0.8f, true));
        g.fillPath (cap);
        // Sombra de las estrias
        for (int i = 0; i < flutes; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * ((float) i + 0.5f) / (float) flutes;
            const float light = 0.5f + 0.5f * std::cos (a);
            g.setColour (juce::Colours::white.withAlpha (0.04f + 0.08f * light));
            g.drawLine ({ c.getPointOnCircumference (capR * 0.97f, a), c.getPointOnCircumference (capR * 0.80f, a) }, 1.0f);
        }
        // Cara superior lisa con reflejo
        auto top = juce::Rectangle<float> (capR * 1.6f, capR * 1.6f).withCentre (c);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2c2827), top.getX(), top.getY(), juce::Colour (0xff070606), top.getRight(), top.getBottom(), false));
        g.fillEllipse (top);
        auto spec = juce::Rectangle<float> (capR * 0.95f, capR * 0.55f).withCentre (c.translated (-capR * 0.18f, -capR * 0.38f));
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.30f), spec.getCentreX(), spec.getCentreY(),
                                                 juce::Colours::white.withAlpha (0.0f), spec.getRight(), spec.getCentreY(), true));
        g.fillEllipse (spec);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.strokePath (cap, juce::PathStrokeType (0.8f));
    }

    // Cubo redondo bajo un selector "cabeza de gallo"
    inline void bakeliteHub (juce::Graphics& g, juce::Rectangle<float> base)
    {
        const auto c = base.getCentre();
        const float r = base.getWidth() * 0.5f * 0.46f;
        auto hub = juce::Rectangle<float> (r * 2, r * 2).withCentre (c);
        juce::Path p; p.addEllipse (hub);
        juce::DropShadow (juce::Colours::black.withAlpha (0.8f), juce::roundToInt (r * 0.5f), { 0, juce::roundToInt (r * 0.25f) }).drawForPath (g, p);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3634), c.x - r * 0.4f, c.y - r * 0.5f, juce::Colour (0xff050404), c.x + r * 0.6f, c.y + r * 0.8f, true));
        g.fillEllipse (hub);
        g.setColour (juce::Colours::black);
        g.drawEllipse (hub, 1.0f);
    }

    // Base moleteada (estatica: la luz no se mueve al girar)
    inline void knurledBase (juce::Graphics& g, juce::Rectangle<float> base)
    {
        const auto c = base.getCentre();
        const float r = base.getWidth() * 0.5f;

        // Sombra proyectada
        juce::Path disc;
        disc.addEllipse (base);
        juce::DropShadow (juce::Colours::black.withAlpha (0.75f), juce::roundToInt (r * 0.35f), { 0, juce::roundToInt (r * 0.16f) }).drawForPath (g, disc);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3a3c), c.x, base.getY(),
                                                 juce::Colour (0xff0b0b0c), c.x, base.getBottom(), false));
        g.fillEllipse (base);

        // Estrias
        const int ridges = 56;
        for (int i = 0; i < ridges; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) ridges;
            const auto p1 = c.getPointOnCircumference (r * 0.995f, a);
            const auto p2 = c.getPointOnCircumference (r * 0.80f, a);
            const float light = 0.5f + 0.5f * std::cos (a);  // mas luz arriba
            g.setColour (juce::Colours::white.withAlpha (0.05f + 0.13f * light));
            g.drawLine ({ p1, p2 }, 1.1f);
            const auto q1 = c.getPointOnCircumference (r * 0.995f, a + 0.04f);
            const auto q2 = c.getPointOnCircumference (r * 0.80f, a + 0.04f);
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.drawLine ({ q1, q2 }, 1.0f);
        }

        // Brillo superior y borde
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.22f), c.x, base.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), c.x, c.y, false));
        g.drawEllipse (base.reduced (0.8f), 1.4f);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawEllipse (base, 1.0f);

        // Escalon interior (sombra de la tapa)
        const float capR = r * 0.74f;
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillEllipse (juce::Rectangle<float> (capR * 2 + 4, capR * 2 + 4).withCentre (c.translated (0, 1.5f)));
    }
}

class VintageKnobLnf : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        const bool selector = s.getProperties().contains ("selector");
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        const float size = juce::jmin (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (size, size).reduced (size * 0.12f);
        const auto c = r.getCentre();
        const float radius = r.getWidth() * 0.5f;
        const float angle = startAngle + pos * (endAngle - startAngle);

        // Capa estatica cacheada (sombra, base, tapa)
        const bool bakelite = s.getProperties().contains ("bakelite");
        const int key = juce::roundToInt (size) * 8 + (selector ? 1 : 0) + (s.getProperties().contains ("black") ? 2 : 0) + (bakelite ? 4 : 0);
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        auto& img = cache[key];
        if (img.isNull() || std::abs (cachedScale[key] - scale) > 0.01f)
        {
            const int px = juce::roundToInt (size * scale);
            img = juce::Image (juce::Image::ARGB, px, px, true);
            juce::Graphics ig (img);
            ig.addTransform (juce::AffineTransform::scale (scale));
            auto local = juce::Rectangle<float> (size, size).reduced (size * 0.12f);
            if (bakelite)
            {
                if (selector) KnobRender::bakeliteHub (ig, local);
                else          KnobRender::bakeliteSkirt (ig, local);
            }
            else
            {
                KnobRender::knurledBase (ig, local);
                const bool black = s.getProperties().contains ("black");
                auto capR = local.reduced (local.getWidth() * (selector ? 0.30f : 0.13f));
                if (black) KnobRender::blackCap (ig, capR);
                else       KnobRender::brushedCap (ig, capR);
            }
            cachedScale[key] = scale;
        }
        g.drawImage (img, juce::Rectangle<float> (size, size).withCentre (c));

        if (bakelite && ! selector)
        {
            // Indice blanco grabado en la falda, de la tapa al borde
            const auto p1 = c.getPointOnCircumference (radius * 0.70f, angle);
            const auto p2 = c.getPointOnCircumference (radius * 0.97f, angle);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawLine ({ p1.translated (0, 1), p2.translated (0, 1) }, 3.4f);
            g.setColour (Colours::cream);
            g.drawLine ({ p1, p2 }, 2.4f);
        }
        else if (! selector)
        {
            // Linea grabada en la tapa + punto crema en la base
            const float capR = radius * 0.74f;
            const auto p1 = c.getPointOnCircumference (capR * 0.22f, angle);
            const auto p2 = c.getPointOnCircumference (capR * 0.92f, angle);
            const bool black = s.getProperties().contains ("black");
            g.setColour (juce::Colours::black.withAlpha (0.65f));
            g.drawLine ({ p1.translated (0, 1), p2.translated (0, 1) }, 3.2f);
            const bool red = s.getProperties().contains ("redIndex");
            g.setColour (black ? Colours::cream : (red ? juce::Colour (0xffd7212b) : juce::Colour (0xff2b2b2b)));
            g.drawLine ({ p1, p2 }, 2.4f);
            const auto dot = c.getPointOnCircumference (radius * 0.89f, angle);
            g.setColour (red ? juce::Colour (0xffff3a44) : Colours::cream);
            g.fillEllipse (juce::Rectangle<float> (radius * 0.13f, radius * 0.13f).withCentre (dot));
        }
        else
        {
            // Aleta de aluminio que gira; la luz se calcula en coordenadas de pantalla
            const float len = radius * 0.98f, wid = radius * (bakelite ? 0.50f : 0.30f);
            juce::Path fin;
            if (bakelite)
            {
                // "Cabeza de gallo": punta afilada, cuerpo ancho y cola redondeada
                fin.startNewSubPath (0.0f, -len);
                fin.quadraticTo (wid * 0.30f, -len * 0.55f, wid * 0.5f, -len * 0.05f);
                fin.quadraticTo (wid * 0.62f, len * 0.50f, 0.0f, len * 0.62f);
                fin.quadraticTo (-wid * 0.62f, len * 0.50f, -wid * 0.5f, -len * 0.05f);
                fin.quadraticTo (-wid * 0.30f, -len * 0.55f, 0.0f, -len);
            }
            else
            {
                fin.startNewSubPath (-wid * 0.5f, len * 0.62f);
                fin.lineTo (-wid * 0.5f, -len * 0.55f);
                fin.lineTo (0.0f, -len);
                fin.lineTo (wid * 0.5f, -len * 0.55f);
                fin.lineTo (wid * 0.5f, len * 0.62f);
                fin.quadraticTo (0.0f, len * 0.74f, -wid * 0.5f, len * 0.62f);
            }
            fin.closeSubPath();
            fin.applyTransform (juce::AffineTransform::rotation (angle).translated (c));

            juce::DropShadow (juce::Colours::black.withAlpha (0.7f), juce::roundToInt (radius * 0.18f), { 0, juce::roundToInt (radius * 0.1f) }).drawForPath (g, fin);
            const bool black = s.getProperties().contains ("black");
            if (black)
                g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4644), c.x - radius * 0.5f, c.y - radius * 0.6f,
                                                         juce::Colour (0xff050404), c.x + radius * 0.6f, c.y + radius * 0.7f, false));
            else
                g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe9ebee), c.x - radius * 0.5f, c.y - radius * 0.6f,
                                                         juce::Colour (0xff6f7379), c.x + radius * 0.6f, c.y + radius * 0.7f, false));
            g.fillPath (fin);
            g.setColour (juce::Colours::white.withAlpha (black ? 0.25f : 0.55f));
            g.strokePath (fin, juce::PathStrokeType (1.0f));
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.strokePath (fin, juce::PathStrokeType (0.6f));

            // Linea grabada en la punta
            const auto t1 = c.getPointOnCircumference (len * (bakelite ? 0.18f : 0.25f), angle);
            const auto t2 = c.getPointOnCircumference (len * (bakelite ? 0.90f : 0.86f), angle);
            g.setColour (black ? Colours::cream : juce::Colour (0xff1e1e1e));
            g.drawLine ({ t1, t2 }, 1.8f);
        }
    }

private:
    std::map<int, juce::Image> cache;
    std::map<int, float> cachedScale;
};

// Perilla completa con escala serigrafiada alrededor y nombre debajo
class VintageKnob : public juce::Component
{
public:
    VintageKnob (const juce::String& name, VintageKnobLnf& lnf, bool isSelector = false)
        : title (name), selector (isSelector)
    {
        slider.setLookAndFeel (&lnf);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (juce::degreesToRadians (215.0f), juce::degreesToRadians (505.0f), true);
        slider.setMouseDragSensitivity (selector ? 120 : 260);
        if (selector) slider.getProperties().set ("selector", true);
        slider.onDragStart = [this] { dragging = true; repaint(); };
        slider.onDragEnd = [this] { dragging = false; repaint(); };
        slider.onValueChange = [this] { if (dragging) repaint(); };
        addAndMakeVisible (slider);
    }

    ~VintageKnob() override { slider.setLookAndFeel (nullptr); }

    void attach (juce::AudioProcessorValueTreeState& s, const juce::String& id)
    {
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, id, slider);
        param = s.getParameter (id);
        if (param != nullptr)
            slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

        scaleLabels.clear();
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
            scaleLabels = choice->choices;
        else
            for (int i = 0; i <= 10; i += 2) scaleLabels.add (juce::String (i));
    }

    void setScaleLabels (const juce::StringArray& labels) { scaleLabels = labels; repaint(); }

    // Etiquetas de los extremos (por ejemplo "SHARP" / "BROAD")
    void setEndLabels (const juce::String& left, const juce::String& right) { endLeft = left; endRight = right; }

    void resized() override
    {
        auto r = getLocalBounds();
        const int s = juce::jmin (r.getWidth(), r.getHeight() - 24);
        auto group = r.withSizeKeepingCentre (r.getWidth(), s + 24);
        knobArea = group.removeFromTop (s).withSizeKeepingCentre (s, s);
        nameArea = group.translated (0, -juce::roundToInt (s * 0.06f));
        const int k = juce::roundToInt (s * 0.74f);
        slider.setBounds (knobArea.withSizeKeepingCentre (k, k));
    }

    void paint (juce::Graphics& g) override
    {
        const auto c = knobArea.toFloat().getCentre();
        const float kr = slider.getWidth() * 0.5f * 0.76f;     // radio visual de la perilla
        const float start = juce::degreesToRadians (215.0f), end = juce::degreesToRadians (505.0f);
        const auto ink = inkColour;

        const int n = scaleLabels.size();
        const bool isChoice = selector;
        const int ticks = isChoice ? n : 11;

        // Marcas de escala
        for (int i = 0; i < ticks; ++i)
        {
            const float a = start + (end - start) * (float) i / (float) juce::jmax (1, ticks - 1);
            const bool major = isChoice || (i % 2 == 0);
            const auto p1 = c.getPointOnCircumference (kr * 1.16f, a);
            const auto p2 = c.getPointOnCircumference (kr * (major ? 1.30f : 1.24f), a);
            g.setColour (ink.withAlpha (major ? 0.85f : 0.55f));
            g.drawLine ({ p1, p2 }, major ? 1.6f : 1.0f);
        }

        // Numeros / valores
        g.setFont (engraveFont (isChoice ? 12.0f : 11.5f));
        for (int i = 0; i < n; ++i)
        {
            const float a = start + (end - start) * (float) i / (float) juce::jmax (1, n - 1);
            const auto p = c.getPointOnCircumference (kr * 1.52f, a);
            g.setColour (ink.withAlpha (0.92f));
            g.drawText (scaleLabels[i], juce::Rectangle<float> (40, 14).withCentre (p), juce::Justification::centred);
        }
        if (endLeft.isNotEmpty())
        {
            g.setFont (engraveFont (9.5f));
            g.setColour (ink.withAlpha (0.7f));
            g.drawText (endLeft, juce::Rectangle<float> (50, 12).withCentre (c.getPointOnCircumference (kr * 1.55f, start).translated (-4, 14)), juce::Justification::centred);
            g.drawText (endRight, juce::Rectangle<float> (50, 12).withCentre (c.getPointOnCircumference (kr * 1.55f, end).translated (4, 14)), juce::Justification::centred);
        }

        // Nombre grabado; mientras se gira, en su lugar se muestra el valor exacto
        const bool showValue = dragging && param != nullptr;
        if (showValue)
        {
            const auto txt = param->getCurrentValueAsText();
            g.setFont (monoFont (juce::jmax (11.0f, nameFontSize)));
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), txt) + 14.0f;
            auto box = juce::Rectangle<float> (w, 17.0f).withCentre (nameArea.toFloat().getCentre());
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (Colours::lamp);
            g.drawText (txt, box, juce::Justification::centred);
        }
        else if (drawName)
        {
            g.setFont (engraveFont (nameFontSize, 0.14f));
            g.setColour (inkColour.getPerceivedBrightness() < 0.5f ? juce::Colours::white.withAlpha (0.35f) : juce::Colours::black.withAlpha (0.35f));
            g.drawText (title, nameArea.toFloat().translated (0, 1), juce::Justification::centred);
            g.setColour (ink);
            g.drawText (title, nameArea.toFloat(), juce::Justification::centred);
        }
    }

    juce::Slider slider;
    juce::Colour inkColour = Colours::cream;

    // Geometria para dibujar el dial de metal detras de la perilla (en coordenadas del padre)
    juce::Point<float> getDialCentreInParent() const { return knobArea.toFloat().getCentre() + getPosition().toFloat(); }
    float getDialRadius() const { return (float) slider.getWidth() * 0.5f * 0.76f * 1.74f; }
    const juce::String& getTitle() const { return title; }
    bool drawName = true;
    float nameFontSize = 14.0f;

    // Baquelita negra (falda + cabeza de gallo) con tinta crema sobre el panel del rack
    void setBakeliteStyle()
    {
        slider.getProperties().set ("bakelite", true);
        if (selector) slider.getProperties().set ("black", true);
        inkColour = Colours::cream;
        drawName = true;
        nameFontSize = 11.5f;
        repaint();
    }
    juce::Rectangle<float> getNameAreaInParent() const { return (nameArea + getPosition()).toFloat(); }

    // Estilo sobre dial oscuro: perillas de aluminio con indice rojo; selectores negros tipo "cabeza de gallo"
    void setDarkDialStyle()
    {
        if (selector) slider.getProperties().set ("black", true);
        else { slider.getProperties().remove ("black"); slider.getProperties().set ("redIndex", true); }
        inkColour = Colours::cream;
        drawName = false;
        repaint();
    }

    // Estilo amplificador: tapas negras y tinta oscura sobre el panel metalico
    void setAmpStyle()
    {
        slider.getProperties().set ("black", true);
        inkColour = juce::Colour (0xff1d1812);
        repaint();
    }

private:
    juce::String title, endLeft, endRight;
    bool selector = false, dragging = false;
    juce::RangedAudioParameter* param = nullptr;
    juce::StringArray scaleLabels;
    juce::Rectangle<int> knobArea, nameArea;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};
} // namespace gaux
