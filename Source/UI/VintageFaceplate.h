// Gaux Equalizer - Cara del modo Vintage: amplificador de tubos marca GAUX (modelo GX-1)
#pragma once

#include "VintageKnob.h"
#include "Logo.h"
#include "Widgets.h"
#include "../PluginProcessor.h"

namespace gaux
{
// Boton metalico con LED (para "AVANZADO")
class HardwareButton : public juce::Button
{
public:
    explicit HardwareButton (const juce::String& t) : juce::Button (t) { setClickingTogglesState (true); }

    void paintButton (juce::Graphics& g, bool hover, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        auto led = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.3f);
        auto cap = r.reduced (2.0f, 0.0f);

        juce::Path shape;
        shape.addRoundedRectangle (cap, 4.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.7f), down ? 2 : 5, { 0, down ? 1 : 3 }).drawForPath (g, shape);
        g.setGradientFill (juce::ColourGradient (juce::Colour (down ? 0xff9a9ea3 : 0xffd4d7db), cap.getX(), cap.getY(),
                                                 juce::Colour (0xff6c7076), cap.getX(), cap.getBottom(), false));
        g.fillPath (shape);
        g.setColour (juce::Colours::white.withAlpha (hover ? 0.6f : 0.4f));
        g.drawRoundedRectangle (cap.reduced (0.6f), 4.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (cap, 4.0f, 0.8f);
        g.setFont (engraveFont (12.0f, 0.16f));
        g.setColour (juce::Colour (0xff2a2a2a));
        g.drawText (getButtonText(), cap.translated (0, down ? 1.0f : 0.0f), juce::Justification::centred);

        // LED
        const bool on = getToggleState();
        if (on)
        {
            g.setGradientFill (juce::ColourGradient (Colours::lamp.withAlpha (0.5f), led.getCentreX(), led.getCentreY(),
                                                     Colours::lamp.withAlpha (0.0f), led.getCentreX() + led.getWidth() * 1.4f, led.getCentreY(), true));
            g.fillEllipse (led.expanded (led.getWidth() * 0.9f));
        }
        g.setGradientFill (juce::ColourGradient (on ? juce::Colour (0xffffe0a0) : juce::Colour (0xff5a4020), led.getCentreX() - 2, led.getY() + 2,
                                                 on ? Colours::lamp : juce::Colour (0xff1e150b), led.getRight(), led.getBottom(), true));
        g.fillEllipse (led);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawEllipse (led, 1.0f);
    }
};

// Interruptor de palanca metalico (encendido del equipo)
class ToggleSwitch : public juce::Component
{
public:
    bool on = true;
    std::function<void()> onFlip;

    void mouseDown (const juce::MouseEvent&) override { if (onFlip) onFlip(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const auto c = r.getCentre();
        const float s = juce::jmin (r.getWidth(), r.getHeight());

        // Placa y tuerca hexagonal
        auto plateR = juce::Rectangle<float> (s * 0.5f, s * 0.86f).withCentre (c);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffd0d3d6), plateR.getX(), plateR.getY(), juce::Colour (0xff6d7177), plateR.getRight(), plateR.getBottom(), false));
        g.fillRoundedRectangle (plateR, 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (plateR, 4.0f, 1.0f);
        juce::Path hex;
        hex.addPolygon (c, 6, s * 0.17f, 0.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffeceef0), c.x - s * 0.15f, c.y - s * 0.15f, juce::Colour (0xff5d6167), c.x + s * 0.15f, c.y + s * 0.15f, false));
        g.fillPath (hex);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.strokePath (hex, juce::PathStrokeType (0.8f));

        // Palanca (bat) arriba = ON, abajo = OFF, con sombra
        const float dir = on ? -1.0f : 1.0f;
        const auto tip = c.translated (0, dir * s * 0.40f);
        juce::Path lever;
        lever.startNewSubPath (c.translated (-s * 0.045f, 0));
        lever.lineTo (tip.translated (-s * 0.075f, 0));
        lever.lineTo (tip.translated (s * 0.075f, 0));
        lever.lineTo (c.translated (s * 0.045f, 0));
        lever.closeSubPath();
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillPath (lever, juce::AffineTransform::translation (3, 4));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff4f5f6), c.x - s * 0.06f, c.y, juce::Colour (0xff7b7f85), c.x + s * 0.06f, c.y, false));
        g.fillPath (lever);
        auto knob = juce::Rectangle<float> (s * 0.17f, s * 0.17f).withCentre (tip);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white, knob.getX(), knob.getY(), juce::Colour (0xff80848a), knob.getRight(), knob.getBottom(), true));
        g.fillEllipse (knob);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.drawEllipse (knob, 0.8f);
    }
};

class VintageFaceplate : public juce::Component, private juce::Timer
{
public:
    explicit VintageFaceplate (GauxEqProcessor& p) : proc (p)
    {
        auto& s = p.apvts;
        auto setup = [&] (VintageKnob& k, const juce::String& id)
        {
            k.attach (s, id);
            k.setBakeliteStyle();
            addAndMakeVisible (k);
        };
        // PROGRAM
        setup (lowFreq, ParamID::vLowFreq);   setup (lowPush, ParamID::vLowBoost);  setup (lowCut, ParamID::vLowAtten);
        setup (highFreq, ParamID::vHighFreq); setup (highPush, ParamID::vHighBoost); setup (highWidth, ParamID::vHighBw);
        setup (softFreq, ParamID::vAttenFreq); setup (soften, ParamID::vHighAtten);
        setup (airFreq, ParamID::vAirFreq);   setup (air, ParamID::vAirGain);
        setup (glow, ParamID::vTube);         setup (iron, ParamID::vIron);
        // MID
        setup (lmFreq, ParamID::vLmFreq);  setup (lmBoost, ParamID::vLmBoost);
        setup (dipFreq, ParamID::vDipFreq); setup (dip, ParamID::vDip);
        setup (hmFreq, ParamID::vHmFreq);  setup (hmBoost, ParamID::vHmBoost);
        // FILTER + salida
        setup (lowCutSel, ParamID::vLowCut); setup (highCutSel, ParamID::vHighCut);
        setup (output, ParamID::output);
        highWidth.setEndLabels ("SHARP", "BROAD");
        output.setScaleLabels ({ "-24", "-12", "0", "+12", "+24" });
        glow.setScaleLabels ({ "0", "20", "40", "60", "80", "100" });
        iron.setScaleLabels ({ "0", "20", "40", "60", "80", "100" });

        auto setupIn = [&] (ToggleSwitch& t, const juce::String& id)
        {
            addAndMakeVisible (t);
            t.onFlip = [this, id] { auto* prm = proc.apvts.getParameter (id); setParamValue (proc.apvts, id, prm->getValue() > 0.5f ? 0.0f : 1.0f); };
        };
        setupIn (programIn, ParamID::vProgramIn);
        setupIn (midIn, ParamID::vMidIn);
        setupIn (filterIn, ParamID::vFilterIn);

        addAndMakeVisible (powerSwitch);
        powerSwitch.onFlip = [this] { setParamValue (proc.apvts, ParamID::bypass, proc.params.bypass->load() > 0.5f ? 0.0f : 1.0f); };
        addAndMakeVisible (advanced);
        advanced.setTooltip ("Muestra la curva del EQ y el analizador");
        advanced.onClick = [this] { if (onAdvancedToggled) onAdvancedToggled (advanced.getToggleState()); };
    }

    ~VintageFaceplate() override { stopTimer(); }

    void visibilityChanged() override { if (isVisible() && getParentComponent() != nullptr) startTimerHz (15); else stopTimer(); }
    void parentHierarchyChanged() override { visibilityChanged(); }

    std::function<void (bool)> onAdvancedToggled;
    HardwareButton advanced { "AVANZADO" };

    void resized() override
    {
        panelImage = {};
        sections.clear();
        auto r = getLocalBounds().reduced (10, 6);
        const int gap = 6;
        // compacto (con la pantalla AVANZADO abierta debajo): mismas perillas, un poco mas pequenas
        const bool compact = getHeight() < 650;
        units[0] = { "FILTER", "HIGH / LOW PASS", r.removeFromTop (compact ? 138 : 150) };
        r.removeFromTop (gap);
        units[1] = { "MID", "MID-RANGE EQUALIZER", r.removeFromTop (compact ? 150 : 182) };
        r.removeFromTop (gap);
        units[2] = { "PROGRAM", "PROGRAM EQUALIZER", r };

        for (int i = 0; i < 3; ++i)
        {
            auto c = content (i);
            auto lab = c.removeFromLeft (kLabelW);
            auto sw = lab.withTrimmedTop (lab.getHeight() - 58).removeFromLeft (lab.getWidth() / 2);
            ToggleSwitch* t = i == 0 ? &filterIn : (i == 1 ? &midIn : &programIn);
            t->setBounds (sw.withSizeKeepingCentre (28, 42).translated (4, -6));
            lamps[i] = juce::Rectangle<float> (18, 18).withCentre ({ (float) lab.getRight() - lab.getWidth() * 0.27f, (float) t->getBounds().getCentreY() });
        }

        // ---- FILTER: LOW CUT | logo | HIGH CUT | OUTPUT | encendido ----
        {
            auto c = content (0).withTrimmedLeft (kLabelW + 8);
            const int kw = 128;
            lowCutSel.setBounds (c.removeFromLeft (kw));
            auto pw = c.removeFromRight (170);
            output.setBounds (c.removeFromRight (kw));
            c.removeFromRight (8);
            highCutSel.setBounds (c.removeFromRight (kw));
            logoArea = c.reduced (14, 18).toFloat();
            const float lh = juce::jmin (logoArea.getHeight(), logoArea.getWidth() / juce::jmax (0.1f, Logo::ampBadgeWidth (1.0f)));
            logoArea = logoArea.withSizeKeepingCentre (Logo::ampBadgeWidth (lh), lh);

            auto top = pw.removeFromTop (pw.getHeight() - 44);
            advanced.setBounds (pw.withSizeKeepingCentre (156, 32).translated (0, 2));
            auto l = top.removeFromLeft (top.getWidth() / 2);
            powerSwitch.setBounds (l.withSizeKeepingCentre (36, 46).translated (0, -6));
            jewelArea = top.toFloat().withSizeKeepingCentre (30, 30).translated (0, -6);
        }

        // ---- MID: LOW PEAK | DIP | HIGH PEAK ----
        {
            auto c = content (1).withTrimmedLeft (kLabelW + 8).reduced (0, 4);
            const int sw = c.getWidth() / 3;
            const char* titles[] = { "LOW PEAK", "DIP", "HIGH PEAK" };
            VintageKnob* ks[3][2] = { { &lmFreq, &lmBoost }, { &dipFreq, &dip }, { &hmFreq, &hmBoost } };
            for (int i = 0; i < 3; ++i)
            {
                auto sec = c.removeFromLeft (sw);
                sections.add ({ titles[i], sec.removeFromTop (24) });
                const int w = sec.getWidth() / 2;
                ks[i][0]->setBounds (sec.removeFromLeft (w));
                ks[i][1]->setBounds (sec);
            }
        }

        // ---- PROGRAM: dos filas (ganancias arriba, frecuencias y forma abajo) ----
        {
            auto c = content (2).withTrimmedLeft (kLabelW + 8).reduced (0, 4);
            const int col = c.getWidth() / 7;
            struct Sec { const char* title; int cols; VintageKnob* top[2]; VintageKnob* bottom; };
            Sec secs[] = { { "LOW FREQUENCY", 2, { &lowPush, &lowCut }, &lowFreq },
                           { "HIGH FREQUENCY", 2, { &highPush, &highWidth }, &highFreq },
                           { "SOFTEN", 1, { &soften, nullptr }, &softFreq },
                           { "AIR", 1, { &air, nullptr }, &airFreq },
                           { "TUBE", 1, { &glow, nullptr }, &iron } };
            for (auto& sc : secs)
            {
                auto sec = c.removeFromLeft (col * sc.cols);
                sections.add ({ sc.title, sec.removeFromTop (24) });
                auto rowTop = sec.removeFromTop (sec.getHeight() / 2);
                if (sc.cols == 2)
                {
                    sc.top[0]->setBounds (rowTop.removeFromLeft (col));
                    sc.top[1]->setBounds (rowTop);
                }
                else sc.top[0]->setBounds (rowTop);
                sc.bottom->setBounds (sec.withSizeKeepingCentre (col, sec.getHeight()));
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (panelImage.isNull()) buildPanel();
        g.drawImage (panelImage, getLocalBounds().toFloat());

        const bool on = proc.params.bypass->load() < 0.5f;
        const bool ins[] = { proc.params.vFilterIn->load() > 0.5f, proc.params.vMidIn->load() > 0.5f, proc.params.vProgramIn->load() > 0.5f };
        for (int i = 0; i < 3; ++i) drawJewel (g, lamps[i], on && ins[i]);
        drawJewel (g, jewelArea, on);
    }

private:
    static constexpr int kEarW = 34, kLabelW = 104;
    struct Unit { juce::String name, sub; juce::Rectangle<int> area; };
    struct Section { juce::String title; juce::Rectangle<int> area; };

    juce::Rectangle<int> content (int i) const { return units[i].area.reduced (kEarW + 6, 8); }

    void timerCallback() override
    {
        // Solo se redibuja lo que cambio (interruptores y pilotos); el rack es estatico
        const bool st[4] = { proc.params.bypass->load() < 0.5f, proc.params.vProgramIn->load() > 0.5f,
                             proc.params.vMidIn->load() > 0.5f, proc.params.vFilterIn->load() > 0.5f };
        if (st[0] == powerSwitch.on && st[1] == programIn.on && st[2] == midIn.on && st[3] == filterIn.on && ! firstPoll) return;
        firstPoll = false;
        powerSwitch.on = st[0]; programIn.on = st[1]; midIn.on = st[2]; filterIn.on = st[3];
        for (auto* t : { &powerSwitch, &programIn, &midIn, &filterIn }) t->repaint();
        for (auto& l : lamps) repaint (l.expanded (24).toNearestInt());
        repaint (jewelArea.expanded (24).toNearestInt());
    }

    // Piloto tipo joya roja facetada
    static void drawJewel (juce::Graphics& g, juce::Rectangle<float> area, bool on)
    {
        if (area.isEmpty()) return;
        const auto c = area.getCentre();
        const float r = area.getWidth() * 0.5f;
        if (on)
        {
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffff2a2a).withAlpha (0.55f), c.x, c.y, juce::Colour (0x00ff2a2a), c.x + r * 2.6f, c.y, true));
            g.fillEllipse (area.expanded (r * 1.6f));
        }
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff0f0f0), area.getX(), area.getY(), juce::Colour (0xff4a4a4a), area.getRight(), area.getBottom(), false));
        g.fillEllipse (area.expanded (3));
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawEllipse (area.expanded (3), 1.0f);
        const auto base = on ? juce::Colour (0xffe8141c) : juce::Colour (0xff4a0c0e);
        g.setColour (base);
        g.fillEllipse (area);
        const int facets = 10;
        for (int i = 0; i < facets; ++i)
        {
            juce::Path seg;
            const float a0 = juce::MathConstants<float>::twoPi * i / facets;
            seg.addPieSegment (area, a0, a0 + juce::MathConstants<float>::twoPi / facets, 0.45f);
            g.setColour ((i % 2 == 0 ? juce::Colours::white : juce::Colours::black).withAlpha (on ? 0.16f : 0.25f));
            g.fillPath (seg);
        }
        auto inner = area.reduced (r * 0.55f);
        g.setGradientFill (juce::ColourGradient (on ? juce::Colour (0xffffd0c0) : juce::Colour (0xff6a1a1a), inner.getCentreX(), inner.getCentreY(),
                                                 base, inner.getRight(), inner.getBottom(), true));
        g.fillEllipse (inner);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillEllipse (juce::Rectangle<float> (r * 0.3f, r * 0.22f).withCentre (c.translated (-r * 0.3f, -r * 0.35f)));
    }

    // Todo lo estatico (rack, paneles, serigrafia, logo) en una imagen cacheada
    void buildPanel()
    {
        const float scale = 2.0f;
        const int w = juce::jmax (1, getWidth()), h = juce::jmax (1, getHeight());
        panelImage = juce::Image (juce::Image::ARGB, juce::roundToInt (w * scale), juce::roundToInt (h * scale), true);
        juce::Graphics g (panelImage);
        g.addTransform (juce::AffineTransform::scale (scale));
        juce::Random rnd (1957);

        // Fondo: rieles del rack en negro con grano
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1716), 0, 0, juce::Colour (0xff080707), 0, (float) h, false));
        g.fillAll();
        for (int i = 0; i < 18000; ++i)
        {
            g.setColour ((rnd.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.03f + rnd.nextFloat() * 0.05f));
            g.fillEllipse (rnd.nextFloat() * w, rnd.nextFloat() * h, 1.2f, 1.2f);
        }

        for (int i = 0; i < 3; ++i) drawUnitPlate (g, units[i], rnd);

        // Logo GAUX grande en la unidad superior
        Logo::drawAmpBadge (g, logoArea);

        // Etiquetas de seccion grabadas con filetes
        for (auto& sc : sections)
        {
            auto r = sc.area.toFloat();
            g.setFont (displayFont (12.0f, 0.3f));
            const float tw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), sc.title) + 16.0f;
            g.setColour (Colours::cream.withAlpha (0.92f));
            g.drawText (sc.title, r, juce::Justification::centred);
            g.setColour (Colours::cream.withAlpha (0.45f));
            const float y = r.getCentreY();
            g.fillRect (juce::Rectangle<float> (r.getX() + 10, y, juce::jmax (0.0f, (r.getWidth() - tw) * 0.5f - 10), 1.0f));
            g.fillRect (juce::Rectangle<float> (r.getCentreX() + tw * 0.5f, y, juce::jmax (0.0f, (r.getWidth() - tw) * 0.5f - 10), 1.0f));
        }
        // Separadores verticales finos entre secciones de una misma unidad
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        for (int i = 1; i < sections.size(); ++i)
            if (sections[i].area.getY() == sections[i - 1].area.getY())
            {
                const float x = (float) sections[i].area.getX();
                const auto& u = sections[i].area.getY() < units[2].area.getY() ? units[1].area : units[2].area;
                g.fillRect (juce::Rectangle<float> (x - 0.5f, (float) sections[i].area.getY() + 6, 1.0f, (float) (u.getBottom() - sections[i].area.getY() - 20)));
            }

        // Rotulos de los interruptores
        auto tagBelow = [&] (juce::Component& c, const juce::String& t)
        {
            g.setFont (engraveFont (9.5f, 0.3f));
            g.setColour (Colours::cream.withAlpha (0.85f));
            g.drawText (t, c.getBounds().toFloat().withY ((float) c.getBottom() + 1).withHeight (12).withSizeKeepingCentre (70, 12), juce::Justification::centred);
        };
        tagBelow (programIn, "IN"); tagBelow (midIn, "IN"); tagBelow (filterIn, "IN");
        tagBelow (powerSwitch, "POWER");
    }

    // Una unidad de rack: panel de esmalte rojo sangre de toro martillado, orejas con ranuras y tornillos
    void drawUnitPlate (juce::Graphics& g, const Unit& u, juce::Random& rnd)
    {
        auto r = u.area.toFloat();
        juce::Path pp; pp.addRoundedRectangle (r, 5.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.9f), 14, { 0, 5 }).drawForPath (g, pp);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff5a1a19), r.getX(), r.getY(), juce::Colour (0xff341010), r.getX(), r.getBottom(), false));
        g.fillPath (pp);
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (pp);
            // Textura martillada: manchas suaves claras y oscuras
            for (int i = 0; i < (int) (r.getWidth() * r.getHeight() / 90.0f); ++i)
            {
                const float x = r.getX() + rnd.nextFloat() * r.getWidth(), y = r.getY() + rnd.nextFloat() * r.getHeight();
                const float d = 3.0f + rnd.nextFloat() * 7.0f;
                g.setColour ((rnd.nextBool() ? juce::Colour (0xffff8a70) : juce::Colours::black).withAlpha (0.025f + rnd.nextFloat() * 0.03f));
                g.fillEllipse (x, y, d, d);
            }
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f), r.getX(), r.getY(),
                                                     juce::Colours::white.withAlpha (0.0f), r.getX(), r.getY() + r.getHeight() * 0.5f, false));
            g.fillRect (r);
            // Orejas del rack un poco mas oscuras
            g.setColour (juce::Colours::black.withAlpha (0.22f));
            g.fillRect (r.withWidth ((float) kEarW));
            g.fillRect (r.withLeft (r.getRight() - (float) kEarW));
        }
        g.setColour (juce::Colours::white.withAlpha (0.16f));
        g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.9f));
        g.strokePath (pp, juce::PathStrokeType (1.2f));

        // Ranuras y tornillos de las orejas
        for (float ex : { r.getX() + kEarW * 0.5f, r.getRight() - kEarW * 0.5f })
            for (float ey : { r.getY() + 24.0f, r.getBottom() - 24.0f })
            {
                auto slot = juce::Rectangle<float> (14.0f, 22.0f).withCentre ({ ex, ey });
                g.setColour (juce::Colours::black.withAlpha (0.85f));
                g.fillRoundedRectangle (slot, 7.0f);
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.drawRoundedRectangle (slot.translated (0, 0.8f), 7.0f, 0.8f);
                drawScrew (g, { ex, ey }, 5.2f);
            }

        // Rotulo de la unidad (columna izquierda)
        auto lab = r.reduced ((float) kEarW + 8.0f, 10.0f).withWidth ((float) kLabelW);
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (lab.getRight() + 4, r.getY() + 10, 1.0f, r.getHeight() - 20));
        g.setFont (displayFont (17.0f, 0.12f));
        g.setColour (Colours::cream);
        g.drawText ("GAUX", lab.withHeight (20), juce::Justification::centredLeft);
        g.setFont (engraveFont (9.5f, 0.2f));
        g.setColour (Colours::cream.withAlpha (0.8f));
        g.drawFittedText (u.sub, lab.withTrimmedTop (22).withHeight (26).toNearestInt(), juce::Justification::topLeft, 2);
        g.setFont (engraveFont (9.0f, 0.3f));
        g.setColour (Colours::brass.withAlpha (0.9f));
        g.drawText ("GX-1 " + u.name, lab.withTrimmedTop (48).withHeight (12), juce::Justification::centredLeft);
    }

    static void drawScrew (juce::Graphics& g, juce::Point<float> c, float r)
    {
        auto b = juce::Rectangle<float> (r * 2, r * 2).withCentre (c);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillEllipse (b.translated (0, 1.2f).expanded (0.8f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe8e2d4), b.getX(), b.getY(), juce::Colour (0xff6a6458), b.getRight(), b.getBottom(), false));
        g.fillEllipse (b);
        g.setColour (juce::Colour (0xff2a2a2a));
        const float s = r * 0.62f;
        g.drawLine (c.x - s, c.y - s * 0.3f, c.x + s, c.y + s * 0.3f, 1.2f);
        g.drawLine (c.x - s * 0.3f, c.y + s, c.x + s * 0.3f, c.y - s, 1.2f);
    }

    GauxEqProcessor& proc;
    VintageKnobLnf knobLnf;
    VintageKnob lowFreq { "FREQ", knobLnf, true }, lowPush { "PUSH", knobLnf }, lowCut { "CUT", knobLnf };
    VintageKnob highFreq { "FREQ", knobLnf, true }, highPush { "PUSH", knobLnf }, highWidth { "WIDTH", knobLnf };
    VintageKnob softFreq { "FREQ", knobLnf, true }, soften { "SOFTEN", knobLnf };
    VintageKnob airFreq { "FREQ", knobLnf, true }, air { "AIR", knobLnf };
    VintageKnob glow { "GLOW", knobLnf }, iron { "IRON", knobLnf };
    VintageKnob lmFreq { "FREQ", knobLnf, true }, lmBoost { "PUSH", knobLnf };
    VintageKnob dipFreq { "FREQ", knobLnf, true }, dip { "CUT", knobLnf };
    VintageKnob hmFreq { "FREQ", knobLnf, true }, hmBoost { "PUSH", knobLnf };
    VintageKnob lowCutSel { "LOW CUT", knobLnf, true }, highCutSel { "HIGH CUT", knobLnf, true };
    VintageKnob output { "OUTPUT", knobLnf };
    ToggleSwitch powerSwitch, programIn, midIn, filterIn;
    Unit units[3];
    juce::Array<Section> sections;
    juce::Rectangle<float> lamps[3], logoArea, jewelArea;
    bool firstPoll = true;
    juce::Image panelImage;
};
} // namespace gaux
