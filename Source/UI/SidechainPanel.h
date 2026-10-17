// Gaux Equalizer - Sidechain general: boton de la cabecera y su panel de ajustes
#pragma once

#include "Widgets.h"
#include "../PluginProcessor.h"
#include "FrameClock.h"

namespace gaux
{
// Boton "SC" que muestra la senal del sidechain cuando llega
class ScChip : public Chip
{
public:
    ScChip() : Chip ("SC") {}
    float level = 0.0f;      // 0..1 nivel del sidechain
    float duck = 0.0f;       // 0..1 cuanto se encoge la curva
    bool signal = false;

    void paintButton (juce::Graphics& g, bool hover, bool down) override
    {
        Chip::paintButton (g, hover, down);
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        // Punto: gris sin senal, color cuando llega audio de la otra pista
        const auto dot = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ r.getX() + 11.0f, r.getCentreY() });
        g.setColour (signal ? accent.withAlpha (0.4f + 0.6f * level) : Colours::textDim.withAlpha (0.4f));
        g.fillEllipse (dot);
        // Barra de "duck" abajo
        if (selected && duck > 0.01f)
        {
            g.setColour (Colours::dynamic);
            g.fillRoundedRectangle (juce::Rectangle<float> ((r.getWidth() - 16.0f) * duck, 2.0f).withPosition (r.getX() + 8.0f, r.getBottom() - 4.0f), 1.0f);
        }
    }
};

// Panel que se abre al hacer clic en SC
class SidechainPanel : public juce::Component, private juce::Timer
{
public:
    SidechainPanel (GauxEqProcessor& p, bool vintage) : proc (p), vintageLook (vintage)
    {
        onChip.setTooltip ("Prende el sidechain general");
        onChip.onClick = [this]
        {
            setParamValue (proc.apvts, ParamID::scOn, proc.params.scOn->load() > 0.5f ? 0.0f : 1.0f);
            proc.history.commit();
        };
        addAndMakeVisible (onChip);
        duck.attach (p.apvts, ParamID::scDuck);
        thr.attach (p.apvts, ParamID::scThr);
        rel.attach (p.apvts, ParamID::scRel);
        duck.slider.setTooltip ("Cuanto se aplana la curva del EQ cuando suena el sidechain (100 % = queda plana)");
        thr.slider.setTooltip ("Nivel del sidechain a partir del cual actua (unos 8 dB debajo del pico del kick: la curva se aplana del todo en cada golpe)");
        rel.slider.setTooltip ("Cuanto tarda la curva en volver despues de cada golpe (corto = vuelve rapido entre kicks)");
        for (auto* k : { &duck, &thr, &rel }) { k->setAccent (Colours::dynamic); addAndMakeVisible (*k); }
        setSize (390, 236);
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.fillAll (Colours::glass);
        g.setColour (Colours::text);
        g.setFont (displayFont (15.0f, 0.12f));
        g.drawText ("SIDECHAIN", r.withHeight (34).withTrimmedLeft (16), juce::Justification::centredLeft);

        // Medidor: nivel del sidechain y su threshold
        auto m = meterArea.toFloat();
        g.setColour (Colours::ink);
        g.fillRoundedRectangle (m, 3.0f);
        auto toX = [&] (float db) { return m.getX() + m.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
        const float lvl = proc.sidechainDb.load(), t = proc.params.scThr->load();
        g.setColour (Colours::dynamic.withAlpha (0.35f));
        g.fillRoundedRectangle (m.withRight (juce::jmin (toX (lvl), toX (t))), 3.0f);
        if (toX (lvl) > toX (t)) { g.setColour (Colours::dynamic); g.fillRect (m.withLeft (toX (t)).withRight (toX (lvl))); }
        g.setColour (juce::Colours::white);
        g.fillRect (juce::Rectangle<float> (toX (t) - 1.0f, m.getY() - 3.0f, 2.0f, m.getHeight() + 6.0f));

        g.setFont (uiFont (11.5f));
        const bool sig = proc.sidechainActive.load();
        g.setColour (sig ? Colours::dynamic : Colours::textDim);
        g.drawText (sig ? juce::String ("Senal recibida   -   curva al ") + juce::String (juce::roundToInt ((1.0f - proc.scDuckNow.load()) * 100.0f)) + " %"
                        : juce::String ("Sin senal: en Ableton elige la pista en Sidechain > Audio From"),
                    statusArea, juce::Justification::centredLeft);

        g.setColour (Colours::textDim.withAlpha (0.85f));
        g.setFont (uiFont (10.5f));
        g.drawFittedText (vintageLook ? "Cuando suena la otra pista (por ejemplo el kick), toda la curva del Vintage se aplana y vuelve al soltar."
                                      : "Cuando suena la otra pista (por ejemplo el kick), toda la curva se aplana y vuelve al soltar. Ademas, todas las bandas dinamicas escuchan al sidechain.",
                          helpArea, juce::Justification::topLeft, 3);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 0);
        auto head = r.removeFromTop (34);
        onChip.setBounds (head.removeFromRight (64).withSizeKeepingCentre (64, 24));
        meterArea = r.removeFromTop (12).reduced (0, 2);
        statusArea = r.removeFromTop (22);
        auto knobs = r.removeFromTop (110);
        const int kw = knobs.getWidth() / 3;
        duck.setBounds (knobs.removeFromLeft (kw).reduced (8, 0));
        thr.setBounds (knobs.removeFromLeft (kw).reduced (8, 0));
        rel.setBounds (knobs.reduced (8, 0));
        helpArea = r.reduced (0, 4).toNearestInt();
    }

private:
    void timerCallback() override
    {
        const bool on = proc.params.scOn->load() > 0.5f;
        if (onChip.selected != on) { onChip.selected = on; onChip.setButtonText (on ? "ON" : "OFF"); onChip.repaint(); }
        const bool sig = proc.sidechainActive.load();
        const int pct = juce::roundToInt ((1.0f - proc.scDuckNow.load()) * 100.0f);
        if (sig != lastSig || (sig && pct != lastPct)) { lastSig = sig; lastPct = pct; repaint (statusArea); }
    }

    // Medidor al ritmo del monitor (solo si se movio)
    void frame (float)
    {
        const float lvl = proc.sidechainDb.load(), t = proc.params.scThr->load();
        if (std::abs (lvl - lastLvl) > 0.1f || t != lastThr) { lastLvl = lvl; lastThr = t; repaint (meterArea.expanded (2, 6)); }
    }
    float lastLvl = -200.0f, lastThr = 0.0f;
    bool lastSig = false;
    int lastPct = -1;
    FrameClock clock { *this, [this] (float dt) { frame (dt); } };

    GauxEqProcessor& proc;
    bool vintageLook;
    Chip onChip { "OFF" };
    ModernKnob duck { "DUCK" }, thr { "THRESH" }, rel { "RELEASE" };
    juce::Rectangle<int> meterArea, statusArea, helpArea;
};
} // namespace gaux
