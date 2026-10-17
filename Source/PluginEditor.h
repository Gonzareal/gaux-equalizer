// Gaux Equalizer - Ventana principal del plugin
#pragma once

#include "PluginProcessor.h"
#include "UI/GauxLookAndFeel.h"
#include "UI/Widgets.h"
#include "UI/Logo.h"
#include "UI/Mascot.h"
#include "UI/SpectrumDisplay.h"
#include "UI/BandEditor.h"
#include "UI/VintageFaceplate.h"
#include "UI/PresetBar.h"
#include "UI/SidechainPanel.h"
#include "UI/MatchPanel.h"
#include "UI/PianoStrip.h"

namespace gaux
{
// Selector de modo (se dibuja distinto segun el modo activo)
class ModeSwitch : public juce::Component
{
public:
    std::function<void (int)> onChange;
    int mode = 0;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool v = mode == 1;
        const float rad = r.getHeight() * 0.5f;
        g.setColour (v ? juce::Colour (0xff0d0e0f) : Colours::ink);
        g.fillRoundedRectangle (r, rad);
        g.setColour (v ? juce::Colour (0xff3a3c3e) : Colours::lineHi);
        g.drawRoundedRectangle (r, rad, 1.0f);

        auto half = r.reduced (3.0f);
        auto sel = mode == 0 ? half.removeFromLeft (half.getWidth() * 0.5f) : half.removeFromRight (half.getWidth() * 0.5f);
        if (v)
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff7a2a22), sel.getX(), sel.getY(), juce::Colour (0xff3a0f0c), sel.getX(), sel.getBottom(), false));
        else
        {
            juce::DropShadow (Colours::accent.withAlpha (0.6f), 12, {}).drawForRectangle (g, sel.toNearestInt());
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffff2f4c), sel.getX(), sel.getY(), juce::Colour (0xffb30f28), sel.getX(), sel.getBottom(), false));
        }
        g.fillRoundedRectangle (sel, sel.getHeight() * 0.5f);

        auto lr = r;
        auto left = lr.removeFromLeft (lr.getWidth() * 0.5f);
        g.setFont (v ? engraveFont (13.0f, 0.22f) : displayFont (13.0f, 0.14f));
        g.setColour (mode == 0 ? juce::Colours::white : (v ? Colours::cream.withAlpha (0.45f) : Colours::textDim));
        g.drawText ("PARAMETRIC", left, juce::Justification::centred);
        g.setColour (mode == 1 ? Colours::cream : Colours::textDim);
        g.drawText ("VINTAGE", lr, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int m = e.position.x < getWidth() * 0.5f ? 0 : 1;
        if (m != mode && onChange) onChange (m);
    }
};

// Boton de encendido grande (bypass del plugin)
class BigPower : public juce::Button
{
public:
    BigPower() : juce::Button ("power") { setTooltip ("Encender / apagar el plugin (bypass)"); }
    bool on = true, vintage = false;

    void paintButton (juce::Graphics& g, bool hover, bool) override
    {
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        const float s = juce::jmin (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (s, s);
        const auto col = vintage ? Colours::lamp : Colours::accent;
        if (on)
        {
            juce::DropShadow (col.withAlpha (0.8f), 16, {}).drawForRectangle (g, r.toNearestInt().reduced (4));
            g.setGradientFill (juce::ColourGradient (col.brighter (0.2f), r.getCentreX(), r.getY(), col.darker (0.5f), r.getCentreX(), r.getBottom(), false));
        }
        else
            g.setColour (hover ? Colours::lineHi : Colours::line);
        g.fillEllipse (r);
        auto ic = r.reduced (s * 0.28f);
        juce::Path arc;
        arc.addCentredArc (ic.getCentreX(), ic.getCentreY() + 1, ic.getWidth() * 0.5f, ic.getHeight() * 0.5f, 0,
                           juce::degreesToRadians (38.0f), juce::degreesToRadians (322.0f), true);
        g.setColour (on ? juce::Colours::white : Colours::textDim);
        g.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.drawLine (ic.getCentreX(), ic.getY() - 2.0f, ic.getCentreX(), ic.getCentreY(), 2.0f);
    }
};

// Boton de banda en la barra inferior
class BandChip : public juce::Button
{
public:
    explicit BandChip (int b) : juce::Button ("band" + juce::String (b + 1)), band (b) {}
    bool on = false, selected = false, dyn = false;
    int stereo = 0;
    const int band;

    void paintButton (juce::Graphics& g, bool hover, bool) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f, 2.0f);
        const auto col = Colours::bandFor (band, stereo);
        const float rad = 5.0f;
        if (selected)
        {
            g.setColour (col.withAlpha (0.2f));
            g.fillRoundedRectangle (r, rad);
            g.setColour (col);
            g.drawRoundedRectangle (r, rad, 1.2f);
        }
        else if (on)
        {
            g.setColour (col.withAlpha (0.10f));
            g.fillRoundedRectangle (r, rad);
        }
        else if (hover)
        {
            g.setColour (Colours::line);
            g.fillRoundedRectangle (r, rad);
        }
        if (on)
        {
            g.setColour (col);
            g.fillRect (r.withHeight (2.5f).reduced (4, 0).withY (r.getBottom() - 3.0f));
        }
        g.setColour (on ? (selected ? juce::Colours::white : col) : Colours::textDim.withAlpha (0.7f));
        g.setFont (monoFont (10.5f));
        g.drawText (juce::String (band + 1), r.translated (0, 1), juce::Justification::centred);
        if (on && stereo != 0)
        {
            g.setColour (col);
            g.setFont (monoFont (7.5f));
            g.drawText (Colours::stereoLetter (stereo), r.reduced (2, 1), juce::Justification::topRight);
        }
        if (on && dyn)
        {
            g.setColour (Colours::dynamic);
            g.fillEllipse (juce::Rectangle<float> (3.5f, 3.5f).withPosition (r.getX() + 2.5f, r.getY() + 2.5f));
        }
    }
};

// Medidor vertical de nivel (entrada o salida)
class LevelMeter : public juce::Component
{
public:
    // dt en segundos reales: caida de 45 dB/s (pico) y 30 dB/s (RMS), retencion de 1.5 s
    void setLevel (float peak, float rms, float dt)
    {
        const float pDb = juce::Decibels::gainToDecibels (peak, -80.0f);
        const float rDb = juce::Decibels::gainToDecibels (rms, -80.0f);
        const float before = peakDb + rmsDb * 0.37f + holdDb * 0.11f;
        peakDb = juce::jmax (pDb, peakDb - 45.0f * dt);
        rmsDb = juce::jmax (rDb, rmsDb - 30.0f * dt);
        holdTime -= dt;
        if (pDb > holdDb || holdTime <= 0.0f) { holdDb = pDb; holdTime = 1.5f; }
        if (std::abs (peakDb + rmsDb * 0.37f + holdDb * 0.11f - before) > 0.02f) repaint();   // quieto: no se dibuja
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::ink);
        g.fillRoundedRectangle (r, 2.0f);
        auto toY = [&] (float db) { return r.getBottom() - r.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 63.0f); };
        auto bar = r.reduced (1.0f).withTop (toY (peakDb));
        juce::ColourGradient grad (Colours::accent, 0, r.getBottom(), juce::Colour (0xffffe08a), 0, r.getY(), false);
        grad.addColour (0.7, Colours::orange);
        g.setGradientFill (grad);
        g.fillRect (bar);
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.fillRect (r.reduced (1.0f).withTop (toY (rmsDb)).withHeight (1.5f));
        g.setColour (holdDb > -0.1f ? juce::Colours::white : Colours::emberHot);
        g.fillRect (r.reduced (1.0f).withTop (toY (holdDb)).withHeight (1.5f));
    }

private:
    float peakDb = -80.0f, rmsDb = -80.0f, holdDb = -80.0f;
    float holdTime = 0.0f;
};
// Indicador de true peak de la salida (dBTP). Clic para reiniciar la retencion.
class TruePeakLabel : public juce::Component, public juce::SettableTooltipClient
{
public:
    TruePeakLabel() { setTooltip ("True peak de la salida (pico real entre muestras, como lo mide un servicio de streaming). Clic para reiniciar."); }
    float db = -100.0f;
    std::function<void()> onReset;
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool hot = db > -1.0f;
        g.setColour (Colours::ink.withAlpha (0.75f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (hot ? Colours::accent : Colours::lineHi);
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
        g.setFont (monoFont (10.5f));
        g.setColour (hot ? Colours::accent.brighter (0.4f) : Colours::text);
        g.drawText ("TP " + (db <= -99.0f ? juce::String ("-inf") : juce::String (db, 1)) + " dBTP", r, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent&) override { if (onReset) onReset(); }
};
} // namespace gaux

// Toda la interfaz a su tamano de diseno; la ventana del plugin la escala
class MainView : public juce::Component, private juce::Timer
{
public:
    explicit MainView (GauxEqProcessor&);
    ~MainView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void selectBand (int b);
    void setAdvanced (bool open);
    gaux::Mascot& getMascot() { return mascot; }
    void setScaleLabel (int percent) { scaleChip.setButtonText (juce::String (percent) + "%"); scaleChip.repaint(); }

    std::function<void()> onDesignSizeChanged;
    std::function<void (float)> onScaleRequested;

    static constexpr int kWidth = 1160, kHeight = 760, kHeaderH = 64, kFooterH = 58, kStripH = 196, kPianoH = 22, kDrawerH = 270;

private:
    void timerCallback() override;
    void setMode (int mode);
    void updateModeView();
    void showScaleMenu();
    void updateUndoButtons();
    void doUndo (bool redo);

    GauxEqProcessor& proc;
    gaux::GauxLookAndFeel lnf;

    gaux::ModeSwitch modeSwitch;
    gaux::BigPower power;
    gaux::Mascot mascot;
    juce::OwnedArray<gaux::Chip> qualityChips;
    gaux::Chip scaleChip { "100%" };
    gaux::Chip zoneChip { "ZONE" };
    gaux::ScChip scChip;
    gaux::Chip matchChip { "MATCH" };
    gaux::UndoArrow undoButton { false }, redoButton { true };
    gaux::PresetBar presetBar;
    gaux::ABSwitch abSwitch;
    bool wheelPending = false;
    int wheelQuietTicks = 0, undoTick = 0;

    // Parametrico
    gaux::SpectrumDisplay display;
    gaux::PianoStrip piano { proc, display };
    bool pianoShown = true;
    gaux::BandEditor bandEditor;
    juce::OwnedArray<gaux::BandChip> bandChips;
    gaux::Chip analyzerChip { "ANALYZER" }, autoGainChip { "AUTO GAIN" }, phaseChip { "MIN" };
    gaux::ModernKnob scaleKnob { "SCALE" }, outputKnob { "OUTPUT" };
    gaux::LevelMeter inMeter, outMeter;
    void frame (float dt);   // medidores y destello del logo, al ritmo del monitor
    bool logoGlitching = false;
    gaux::FrameClock frameClock { *this, [this] (float dt) { frame (dt); } };
    gaux::TruePeakLabel truePeakLabel;
    juce::Rectangle<int> stripArea;

    // Vintage
    gaux::VintageFaceplate faceplate;
    gaux::SpectrumDisplay drawerDisplay;

    bool vintageView = false, advancedOpen = false;
    int selectedBand = -1;
    juce::Rectangle<float> logoArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainView)
};

class GauxEqEditor : public juce::AudioProcessorEditor
{
public:
    explicit GauxEqEditor (GauxEqProcessor&);
    ~GauxEqEditor() override { proc.uiActive.fetch_sub (1); }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }
    void resized() override;

    void selectBand (int b) { view.selectBand (b); }
    void setAdvanced (bool open) { view.setAdvanced (open); }
    gaux::Mascot& getMascot() { return view.getMascot(); }
    void setUiScale (float s);
    float getUiScale() const { return uiScale; }

private:
    void designSizeChanged();

    GauxEqProcessor& proc;
    MainView view;
    float uiScale = 0.85f;
    juce::TooltipWindow tooltips { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GauxEqEditor)
};
