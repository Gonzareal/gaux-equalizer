// Gaux Equalizer - Controles del modo parametrico (perillas, chips, iconos de filtro)
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "GauxLookAndFeel.h"
#include "../DSP/EqBand.h"

namespace gaux
{
// Perilla moderna con nombre arriba y valor abajo
class ModernKnob : public juce::Component
{
public:
    explicit ModernKnob (const juce::String& name)
    {
        slider.setLookAndFeel (&lnf.get());
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 86, 16);
        slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
        slider.setMouseDragSensitivity (220);
        slider.setVelocityBasedMode (false);
        addAndMakeVisible (slider);

        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (labelFont (9.5f));
        label.setColour (juce::Label::textColourId, Colours::textDim);
        label.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
    }

    ~ModernKnob() override { slider.setLookAndFeel (nullptr); }

    void attach (juce::AudioProcessorValueTreeState& s, const juce::String& id)
    {
        attachment.reset();
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (s, id, slider);
        if (auto* p = s.getParameter (id))
            slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    }

    void setAccent (juce::Colour c) { slider.setColour (juce::Slider::rotarySliderFillColourId, c); slider.repaint(); }

    // Version compacta: perilla pequena con el valor a la derecha
    void setCompact()
    {
        compact = true;
        label.setVisible (false);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 18);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (! compact) label.setBounds (r.removeFromTop (13));
        slider.setBounds (r);
    }

    juce::Slider slider;
    juce::Label label;
    bool compact = false;

private:
    juce::SharedResourcePointer<GauxLookAndFeel> lnf;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// Pastilla seleccionable con texto
class Chip : public juce::Button
{
public:
    explicit Chip (const juce::String& label) : juce::Button (label) {}

    juce::Colour accent = Colours::accent;
    bool selected = false;
    float fontSize = 10.5f;

    void paintButton (juce::Graphics& g, bool hover, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float rad = r.getHeight() * 0.5f;
        if (selected)
        {
            g.setColour (accent.withAlpha (0.16f));
            g.fillRoundedRectangle (r, rad);
            g.setColour (accent.withAlpha (0.75f));
            g.drawRoundedRectangle (r, rad, 1.0f);
        }
        else
        {
            g.setColour (Colours::line.withAlpha (hover ? 1.0f : 0.6f));
            g.fillRoundedRectangle (r, rad);
        }
        g.setColour (selected ? accent.brighter (0.3f) : (hover || down ? Colours::text : Colours::textDim));
        g.setFont (labelFont (fontSize));
        g.drawText (getButtonText(), r, juce::Justification::centred);
    }
};

// Boton para borrar la banda (X dentro de un circulo)
class DeleteButton : public juce::Button
{
public:
    DeleteButton() : juce::Button ("delete") {}
    void paintButton (juce::Graphics& g, bool hover, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        r = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), juce::jmin (r.getWidth(), r.getHeight()));
        g.setColour (hover || down ? Colours::accent.withAlpha (0.22f) : Colours::line.withAlpha (0.7f));
        g.fillEllipse (r);
        g.setColour (hover || down ? Colours::accent : Colours::lineHi);
        g.drawEllipse (r, 1.0f);
        const auto x = r.reduced (r.getWidth() * 0.32f);
        g.setColour (hover || down ? juce::Colours::white : Colours::textDim);
        g.drawLine ({ x.getTopLeft(), x.getBottomRight() }, 1.8f);
        g.drawLine ({ x.getTopRight(), x.getBottomLeft() }, 1.8f);
    }
};

// Icono dibujado de cada tipo de filtro
inline juce::Path filterIcon (int type, juce::Rectangle<float> r)
{
    juce::Path p;
    const float x0 = r.getX(), x1 = r.getRight(), w = r.getWidth();
    const float mid = r.getCentreY(), top = r.getY() + 1, bot = r.getBottom() - 1;
    switch ((BandType) type)
    {
        case BandType::Bell:
            p.startNewSubPath (x0, mid);
            p.cubicTo (x0 + w * 0.32f, mid, x0 + w * 0.36f, top, x0 + w * 0.5f, top);
            p.cubicTo (x0 + w * 0.64f, top, x0 + w * 0.68f, mid, x1, mid);
            break;
        case BandType::LowShelf:
            p.startNewSubPath (x0, top);
            p.lineTo (x0 + w * 0.3f, top);
            p.cubicTo (x0 + w * 0.5f, top, x0 + w * 0.5f, mid + 3, x0 + w * 0.7f, mid + 3);
            p.lineTo (x1, mid + 3);
            break;
        case BandType::HighShelf:
            p.startNewSubPath (x0, mid + 3);
            p.lineTo (x0 + w * 0.3f, mid + 3);
            p.cubicTo (x0 + w * 0.5f, mid + 3, x0 + w * 0.5f, top, x0 + w * 0.7f, top);
            p.lineTo (x1, top);
            break;
        case BandType::LowCut:
            p.startNewSubPath (x0 + w * 0.12f, bot);
            p.cubicTo (x0 + w * 0.3f, mid - 2, x0 + w * 0.38f, mid - 3, x0 + w * 0.55f, mid - 3);
            p.lineTo (x1, mid - 3);
            break;
        case BandType::HighCut:
            p.startNewSubPath (x0, mid - 3);
            p.lineTo (x0 + w * 0.45f, mid - 3);
            p.cubicTo (x0 + w * 0.62f, mid - 3, x0 + w * 0.7f, mid - 2, x0 + w * 0.88f, bot);
            break;
        case BandType::Notch:
            p.startNewSubPath (x0, top + 3);
            p.lineTo (x0 + w * 0.38f, top + 3);
            p.cubicTo (x0 + w * 0.47f, top + 3, x0 + w * 0.47f, bot, x0 + w * 0.5f, bot);
            p.cubicTo (x0 + w * 0.53f, bot, x0 + w * 0.53f, top + 3, x0 + w * 0.62f, top + 3);
            p.lineTo (x1, top + 3);
            break;
        case BandType::BandPass:   // solo pasa la joroba del medio
            p.startNewSubPath (x0 + w * 0.08f, bot);
            p.cubicTo (x0 + w * 0.3f, bot - 2, x0 + w * 0.36f, top, x0 + w * 0.5f, top);
            p.cubicTo (x0 + w * 0.64f, top, x0 + w * 0.7f, bot - 2, x0 + w * 0.92f, bot);
            break;
        case BandType::TiltShelf:  // baja de un lado y sube del otro, girando en el centro
            p.startNewSubPath (x0, bot - 1);
            p.lineTo (x0 + w * 0.25f, bot - 1);
            p.cubicTo (x0 + w * 0.45f, bot - 1, x0 + w * 0.55f, top + 1, x0 + w * 0.75f, top + 1);
            p.lineTo (x1, top + 1);
            p.startNewSubPath (x0 + w * 0.5f - 1.5f, mid);   // pivote
            p.addEllipse (x0 + w * 0.5f - 1.5f, mid - 1.5f, 3.0f, 3.0f);
            break;
        case BandType::AllPass:    // curva plana con un giro de fase en el medio
            p.startNewSubPath (x0, mid);
            p.lineTo (x0 + w * 0.3f, mid);
            p.cubicTo (x0 + w * 0.42f, mid, x0 + w * 0.42f, top + 1, x0 + w * 0.5f, top + 1);
            p.cubicTo (x0 + w * 0.58f, top + 1, x0 + w * 0.58f, mid, x0 + w * 0.5f, mid);
            p.cubicTo (x0 + w * 0.42f, mid, x0 + w * 0.42f, bot - 1, x0 + w * 0.5f, bot - 1);
            p.cubicTo (x0 + w * 0.58f, bot - 1, x0 + w * 0.58f, mid, x0 + w * 0.7f, mid);
            p.lineTo (x1, mid);
            break;
    }
    return p;
}

class FilterTypeButton : public juce::Button
{
public:
    explicit FilterTypeButton (int t) : juce::Button (kBandTypeNames[t]), type (t) { setTooltip (kBandTypeNames[t]); }

    juce::Colour accent = Colours::accent;
    bool selected = false;

    void paintButton (juce::Graphics& g, bool hover, bool) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (selected)
        {
            g.setColour (accent.withAlpha (0.14f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (accent.withAlpha (0.6f));
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
        }
        else if (hover)
        {
            g.setColour (Colours::line);
            g.fillRoundedRectangle (r, 5.0f);
        }
        auto icon = filterIcon (type, r.reduced (6.0f, 6.5f));
        g.setColour (selected ? accent : (hover ? Colours::text : Colours::textDim));
        g.strokePath (icon, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const int type;
};

// Boton de encendido (icono de power)
class PowerButton : public juce::ToggleButton
{
public:
    juce::Colour accent = Colours::accent;

    void paintButton (juce::Graphics& g, bool hover, bool) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        const float s = juce::jmin (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (s, s);
        const bool on = getToggleState();
        const auto col = on ? accent : (hover ? Colours::text : Colours::textDim);
        if (on)
        {
            g.setColour (accent.withAlpha (0.16f));
            g.fillEllipse (r);
        }
        g.setColour (col.withAlpha (on ? 0.8f : 0.5f));
        g.drawEllipse (r.reduced (0.5f), 1.0f);

        auto ic = r.reduced (s * 0.27f);
        juce::Path arc;
        arc.addCentredArc (ic.getCentreX(), ic.getCentreY() + 0.5f, ic.getWidth() * 0.5f, ic.getHeight() * 0.5f, 0,
                           juce::degreesToRadians (35.0f), juce::degreesToRadians (325.0f), true);
        g.setColour (col);
        g.strokePath (arc, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.drawLine (ic.getCentreX(), ic.getY() - 1.0f, ic.getCentreX(), ic.getCentreY(), 1.5f);
    }
};

// Boton de solo (icono de audifonos)
class SoloButton : public juce::Button
{
public:
    SoloButton() : juce::Button ("solo") { setTooltip ("Solo: escuchar solo esta banda (tambien Alt + clic sostenido en el punto)"); }
    bool active = false;
    juce::Colour accent = Colours::accent;

    void paintButton (juce::Graphics& g, bool hover, bool) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        const float rad = 6.0f;
        if (active)
        {
            g.setColour (accent.withAlpha (0.22f));
            g.fillRoundedRectangle (r, rad);
            g.setColour (accent);
            g.drawRoundedRectangle (r, rad, 1.2f);
        }
        else
        {
            g.setColour (Colours::line.withAlpha (hover ? 1.0f : 0.6f));
            g.fillRoundedRectangle (r, rad);
        }
        auto ic = r.reduced (r.getHeight() * 0.24f);
        const auto col = active ? accent.brighter (0.3f) : (hover ? Colours::text : Colours::textDim);
        g.setColour (col);
        juce::Path band;
        band.addCentredArc (ic.getCentreX(), ic.getCentreY() + ic.getHeight() * 0.1f, ic.getWidth() * 0.42f, ic.getHeight() * 0.48f,
                            0, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
        g.strokePath (band, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        const float cw = ic.getWidth() * 0.2f, ch = ic.getHeight() * 0.42f;
        g.fillRoundedRectangle (juce::Rectangle<float> (ic.getX() + ic.getWidth() * 0.08f - cw * 0.5f, ic.getCentreY() + ic.getHeight() * 0.08f, cw, ch), 1.5f);
        g.fillRoundedRectangle (juce::Rectangle<float> (ic.getRight() - ic.getWidth() * 0.08f - cw * 0.5f, ic.getCentreY() + ic.getHeight() * 0.08f, cw, ch), 1.5f);
    }
};

// Nombre de nota musical de una frecuencia (ej. "A1 +12c")
inline juce::String noteName (double hz)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const double midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
    const int n = (int) std::round (midi);
    const int cents = (int) std::round ((midi - n) * 100.0);
    const int octave = n / 12 - 1;
    juce::String s = juce::String (names[((n % 12) + 12) % 12]) + juce::String (octave);
    if (cents != 0) s << " " << (cents > 0 ? "+" : "") << cents << "c";
    return s;
}

// Utilidad: fijar un parametro desde la interfaz con gesto completo (para automatizacion)
inline void setParamValue (juce::AudioProcessorValueTreeState& s, const juce::String& id, float value)
{
    if (auto* p = s.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}
} // namespace gaux
