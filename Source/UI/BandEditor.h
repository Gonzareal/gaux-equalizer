// Gaux Equalizer - Panel flotante de la banda seleccionada
//  EQ (tipo, Freq/Gain/Q) | HARMONICS (saturacion) | DYNAMIC (EQ dinamica) + canal y solo
#pragma once

#include "Widgets.h"
#include "../PluginProcessor.h"
#include "FrameClock.h"

namespace gaux
{
// Medidor de la EQ dinamica: nivel de la banda vs threshold, y cuanto esta actuando
class DynMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    float levelDb = -100.0f, thresholdDb = -24.0f, actionDb = 0.0f;
    bool active = false, waitingSidechain = false;
    juce::Colour accent = Colours::dynamic;
    GauxEqProcessor* proc = nullptr;
    int band = 0;

    // Al ritmo del monitor: lee el detector de la banda y solo redibuja si cambio algo visible
    void frame (float)
    {
        if (proc == nullptr || ! active) return;
        const float lv = proc->detectorDb[(size_t) band].load(), ac = proc->dynGainDb[(size_t) band].load();
        const float th = proc->thresholdDb[(size_t) band].load();
        if (std::abs (lv - levelDb) > 0.05f || std::abs (ac - actionDb) > 0.02f || std::abs (th - thresholdDb) > 0.02f)
        {
            levelDb = lv; actionDb = ac; thresholdDb = th;
            repaint();
        }
    }
    FrameClock clock { *this, [this] (float dt) { frame (dt); } };

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        auto readout = r.removeFromRight (52.0f);
        auto bar = r.reduced (0, r.getHeight() * 0.28f);
        auto toX = [&] (float db) { return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };

        g.setColour (Colours::ink);
        g.fillRoundedRectangle (bar, 3.0f);
        if (active)
        {
            const float lx = toX (levelDb), tx = toX (thresholdDb);
            // Parte bajo el umbral (tenue) y parte que lo supera (brillante: ahi actua la dinamica)
            g.setColour (accent.withAlpha (0.35f));
            g.fillRoundedRectangle (bar.withRight (juce::jmin (lx, tx)), 3.0f);
            if (lx > tx)
            {
                g.setColour (accent);
                g.fillRect (bar.withLeft (tx).withRight (lx));
            }
            g.setColour (juce::Colours::white);
            g.fillRect (juce::Rectangle<float> (tx - 1.0f, bar.getY() - 4.0f, 2.0f, bar.getHeight() + 8.0f));
        }
        g.setColour (Colours::lineHi);
        g.drawRoundedRectangle (bar, 3.0f, 1.0f);

        g.setFont (monoFont (11.0f));
        g.setColour (active && std::abs (actionDb) > 0.05f ? accent.brighter (0.3f) : Colours::textDim);
        if (active && waitingSidechain)
        {
            g.setColour (Colours::textDim);
            g.drawText ("SIN SC", readout, juce::Justification::centredRight);
            return;
        }
        g.drawText (active ? formatDb (actionDb) : "--", readout, juce::Justification::centredRight);
    }
};

class BandEditor : public juce::Component, private juce::Timer
{
public:
    static constexpr int kWidth = 964, kHeight = 186;

    explicit BandEditor (GauxEqProcessor& p) : proc (p)
    {
        addAndMakeVisible (power);
        power.setTooltip ("Bypass de la banda (sigue puesta, en gris, pero no suena)  -  tecla 0: todas las elegidas");
        power.setClickingTogglesState (false);
        power.onClick = [this]
        {
            setParamValue (proc.apvts, ParamID::band (band, "byp"), proc.params.bands[(size_t) band].byp->load() > 0.5f ? 0.0f : 1.0f);
            proc.history.commit();
            refresh();
        };

        for (int t = 0; t < kNumBandTypes; ++t)
        {
            auto* b = typeButtons.add (new FilterTypeButton (t));
            b->onClick = [this, t] { setParamValue (proc.apvts, ParamID::band (band, "type"), (float) t); refresh(); };
            addAndMakeVisible (b);
        }
        // Pendientes ordenadas de suave a empinada (el indice guardado de cada una no cambia)
        for (int i = 0; i < kNumSlopes; ++i)
        {
            const int s = kSlopeDisplayOrder[i];
            auto* c = slopeChips.add (new Chip (kSlopeShort[s]));
            c->fontSize = 9.5f;
            c->setTooltip (s == kSlopeBrick ? juce::String ("Brickwall: 144 dB/oct en fase minima; una pared ideal en Lineal y Natural")
                                            : juce::String (kSlopeNames[s]));
            c->onClick = [this, s] { setParamValue (proc.apvts, ParamID::band (band, "slope"), (float) s); refresh(); };
            addChildComponent (c);
        }
        const char* satNames[] = { "TUBE", "TAPE", "SOFT", "HARD", "FUZZ" };
        for (int s = 0; s < kNumSatTypes; ++s)
        {
            auto* c = satChips.add (new Chip (satNames[s]));
            c->accent = Colours::ember;
            c->fontSize = 9.0f;
            const char* tips[] = { "TUBE: valvula, armonicos pares (2do). Calidez y cuerpo",
                                   "TAPE: cinta, satura suave y temprano, agudos redondos. Pegamento",
                                   "SOFT: consola, limpio hasta la rodilla y luego redondea",
                                   "HARD: clip duro, limpio hasta que muerde. Ataque y presencia",
                                   "FUZZ: fuzz de silicio, pares e impares, agresivo (bajos growl)" };
            c->setTooltip (tips[s]);
            c->onClick = [this, s] { setParamValue (proc.apvts, ParamID::band (band, "satType"), (float) s); refresh(); };
            addAndMakeVisible (c);
        }
        const char* stNames[] = { "ST", "L", "R", "M", "S" };
        const char* stTips[] = { "Stereo", "Solo canal izquierdo", "Solo canal derecho", "Mid (centro)", "Side (lados)" };
        for (int s = 0; s < kNumStereoModes; ++s)
        {
            auto* c = stereoChips.add (new Chip (stNames[s]));
            c->setTooltip (stTips[s]);
            c->fontSize = 10.0f;
            c->onClick = [this, s] { setParamValue (proc.apvts, ParamID::band (band, "stereo"), (float) s); refresh(); };
            addAndMakeVisible (c);
        }

        del.setTooltip ("Borrar esta banda");
        del.onClick = [this] { deleteBand(); };
        addAndMakeVisible (del);

        autoChip.setTooltip ("Threshold automatico: se ubica solo, justo sobre el nivel medio de la banda");
        autoChip.fontSize = 9.0f;
        autoChip.onClick = [this] { toggle ("autoThr"); };
        addAndMakeVisible (autoChip);
        extChip.setTooltip ("Sidechain externo: la banda reacciona a otra pista (en Ableton, elige la pista en el menu Sidechain del plugin)");
        extChip.fontSize = 9.0f;
        extChip.onClick = [this] { toggle ("ext"); };
        addAndMakeVisible (extChip);
        specChip.setTooltip ("SPECTRAL: cada frecuencia de la banda tiene su propio detector; solo se procesa la que pasa el umbral "
                             "(resonancias, silbidos, notas que sobresalen). Con AUTO doma lo que sobresale de sus vecinas. Agrega ~43 ms de latencia (el DAW la compensa).");
        specChip.fontSize = 9.0f;
        specChip.onClick = [this] { toggle ("spec"); };
        addAndMakeVisible (specChip);

        dynChip.setTooltip ("EQ dinamica: la banda solo actua cuando la senal supera el umbral");
        dynChip.onClick = [this] { setParamValue (proc.apvts, ParamID::band (band, "dyn"), proc.params.bands[(size_t) band].dyn->load() > 0.5f ? 0.0f : 1.0f); refresh(); };
        addAndMakeVisible (dynChip);
        addAndMakeVisible (dynMeter);
        dynMeter.setTooltip ("Nivel de la banda (barra), threshold (linea blanca) y cuanto esta actuando la dinamica ahora");

        solo.onClick = [this] { proc.soloBand.store (proc.soloBand.load() == band ? -1 : band); refresh(); };
        addAndMakeVisible (solo);

        for (auto* k : { &freq, &gain, &q, &drive, &mix, &thr, &range, &atk, &rel }) addAndMakeVisible (*k);
        gain.slider.getProperties().set ("bipolar", true);
        range.slider.getProperties().set ("bipolar", true);
        for (auto* k : { &drive, &mix }) k->setAccent (Colours::ember);
        // Todo lo dinamico en amarillo ambar
        for (auto* k : { &thr, &range, &atk, &rel }) k->setAccent (Colours::dynamic);
        for (auto* c : { &dynChip, &autoChip, &extChip, &specChip }) c->accent = Colours::dynamic;

        setBand (0);
        startTimerHz (30);
    }

    void setBand (int b)
    {
        band = b;
        lastPaintKey.clear();
        auto& s = proc.apvts;
        freq.attach (s, ParamID::band (b, "freq"));
        gain.attach (s, ParamID::band (b, "gain"));
        q.attach (s, ParamID::band (b, "q"));
        drive.attach (s, ParamID::band (b, "drive"));
        mix.attach (s, ParamID::band (b, "mix"));
        thr.attach (s, ParamID::band (b, "thr"));
        range.attach (s, ParamID::band (b, "range"));
        atk.attach (s, ParamID::band (b, "atk"));
        rel.attach (s, ParamID::band (b, "rel"));

        applyAccent();
        refresh();
    }

    int getBand() const { return band; }
    std::function<void()> onDeleted;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (6.0f);
        const auto col = colourNow();

        juce::Path shape;
        shape.addRoundedRectangle (r, 14.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.8f), 28, { 0, 10 }).drawForPath (g, shape);
        g.setGradientFill (juce::ColourGradient (Colours::glass.withAlpha (0.97f), r.getX(), r.getY(),
                                                 Colours::ink.withAlpha (0.97f), r.getX(), r.getBottom(), false));
        g.fillPath (shape);

        // Linea de color de la banda arriba y borde
        g.setGradientFill (juce::ColourGradient (col.withAlpha (0.0f), r.getX(), r.getY(), col.withAlpha (0.0f), r.getRight(), r.getY(), false));
        {
            juce::ColourGradient cg (col.withAlpha (0.0f), r.getX() + 20, r.getY(), col.withAlpha (0.0f), r.getRight() - 20, r.getY(), false);
            cg.addColour (0.5, col);
            g.setGradientFill (cg);
            g.fillRect (juce::Rectangle<float> (r.getX() + 20, r.getY(), r.getWidth() - 40, 2.0f));
        }
        g.setColour (Colours::lineHi.withAlpha (0.8f));
        g.strokePath (shape, juce::PathStrokeType (1.0f));

        // Banda + nota
        const auto s = proc.params.bandRaw (band);
        g.setColour (col);
        g.setFont (displayFont (16.0f, 0.06f));
        g.drawText ("BAND " + juce::String (band + 1), bandLabelArea, juce::Justification::centredLeft);
        if (s.stereo != 0)
        {
            // Etiqueta del canal (L / R / M / S) junto al nombre
            g.setFont (monoFont (10.0f));
            const auto tag = juce::Rectangle<float> (18.0f, 16.0f).withPosition (noteArea.getRight() - 4.0f, noteArea.getCentreY() - 8.0f);
            g.setColour (col.withAlpha (0.2f));
            g.fillRoundedRectangle (tag, 4.0f);
            g.setColour (col);
            g.drawText (Colours::stereoLetter (s.stereo), tag, juce::Justification::centred);
        }
        g.setColour (Colours::textDim);
        g.setFont (monoFont (10.5f));
        g.drawText (noteName (s.freq), noteArea, juce::Justification::centredLeft);

        // Separadores de secciones
        g.setColour (Colours::line);
        for (int x : { div1, div2 })
            g.drawVerticalLine (x, (float) sectionTop, (float) getHeight() - 18.0f);

        g.setColour (Colours::textDim);
        g.setFont (labelFont (9.5f));
        g.drawText ("FILTER", eqTitleArea, juce::Justification::centredLeft);

        const bool canSat = typeCanSaturate (s.type);
        const bool isCut = typeHasSlope (s.type);
        const bool satActive = canSat && s.driveDb > 0.01;
        g.setFont (labelFont (9.5f));
        if (canSat)
        {
            g.setColour (satActive ? Colours::ember : Colours::textDim);
            g.drawText ("HARMONICS", satTitleArea, juce::Justification::centredLeft);
        }
        else if (isCut)
        {
            g.setColour (Colours::textDim);
            g.drawText ("SLOPE", satTitleArea, juce::Justification::centredLeft);
            g.setColour (Colours::textDim.withAlpha (0.7f));
            g.drawText ("dB / oct", slopeTitleArea, juce::Justification::centredLeft);
        }
        if (! canSat)
        {
            g.setColour (Colours::textDim.withAlpha (0.6f));
            g.setFont (uiFont (11.5f));
            const juce::String why = s.type == (int) BandType::AllPass ? "All Pass: solo gira la fase (la curva queda plana)"
                                   : s.type == (int) BandType::Notch ? "Sin saturacion en Notch" : "";
            g.drawText (isCut ? juce::String() : why, satKnobArea, juce::Justification::centred);
        }
        if (! typeCanBeDynamic (s.type))
        {
            g.setColour (Colours::textDim.withAlpha (0.6f));
            g.setFont (uiFont (11.5f));
            g.drawText ("Dinamica disponible en Bell, Shelf y Tilt", dynKnobArea, juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6).reduced (16, 12);
        auto row1 = r.removeFromTop (30);
        power.setBounds (row1.removeFromLeft (28));
        row1.removeFromLeft (10);
        bandLabelArea = row1.removeFromLeft (74);
        noteArea = row1.removeFromLeft (64);
        row1.removeFromLeft (20);
        for (auto* t : typeButtons) t->setBounds (row1.removeFromLeft (36).reduced (1, 0));

        del.setBounds (row1.removeFromRight (30));
        row1.removeFromRight (8);
        solo.setBounds (row1.removeFromRight (40).reduced (2, 0));
        row1.removeFromRight (10);
        for (int i = stereoChips.size() - 1; i >= 0; --i) stereoChips[i]->setBounds (row1.removeFromRight (34).reduced (2, 3));

        r.removeFromTop (10);
        sectionTop = r.getY();
        const int kw = 84;

        // Seccion EQ
        auto eq = r.removeFromLeft (kw * 3 + 8);
        eqTitleArea = eq.removeFromTop (22);
        eq.removeFromTop (4);
        for (auto* k : { &freq, &gain, &q }) k->setBounds (eq.removeFromLeft (kw).reduced (4, 0));

        // Seccion HARMONICS (o pendiente en los cortes)
        div1 = r.getX() + 9;
        r.removeFromLeft (20);
        auto sat = r.removeFromLeft (282);
        auto satHead = sat.removeFromTop (22);
        satTitleArea = satHead.removeFromLeft (72);
        auto chipRow = satHead;
        for (auto* c : satChips) c->setBounds (chipRow.removeFromLeft (42).reduced (2, 1));
        sat.removeFromTop (4);
        satKnobArea = sat;
        // Pendientes (cortes y pasa-banda): una fila en el centro de la seccion, en lugar de las perillas
        {
            auto slopeRow = sat.withSizeKeepingCentre (282, 26).translated (0, 8);
            slopeTitleArea = slopeRow.translated (0, -26);
            for (int i = 0; i < slopeChips.size(); ++i)
                slopeChips[i]->setBounds (slopeRow.removeFromLeft (kSlopeDisplayOrder[i] == kSlopeBrick ? 50 : 29).reduced (1, 1));
        }
        auto sk = sat.withSizeKeepingCentre (kw * 2, sat.getHeight());
        drive.setBounds (sk.removeFromLeft (kw).reduced (4, 0));
        mix.setBounds (sk.reduced (4, 0));

        // Seccion DYNAMIC
        div2 = r.getX() + 9;
        r.removeFromLeft (20);
        auto dyn = r;
        auto dynHead = dyn.removeFromTop (22);
        dynChip.setBounds (dynHead.removeFromLeft (82).reduced (0, 1));
        dynHead.removeFromLeft (4);
        autoChip.setBounds (dynHead.removeFromLeft (46).reduced (1, 1));
        extChip.setBounds (dynHead.removeFromLeft (40).reduced (1, 1));
        specChip.setBounds (dynHead.removeFromLeft (70).reduced (1, 1));
        dynHead.removeFromLeft (8);
        dynMeter.setBounds (dynHead);
        dyn.removeFromTop (4);
        dynKnobArea = dyn;
        const int dw = dyn.getWidth() / 4;
        for (auto* k : { &thr, &range, &atk, &rel }) k->setBounds (dyn.removeFromLeft (dw).reduced (2, 0));
    }

private:
    void timerCallback() override { refresh(); }

    juce::Colour colourNow() const
    {
        return Colours::bandFor (band, (int) proc.params.bands[(size_t) band].stereo->load());
    }

    // Color de la banda segun su canal (ST rojo/naranja, L azul, R verde, M blanco, S violeta)
    void applyAccent()
    {
        const auto col = colourNow();
        lastStereo = (int) proc.params.bands[(size_t) band].stereo->load();
        power.accent = col;
        for (auto* k : { &freq, &gain, &q }) k->setAccent (col);
        for (auto* t : typeButtons) t->accent = col;
        for (auto* c : slopeChips) c->accent = col;
        for (int i = 0; i < stereoChips.size(); ++i) stereoChips[i]->accent = i == 0 ? Colours::band (band) : Colours::stereo (i);
        solo.accent = col;
        for (auto* c : getChildren()) c->repaint();
        repaint();
    }

    void toggle (const char* name)
    {
        auto* p = proc.apvts.getParameter (ParamID::band (band, name));
        setParamValue (proc.apvts, ParamID::band (band, name), p->getValue() > 0.5f ? 0.0f : 1.0f);
        refresh();
    }

    void deleteBand()
    {
        setParamValue (proc.apvts, ParamID::band (band, "on"), 0.0f);
        if (proc.soloBand.load() == band) proc.soloBand.store (-1);
        if (onDeleted) onDeleted();
    }

    void refresh()
    {
        const auto s = proc.params.bandRaw (band);
        {
            const bool lit = ! proc.params.isBypassed (band);
            if (power.getToggleState() != lit) power.setToggleState (lit, juce::dontSendNotification);
        }
        for (auto* t : typeButtons) { const bool sel = t->type == s.type; if (t->selected != sel) { t->selected = sel; t->repaint(); } }

        const bool hasSlope = typeHasSlope (s.type);
        const bool isBP = s.type == (int) BandType::BandPass;
        for (int i = 0; i < slopeChips.size(); ++i)
        {
            auto* c = slopeChips[i];
            const int idx = kSlopeDisplayOrder[i];
            // el pasa-banda llega hasta 72 dB/oct (12 secciones)
            c->setVisible (hasSlope && ! (isBP && slopeDbPerOct (idx) > 72));
            const bool sel = idx == s.slope;
            if (c->selected != sel) { c->selected = sel; c->repaint(); }
        }
        gain.setEnabled (typeHasGain (s.type));

        const bool canSat = typeCanSaturate (s.type);
        for (int i = 0; i < satChips.size(); ++i)
        {
            auto* c = satChips[i];
            c->setVisible (canSat);
            const bool sel = i == s.satType;
            if (c->selected != sel) { c->selected = sel; c->repaint(); }
        }
        drive.setVisible (canSat);
        mix.setVisible (canSat);
        mix.setEnabled (s.driveDb > 0.01);

        for (int i = 0; i < stereoChips.size(); ++i)
        {
            const bool sel = i == s.stereo;
            if (stereoChips[i]->selected != sel) { stereoChips[i]->selected = sel; stereoChips[i]->repaint(); }
        }

        const bool canDyn = typeCanBeDynamic (s.type);
        dynChip.setVisible (canDyn);
        if (dynChip.selected != s.dynOn) { dynChip.selected = s.dynOn; dynChip.repaint(); }
        dynMeter.setVisible (canDyn);
        const bool scAll = proc.params.scOn->load() > 0.5f;
        {
            // nivel, threshold efectivo y accion los actualiza el propio medidor al ritmo del monitor
            const bool act = s.dynOn && s.on, wait = (s.extSc || scAll) && ! proc.sidechainActive.load();
            dynMeter.proc = &proc;
            if (dynMeter.band != band || dynMeter.active != act || dynMeter.waitingSidechain != wait)
            {
                dynMeter.band = band; dynMeter.active = act; dynMeter.waitingSidechain = wait;
                dynMeter.thresholdDb = proc.thresholdDb[(size_t) band].load();
                dynMeter.levelDb = proc.detectorDb[(size_t) band].load();
                dynMeter.actionDb = proc.dynGainDb[(size_t) band].load();
                dynMeter.repaint();
            }
        }
        for (auto* c : { &autoChip, &extChip, &specChip }) c->setVisible (canDyn);
        if (specChip.selected != s.spectral) { specChip.selected = s.spectral; specChip.repaint(); }
        if (autoChip.selected != s.autoThr) { autoChip.selected = s.autoThr; autoChip.repaint(); }
        if (extChip.selected != (s.extSc || scAll)) { extChip.selected = s.extSc || scAll; extChip.repaint(); }
        for (auto* k : { &thr, &range, &atk, &rel }) { k->setVisible (canDyn); k->setEnabled (s.dynOn); }
        thr.setEnabled (s.dynOn && ! s.autoThr);
        if (s.stereo != lastStereo) { lastStereo = s.stereo; applyAccent(); }

        const bool soloOn = proc.soloBand.load() == band;
        if (solo.active != soloOn) { solo.active = soloOn; solo.repaint(); }

        // Repintar el fondo solo cuando cambia algo que se dibuja ahi (nota, tipo, estados)
        const juce::String key = noteName (s.freq) + juce::String (s.type) + (s.driveDb > 0.01 ? "s" : "") + (s.dynOn ? "d" : "")
                                 + juce::String (s.stereo);
        if (key != lastPaintKey) { lastPaintKey = key; repaint(); }
    }

    GauxEqProcessor& proc;
    int band = 0;
    PowerButton power;
    juce::OwnedArray<FilterTypeButton> typeButtons;
    juce::OwnedArray<Chip> slopeChips, satChips, stereoChips;
    Chip dynChip { "DYNAMIC" }, autoChip { "AUTO" }, extChip { "EXT" }, specChip { "SPECTRAL" };
    DeleteButton del;
    int lastStereo = -1;
    DynMeter dynMeter;
    juce::String lastPaintKey;
    SoloButton solo;
    ModernKnob freq { "FREQ" }, gain { "GAIN" }, q { "Q" }, drive { "DRIVE" }, mix { "MIX" };
    ModernKnob thr { "THRESH" }, range { "RANGE" }, atk { "ATTACK" }, rel { "RELEASE" };
    juce::Rectangle<int> bandLabelArea, noteArea, eqTitleArea, satTitleArea, satKnobArea, dynKnobArea, slopeTitleArea;
    int div1 = 0, div2 = 0, sectionTop = 0;
};
} // namespace gaux
