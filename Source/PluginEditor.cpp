// Gaux Equalizer - Ventana principal del plugin
#include "PluginEditor.h"

using namespace gaux;

MainView::MainView (GauxEqProcessor& p)
    : proc (p), mascot (p), display (p), bandEditor (p), faceplate (p), drawerDisplay (p), presetBar (p)
{
    setLookAndFeel (&lnf);

    modeSwitch.onChange = [this] (int m) { setMode (m); };
    addAndMakeVisible (modeSwitch);

    power.onClick = [this] { setParamValue (proc.apvts, ParamID::bypass, proc.params.bypass->load() > 0.5f ? 0.0f : 1.0f); };
    addAndMakeVisible (power);
    addAndMakeVisible (mascot);

    const char* q[] = { "1x", "2x", "4x" };
    for (int i = 0; i < 3; ++i)
    {
        auto* c = qualityChips.add (new Chip (q[i]));
        c->setTooltip (i == 0 ? "Calidad normal" : "Oversampling " + juce::String (q[i]) + ": saturacion mas limpia, mas CPU");
        c->onClick = [this, i] { setParamValue (proc.apvts, ParamID::quality, (float) i); };
        addAndMakeVisible (c);
    }
    scaleChip.setTooltip ("Tamano de la interfaz (tambien puedes arrastrar la esquina inferior derecha)");
    scaleChip.onClick = [this] { showScaleMenu(); };
    addAndMakeVisible (scaleChip);
    zoneChip.setTooltip ("Zona de proceso: el plugin solo actua entre los dos tiradores de la pantalla; graves y agudos de afuera pasan intactos");
    zoneChip.onClick = [this] { setParamValue (proc.apvts, ParamID::rangeOn, proc.params.rangeOn->load() > 0.5f ? 0.0f : 1.0f); };
    addAndMakeVisible (zoneChip);
    scChip.setTooltip ("Sidechain general: la curva del EQ se aplana cuando suena otra pista (por ejemplo el kick)");
    scChip.onClick = [this]
    {
        auto panel = std::make_unique<gaux::SidechainPanel> (proc, vintageView);
        panel->setLookAndFeel (&lnf);
        juce::CallOutBox::launchAsynchronously (std::move (panel), scChip.getScreenBounds(), nullptr);
    };
    addAndMakeVisible (scChip);
    matchChip.setTooltip ("EQ MATCH: que esta pista suene con el balance de una referencia (un tema o el sidechain)");
    matchChip.accent = Colours::match;
    matchChip.onClick = [this]
    {
        auto panel = std::make_unique<gaux::MatchPanel> (proc);
        juce::CallOutBox::launchAsynchronously (std::move (panel), matchChip.getScreenBounds(), nullptr);
    };
    addAndMakeVisible (matchChip);

    // Deshacer / rehacer: cada clic del mouse empieza un paso nuevo del historial
    undoButton.setTooltip ("Deshacer (Cmd+Z)");
    redoButton.setTooltip ("Rehacer (Cmd+Shift+Z)");
    undoButton.onClick = [this] { doUndo (false); };
    redoButton.onClick = [this] { doUndo (true); };
    addAndMakeVisible (undoButton);
    addAndMakeVisible (redoButton);
    addAndMakeVisible (presetBar);
    abSwitch.onSelect = [this] (int slot)
    {
        proc.history.commit();
        proc.ab.switchTo (slot);
        proc.history.commit();
        abSwitch.active = proc.ab.active(); abSwitch.repaint();
    };
    abSwitch.onCopy = [this] { proc.ab.copyToOther(); };
    abSwitch.active = proc.ab.active();
    addAndMakeVisible (abSwitch);
    addMouseListener (this, true);
    setWantsKeyboardFocus (true);
    updateUndoButtons();

    // ---- Parametrico ----
    display.onBandSelected = [this] (int b) { selectBand (b); };
    display.setComponentID ("display");
    bandEditor.setComponentID ("bandEditor");
    bandEditor.onDeleted = [this] { selectBand (-1); };
    addChildComponent (display);
    addChildComponent (bandEditor);

    for (int b = 0; b < kNumBands; ++b)
    {
        auto* c = bandChips.add (new BandChip (b));
        c->onClick = [this, b] { selectBand (selectedBand == b ? -1 : b); };
        c->setTooltip ("Banda " + juce::String (b + 1));
        addChildComponent (c);
    }
    analyzerChip.onClick = [this] { setParamValue (proc.apvts, ParamID::analyzer, proc.params.analyzer->load() > 0.5f ? 0.0f : 1.0f); };
    autoGainChip.onClick = [this] { setParamValue (proc.apvts, ParamID::autoGain, proc.params.autoGain->load() > 0.5f ? 0.0f : 1.0f); };
    autoGainChip.setTooltip ("Compensa automaticamente el cambio de volumen del EQ (para comparar en igualdad)");
    addChildComponent (analyzerChip);
    addChildComponent (autoGainChip);
    phaseChip.setTooltip ("Fase: MIN = fase minima, sin latencia (para tocar y mezclar). NAT = fase natural: suena como el EQ analogico "
                          "ideal (misma curva y misma fase hasta 20 kHz) con solo 1.3 ms de latencia, y todo sigue funcionando. "
                          "LIN = fase lineal: sin corrimiento de fase (mastering, grupos). LIN HQ = graves aun mas exactos, mas latencia. "
                          "En fase lineal la dinamica, la saturacion y el sidechain general quedan en pausa.");
    phaseChip.onClick = [this]
    {
        juce::PopupMenu m;
        const int cur = (int) proc.params.phase->load();
        m.addSectionHeader ("FASE");
        m.addItem (1, "Minima (0 latencia) - para tocar, mezclar, dinamica y saturacion", true, cur == 0);
        m.addItem (4, "Natural (1.3 ms) - suena como el analogico: curva y fase exactas, con dinamica y saturacion", true, cur == 3);
        m.addItem (2, "Lineal (~170 ms de latencia) - mastering y grupos, sin corrimiento de fase", true, cur == 1);
        m.addItem (3, "Lineal HQ (~680 ms) - graves exactos hasta 20 Hz", true, cur == 2);
        juce::Component::SafePointer<MainView> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&phaseChip), [safe] (int r)
        {
            if (safe == nullptr || r <= 0) return;
            setParamValue (safe->proc.apvts, ParamID::phase, (float) (r - 1));
            safe->proc.history.commit();
        });
    };
    addChildComponent (phaseChip);
    truePeakLabel.onReset = [this] { proc.resetTruePeak(); };
    addChildComponent (truePeakLabel);

    scaleKnob.setCompact();
    scaleKnob.attach (p.apvts, ParamID::gainScale);
    scaleKnob.slider.setTooltip ("Gain Scale: escala la ganancia de todas las bandas a la vez");
    outputKnob.setCompact();
    outputKnob.attach (p.apvts, ParamID::output);
    outputKnob.slider.getProperties().set ("bipolar", true);
    for (auto* k : { &scaleKnob, &outputKnob }) { k->setAccent (Colours::accent); addChildComponent (*k); }
    addChildComponent (inMeter);
    addChildComponent (outMeter);

    // ---- Vintage ----
    faceplate.onAdvancedToggled = [this] (bool open) { setAdvanced (open); };
    addChildComponent (faceplate);
    drawerDisplay.setVintageMode (true);
    addChildComponent (drawerDisplay);

    vintageView = p.params.mode->load() > 0.5f;
    setSize (kWidth, kHeight);
    updateModeView();
    startTimerHz (30);
}

MainView::~MainView()
{
    stopTimer();

    setLookAndFeel (nullptr);
}

void MainView::setMode (int mode)
{
    setParamValue (proc.apvts, ParamID::mode, (float) mode);
    vintageView = mode == 1;
    updateModeView();
}

void MainView::updateModeView()
{
    modeSwitch.mode = vintageView ? 1 : 0;
    modeSwitch.repaint();
    power.vintage = vintageView;
    mascot.vintageStyle = vintageView;

    display.setVisible (! vintageView);
    for (auto* c : bandChips) c->setVisible (! vintageView);
    matchChip.setVisible (! vintageView);   // el match crea bandas del Parametrico
    for (juce::Component* c : { (juce::Component*) &analyzerChip, (juce::Component*) &autoGainChip, (juce::Component*) &phaseChip, (juce::Component*) &scaleKnob,
                                (juce::Component*) &outputKnob, (juce::Component*) &inMeter, (juce::Component*) &outMeter, (juce::Component*) &truePeakLabel })
        c->setVisible (! vintageView);
    bandEditor.setVisible (! vintageView && selectedBand >= 0);

    faceplate.setVisible (vintageView);
    drawerDisplay.setVisible (vintageView && advancedOpen);

    for (auto* c : qualityChips) c->accent = vintageView ? Colours::lamp : Colours::accent;

    for (gaux::Chip* c : { (gaux::Chip*) &scaleChip, (gaux::Chip*) &zoneChip, (gaux::Chip*) &scChip }) c->accent = vintageView ? Colours::lamp : Colours::accent;
    for (auto* b : { &undoButton, &redoButton }) b->accent = vintageView ? Colours::lamp : Colours::accent;
    presetBar.setAccent (vintageView ? Colours::lamp : Colours::accent);
    abSwitch.accent = vintageView ? Colours::lamp : Colours::accent; abSwitch.repaint();

    const int h = kHeight + (vintageView && advancedOpen ? kDrawerH : 0);
    if (getHeight() != h)
    {
        setSize (kWidth, h);
        if (onDesignSizeChanged) onDesignSizeChanged();
    }
    resized();
    repaint();
}

void MainView::setAdvanced (bool open)
{
    advancedOpen = open;
    faceplate.advanced.setToggleState (open, juce::dontSendNotification);
    updateModeView();
}

void MainView::selectBand (int b)
{
    selectedBand = b;
    display.setSelectedBand (b);
    if (b >= 0) bandEditor.setBand (b);
    bandEditor.setVisible (! vintageView && b >= 0);
    repaint (stripArea);
}

void MainView::mouseUp (const juce::MouseEvent& e)
{
    // Al terminar cada accion con el mouse se guarda un paso en el historial
    if (e.eventComponent != &undoButton && e.eventComponent != &redoButton)
        proc.history.commit();
    updateUndoButtons();
}

void MainView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&)
{
    wheelPending = true;
}

bool MainView::keyPressed (const juce::KeyPress& k)
{
    if (k.getModifiers().isCommandDown() && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z'))
    {
        doUndo (k.getModifiers().isShiftDown());
        return true;
    }
    if (k.getModifiers().isCommandDown() && (k.getKeyCode() == 'Y' || k.getKeyCode() == 'y'))
    {
        doUndo (true);
        return true;
    }
    return false;
}

void MainView::doUndo (bool redo)
{
    if (redo) proc.history.redo(); else proc.history.undo();
    updateUndoButtons();
    if (selectedBand >= 0) bandEditor.setBand (selectedBand);
    repaint();
}

void MainView::updateUndoButtons()
{
    const bool u = proc.history.canUndo(), r = proc.history.canRedo();
    if (undoButton.available != u) { undoButton.available = u; undoButton.repaint(); }
    if (redoButton.available != r) { redoButton.available = r; redoButton.repaint(); }
}

void MainView::showScaleMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Tamano de la interfaz");
    const int opts[] = { 50, 60, 70, 75, 85, 100, 115, 125, 150, 175, 200 };
    const int current = scaleChip.getButtonText().getIntValue();
    for (int o : opts) m.addItem (o, juce::String (o) + " %", true, o == current);
    juce::Component::SafePointer<MainView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&scaleChip), [safe] (int r)
    {
        if (safe != nullptr && r > 0 && safe->onScaleRequested) safe->onScaleRequested ((float) r / 100.0f);
    });
}

void MainView::timerCallback()
{
    if (wheelPending && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
    {
        if (++wheelQuietTicks > 12) { proc.history.commit(); wheelPending = false; wheelQuietTicks = 0; }
    }
    if ((++undoTick % 6) == 0) updateUndoButtons();
    // Sigue cambios hechos desde el DAW (automatizacion, presets)
    const bool v = proc.params.mode->load() > 0.5f;
    if (v != vintageView) { vintageView = v; updateModeView(); }

    const bool on = proc.params.bypass->load() < 0.5f;
    if (power.on != on) { power.on = on; power.repaint(); }

    const int quality = (int) proc.params.quality->load();
    for (int i = 0; i < qualityChips.size(); ++i)
        if (qualityChips[i]->selected != (i == quality)) { qualityChips[i]->selected = i == quality; qualityChips[i]->repaint(); }

    const bool an = proc.params.analyzer->load() > 0.5f;
    if (analyzerChip.selected != an) { analyzerChip.selected = an; analyzerChip.repaint(); }
    const bool ag = proc.params.autoGain->load() > 0.5f;
    if (autoGainChip.selected != ag) { autoGainChip.selected = ag; autoGainChip.repaint(); }
    {
        const float tp = juce::Decibels::gainToDecibels (proc.truePeak.load(), -100.0f);
        if (std::abs (tp - truePeakLabel.db) > 0.05f) { truePeakLabel.db = tp; truePeakLabel.repaint(); }
        if (abSwitch.active != proc.ab.active()) { abSwitch.active = proc.ab.active(); abSwitch.repaint(); }
        repaint (phaseChip.getBounds().translated (0, 17).withHeight (12));
    }
    {
        const int ph = (int) proc.params.phase->load();
        const juce::String txt = ph == 0 ? "MIN" : ph == 1 ? "LIN" : ph == 2 ? "LIN HQ" : "NAT";
        if (phaseChip.getButtonText() != txt || phaseChip.selected != (ph > 0)) { phaseChip.setButtonText (txt); phaseChip.selected = ph > 0; phaseChip.repaint(); repaint (phaseChip.getBounds().translated (0, 17).withHeight (12)); }
    }
    const bool zn = proc.params.rangeOn->load() > 0.5f;
    if (zoneChip.selected != zn) { zoneChip.selected = zn; zoneChip.repaint(); }
    {
        const bool on = proc.params.scOn->load() > 0.5f, sig = proc.sidechainActive.load();
        const float lvl = juce::jlimit (0.0f, 1.0f, (proc.sidechainDb.load() + 60.0f) / 60.0f), dk = proc.scDuckNow.load();
        if (scChip.selected != on || scChip.signal != sig || std::abs (scChip.level - lvl) > 0.02f || std::abs (scChip.duck - dk) > 0.01f)
        { scChip.selected = on; scChip.signal = sig; scChip.level = lvl; scChip.duck = dk; scChip.repaint(); }
    }

    for (auto* c : bandChips)
    {
        const bool bOn = proc.params.bands[(size_t) c->band].on->load() > 0.5f;
        const bool sel = c->band == selectedBand;
        const auto& br = proc.params.bands[(size_t) c->band];
        const int st = (int) br.stereo->load();
        const bool dy = br.dyn->load() > 0.5f && typeCanBeDynamic ((int) br.type->load());
        if (c->on != bOn || c->selected != sel || c->stereo != st || c->dyn != dy)
        { c->on = bOn; c->selected = sel; c->stereo = st; c->dyn = dy; c->repaint(); }

    }
}

void MainView::frame (float dt)
{
    if (vintageView) return;
    inMeter.setLevel (proc.inputPeak.load(), proc.inputRms.load(), dt);
    outMeter.setLevel (proc.outputPeak.load(), proc.outputRms.load(), dt);
    // El logo solo se mueve durante su destello (0.14 s cada 5.2 s): el resto del tiempo no se redibuja
    const double ph = std::fmod (juce::Time::getMillisecondCounterHiRes() * 0.001, 5.2);
    const bool glitch = ph < 0.16;
    if (glitch || logoGlitching) repaint (logoArea.expanded (10).toNearestInt());
    logoGlitching = glitch;
}

void MainView::paint (juce::Graphics& g)
{
    auto header = getLocalBounds().removeFromTop (kHeaderH).toFloat();
    const double t = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if (vintageView)
    {
        // Barra superior de metal oscuro
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff221c19), 0, 0, juce::Colour (0xff0c0a09), 0, header.getBottom(), false));
        g.fillRect (header);
        g.setColour (juce::Colours::black);
        g.drawHorizontalLine (kHeaderH - 1, 0, (float) getWidth());
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawHorizontalLine (0, 0, (float) getWidth());
        g.setFont (engraveFont (13.0f, 0.4f));
        g.setColour (Colours::cream.withAlpha (0.55f));
        g.drawText ("GAUX  EQ", logoArea.withWidth (120), juce::Justification::centredLeft);

        if (advancedOpen)
        {
            auto drawer = getLocalBounds().withTop (kHeight).toFloat();
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1c1d1e), 0, drawer.getY(), juce::Colour (0xff0b0b0c), 0, drawer.getBottom(), false));
            g.fillRect (drawer);
            g.setColour (juce::Colours::black);
            g.fillRect (drawer.withHeight (4));
            auto bezel = drawerDisplay.getBounds().toFloat().expanded (6);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3b3d), bezel.getX(), bezel.getY(), juce::Colour (0xff0f0f10), bezel.getX(), bezel.getBottom(), false));
            g.fillRoundedRectangle (bezel, 6.0f);
            g.setColour (Colours::cream.withAlpha (0.55f));
            g.setFont (engraveFont (10.0f, 0.3f));
            g.drawText ("RESPONSE  /  ANALYZER", bezel.withY (bezel.getBottom() + 2).withHeight (14), juce::Justification::centred);
        }
        return;
    }

    g.fillAll (Colours::ink);

    // Barra superior con una linea roja de neon en la base
    g.setGradientFill (juce::ColourGradient (Colours::inkHi, 0, 0, Colours::ink, 0, header.getBottom(), false));
    g.fillRect (header);
    {
        juce::ColourGradient neon (Colours::accent.withAlpha (0.0f), 0, header.getBottom(), Colours::accent.withAlpha (0.0f), (float) getWidth(), header.getBottom(), false);
        neon.addColour (0.25, Colours::accent.withAlpha (0.9f));
        neon.addColour (0.75, Colours::orange.withAlpha (0.8f));
        g.setGradientFill (neon);
        g.fillRect (header.withTop (header.getBottom() - 1.5f));
    }
    Logo::drawRave (g, logoArea, t);
    g.setFont (labelFont (7.5f).withExtraKerningFactor (0.9f));
    g.setColour (Colours::textDim);
    g.drawText ("EQUALIZER", logoArea.withY (logoArea.getBottom() + 3).withHeight (10).translated (2, 0), juce::Justification::centredLeft);

    // Franja del panel de banda (fija, como en Pro-Q)
    {
        auto st = stripArea.toFloat();
        g.setGradientFill (juce::ColourGradient (Colours::inkHi, 0, st.getY(), Colours::ink, 0, st.getBottom(), false));
        g.fillRect (st);
        g.setColour (Colours::line);
        g.drawHorizontalLine ((int) st.getY(), 0, (float) getWidth());
        if (selectedBand < 0)
        {
            g.setColour (Colours::textDim);
            g.setFont (uiFont (14.0f));
            g.drawText ("Doble clic en la pantalla para crear una banda  -  clic en un punto para editarla", st, juce::Justification::centred);
            g.setFont (uiFont (11.5f));
            g.setColour (Colours::textDim.withAlpha (0.7f));
            g.drawText ("Arrastra para mover  -  Shift: ajuste fino  -  Rueda: Q  -  Alt + clic: solo  -  X o doble clic en el punto: borrar",
                        st.translated (0, 24), juce::Justification::centred);
            g.drawText ("Shift + clic o un recuadro: varias bandas a la vez (Supr las borra)  -  Cmd+C / Cmd+V: copiar y pegar la curva  -  Clic derecho: opciones",
                        st.translated (0, 44), juce::Justification::centred);
        }
    }

    // Barra inferior
    auto footer = getLocalBounds().removeFromBottom (kFooterH).toFloat();
    g.setColour (Colours::inkHi);
    g.fillRect (footer);
    g.setColour (Colours::line);
    g.drawHorizontalLine ((int) footer.getY(), 0, (float) getWidth());

    g.setColour (Colours::textDim);
    g.setFont (labelFont (8.5f));
    g.drawText ("SCALE", scaleKnob.getBounds().toFloat().translated (-48, 0).withWidth (44), juce::Justification::centredRight);
    g.drawText ("OUT", outputKnob.getBounds().toFloat().translated (-40, 0).withWidth (36), juce::Justification::centredRight);
    g.drawText ("IN", inMeter.getBounds().toFloat().withY (footer.getBottom() - 13).withHeight (12).expanded (8, 0), juce::Justification::centred);
    g.drawText ("OUT", outMeter.getBounds().toFloat().withY (footer.getBottom() - 13).withHeight (12).expanded (8, 0), juce::Justification::centred);
    {
        // latencia de la fase elegida
        // (fase lineal, o dinamica espectral: cualquier latencia que el DAW este compensando)
        const bool lin = proc.linearActiveNow.load() || proc.getLatencySamples() > 0;
        g.setFont (monoFont (9.0f));
        g.setColour (proc.linearActiveNow.load() ? Colours::accent : (lin ? Colours::dynamic : Colours::textDim.withAlpha (0.7f)));
        const double fsr = proc.currentSampleRate.load();
        const double ms = 1000.0 * proc.getLatencySamples() / juce::jmax (1.0, fsr);
        g.drawText (lin ? (ms < 9.95 ? juce::String (ms, 1) : juce::String (juce::roundToInt (ms))) + " ms" : juce::String ("0 ms"),
                    phaseChip.getBounds().toFloat().translated (0, 17).withHeight (12), juce::Justification::centred);
    }
    if (proc.params.autoGain->load() > 0.5f)
    {
        g.setFont (monoFont (9.5f));
        g.setColour (Colours::accent);
        g.drawText (formatDb (proc.autoGainDb.load()), autoGainChip.getBounds().toFloat().translated (0, 17).withHeight (12), juce::Justification::centred);
    }
}

void MainView::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (kHeaderH);
    auto hl = header.reduced (16, 0);
    power.setBounds (hl.removeFromLeft (44).withSizeKeepingCentre (42, 42));
    hl.removeFromLeft (14);
    const float logoH = 24.0f;
    logoArea = juce::Rectangle<float> ((float) hl.getX(), (float) hl.getCentreY() - logoH * 0.5f - 4.0f, Logo::raveWidth (logoH), logoH);
    hl.removeFromLeft (juce::roundToInt (logoArea.getWidth()) + 12);
    undoButton.setBounds (hl.removeFromLeft (30).withSizeKeepingCentre (30, 28));
    hl.removeFromLeft (2);
    redoButton.setBounds (hl.removeFromLeft (30).withSizeKeepingCentre (30, 28));
    hl.removeFromLeft (10);
    modeSwitch.setBounds (hl.removeFromLeft (164).withSizeKeepingCentre (164, 34));
    hl.removeFromLeft (8);
    abSwitch.setBounds (hl.removeFromLeft (80).withSizeKeepingCentre (80, 28));

    auto hr = header.reduced (16, 0);
    scaleChip.setBounds (hr.removeFromRight (56).withSizeKeepingCentre (56, 28));
    hr.removeFromRight (8);
    auto q = hr.removeFromRight (102).withSizeKeepingCentre (102, 28);
    for (auto* c : qualityChips) c->setBounds (q.removeFromLeft (34).reduced (2, 0));
    hr.removeFromRight (8);
    mascot.setBounds (hr.removeFromRight (50).withSizeKeepingCentre (50, 50));
    hr.removeFromRight (8);
    zoneChip.setBounds (hr.removeFromRight (58).withSizeKeepingCentre (58, 28));
    hr.removeFromRight (8);
    scChip.setBounds (hr.removeFromRight (48).withSizeKeepingCentre (48, 28));
    hr.removeFromRight (6);
    matchChip.setBounds (hr.removeFromRight (60).withSizeKeepingCentre (60, 28));
    hr.removeFromRight (10);
    // La barra de presets ocupa el espacio que queda libre entre el selector de modo y los botones
    {
        const int left = abSwitch.getRight() + 10;
        const int w = juce::jlimit (110, 230, hr.getRight() - left);
        presetBar.setBounds (juce::Rectangle<int> (hr.getRight() - w, hr.getY(), w, hr.getHeight()).withSizeKeepingCentre (w, 30));
    }

    // Parametrico
    auto main = juce::Rectangle<int> (0, kHeaderH, kWidth, kHeight - kHeaderH);
    auto footer = main.removeFromBottom (kFooterH);
    stripArea = main.removeFromBottom (kStripH);
    display.setBounds (main);
    truePeakLabel.setBounds (juce::Rectangle<int> (main.getCentreX() - 59, main.getY() + 10, 118, 22));
    bandEditor.setBounds (stripArea.withSizeKeepingCentre (BandEditor::kWidth, BandEditor::kHeight));
    auto f = footer.reduced (14, 12);
    for (auto* c : bandChips) c->setBounds (f.removeFromLeft (23).reduced (1, 0));   // 24 bandas en una fila
    f.removeFromLeft (14);
    analyzerChip.setBounds (f.removeFromLeft (86).reduced (2, 4));
    autoGainChip.setBounds (f.removeFromLeft (90).reduced (2, 4));
    phaseChip.setBounds (f.removeFromLeft (58).reduced (2, 4));
    outMeter.setBounds (f.removeFromRight (8).withTrimmedBottom (12).translated (0, -2).withY (footer.getY() + 8).withHeight (footer.getHeight() - 24));
    f.removeFromRight (10);
    inMeter.setBounds (f.removeFromRight (8).withY (footer.getY() + 8).withHeight (footer.getHeight() - 24));
    f.removeFromRight (16);
    outputKnob.setBounds (f.removeFromRight (104));
    f.removeFromRight (44);
    scaleKnob.setBounds (f.removeFromRight (104));

    // Vintage
    faceplate.setBounds (0, kHeaderH, kWidth, kHeight - kHeaderH);
    drawerDisplay.setBounds (juce::Rectangle<int> (0, kHeight, kWidth, kDrawerH).reduced (54, 26).translated (0, 2));
}

//==============================================================================
GauxEqEditor::GauxEqEditor (GauxEqProcessor& p) : AudioProcessorEditor (&p), proc (p), view (p)
{
    proc.uiActive.fetch_add (1);   // con la ventana abierta se calculan medidores, analizador y calavera
    uiScale = juce::jlimit (0.5f, 2.0f, (float) (double) p.apvts.state.getProperty ("uiScale", 0.85));
    view.onDesignSizeChanged = [this] { designSizeChanged(); };
    view.onScaleRequested = [this] (float s) { setUiScale (s); };
    addAndMakeVisible (view);
    setResizable (true, true);
    designSizeChanged();
}

void GauxEqEditor::designSizeChanged()
{
    const int w = view.getWidth(), h = view.getHeight();
    const float target = uiScale;   // setResizeLimits puede llamar a resized() y cambiar uiScale
    setResizeLimits (w / 2, h / 2, w * 2, h * 2);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) w / (double) h);
    setSize (juce::roundToInt (w * target), juce::roundToInt (h * target));
}

void GauxEqEditor::setUiScale (float s)
{
    uiScale = juce::jlimit (0.5f, 2.0f, s);
    setSize (juce::roundToInt (view.getWidth() * uiScale), juce::roundToInt (view.getHeight() * uiScale));
}

void GauxEqEditor::resized()
{
    const float s = (float) getWidth() / (float) juce::jmax (1, view.getWidth());
    uiScale = s;
    view.setTopLeftPosition (0, 0);
    view.setTransform (juce::AffineTransform::scale (s));
    view.setScaleLabel (juce::roundToInt (s * 100.0f));
    proc.apvts.state.setProperty ("uiScale", (double) s, nullptr);
}
