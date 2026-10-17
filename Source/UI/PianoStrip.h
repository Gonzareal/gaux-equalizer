// Gaux Equalizer - Teclado de piano (abajo del panel de la banda, alineado con la pantalla)
// Cada tecla esta exactamente en la frecuencia de su nota, debajo del espectro. Teclas blancas en rojo GAUX,
// negras en negro. Se iluminan: la nota de la banda elegida (en su color) y las resonancias marcadas por el
// analizador (blanco caliente). Clic en una tecla = llevar la banda elegida a esa nota (afinar el EQ al tema).
#pragma once

#include "SpectrumDisplay.h"

namespace gaux
{
class PianoStrip : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    PianoStrip (GauxEqProcessor& p, SpectrumDisplay& d) : proc (p), display (d)
    {
        setOpaque (true);
        startTimerHz (20);   // (solo redibuja cuando cambia algo)
    }

    std::function<int()> getSelectedBand;
    // Colores: true = blancas negras y negras rojas (el elegido); false = blancas rojas y negras negras
    static inline bool inverted = true;

    static int noteOf (double f) { return (int) std::lround (69.0 + 12.0 * std::log2 (std::max (1.0, f) / 440.0)); }
    static double freqOfNote (int n) { return 440.0 * std::pow (2.0, (n - 69) / 12.0); }
    static bool isBlack (int n) { const int pc = ((n % 12) + 12) % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
    static juce::String noteName (int n)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1);
    }

    // Para pruebas: la nota que esta bajo un punto del teclado
    int noteAt (float x) const
    {
        for (int n = kLow; n <= kHigh; ++n) { const auto r = keyRange (n); if (x >= r.getStart() && x < r.getEnd()) return n; }
        return -1;
    }
    float keyCentre (int n) const { return keyRange (n).getStart() * 0.5f + keyRange (n).getEnd() * 0.5f; }
    int litBandNote() const { return bandNote; }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.fillAll (juce::Colour (0xff070405));
        const float top = b.getY() + 2.0f, h = b.getHeight() - 3.0f, blackH = h * 0.6f;
        const juce::Colour redTop (0xffd8283a), redBot (0xff8c1020), blackKey (0xff0b0708);
        const juce::Colour whiteTop = inverted ? juce::Colour (0xff1e1417) : redTop, whiteBot = inverted ? juce::Colour (0xff0d0809) : redBot;
        const juce::Colour seam = inverted ? juce::Colour (0xff4a1420) : juce::Colour (0xff3a0810);

        // teclas blancas: una franja con degradado (como teclas iluminadas desde arriba)
        g.setGradientFill (juce::ColourGradient (whiteTop, 0.0f, top, whiteBot, 0.0f, top + h, false));
        g.fillRect (juce::Rectangle<float> (b.getX(), top, b.getWidth(), h));
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.fillRect (juce::Rectangle<float> (b.getX(), top, b.getWidth(), 1.0f));

        for (int n = kLow; n <= kHigh; ++n)
        {
            const auto r = keyRange (n);
            if (r.getEnd() < b.getX() || r.getStart() > b.getRight()) continue;
            const float x0 = r.getStart(), x1 = r.getEnd();
            const bool black = isBlack (n);
            const bool sel = n == bandNote, peak = std::find (peakNotes.begin(), peakNotes.end(), n) != peakNotes.end(), hov = n == hoverNote;
            if (! black)
            {
                if (sel || peak || hov)
                {
                    const auto c = sel ? bandColour : peak ? juce::Colour (0xfffff0e8) : juce::Colours::white.withAlpha (0.35f);
                    g.setColour (c.withAlpha (sel ? 0.95f : peak ? 0.8f : 0.35f));
                    g.fillRect (juce::Rectangle<float> (x0, top, x1 - x0, h));
                }
                // separacion entre dos blancas seguidas (E-F y B-C)
                const int pc = ((n % 12) + 12) % 12;
                if (pc == 4 || pc == 11) { g.setColour (seam); g.fillRect (juce::Rectangle<float> (x1 - 0.5f, top, 1.0f, h)); }
            }
        }
        for (int n = kLow; n <= kHigh; ++n)
        {
            if (! isBlack (n)) continue;
            const auto r = keyRange (n);
            if (r.getEnd() < b.getX() || r.getStart() > b.getRight()) continue;
            const float x0 = r.getStart(), x1 = r.getEnd(), xc = (x0 + x1) * 0.5f;
            // el borde entre las dos blancas queda debajo de la negra
            g.setColour (seam);
            g.fillRect (juce::Rectangle<float> (xc - 0.5f, top + blackH, 1.0f, h - blackH));
            const bool sel = n == bandNote, peak = std::find (peakNotes.begin(), peakNotes.end(), n) != peakNotes.end(), hov = n == hoverNote;
            auto key = juce::Rectangle<float> (x0 + 0.5f, top, juce::jmax (1.0f, x1 - x0 - 1.0f), blackH);
            if (sel || peak || hov || ! inverted)
                g.setColour (sel ? bandColour : peak ? juce::Colour (0xfffff0e8) : hov ? juce::Colour (inverted ? 0xffff5a6a : 0xff3a2a2e) : blackKey);
            else
                g.setGradientFill (juce::ColourGradient (redTop, 0.0f, key.getY(), redBot, 0.0f, key.getBottom(), false));
            g.fillRoundedRectangle (key.withTrimmedTop (-2.0f), 1.5f);
            if (! sel && ! peak)
            {
                g.setColour (juce::Colours::white.withAlpha (0.08f));
                g.fillRect (key.withHeight (1.0f).translated (0, 0.5f));
            }
        }
        // La nota de la banda elegida: resplandor de su color + borde blanco y una marca arriba, para que se
        // distinga de las teclas rojas
        if (bandNote >= kLow && bandNote <= kHigh)
        {
            const auto r = keyRange (bandNote);
            const bool blk = isBlack (bandNote);
            auto key = juce::Rectangle<float> (r.getStart() + 0.5f, top, juce::jmax (2.0f, r.getLength() - 1.0f), blk ? blackH : h);
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (bandColour.withAlpha (0.16f * (4 - i)));
                g.fillRoundedRectangle (key.expanded ((float) i * 1.6f, 0.0f).withY (top - 1.0f).withHeight (h + 1.0f), 2.5f);
            }
            g.setColour (bandColour.brighter (0.25f));
            g.fillRoundedRectangle (key, 1.5f);
            g.setColour (juce::Colours::white.withAlpha (0.95f));
            g.drawRoundedRectangle (key.expanded (0.5f), 1.5f, 1.2f);
            juce::Path t; const float cx = key.getCentreX();
            t.addTriangle (cx - 3.5f, b.getY(), cx + 3.5f, b.getY(), cx, b.getY() + 3.5f);
            g.fillPath (t);
        }

        // C1 ... C9 (en la parte de abajo de cada Do)
        g.setFont (monoFont (7.5f));
        for (int n = kLow; n <= kHigh; ++n)
        {
            if (((n % 12) + 12) % 12 != 0) continue;
            const auto r = keyRange (n);
            if (r.getStart() < b.getX() - 2 || r.getEnd() > b.getRight()) continue;
            g.setColour (n == bandNote ? juce::Colours::black : inverted ? juce::Colour (0xffff6b78).withAlpha (0.9f) : juce::Colours::white.withAlpha (0.85f));
            g.drawText (noteName (n), juce::Rectangle<float> (r.getStart() + 1.0f, top + blackH, 24.0f, h - blackH), juce::Justification::centredLeft);
        }
        // linea de luz arriba (une el teclado con el panel)
        g.setColour (Colours::accent.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (b.getX(), b.getY(), b.getWidth(), 1.0f));
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int n = noteAt (e.position.x);
        if (n != hoverNote)
        {
            hoverNote = n;
            const int band = getSelectedBand ? getSelectedBand() : -1;
            setTooltip (n < kLow ? juce::String() : noteName (n) + "  -  " + juce::String (freqOfNote (n), freqOfNote (n) < 100 ? 1 : 0) + " Hz"
                        + (band >= 0 ? "  -  clic: llevar la banda " + juce::String (band + 1) + " a esta nota" : juce::String()));
            repaint();
        }
    }
    void mouseExit (const juce::MouseEvent&) override { if (hoverNote >= 0) { hoverNote = -1; repaint(); } }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int n = noteAt (e.position.x);
        const int band = getSelectedBand ? getSelectedBand() : -1;
        if (n < kLow || band < 0 || band >= kNumBands || proc.params.bands[(size_t) band].on->load() < 0.5f) return;
        if (auto* p = proc.apvts.getParameter (ParamID::band (band, "freq")))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 ((float) freqOfNote (n)));
            p->endChangeGesture();
            proc.history.commit();
        }
        timerCallback();
    }

    void timerCallback() override
    {
        // que se ilumina: la nota de la banda elegida y las resonancias marcadas
        const int band = getSelectedBand ? getSelectedBand() : -1;
        int bn = -1000; juce::Colour bc;
        if (band >= 0 && band < kNumBands && proc.params.bands[(size_t) band].on->load() > 0.5f)
        {
            bn = noteOf (proc.params.bands[(size_t) band].freq->load());
            bc = Colours::bandFor (band, (int) proc.params.bands[(size_t) band].stereo->load());
        }
        std::vector<int> pk;
        for (auto& p : display.grabPeaks()) if (p.alive && p.alpha > 0.5f) pk.push_back (noteOf (p.f));
        std::sort (pk.begin(), pk.end());
        const float x0 = display.xForFreq (100.0), x1 = display.xForFreq (1000.0);
        if (bn != bandNote || bc != bandColour || pk != peakNotes || x0 != lastX0 || x1 != lastX1)
        {
            bandNote = bn; bandColour = bc; peakNotes = pk; lastX0 = x0; lastX1 = x1;
            repaint();
        }
    }

private:
    static constexpr int kLow = 16, kHigh = 135;   // E0 (20.6 Hz) ... D#10 (19.9 kHz)

    // Donde cae una tecla (coordenadas propias): media nota a cada lado, con la escala de la pantalla
    juce::Range<float> keyRange (int n) const
    {
        const float off = (float) (display.getX() - getX());
        const float a = display.xForFreq (freqOfNote (n) * std::pow (2.0, -1.0 / 24.0)) + off;
        const float c = display.xForFreq (freqOfNote (n) * std::pow (2.0, 1.0 / 24.0)) + off;
        return { a, c };
    }

    GauxEqProcessor& proc;
    SpectrumDisplay& display;
    int bandNote = -1000, hoverNote = -1;
    juce::Colour bandColour;
    std::vector<int> peakNotes;
    float lastX0 = 0, lastX1 = 0;
};
} // namespace gaux
