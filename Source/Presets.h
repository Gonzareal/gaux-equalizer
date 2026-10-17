// Gaux Equalizer - Presets de fabrica (disenados para GAUX) y presets del usuario
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Parameters.h"

namespace gaux
{
struct FactoryPreset
{
    juce::String category, name;
    std::vector<std::pair<juce::String, float>> values;   // id -> valor real (indices en los selectores)
};

// Constructor comodo de presets
struct PB
{
    FactoryPreset p;
    PB (const char* cat, const char* name) { p.category = cat; p.name = name; }

    PB& set (const juce::String& id, float v) { p.values.push_back ({ id, v }); return *this; }
    PB& band (int b, BandType type, float freq, float gain = 0.0f, float q = 0.707f, int slope = 1)
    {
        set (ParamID::band (b, "on"), 1); set (ParamID::band (b, "type"), (float) type);
        set (ParamID::band (b, "freq"), freq); set (ParamID::band (b, "gain"), gain);
        set (ParamID::band (b, "q"), q); set (ParamID::band (b, "slope"), (float) slope);
        return *this;
    }
    PB& bell (int b, float f, float g, float q) { return band (b, BandType::Bell, f, g, q); }
    PB& lowShelf (int b, float f, float g, float q = 0.7f) { return band (b, BandType::LowShelf, f, g, q); }
    PB& highShelf (int b, float f, float g, float q = 0.7f) { return band (b, BandType::HighShelf, f, g, q); }
    PB& lowCut (int b, float f, int slope = 1) { return band (b, BandType::LowCut, f, 0, 0.707f, slope); }
    PB& highCut (int b, float f, int slope = 1) { return band (b, BandType::HighCut, f, 0, 0.707f, slope); }
    PB& sat (int b, SatType t, float drive, float mix = 100) { set (ParamID::band (b, "satType"), (float) t); set (ParamID::band (b, "drive"), drive); return set (ParamID::band (b, "mix"), mix); }
    PB& dyn (int b, float thr, float range, float atk = 10, float rel = 120, bool autoThr = false)
    {
        set (ParamID::band (b, "dyn"), 1); set (ParamID::band (b, "thr"), thr); set (ParamID::band (b, "range"), range);
        set (ParamID::band (b, "atk"), atk); set (ParamID::band (b, "rel"), rel);
        return set (ParamID::band (b, "autoThr"), autoThr ? 1.0f : 0.0f);
    }
    PB& chan (int b, int stereoMode) { return set (ParamID::band (b, "stereo"), (float) stereoMode); }
    PB& vintage() { return *this; }   // se reconoce como Vintage por sus parametros v_*
    operator FactoryPreset() const { return p; }
};

inline const std::vector<FactoryPreset>& factoryPresets()
{
    using S = SatType;
    constexpr int M = 3, Sd = 4;   // canales Mid / Side
    static const std::vector<FactoryPreset> list = {
        // ---------------- VOCES ----------------
        PB ("Voces", "Voz Lead Clara").lowCut (0, 90, 2).bell (1, 250, -2.5f, 1.2f).bell (2, 3200, 2.5f, 0.9f)
            .bell (3, 6800, 0, 4.0f).dyn (3, -30, -6, 1, 60).highShelf (4, 11000, 3.0f, 0.6f),
        PB ("Voces", "Voz Rap / Presencia").lowCut (0, 100, 2).bell (1, 180, 1.5f, 1.0f).bell (2, 450, -3.0f, 1.5f)
            .bell (3, 2500, 3.0f, 1.0f).sat (3, S::Tape, 6, 60).bell (4, 5500, 0, 3.5f).dyn (4, -28, -5, 1, 70).highShelf (5, 12000, 2.0f),
        PB ("Voces", "Voz Aire y Brillo").lowCut (0, 120, 2).bell (1, 300, -2.0f, 1.0f).bell (2, 1200, -1.5f, 2.0f)
            .highShelf (3, 9000, 4.0f, 0.55f).bell (4, 7200, 0, 4.0f).dyn (4, -30, -6, 1, 50),
        PB ("Voces", "Coros Anchos").lowCut (0, 200, 2).lowShelf (1, 320, -3.0f).bell (2, 2000, -2.0f, 1.2f)
            .highShelf (3, 8000, 3.0f).bell (4, 3500, 2.5f, 0.8f).chan (4, Sd),
        PB ("Voces", "De-Esser Dinamico").bell (0, 6500, 0, 3.0f).dyn (0, -28, -8, 1, 60).bell (1, 9500, 0, 2.5f).dyn (1, -32, -5, 1, 60),
        PB ("Voces", "Voz Lo-Fi Telefono").lowCut (0, 400, 3).highCut (1, 3500, 2).bell (2, 1500, 4.0f, 1.0f).sat (2, S::Tape, 12, 100),
        PB ("Voces", "Voz Calida (Tubo)").lowCut (0, 80, 1).lowShelf (1, 200, 1.5f).sat (1, S::Tube, 8, 70).bell (2, 900, -1.5f, 1.2f)
            .bell (3, 3000, 1.5f, 0.8f).sat (3, S::Tube, 6, 50).highShelf (4, 12000, 1.5f),

        // ---------------- BATERIAS ----------------
        PB ("Baterias", "Kick Tech House").lowCut (0, 25, 2).bell (1, 55, 3.0f, 1.2f).sat (1, S::Tube, 6, 70).bell (2, 300, -4.0f, 1.4f)
            .bell (3, 3500, 3.0f, 1.5f).highCut (4, 16000, 1),
        PB ("Baterias", "Kick Sub Profundo").lowCut (0, 22, 3).lowShelf (1, 65, 3.0f, 0.8f).bell (2, 250, -5.0f, 1.0f).bell (3, 2000, -2.0f, 1.0f),
        PB ("Baterias", "Kick Dubstep Pegada").lowCut (0, 30, 2).bell (1, 70, 2.5f, 1.4f).sat (1, S::Hard, 8, 50).bell (2, 400, -5.0f, 1.2f)
            .bell (3, 5000, 4.0f, 1.2f).sat (3, S::Soft, 10, 60),
        PB ("Baterias", "Snare / Clap").lowCut (0, 120, 2).bell (1, 200, 2.0f, 1.4f).bell (2, 800, -2.5f, 2.0f).bell (3, 5000, 3.0f, 1.0f)
            .highShelf (4, 10000, 2.0f),
        PB ("Baterias", "Hats y Tops").lowCut (0, 400, 2).bell (1, 3000, -2.0f, 1.5f).highShelf (2, 10000, 2.5f)
            .bell (3, 8000, 0, 2.5f).dyn (3, -26, -5, 2, 60),
        PB ("Baterias", "Percusion").lowCut (0, 150, 2).bell (1, 1000, -2.0f, 1.0f).highShelf (2, 8000, 2.0f).bell (3, 4000, 1.5f, 1.0f).chan (3, Sd),
        PB ("Baterias", "Drum Bus Punch").lowCut (0, 25, 1).lowShelf (1, 80, 1.5f).sat (1, S::Tape, 4, 60).bell (2, 400, -1.5f, 0.8f)
            .bell (3, 4000, 1.5f, 0.9f).sat (3, S::Tape, 4, 50).highShelf (4, 12000, 1.5f),
        PB ("Baterias", "Drum Bus Control").lowCut (0, 30, 1).bell (1, 250, 0, 1.0f).dyn (1, -24, -3, 15, 150).bell (2, 3000, 0, 1.2f)
            .dyn (2, -26, -3, 5, 100).highShelf (3, 11000, 1.0f),

        // ---------------- BAJO ----------------
        PB ("Bajo", "Bajo Sub Limpio").lowCut (0, 28, 2).bell (1, 60, 2.0f, 1.0f).bell (2, 250, -3.0f, 1.0f).lowCut (3, 140, 2).chan (3, Sd),
        PB ("Bajo", "Bajo Growl Dubstep").lowCut (0, 30, 2).bell (1, 120, 2.0f, 1.0f).bell (2, 700, 3.0f, 1.0f).sat (2, S::Tube, 10, 70)
            .bell (3, 2500, 2.0f, 1.2f).bell (4, 300, 0, 1.4f).dyn (4, -24, -4, 10, 120),
        PB ("Bajo", "Bajo House Redondo").lowCut (0, 30, 2).lowShelf (1, 90, 1.5f).sat (1, S::Tape, 5, 60).bell (2, 350, -2.0f, 1.2f)
            .bell (3, 1200, 1.5f, 1.0f).highCut (4, 8000, 1).lowCut (5, 120, 2).chan (5, Sd),

        // ---------------- SINTES ----------------
        PB ("Sintes", "Lead Brillante").lowCut (0, 150, 2).bell (1, 400, -2.0f, 1.0f).bell (2, 3000, 2.0f, 1.0f).highShelf (3, 10000, 3.0f),
        PB ("Sintes", "Pad Ancho").lowCut (0, 200, 2).bell (1, 500, -2.0f, 0.7f).highShelf (2, 6000, 3.0f).chan (2, Sd)
            .bell (3, 2000, -1.0f, 1.0f).chan (3, M),
        PB ("Sintes", "Pluck / Arp").lowCut (0, 250, 2).bell (1, 1000, 1.5f, 1.0f).highShelf (2, 8000, 2.0f).highCut (3, 16000, 1),
        PB ("Sintes", "Synth Grueso (Saturado)").lowCut (0, 80, 2).bell (1, 200, 2.0f, 0.8f).sat (1, S::Tube, 10, 60)
            .bell (2, 1500, 2.0f, 1.0f).sat (2, S::Tape, 8, 60).highShelf (3, 9000, 1.5f),

        // ---------------- BUSES ----------------
        PB ("Buses", "Mix Bus Pegamento").lowCut (0, 20, 0).lowShelf (1, 100, 1.0f).bell (2, 300, -1.0f, 0.7f).highShelf (3, 12000, 1.5f, 0.6f)
            .bell (4, 3000, 0, 1.0f).dyn (4, -22, -2, 10, 150),
        PB ("Buses", "Bus Instrumental").lowCut (0, 40, 1).bell (1, 250, -1.5f, 0.9f).bell (2, 2500, 0, 1.5f).dyn (2, -24, -3, 8, 120)
            .highShelf (3, 10000, 1.0f),
        PB ("Buses", "Graves en Mono").lowCut (0, 120, 3).chan (0, Sd),
        PB ("Buses", "Bus de Voces").lowCut (0, 100, 2).bell (1, 300, -1.5f, 1.0f).bell (2, 5500, 0, 2.5f).dyn (2, -26, -4, 2, 80)
            .highShelf (3, 11000, 2.0f),

        // ---------------- MASTERING ----------------
        PB ("Mastering", "Master Tech House").lowCut (0, 20, 0).lowShelf (1, 60, 1.0f, 0.7f).bell (2, 250, -0.8f, 0.8f).bell (3, 3000, 0.6f, 0.7f)
            .highShelf (4, 14000, 1.5f, 0.6f).lowCut (5, 100, 1).chan (5, Sd).set (ParamID::autoGain, 1),
        PB ("Mastering", "Master Dubstep / Bass").lowCut (0, 25, 2).bell (1, 50, 1.0f, 1.0f).bell (2, 400, -1.0f, 0.9f)
            .bell (3, 3000, 0, 1.0f).dyn (3, -24, -2, 10, 150).highShelf (4, 12000, 1.5f).lowCut (5, 120, 1).chan (5, Sd).set (ParamID::autoGain, 1),
        PB ("Mastering", "Master Aire Suave").highShelf (0, 16000, 2.0f, 0.5f).bell (1, 2000, -0.5f, 0.6f).set (ParamID::autoGain, 1),
        PB ("Mastering", "Master Graves Controlados").lowCut (0, 20, 0).bell (1, 80, 0, 1.2f).dyn (1, -20, -3, 20, 200)
            .lowCut (2, 150, 1).chan (2, Sd).highShelf (3, 12000, 1.0f).set (ParamID::autoGain, 1),
        PB ("Mastering", "Master Ancho (M/S)").lowCut (0, 20, 0).highShelf (1, 8000, 1.5f).chan (1, Sd).bell (2, 300, -1.0f, 0.8f).chan (2, Sd)
            .bell (3, 1000, 0.5f, 0.7f).chan (3, M).set (ParamID::autoGain, 1),

        // ---------------- VINTAGE ----------------
        PB ("Vintage", "Truco de Graves 60 Hz").vintage().set (ParamID::vLowFreq, 2).set (ParamID::vLowBoost, 6).set (ParamID::vLowAtten, 4),
        PB ("Vintage", "Kick 100 Hz + Click").vintage().set (ParamID::vLowFreq, 3).set (ParamID::vLowBoost, 5).set (ParamID::vLowAtten, 3)
            .set (ParamID::vHighFreq, 2).set (ParamID::vHighBoost, 3).set (ParamID::vHighBw, 3),
        PB ("Vintage", "Voz Vintage").vintage().set (ParamID::vLowCut, 3).set (ParamID::vDipFreq, 1).set (ParamID::vDip, 2)
            .set (ParamID::vHmFreq, 2).set (ParamID::vHmBoost, 2).set (ParamID::vHighFreq, 4).set (ParamID::vHighBoost, 3)
            .set (ParamID::vHighBw, 6).set (ParamID::vAttenFreq, 2).set (ParamID::vHighAtten, 1).set (ParamID::vTube, 20),
        PB ("Vintage", "Snare Vintage").vintage().set (ParamID::vLowCut, 2).set (ParamID::vLmFreq, 0).set (ParamID::vLmBoost, 2.5f)
            .set (ParamID::vDipFreq, 3).set (ParamID::vDip, 3).set (ParamID::vHmFreq, 4).set (ParamID::vHmBoost, 3)
            .set (ParamID::vHighFreq, 4).set (ParamID::vHighBoost, 2),
        PB ("Vintage", "Bajo Vintage").vintage().set (ParamID::vLowFreq, 2).set (ParamID::vLowBoost, 5).set (ParamID::vLowAtten, 2)
            .set (ParamID::vDipFreq, 1).set (ParamID::vDip, 3).set (ParamID::vHmFreq, 0).set (ParamID::vHmBoost, 2).set (ParamID::vIron, 40),
        PB ("Vintage", "Synth Calidez").vintage().set (ParamID::vDipFreq, 2).set (ParamID::vDip, 2).set (ParamID::vHighFreq, 3)
            .set (ParamID::vHighBoost, 3).set (ParamID::vAttenFreq, 1).set (ParamID::vHighAtten, 2).set (ParamID::vTube, 35).set (ParamID::vIron, 25),
        PB ("Vintage", "Bus Vintage").vintage().set (ParamID::vLowFreq, 1).set (ParamID::vLowBoost, 3).set (ParamID::vLowAtten, 2)
            .set (ParamID::vHighFreq, 5).set (ParamID::vHighBoost, 3).set (ParamID::vHighBw, 7).set (ParamID::vIron, 30).set (ParamID::vTube, 15),
        PB ("Vintage", "Master Vintage").vintage().set (ParamID::vLowFreq, 1).set (ParamID::vLowBoost, 2).set (ParamID::vLowAtten, 1)
            .set (ParamID::vHighFreq, 6).set (ParamID::vHighBoost, 2.5f).set (ParamID::vHighBw, 8).set (ParamID::vAttenFreq, 2)
            .set (ParamID::vHighAtten, 1).set (ParamID::vIron, 20).set (ParamID::vAirFreq, 3).set (ParamID::vAirGain, 2),
        PB ("Vintage", "Lo-Fi Radio").vintage().set (ParamID::vLowCut, 6).set (ParamID::vHighCut, 3).set (ParamID::vLmFreq, 4)
            .set (ParamID::vLmBoost, 4).set (ParamID::vIron, 70).set (ParamID::vTube, 60),
    };
    return list;
}

// Historial de deshacer / rehacer por "fotos" de todos los parametros.
// Se toma una foto al terminar cada accion (soltar el mouse, rueda, cargar un preset).
// La automatizacion del DAW no crea pasos.
// Comparacion A/B: dos ajustes completos para comparar al instante (se guardan con la sesion)
class ABCompare
{
public:
    ABCompare (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& s) : proc (p), apvts (s) {}

    int active() const { return activeSlot; }

    void switchTo (int slot)
    {
        slot = juce::jlimit (0, 1, slot);
        if (slot == activeSlot) return;
        slots[(size_t) activeSlot] = capture();
        if (slots[(size_t) slot].empty()) slots[(size_t) slot] = slots[(size_t) activeSlot];
        apply (slots[(size_t) slot]);
        activeSlot = slot;
        save();
    }

    // Copia el ajuste que suena ahora al otro lado (A -> B o B -> A)
    void copyToOther()
    {
        slots[(size_t) activeSlot] = capture();
        slots[(size_t) (1 - activeSlot)] = slots[(size_t) activeSlot];
        save();
    }

    // Al abrir una sesion: recuperar A, B y cual estaba sonando
    void restore()
    {
        activeSlot = juce::jlimit (0, 1, (int) apvts.state.getProperty ("abActive", 0));
        for (int i = 0; i < 2; ++i)
        {
            slots[(size_t) i].clear();
            juce::StringArray t; t.addTokens (apvts.state.getProperty (i == 0 ? "abA" : "abB").toString(), ",", {});
            for (auto& x : t) if (x.isNotEmpty()) slots[(size_t) i].push_back (x.getFloatValue());
        }
    }

private:
    static bool skip (juce::AudioProcessorParameter* p)
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            return rp->getParameterID() == ParamID::bypass || rp->getParameterID() == ParamID::analyzer;
        return true;
    }
    std::vector<float> capture() const
    {
        std::vector<float> v;
        for (auto* p : proc.getParameters()) v.push_back (p->getValue());
        return v;
    }
    void apply (const std::vector<float>& v)
    {
        auto& ps = proc.getParameters();
        for (int i = 0; i < ps.size() && i < (int) v.size(); ++i)
        {
            auto* p = ps[i];
            if (skip (p) || std::abs (p->getValue() - v[(size_t) i]) < 1.0e-7f) continue;
            p->beginChangeGesture();
            p->setValueNotifyingHost (v[(size_t) i]);
            p->endChangeGesture();
        }
    }
    void save()
    {
        for (int i = 0; i < 2; ++i)
        {
            juce::StringArray t; for (float f : slots[(size_t) i]) t.add (juce::String (f, 6));
            apvts.state.setProperty (i == 0 ? "abA" : "abB", t.joinIntoString (","), nullptr);
        }
        apvts.state.setProperty ("abActive", activeSlot, nullptr);
    }

    juce::AudioProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    std::array<std::vector<float>, 2> slots;
    int activeSlot = 0;
};

class UndoHistory
{
public:
    explicit UndoHistory (juce::AudioProcessor& p) : proc (p) {}

    void reset() { undoStack.clear(); redoStack.clear(); last = capture(); }

    // Registra un paso si algo cambio desde la ultima foto
    void commit()
    {
        auto cur = capture();
        if (last.empty()) { last = cur; return; }
        if (! differs (cur, last)) return;
        undoStack.push_back (last);
        if (undoStack.size() > 200) undoStack.erase (undoStack.begin());
        redoStack.clear();
        last = std::move (cur);
    }

    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }

    void undo()
    {
        commit();
        if (undoStack.empty()) return;
        redoStack.push_back (capture());
        apply (undoStack.back());
        undoStack.pop_back();
        last = capture();
    }

    void redo()
    {
        commit();
        if (redoStack.empty()) return;
        undoStack.push_back (capture());
        apply (redoStack.back());
        redoStack.pop_back();
        last = capture();
    }

private:
    static bool skip (juce::AudioProcessorParameter* p)
    {
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            return rp->getParameterID() == ParamID::bypass;
        return true;
    }
    std::vector<float> capture() const
    {
        std::vector<float> v;
        for (auto* p : proc.getParameters()) v.push_back (p->getValue());
        return v;
    }
    static bool differs (const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size()) return true;
        for (size_t i = 0; i < a.size(); ++i) if (std::abs (a[i] - b[i]) > 1.0e-6f) return true;
        return false;
    }
    void apply (const std::vector<float>& v)
    {
        auto& ps = proc.getParameters();
        for (int i = 0; i < ps.size() && i < (int) v.size(); ++i)
        {
            auto* p = ps[i];
            if (skip (p) || std::abs (p->getValue() - v[(size_t) i]) < 1.0e-7f) continue;
            p->beginChangeGesture();
            p->setValueNotifyingHost (v[(size_t) i]);
            p->endChangeGesture();
        }
    }

    juce::AudioProcessor& proc;
    std::vector<float> last;
    std::vector<std::vector<float>> undoStack, redoStack;
};

// Presets separados por modo: el Parametrico y el Vintage tienen cada uno su lista.
// Cargar un preset nunca cambia de modo ni toca los ajustes del otro modo.
class PresetManager
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState& s) : apvts (s) {}

    struct Entry { bool user = false; int factoryIndex = -1; juce::File file; juce::String name, category; };

    static juce::File userFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Gaux EQ").getChildFile ("Presets");
    }

    static bool isVintagePreset (const FactoryPreset& p)
    {
        for (auto& [id, v] : p.values) if (id.startsWith ("v_")) return true;
        return false;
    }

    bool vintageNow() const { return apvts.getRawParameterValue (ParamID::mode)->load() > 0.5f; }

    // Lista del modo actual: primero los de fabrica, luego los del usuario
    std::vector<Entry> entries() const
    {
        const bool v = vintageNow();
        std::vector<Entry> list;
        const auto& fp = factoryPresets();
        for (int i = 0; i < (int) fp.size(); ++i)
            if (isVintagePreset (fp[(size_t) i]) == v)
                list.push_back ({ false, i, {}, fp[(size_t) i].name, fp[(size_t) i].category });
        auto files = userFolder().findChildFiles (juce::File::findFiles, false, "*.gauxpreset");
        files.sort();
        for (auto& f : files)
            if (auto xml = juce::XmlDocument::parse (f))
                if (xml->hasTagName ("GAUXPRESET") && (xml->getStringAttribute ("mode") == "vintage") == v)
                    list.push_back ({ true, -1, f, xml->getStringAttribute ("name", f.getFileNameWithoutExtension()), "Mis presets" });
        return list;
    }

    juce::String currentName() const { return apvts.state.getProperty (key ("presetName"), "Default").toString(); }
    int currentIndex() const { return (int) apvts.state.getProperty (key ("presetIndex"), -1); }

    void load (int listIndex)
    {
        auto list = entries();
        if (listIndex < 0 || listIndex >= (int) list.size()) return;
        const auto& e = list[(size_t) listIndex];
        if (e.user) loadUser (e.file);
        else        applyFactory (e.factoryIndex);
        apvts.state.setProperty (key ("presetIndex"), listIndex, nullptr);
    }

    void step (int delta)
    {
        const int n = (int) entries().size();
        if (n == 0) return;
        const int cur = currentIndex();
        load (((cur < 0 ? (delta > 0 ? -1 : 0) : cur) + delta + n) % n);
    }

    void loadDefault()
    {
        begin ("Default");
        resetDomain (vintageNow());
        end();
        apvts.state.setProperty (key ("presetIndex"), -1, nullptr);
    }

    bool saveUser (const juce::String& name)
    {
        auto clean = juce::File::createLegalFileName (name.trim());
        if (clean.isEmpty()) return false;
        const bool v = vintageNow();
        userFolder().createDirectory();
        auto file = userFolder().getChildFile (clean + (v ? " (Vintage)" : "") + ".gauxpreset");
        juce::XmlElement xml ("GAUXPRESET");
        xml.setAttribute ("name", name.trim());
        xml.setAttribute ("mode", v ? "vintage" : "parametric");
        for (auto* p : apvts.processor.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                if (belongs (rp->getParameterID(), v))
                {
                    auto* e = xml.createNewChildElement ("P");
                    e->setAttribute ("id", rp->getParameterID());
                    e->setAttribute ("v", (double) rp->convertFrom0to1 (rp->getValue()));
                }
        if (! xml.writeTo (file)) return false;
        apvts.state.setProperty (key ("presetName"), name.trim(), nullptr);
        auto list = entries();
        for (int i = 0; i < (int) list.size(); ++i)
            if (list[(size_t) i].file == file) apvts.state.setProperty (key ("presetIndex"), i, nullptr);
        return true;
    }

    UndoHistory* history = nullptr;

private:
    juce::Identifier key (const char* base) const { return juce::String (base) + (vintageNow() ? "_v" : "_p"); }

    // Que parametros pertenecen a cada modo
    static bool belongs (const juce::String& id, bool vintage)
    {
        if (vintage) return id.startsWith ("v_");
        return (id.startsWith ("b") && id.containsChar ('_')) || id == ParamID::gainScale || id == ParamID::autoGain;
    }

    void begin (const juce::String& name)
    {
        if (history != nullptr) history->commit();   // cierra lo que habia antes del preset
        apvts.state.setProperty (key ("presetName"), name, nullptr);
    }
    void end() { if (history != nullptr) history->commit(); }

    void setReal (const juce::String& id, float v)
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (v));
            p->endChangeGesture();
        }
    }

    void resetDomain (bool vintage)
    {
        for (auto* p : apvts.processor.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                if (belongs (rp->getParameterID(), vintage) && std::abs (rp->getValue() - rp->getDefaultValue()) > 1.0e-6f)
                {
                    rp->beginChangeGesture();
                    rp->setValueNotifyingHost (rp->getDefaultValue());
                    rp->endChangeGesture();
                }
    }

    void applyFactory (int i)
    {
        const auto& fp = factoryPresets()[(size_t) i];
        const bool v = isVintagePreset (fp);
        begin (fp.name);
        resetDomain (v);
        for (auto& [id, val] : fp.values)
            if (belongs (id, v)) setReal (id, val);
        end();
    }

    void loadUser (const juce::File& f)
    {
        auto xml = juce::XmlDocument::parse (f);
        if (xml == nullptr || ! xml->hasTagName ("GAUXPRESET")) return;
        const bool v = xml->getStringAttribute ("mode") == "vintage";
        begin (xml->getStringAttribute ("name", f.getFileNameWithoutExtension()));
        resetDomain (v);
        for (auto* e : xml->getChildWithTagNameIterator ("P"))
            if (belongs (e->getStringAttribute ("id"), v))
                setReal (e->getStringAttribute ("id"), (float) e->getDoubleAttribute ("v"));
        end();
    }

    juce::AudioProcessorValueTreeState& apvts;
};
} // namespace gaux
