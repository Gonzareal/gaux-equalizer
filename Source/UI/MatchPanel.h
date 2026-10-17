// Gaux Equalizer - Panel EQ MATCH (se abre con el boton MATCH de la cabecera)
#pragma once

#include "Widgets.h"
#include "../PluginProcessor.h"

namespace gaux
{
class MatchPanel : public juce::Component, private juce::Timer
{
public:
    explicit MatchPanel (GauxEqProcessor& p) : proc (p)
    {
        auto& m = proc.match;
        for (auto* c : { &fileChip, &scChip, &learnChip, &clearChip, &applyChip, &softChip, &medChip, &fineChip }) { c->accent = Colours::match; addAndMakeVisible (*c); }
        fileChip.setTooltip ("Elegir un tema de referencia (WAV, AIFF, FLAC, MP3...). Se analiza el tema completo.");
        fileChip.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Tema de referencia", juce::File(), proc.match.getWildcard());
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [safe = juce::Component::SafePointer<MatchPanel> (this)] (const juce::FileChooser& fc)
                                  {
                                      if (safe == nullptr) return;
                                      const auto f = fc.getResult();
                                      if (f.existsAsFile()) { safe->proc.match.loadReferenceFile (f); safe->dirty = true; }
                                  });
        };
        scChip.setTooltip ("Usar como referencia lo que entra por el sidechain (otra pista). Mientras esta encendido, aprende.");
        scChip.onClick = [this] { auto& mm = proc.match; const bool on = ! (mm.referenceKind() == MatchEngine::RefKind::Sidechain && mm.learnRef.load()); mm.useSidechain (on); dirty = true; };
        learnChip.setTooltip ("Aprender el sonido de ESTA pista mientras suena (unos segundos de la parte mas representativa)");
        learnChip.onClick = [this] { proc.match.setLearnSource (! proc.match.learnSrc.load()); dirty = true; };
        clearChip.setTooltip ("Olvidar lo aprendido de esta pista");
        clearChip.onClick = [this] { proc.match.clearSource(); dirty = true; };
        applyChip.setTooltip ("Crear las bandas del match (en los lugares libres). Cmd+Z deshace.");
        applyChip.onClick = [this] { apply(); };
        softChip.setTooltip ("Detalle suave: solo el balance general (1/2 octava)");
        medChip.setTooltip ("Detalle medio (1/3 de octava)");
        fineChip.setTooltip ("Detalle fino (1/6 de octava): sigue mas de cerca la referencia");
        softChip.onClick = [this] { detail = 0.5; dirty = true; };
        medChip.onClick = [this] { detail = 1.0 / 3.0; dirty = true; };
        fineChip.onClick = [this] { detail = 1.0 / 6.0; dirty = true; };

        amount.slider.setRange (0.0, 100.0, 1.0); amount.slider.setValue (100.0, juce::dontSendNotification);
        amount.slider.setTextValueSuffix (" %"); amount.slider.setDoubleClickReturnValue (true, 100.0);
        bands.slider.setRange (1.0, 12.0, 1.0); bands.slider.setValue (8.0, juce::dontSendNotification); bands.slider.setDoubleClickReturnValue (true, 8.0);
        for (auto* k : { &amount, &bands }) { k->setAccent (Colours::match); k->slider.onValueChange = [this] { dirty = true; }; addAndMakeVisible (*k); }
        amount.slider.setTooltip ("Cuanto del match se aplica (100 % = igual a la referencia)");
        bands.slider.setTooltip ("Cuantas bandas puede usar el match como maximo");
        m.previewVisible.store (true);
        setSize (440, 318);
        startTimerHz (15);
    }

    ~MatchPanel() override
    {
        stopTimer();
        // al cerrar se deja de aprender (lo aprendido se conserva)
        proc.match.setLearnSource (false);
        proc.match.learnRef.store (false);
        proc.match.previewVisible.store (false);
    }

    void paint (juce::Graphics& g) override
    {
        auto& m = proc.match;
        g.fillAll (Colours::glass);
        g.setColour (Colours::text);
        g.setFont (displayFont (15.0f, 0.12f));
        g.drawText ("EQ MATCH", juce::Rectangle<int> (16, 0, 200, 34), juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        g.setFont (uiFont (10.5f));
        g.drawText ("Que esta pista suene con el balance de una referencia", juce::Rectangle<int> (130, 0, getWidth() - 146, 34), juce::Justification::centredRight);

        auto section = [&] (juce::Rectangle<int> r, const char* title)
        {
            g.setColour (Colours::textDim);
            g.setFont (labelFont (9.5f));
            g.drawText (title, r, juce::Justification::centredLeft);
        };
        section (refTitle, "REFERENCIA");
        section (srcTitle, "ESTA PISTA");
        section (detailTitle, "DETALLE");

        // estados
        g.setFont (uiFont (11.0f));
        juce::String refTxt; juce::Colour refCol = Colours::textDim;
        if (m.analyzingFile()) { refTxt = m.referenceName() + "  -  analizando " + juce::String (juce::roundToInt (m.referenceProgress() * 100)) + " %"; refCol = Colours::match; }
        else if (m.fileFailed()) { refTxt = "No se pudo leer el archivo"; refCol = Colours::accent; }
        else if (m.referenceKind() == MatchEngine::RefKind::File && m.referenceReady()) { refTxt = m.referenceName() + "  -  listo (" + fmtTime (m.referenceSeconds()) + ")"; refCol = Colours::match; }
        else if (m.referenceKind() == MatchEngine::RefKind::Sidechain)
        {
            refTxt = (m.learnRef.load() ? (proc.sidechainActive.load() ? "Escuchando el sidechain: " : "Esperando senal en el sidechain: ") : "Sidechain: ") + fmtTime (m.referenceSeconds())
                   + (m.referenceReady() ? "  (listo)" : "");
            refCol = m.referenceReady() ? Colours::match : Colours::text;
        }
        else refTxt = "Elige un tema de referencia o usa el sidechain";
        g.setColour (refCol);
        g.drawText (refTxt, refStatus, juce::Justification::centredLeft);

        juce::String srcTxt = m.learnSrc.load() ? "Aprendiendo: " + fmtTime (m.sourceSeconds()) + (m.sourceReady() ? "  (ya se puede aplicar)" : "  (dale play)")
                                                : (m.sourceSeconds() > 0 ? fmtTime (m.sourceSeconds()) + " aprendidos" : "Dale play y presiona APRENDER");
        g.setColour (m.sourceReady() ? Colours::match : Colours::text);
        g.drawText (srcTxt, srcStatus, juce::Justification::centredLeft);

        if (resultText.isNotEmpty())
        {
            g.setColour (Colours::match);
            g.drawText (resultText, resultArea, juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 0);
        r.removeFromTop (36);
        refTitle = r.removeFromTop (16);
        auto row = r.removeFromTop (28);
        fileChip.setBounds (row.removeFromLeft (96).reduced (0, 2));
        row.removeFromLeft (6);
        scChip.setBounds (row.removeFromLeft (96).reduced (0, 2));
        refStatus = r.removeFromTop (22);
        r.removeFromTop (6);
        srcTitle = r.removeFromTop (16);
        row = r.removeFromTop (28);
        learnChip.setBounds (row.removeFromLeft (96).reduced (0, 2));
        row.removeFromLeft (6);
        clearChip.setBounds (row.removeFromLeft (70).reduced (0, 2));
        srcStatus = r.removeFromTop (22);
        r.removeFromTop (6);
        auto ctl = r.removeFromTop (92);
        amount.setBounds (ctl.removeFromLeft (100));
        bands.setBounds (ctl.removeFromLeft (100));
        ctl.removeFromLeft (10);
        detailTitle = ctl.removeFromTop (16).withY (ctl.getY());
        auto dr = ctl.withTrimmedTop (22).removeFromTop (26);
        for (auto* c : { &softChip, &medChip, &fineChip }) c->setBounds (dr.removeFromLeft (62).reduced (2, 0));
        r.removeFromTop (4);
        auto bottom = r.removeFromTop (30);
        applyChip.setBounds (bottom.removeFromRight (150));
        resultArea = bottom.withTrimmedRight (8);
    }

private:
    static juce::String fmtTime (double s)
    {
        const int t = (int) s;
        return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
    }

    MatchEngine::Settings settings() const
    {
        MatchEngine::Settings s;
        s.amount = amount.slider.getValue() / 100.0;
        s.bands = (int) bands.slider.getValue();
        s.detailOct = detail;
        return s;
    }

    void timerCallback() override
    {
        auto& m = proc.match;
        m.poll (proc.matchSrcFifo, proc.matchRefFifo, proc.currentSampleRate.load());
        // la curva objetivo se recalcula si cambio un ajuste o aprendio mas (como mucho 2 veces por segundo)
        ++ticks;
        if (dirty || ticks % 8 == 0)
        {
            if (m.sourceReady() && m.referenceReady()) m.computeTarget (settings(), proc.currentStaticBands());
            dirty = false;
        }
        const bool scOn = m.referenceKind() == MatchEngine::RefKind::Sidechain && m.learnRef.load();
        if (scChip.selected != scOn) { scChip.selected = scOn; scChip.repaint(); }
        const bool fileOn = m.referenceKind() == MatchEngine::RefKind::File;
        if (fileChip.selected != fileOn) { fileChip.selected = fileOn; fileChip.repaint(); }
        if (learnChip.selected != m.learnSrc.load()) { learnChip.selected = m.learnSrc.load(); learnChip.setButtonText (learnChip.selected ? "APRENDIENDO" : "APRENDER"); learnChip.repaint(); }
        const bool can = m.sourceReady() && m.referenceReady();
        applyChip.setEnabled (can); applyChip.selected = can; applyChip.repaint();
        for (auto [c, v] : { std::pair<Chip*, double> { &softChip, 0.5 }, { &medChip, 1.0 / 3.0 }, { &fineChip, 1.0 / 6.0 } })
            if (c->selected != (std::abs (detail - v) < 1e-6)) { c->selected = std::abs (detail - v) < 1e-6; c->repaint(); }
        repaint (refStatus); repaint (srcStatus);
    }

    void apply()
    {
        auto& m = proc.match;
        if (! m.computeTarget (settings(), proc.currentStaticBands())) return;
        const auto r = m.fit (settings());
        const int placed = proc.applyFittedBands (r.bands);
        resultText = placed > 0 ? juce::String (placed) + " bandas aplicadas (error " + juce::String (r.maxErrorDb, 1) + " dB)"
                                : juce::String ("No hay bandas libres");
        if (placed < (int) r.bands.size() && placed > 0) resultText << "  -  faltaron lugares";
        dirty = true;
        repaint (resultArea);
    }

    GauxEqProcessor& proc;
    Chip fileChip { "ARCHIVO..." }, scChip { "SIDECHAIN" }, learnChip { "APRENDER" }, clearChip { "BORRAR" }, applyChip { "APLICAR MATCH" };
    Chip softChip { "SUAVE" }, medChip { "MEDIO" }, fineChip { "FINO" };
    ModernKnob amount { "AMOUNT" }, bands { "BANDAS" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> refTitle, refStatus, srcTitle, srcStatus, detailTitle, resultArea;
    juce::String resultText;
    double detail = 1.0 / 3.0;
    bool dirty = true;
    int ticks = 0;
};
} // namespace gaux
