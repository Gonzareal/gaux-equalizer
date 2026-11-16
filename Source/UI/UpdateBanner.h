// Gaux Equalizer - Aviso discreto de version nueva (ver UpdateChecker.h)
#pragma once

#include "GauxLookAndFeel.h"
#include "../UpdateChecker.h"

namespace gaux
{
class UpdateBanner : public juce::Component
{
public:
    std::function<void()> onClose;
    std::function<void (const juce::String&)> onSkip;

    void show (const UpdateInfo& u) { info = u; setVisible (true); toFront (false); repaint(); }
    const UpdateInfo& getInfo() const { return info; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.7f), 14, {}).drawForRectangle (g, r.toNearestInt());
        g.setColour (juce::Colour (0xf0120a0b));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (Colours::accent);
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.4f);

        auto a = r.reduced (16.0f, 0.0f);
        layout (a);
        g.setFont (labelFont (12.0f));
        g.setColour (juce::Colours::white);
        const auto ch = info.channel.equalsIgnoreCase ("beta") ? " BETA" : "";
        g.drawText (juce::String (juce::CharPointer_UTF8 ("NUEVA VERSI\xc3\x93N ")) + info.version + ch, textArea, juce::Justification::centredLeft);
        g.setFont (uiFont (12.0f));
        g.setColour (Colours::textDim);
        if (info.title.isNotEmpty()) g.drawText (info.title, subArea, juce::Justification::centredLeft, true);

        auto pill = [&] (juce::Rectangle<float> b, const juce::String& t, bool primary)
        {
            g.setColour (primary ? Colours::accent : juce::Colours::white.withAlpha (0.08f));
            g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
            g.setColour (primary ? juce::Colours::white : Colours::text);
            g.setFont (labelFont (11.0f));
            g.drawText (t, b, juce::Justification::centred);
        };
        pill (downloadArea, "DESCARGAR", true);
        pill (laterArea, juce::String (juce::CharPointer_UTF8 ("M\xc3\x81S TARDE")), false);
        g.setFont (uiFont (11.0f));
        g.setColour (Colours::textDim);
        g.drawText ("omitir", skipArea, juce::Justification::centred);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        if (downloadArea.contains (p)) { juce::URL (info.url).launchInDefaultBrowser(); close(); }
        else if (laterArea.contains (p)) close();
        else if (skipArea.contains (p)) { if (onSkip) onSkip (info.version); close(); }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        setMouseCursor (downloadArea.contains (p) || laterArea.contains (p) || skipArea.contains (p)
                            ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

private:
    void layout (juce::Rectangle<float> a)
    {
        skipArea = a.removeFromRight (44.0f);
        a.removeFromRight (4.0f);
        laterArea = a.removeFromRight (86.0f).withSizeKeepingCentre (86.0f, 24.0f);
        a.removeFromRight (6.0f);
        downloadArea = a.removeFromRight (96.0f).withSizeKeepingCentre (96.0f, 24.0f);
        a.removeFromRight (10.0f);
        textArea = a.removeFromTop (a.getHeight() * 0.5f).withTrimmedTop (3.0f);
        subArea = a.withTrimmedBottom (3.0f);
    }
    void close() { setVisible (false); if (onClose) onClose(); }

    UpdateInfo info;
    juce::Rectangle<float> textArea, subArea, downloadArea, laterArea, skipArea;
};
} // namespace gaux
