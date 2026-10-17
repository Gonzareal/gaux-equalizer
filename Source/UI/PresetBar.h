// Gaux Equalizer - Deshacer / rehacer y barra de presets (en la cabecera)
#pragma once

#include "Widgets.h"
#include "../PluginProcessor.h"

namespace gaux
{
// Flecha curva de deshacer (izquierda) o rehacer (derecha)
class UndoArrow : public juce::Button
{
public:
    explicit UndoArrow (bool isRedo) : juce::Button (isRedo ? "redo" : "undo"), redo (isRedo) {}
    juce::Colour accent = Colours::accent;
    bool available = true;

    void paintButton (juce::Graphics& g, bool hover, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour ((hover && available) ? Colours::line : Colours::line.withAlpha (0.55f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        auto ic = r.withSizeKeepingCentre (r.getHeight() * 0.5f, r.getHeight() * 0.42f).translated (0, 1);
        juce::Path p;
        const float sx = redo ? -1.0f : 1.0f;
        const auto c = ic.getCentre();
        const float w = ic.getWidth() * 0.5f, h = ic.getHeight() * 0.5f;
        // Arco que vuelve hacia atras + punta de flecha
        p.startNewSubPath (c.x + sx * w, c.y + h);
        p.cubicTo (c.x + sx * w, c.y - h * 1.1f, c.x - sx * w * 0.2f, c.y - h * 1.2f, c.x - sx * w * 0.75f, c.y - h * 0.25f);
        auto col = ! available ? Colours::textDim.withAlpha (0.35f) : ((hover || down) ? accent.brighter (0.3f) : Colours::text.withAlpha (0.85f));
        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        juce::Path head;
        const auto tip = juce::Point<float> (c.x - sx * w * 1.05f, c.y + h * 0.05f);
        head.addTriangle (tip, tip.translated (sx * w * 0.75f, -h * 0.05f), tip.translated (sx * w * 0.2f, -h * 0.85f));
        g.fillPath (head);
    }

private:
    bool redo;
};

// Flechita para pasar de preset
class StepArrow : public juce::Button
{
public:
    explicit StepArrow (bool next) : juce::Button (next ? "next" : "prev"), isNext (next) {}
    juce::Colour accent = Colours::accent;
    void paintButton (juce::Graphics& g, bool hover, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        const auto c = r.getCentre();
        const float s = r.getHeight() * 0.18f, d = isNext ? 1.0f : -1.0f;
        juce::Path p;
        p.addTriangle (c.x + d * s, c.y, c.x - d * s * 0.7f, c.y - s, c.x - d * s * 0.7f, c.y + s);
        g.setColour (hover || down ? accent.brighter (0.3f) : Colours::textDim);
        g.fillPath (p);
    }
private:
    bool isNext;
};

// Comparacion A/B: dos ajustes completos; un clic cambia entre ellos, la flecha copia el que suena al otro
class ABSwitch : public juce::Component, public juce::SettableTooltipClient
{
public:
    juce::Colour accent = Colours::accent;
    int active = 0;
    std::function<void (int)> onSelect;
    std::function<void()> onCopy;

    ABSwitch() { setTooltip ("A/B: compara dos ajustes completos al instante. La flecha copia el que suena al otro lado."); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        auto copyR = r.removeFromRight (r.getHeight());
        r.removeFromRight (4.0f);
        const float rad = r.getHeight() * 0.5f;
        g.setColour (Colours::ink.withAlpha (0.8f));
        g.fillRoundedRectangle (r, rad);
        g.setColour (Colours::lineHi);
        g.drawRoundedRectangle (r, rad, 1.0f);
        auto half = r.reduced (2.5f);
        auto sel = active == 0 ? half.removeFromLeft (half.getWidth() * 0.5f) : half.removeFromRight (half.getWidth() * 0.5f);
        g.setColour (accent.withAlpha (0.85f));
        g.fillRoundedRectangle (sel, sel.getHeight() * 0.5f);
        auto lr = r;
        auto left = lr.removeFromLeft (lr.getWidth() * 0.5f);
        g.setFont (displayFont (13.0f, 0.1f));
        g.setColour (active == 0 ? juce::Colours::white : Colours::textDim);
        g.drawText ("A", left, juce::Justification::centred);
        g.setColour (active == 1 ? juce::Colours::white : Colours::textDim);
        g.drawText ("B", lr, juce::Justification::centred);

        // boton copiar (flecha hacia el otro lado)
        g.setColour (copyHover ? Colours::line : Colours::line.withAlpha (0.55f));
        g.fillRoundedRectangle (copyR, copyR.getHeight() * 0.5f);
        juce::Path p;
        const auto c = copyR.getCentre();
        const float w = copyR.getWidth() * 0.22f, d = active == 0 ? 1.0f : -1.0f;
        p.startNewSubPath (c.x - d * w, c.y);
        p.lineTo (c.x + d * w, c.y);
        p.startNewSubPath (c.x + d * w * 0.2f, c.y - w * 0.75f);
        p.lineTo (c.x + d * w, c.y);
        p.lineTo (c.x + d * w * 0.2f, c.y + w * 0.75f);
        g.setColour (copyHover ? accent.brighter (0.3f) : Colours::text.withAlpha (0.85f));
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void mouseMove (const juce::MouseEvent& e) override { const bool h = e.position.x > getWidth() - getHeight(); if (h != copyHover) { copyHover = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { if (copyHover) { copyHover = false; repaint(); } }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.position.x > getWidth() - getHeight()) { if (onCopy) onCopy(); return; }
        const float splitX = (getWidth() - getHeight() - 4) * 0.5f;
        const int s = e.position.x < splitX ? 0 : 1;
        if (onSelect) onSelect (s);
    }

private:
    bool copyHover = false;
};

class PresetBar : public juce::Component, private juce::Timer
{
public:
    explicit PresetBar (GauxEqProcessor& p) : proc (p)
    {
        for (auto* b : { (juce::Button*) &prev, (juce::Button*) &next, (juce::Button*) &nameButton }) addAndMakeVisible (b);
        prev.onClick = [this] { proc.presets.step (-1); refresh(); };
        next.onClick = [this] { proc.presets.step (1); refresh(); };
        nameButton.onClick = [this] { showMenu(); };
        prev.setTooltip ("Preset anterior");
        next.setTooltip ("Preset siguiente");
        nameButton.setTooltip ("Presets de este modo (Parametrico o Vintage). Tambien puedes guardar los tuyos.");
        startTimerHz (4);
        refresh();
    }

    void setAccent (juce::Colour c) { accent = c; prev.accent = next.accent = c; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (Colours::ink.withAlpha (0.8f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (Colours::lineHi);
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        auto r = nameButton.getBounds().toFloat();
        g.setFont (uiFont (12.5f));
        g.setColour (nameButton.isOver() ? juce::Colours::white : Colours::text);
        g.drawFittedText (proc.presets.currentName(), r.reduced (14, 0).toNearestInt(), juce::Justification::centred, 1, 0.8f);
        // pequeno triangulo de menu
        juce::Path t;
        const float x = r.getRight() - 9, y = r.getCentreY();
        t.addTriangle (x - 3.5f, y - 2, x + 3.5f, y - 2, x, y + 2.5f);
        g.setColour (accent.withAlpha (0.8f));
        g.fillPath (t);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        prev.setBounds (r.removeFromLeft (26));
        next.setBounds (r.removeFromRight (26));
        nameButton.setBounds (r);
    }

private:
    struct Invisible : juce::Button
    {
        Invisible() : juce::Button ("preset") {}
        void paintButton (juce::Graphics&, bool, bool) override {}
    };

    void timerCallback() override { refresh(); }
    void refresh()
    {
        const auto n = proc.presets.currentName();
        if (n != lastName) { lastName = n; repaint(); }
    }

    void showMenu()
    {
        juce::PopupMenu m;
        auto& pm = proc.presets;
        const auto list = pm.entries();
        const int cur = pm.currentIndex();
        m.addSectionHeader (pm.vintageNow() ? "Presets VINTAGE" : "Presets PARAMETRICO");
        m.addItem (5000, "Default (todo en cero)");
        m.addSeparator();
        juce::StringArray cats;
        for (auto& e : list) if (! e.user) cats.addIfNotAlreadyThere (e.category);
        for (auto& c : cats)
        {
            juce::PopupMenu sub;
            for (int i = 0; i < (int) list.size(); ++i)
                if (! list[(size_t) i].user && list[(size_t) i].category == c) sub.addItem (1 + i, list[(size_t) i].name, true, cur == i);
            m.addSubMenu (c, sub);
        }
        juce::PopupMenu mine;
        bool anyUser = false;
        for (int i = 0; i < (int) list.size(); ++i)
            if (list[(size_t) i].user) { mine.addItem (1 + i, list[(size_t) i].name, true, cur == i); anyUser = true; }
        if (! anyUser) mine.addItem (-1, "(todavia no hay)", false);
        m.addSubMenu ("Mis presets", mine);
        m.addSeparator();
        m.addItem (5001, "Guardar preset...");
        m.addItem (5002, "Abrir carpeta de presets");

        juce::Component::SafePointer<PresetBar> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&nameButton), [safe] (int r)
        {
            if (safe == nullptr || r <= 0) return;
            auto& p = safe->proc.presets;
            if (r == 5000) p.loadDefault();
            else if (r == 5001) safe->askSave();
            else if (r == 5002) { PresetManager::userFolder().createDirectory(); PresetManager::userFolder().startAsProcess(); }
            else p.load (r - 1);
            safe->refresh();
        });
    }

    void askSave()
    {
        auto* w = new juce::AlertWindow ("Guardar preset", "Nombre del preset:", juce::MessageBoxIconType::NoIcon, this);
        const auto current = proc.presets.currentName();
        w->addTextEditor ("name", current == "Default" ? juce::String ("Mi preset") : current);
        w->addButton ("Guardar", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancelar", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<PresetBar> safe (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
        {
            if (safe != nullptr && result == 1)
            {
                safe->proc.presets.saveUser (w->getTextEditorContents ("name"));
                safe->refresh();
            }
        }), true);
    }

    GauxEqProcessor& proc;
    StepArrow prev { false }, next { true };
    Invisible nameButton;
    juce::Colour accent = Colours::accent;
    juce::String lastName;
};
} // namespace gaux
