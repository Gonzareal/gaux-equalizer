// Gaux Equalizer - Identidad visual: colores, tipografias y estilo base de controles
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

namespace gaux
{
//==============================================================================
// Tipografias incrustadas (licencia OFL)
struct Fonts
{
    static Fonts& get() { static Fonts f; return f; }

    juce::Typeface::Ptr ui, uiBold, engrave, engraveBold, mono, logo;

    Fonts()
    {
        ui      = juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceGroteskRegular_ttf, BinaryData::SpaceGroteskRegular_ttfSize);
        uiBold  = juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceGroteskSemiBold_ttf, BinaryData::SpaceGroteskSemiBold_ttfSize);
        engrave = juce::Typeface::createSystemTypefaceFor (BinaryData::OswaldMedium_ttf, BinaryData::OswaldMedium_ttfSize);
        engraveBold = juce::Typeface::createSystemTypefaceFor (BinaryData::OswaldBold_ttf, BinaryData::OswaldBold_ttfSize);
        mono    = juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoMedium_ttf, BinaryData::JetBrainsMonoMedium_ttfSize);
        logo    = juce::Typeface::createSystemTypefaceFor (BinaryData::LogoFont_ttf, BinaryData::LogoFont_ttfSize);
    }
};

inline juce::Font fontFrom (juce::Typeface::Ptr t, float size, float kerning = 0.0f)
{
    return juce::Font (juce::FontOptions (t).withHeight (size).withKerningFactor (kerning));
}
inline juce::Font uiFont (float size, bool bold = false) { return fontFrom (bold ? Fonts::get().uiBold : Fonts::get().ui, size); }
inline juce::Font labelFont (float size)  { return fontFrom (Fonts::get().uiBold, size, 0.12f); }
inline juce::Font monoFont (float size)   { return fontFrom (Fonts::get().mono, size); }
inline juce::Font engraveFont (float size, float kerning = 0.06f) { return fontFrom (Fonts::get().engrave, size, kerning); }
inline juce::Font logoFont (float size, float kerning = 0.0f) { return fontFrom (Fonts::get().logo, size, kerning); }
inline juce::Font displayFont (float size, float kerning = 0.04f) { return fontFrom (Fonts::get().engraveBold, size, kerning); }

//==============================================================================
namespace Colours
{
    // Modo parametrico: "infrarrojo rave" (negro profundo + rojo neon + toques naranja)
    inline const juce::Colour ink       { 0xff050304 };
    inline const juce::Colour inkHi     { 0xff0d0708 };
    inline const juce::Colour glass     { 0xff150b0c };
    inline const juce::Colour line      { 0xff2a1315 };
    inline const juce::Colour lineHi    { 0xff42191d };
    inline const juce::Colour text      { 0xfff6e9e7 };
    inline const juce::Colour textDim   { 0xff94716f };
    inline const juce::Colour accent    { 0xffff2442 };   // rojo neon
    inline const juce::Colour orange    { 0xffff6a1a };
    inline const juce::Colour ember     { 0xffffa21f };   // saturacion / calor
    inline const juce::Colour emberHot  { 0xffffe7a3 };

    // Modo vintage: placa verde petroleo martillada, serigrafia crema, laton
    inline const juce::Colour plate     { 0xff23403c };
    inline const juce::Colour plateDark { 0xff15282a };
    inline const juce::Colour cream     { 0xffeee3c6 };
    inline const juce::Colour brass     { 0xffc8a052 };
    inline const juce::Colour lamp      { 0xffffa53a };
    inline const juce::Colour stamp     { 0xffd6262b };

    // 24 bandas en la familia rojo-naranja (se distinguen tambien por el numero)
    inline juce::Colour band (int b)
    {
        static const juce::uint32 c[] = { 0xffff2442, 0xffff5a1f, 0xffff9a1a, 0xffffc233,
                                          0xffff3d7f, 0xffff6b5a, 0xffe8173a, 0xffff8c42,
                                          0xffff5470, 0xffff7740, 0xffffb347, 0xffffd966,
                                          0xffff5c9a, 0xffff9180, 0xffd4142f, 0xffffa366,
                                          0xffff4a5e, 0xffff7a2e, 0xffffae4a, 0xffe83a62,
                                          0xffff6f8a, 0xffff9958, 0xffd9263f, 0xffffc27a };
        return juce::Colour (c[(b % 24 + 24) % 24]);
    }

    // Color por canal: ST usa el rojo/naranja de la banda; L, R, M y S tienen su propio tono
    // (fuera de la familia roja para que se distingan de un vistazo). Siempre con su letra.
    inline const juce::Colour chanL    { 0xff3d9bff };   // azul
    inline const juce::Colour chanR    { 0xff2fe08f };   // verde
    inline const juce::Colour chanM    { 0xffe9edf3 };   // blanco (centro)
    inline const juce::Colour chanS    { 0xffb36bff };   // violeta (lados)
    inline const juce::Colour dynamic  { 0xffffc233 };   // EQ dinamica: amarillo ambar
    inline const juce::Colour match    { 0xff38d6c8 };   // EQ MATCH: turquesa (la curva objetivo)
    inline const juce::Colour sketch   { 0xffc6ff3d };   // SKETCH: lima neon (la tinta del dibujo)

    inline juce::Colour stereo (int mode)
    {
        switch (mode) { case 1: return chanL; case 2: return chanR; case 3: return chanM; case 4: return chanS; default: return accent; }
    }
    inline juce::Colour bandFor (int b, int stereoMode) { return stereoMode == 0 ? band (b) : stereo (stereoMode); }
    inline const char* stereoLetter (int mode)
    {
        static const char* l[] = { "", "L", "R", "M", "S" };
        return l[juce::jlimit (0, 4, mode)];
    }
}

//==============================================================================
class GauxLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GauxLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, Colours::ink);
        setColour (juce::Slider::textBoxTextColourId, Colours::text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, Colours::accent.withAlpha (0.3f));
        setColour (juce::Label::textColourId, Colours::text);
        setColour (juce::PopupMenu::backgroundColourId, Colours::glass);
        setColour (juce::PopupMenu::textColourId, Colours::text);
        setColour (juce::PopupMenu::headerTextColourId, Colours::textDim);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::accent.withAlpha (0.18f));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextEditor::backgroundColourId, Colours::glass);
        setColour (juce::TextEditor::textColourId, Colours::text);
        setColour (juce::TextEditor::highlightColourId, Colours::accent.withAlpha (0.3f));
        setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::CaretComponent::caretColourId, Colours::accent);
        setColour (juce::TooltipWindow::backgroundColourId, Colours::glass);
        setColour (juce::TooltipWindow::textColourId, Colours::text);
        setColour (juce::TooltipWindow::outlineColourId, Colours::lineHi);
    }

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override { return Fonts::get().ui; }

    // Perilla moderna (modo parametrico): anillo con arco de valor luminoso
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        const bool enabled = s.isEnabled();
        auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);
        if (! enabled) accent = Colours::textDim.withAlpha (0.35f);

        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
        const float size = juce::jmin (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (size, size);
        const auto c = r.getCentre();
        const float radius = size * 0.5f;
        const float angle = startAngle + pos * (endAngle - startAngle);
        const float arcR = radius - 2.5f;

        juce::Path track;
        track.addCentredArc (c.x, c.y, arcR, arcR, 0, startAngle, endAngle, true);
        g.setColour (Colours::line.brighter (0.15f));
        g.strokePath (track, juce::PathStrokeType (2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const bool bipolar = s.getProperties().contains ("bipolar");
        const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        if (std::abs (angle - from) > 0.01f)
        {
            juce::Path val;
            val.addCentredArc (c.x, c.y, arcR, arcR, 0, juce::jmin (from, angle), juce::jmax (from, angle), true);
            g.setColour (accent.withAlpha (0.22f));
            g.strokePath (val, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (accent);
            g.strokePath (val, juce::PathStrokeType (2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Cuerpo
        auto body = r.reduced (radius * 0.24f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 8, { 0, 3 }).drawForRectangle (g, body.toNearestInt().reduced (4));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff232a37), body.getCentreX(), body.getY(),
                                                 juce::Colour (0xff10141b), body.getCentreX(), body.getBottom(), false));
        g.fillEllipse (body);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.16f), body.getCentreX(), body.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), body.getCentreX(), body.getCentreY(), false));
        g.drawEllipse (body.reduced (0.5f), 1.0f);

        // Punto indicador
        const auto dot = c.getPointOnCircumference (body.getWidth() * 0.5f - 5.5f, angle);
        g.setColour (accent.withAlpha (0.35f));
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (dot));
        g.setColour (enabled ? accent.brighter (0.4f) : accent);
        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (dot));
    }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (monoFont (11.5f));
        l->setJustificationType (juce::Justification::centred);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        return l;
    }

    juce::Font getPopupMenuFont() override { return uiFont (13.5f); }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (Colours::glass);
        g.setColour (Colours::lineHi);
        g.drawRect (0, 0, w, h);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
        g.setColour (Colours::glass);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (Colours::lineHi);
        g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
        g.setColour (Colours::text);
        g.setFont (uiFont (12.5f));
        g.drawFittedText (text, r.reduced (8, 4).toNearestInt(), juce::Justification::centredLeft, 3);
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> pos, juce::Rectangle<int> parent) override
    {
        const int w = juce::jmin (320, (int) juce::GlyphArrangement::getStringWidth (uiFont (12.5f), text) + 20);
        return juce::Rectangle<int> (pos.x + 8, pos.y + 14, w, 26).constrainedWithin (parent);
    }
};
} // namespace gaux
