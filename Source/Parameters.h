// Gaux Equalizer - Definicion de parametros (los que ve el DAW para automatizar)
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/EqBand.h"
#include "DSP/VintageEq.h"

namespace gaux
{
constexpr int kNumBands = 24;
constexpr int kLegacyBands = 16;   // las primeras 16 conservan sus valores por defecto y su lugar de siempre

namespace ParamID
{
    inline const juce::String mode       = "mode";
    inline const juce::String output     = "output";
    inline const juce::String quality    = "quality";
    inline const juce::String analyzer   = "analyzer";
    inline const juce::String bypass     = "bypass";
    inline const juce::String autoGain   = "autoGain";
    inline const juce::String gainScale  = "gainScale";
    inline const juce::String rangeOn    = "rangeOn";
    inline const juce::String rangeLo    = "rangeLo";
    inline const juce::String rangeHi    = "rangeHi";
    inline const juce::String scOn       = "scOn";      // sidechain general
    inline const juce::String scDuck     = "scDuck";    // cuanto se encoge la curva cuando suena el sidechain
    inline const juce::String scThr      = "scThr";     // nivel del sidechain a partir del cual actua
    inline const juce::String scRel      = "scRel";     // cuanto tarda la curva en volver despues del golpe
    inline const juce::String phase      = "phase";     // 0 fase minima (sin latencia), 1 lineal, 2 lineal HQ, 3 natural

    inline juce::String band (int b, const char* name) { return "b" + juce::String (b + 1) + "_" + name; }

    inline const juce::String vLowFreq   = "v_lowFreq";
    inline const juce::String vLowBoost  = "v_lowBoost";
    inline const juce::String vLowAtten  = "v_lowAtten";
    inline const juce::String vHighFreq  = "v_highFreq";
    inline const juce::String vHighBoost = "v_highBoost";
    inline const juce::String vHighBw    = "v_highBw";
    inline const juce::String vAttenFreq = "v_attenFreq";
    inline const juce::String vHighAtten = "v_highAtten";
    inline const juce::String vAirFreq   = "v_airFreq";
    inline const juce::String vAirGain   = "v_airGain";
    inline const juce::String vTube      = "v_tube";
    inline const juce::String vLmFreq    = "v_lmFreq";
    inline const juce::String vLmBoost   = "v_lmBoost";
    inline const juce::String vDipFreq   = "v_dipFreq";
    inline const juce::String vDip       = "v_dip";
    inline const juce::String vHmFreq    = "v_hmFreq";
    inline const juce::String vHmBoost   = "v_hmBoost";
    inline const juce::String vLowCut    = "v_lowCut";
    inline const juce::String vHighCut   = "v_highCut";
    inline const juce::String vIron      = "v_iron";
    inline const juce::String vProgramIn = "v_programIn";
    inline const juce::String vMidIn     = "v_midIn";
    inline const juce::String vFilterIn  = "v_filterIn";
}

// Frecuencias y tipos por defecto de cada banda (todas vienen apagadas)
inline double defaultFreq (int b)
{
    if (b < kLegacyBands) return 30.0 * std::pow (16000.0 / 30.0, b / (double) (kLegacyBands - 1));
    return 40.0 * std::pow (12000.0 / 40.0, (b - kLegacyBands) / (double) (kNumBands - kLegacyBands - 1));
}
inline int defaultType (int b)
{
    if (b == 0) return (int) BandType::LowCut;
    if (b == 1) return (int) BandType::LowShelf;
    if (b == kLegacyBands - 2) return (int) BandType::HighShelf;
    if (b == kLegacyBands - 1) return (int) BandType::HighCut;
    return (int) BandType::Bell;
}

inline juce::StringArray toStringArray (const char* const* names, int n)
{
    juce::StringArray a;
    for (int i = 0; i < n; ++i) a.add (names[i]);
    return a;
}

inline juce::NormalisableRange<float> freqRange()
{
    juce::NormalisableRange<float> r (20.0f, 20000.0f);
    r.setSkewForCentre (632.0f); // centro geometrico 20..20k
    return r;
}

inline juce::String formatFreq (float hz, int = 0)
{
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz"
                         : juce::String (juce::roundToInt (hz)) + " Hz";
}

// Se puede escribir "2500", "2.5k", "2k5", "2.5 kHz" o una NOTA: "A4" (440 Hz), "C#2", "Eb1"...
inline float parseFreq (const juce::String& t)
{
    const auto s = t.trim();
    // nota musical
    if (s.isNotEmpty() && juce::String ("ABCDEFGabcdefg").containsChar (s[0]))
    {
        static const int semis[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
        int k = semis[(juce::CharacterFunctions::toUpperCase (s[0]) - 'A')];
        int pos = 1;
        if (pos < s.length() && (s[pos] == '#' || s[pos] == 's')) { ++k; ++pos; }
        else if (pos < s.length() && (s[pos] == 'b')) { --k; ++pos; }
        const auto rest = s.substring (pos).trim();
        if (rest.containsOnly ("-0123456789") && rest.isNotEmpty())
        {
            const int octave = rest.getIntValue();
            const int midi = (octave + 1) * 12 + k;
            return (float) (440.0 * std::pow (2.0, (midi - 69) / 12.0));
        }
    }
    const auto lower = s.toLowerCase();
    if (lower.contains ("k"))
    {
        // "2k5" = 2500
        const auto before = lower.upToFirstOccurrenceOf ("k", false, false), after = lower.fromFirstOccurrenceOf ("k", false, false).trim();
        if (after.isNotEmpty() && after.containsOnly ("0123456789") && ! before.contains ("."))
            return (before + "." + after).getFloatValue() * 1000.0f;
        return before.getFloatValue() * 1000.0f;
    }
    return lower.getFloatValue();
}

inline juce::String formatDb (float db, int = 0)
{
    return (db > 0.004f ? "+" : "") + juce::String (db, 1) + " dB";
}

inline juce::AudioParameterFloatAttributes freqAttr()
{
    return juce::AudioParameterFloatAttributes().withStringFromValueFunction (formatFreq)
                                                .withValueFromStringFunction (parseFreq);
}

inline juce::AudioParameterFloatAttributes dbAttr()
{
    return juce::AudioParameterFloatAttributes().withStringFromValueFunction (formatDb)
                                                .withValueFromStringFunction ([] (const juce::String& t) { return t.getFloatValue(); });
}

inline juce::AudioParameterFloatAttributes decimalsAttr (int decimals, const juce::String& suffix = {})
{
    return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([decimals, suffix] (float v, int) { return juce::String (v, decimals) + suffix; })
        .withValueFromStringFunction ([] (const juce::String& t) { return t.getFloatValue(); });
}

// Selectores e interruptores que recuerdan el valor EXACTO que les manda el DAW (p. ej. 0.73 en un selector de
// 2 opciones), aunque el sonido use la opcion redondeada. Asi el DAW (y pluginval) lee siempre lo mismo que
// escribio, tambien al guardar y abrir la sesion. El valor redondeado (el que usa el audio) lo guarda el APVTS.
struct GauxChoiceParameter : public juce::AudioParameterChoice
{
    template <typename... Args>
    explicit GauxChoiceParameter (Args&&... args) : juce::AudioParameterChoice (std::forward<Args> (args)...)
    { raw.store (convertTo0to1 ((float) getIndex())); }
    float getValue() const override { return raw.load(); }
    void setValue (float v) override { raw.store (juce::jlimit (0.0f, 1.0f, v)); }
    std::atomic<float> raw { 0.0f };
};
struct GauxBoolParameter : public juce::AudioParameterBool
{
    template <typename... Args>
    explicit GauxBoolParameter (Args&&... args) : juce::AudioParameterBool (std::forward<Args> (args)...)
    { raw.store (get() ? 1.0f : 0.0f); }
    float getValue() const override { return raw.load(); }
    void setValue (float v) override { raw.store (juce::jlimit (0.0f, 1.0f, v)); }
    std::atomic<float> raw { 0.0f };
};

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    int version = 1;

    layout.add (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::mode, version }, "Modo",
                                                        StringArray { "Parametrico", "Vintage" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::output, version }, "Output",
                                                       NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f, dbAttr()));
    layout.add (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::quality, version }, "Calidad (HQ)",
                                                        StringArray { "Normal", "HQ 2x", "HQ 4x" }, 0));
    layout.add (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::analyzer, version }, "Analizador", true));
    layout.add (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::phase, version }, "Fase",
                                                        StringArray { "Minima (0 latencia)", "Lineal", "Lineal HQ", "Natural" }, 0));
    layout.add (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::bypass, version }, "Bypass", false));
    layout.add (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::autoGain, version }, "Auto Gain", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::gainScale, version }, "Gain Scale",
                                                       NormalisableRange<float> (0.0f, 200.0f, 0.1f), 100.0f, decimalsAttr (0, " %")));
    layout.add (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::rangeOn, version }, "Rango de proceso", false));
    layout.add (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::scOn, version }, "Sidechain General", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::scDuck, version }, "Sidechain Duck",
                                                       NormalisableRange<float> (0.0f, 100.0f, 0.1f), 60.0f, decimalsAttr (0, " %")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::scThr, version }, "Sidechain Threshold",
                                                       NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -30.0f, decimalsAttr (1, " dB")));
    {
        NormalisableRange<float> scRelRange (10.0f, 1000.0f, 0.1f); scRelRange.setSkewForCentre (120.0f);
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::scRel, version }, "Sidechain Release",
                                                           scRelRange, 120.0f, decimalsAttr (0, " ms")));
    }
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::rangeLo, version }, "Rango Low", freqRange(), 20.0f, freqAttr()));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::rangeHi, version }, "Rango High", freqRange(), 20000.0f, freqAttr()));

    auto addBand = [&] (int b)
    {
        const String n = "Banda " + String (b + 1) + " ";
        auto group = std::make_unique<AudioProcessorParameterGroup> ("band" + String (b + 1), n.trim(), "|");

        group->addChild (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::band (b, "on"), version }, n + "On", false));
        group->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::band (b, "type"), version }, n + "Tipo",
                                                                 toStringArray (kBandTypeNames, kNumBandTypes), defaultType (b)));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::band (b, "freq"), version }, n + "Freq",
                                                                freqRange(), (float) defaultFreq (b), freqAttr()));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::band (b, "gain"), version }, n + "Gain",
                                                                NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f, dbAttr()));
        NormalisableRange<float> qRange (0.1f, 18.0f, 0.001f);
        qRange.setSkewForCentre (1.0f);
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::band (b, "q"), version }, n + "Q",
                                                                qRange, 0.707f, decimalsAttr (2)));
        group->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::band (b, "slope"), version }, n + "Pendiente",
                                                                 toStringArray (kSlopeNames, kNumSlopes), 1));
        group->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::band (b, "satType"), version }, n + "Sat Tipo",
                                                                 toStringArray (kSatTypeNames, kNumSatTypes), 0));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::band (b, "drive"), version }, n + "Sat Drive",
                                                                NormalisableRange<float> (0.0f, 36.0f, 0.01f), 0.0f, decimalsAttr (1, " dB")));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::band (b, "mix"), version }, n + "Sat Mix",
                                                                NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, decimalsAttr (0, " %")));
        group->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::band (b, "stereo"), version }, n + "Canal",
                                                                 toStringArray (kStereoNames, kNumStereoModes), 0));
        group->addChild (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::band (b, "dyn"), version }, n + "Dinamica", false));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::band (b, "thr"), version }, n + "Threshold",
                                                                NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -24.0f, decimalsAttr (1, " dB")));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::band (b, "range"), version }, n + "Range",
                                                                NormalisableRange<float> (-24.0f, 24.0f, 0.01f), -6.0f, dbAttr()));
        NormalisableRange<float> atk (0.1f, 200.0f, 0.01f); atk.setSkewForCentre (10.0f);
        NormalisableRange<float> rel (5.0f, 2000.0f, 0.1f); rel.setSkewForCentre (120.0f);
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::band (b, "atk"), version }, n + "Attack",
                                                                atk, 10.0f, decimalsAttr (1, " ms")));
        group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::band (b, "rel"), version }, n + "Release",
                                                                rel, 120.0f, decimalsAttr (0, " ms")));
        group->addChild (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::band (b, "autoThr"), version }, n + "Threshold Auto", false));
        group->addChild (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::band (b, "ext"), version }, n + "Sidechain Externo", false));
        layout.add (std::move (group));
    };
    for (int b = 0; b < kLegacyBands; ++b) addBand (b);

    auto knob = [] (float max) { return NormalisableRange<float> (0.0f, max, 0.01f); };
    auto vg = std::make_unique<AudioProcessorParameterGroup> ("vintage", "Vintage", "|");
    vg->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::vLowFreq, version }, "V Low Freq",
                                                          toStringArray (kVLowFreqNames, kNumVLowFreqs), 2));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vLowBoost, version }, "V Low Boost", knob (10), 0.0f, decimalsAttr (1)));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vLowAtten, version }, "V Low Atten", knob (10), 0.0f, decimalsAttr (1)));
    vg->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::vHighFreq, version }, "V High Freq",
                                                          toStringArray (kVHighFreqNames, kNumVHighFreqs), 4));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vHighBoost, version }, "V High Boost", knob (10), 0.0f, decimalsAttr (1)));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vHighBw, version }, "V Bandwidth", knob (10), 5.0f, decimalsAttr (1)));
    vg->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::vAttenFreq, version }, "V Atten Freq",
                                                          toStringArray (kVAttenFreqNames, kNumVAttenFreqs), 1));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vHighAtten, version }, "V High Atten", knob (10), 0.0f, decimalsAttr (1)));
    vg->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::vAirFreq, version }, "V Air Freq",
                                                          toStringArray (kVAirFreqNames, kNumVAirFreqs), 3));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vAirGain, version }, "V Air", knob (10), 0.0f, decimalsAttr (1, " dB")));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vTube, version }, "V Tube", knob (100), 0.0f, decimalsAttr (0, " %")));
    vg->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::vLmFreq, version }, "V Mid Low Freq",
                                                          toStringArray (kVLmFreqNames, kNumVLmFreqs), 1));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vLmBoost, version }, "V Mid Low Peak", knob (10), 0.0f, decimalsAttr (1)));
    vg->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::vDipFreq, version }, "V Dip Freq",
                                                          toStringArray (kVDipFreqNames, kNumVDipFreqs), 4));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vDip, version }, "V Dip", knob (10), 0.0f, decimalsAttr (1)));
    vg->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::vHmFreq, version }, "V Mid High Freq",
                                                          toStringArray (kVHmFreqNames, kNumVHmFreqs), 2));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vHmBoost, version }, "V Mid High Peak", knob (10), 0.0f, decimalsAttr (1)));
    vg->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::vLowCut, version }, "V Low Cut",
                                                          toStringArray (kVLowCutNames, kNumVLowCuts), 0));
    vg->addChild (std::make_unique<GauxChoiceParameter> (ParameterID { ParamID::vHighCut, version }, "V High Cut",
                                                          toStringArray (kVHighCutNames, kNumVHighCuts), kNumVHighCuts - 1));
    vg->addChild (std::make_unique<AudioParameterFloat> (ParameterID { ParamID::vIron, version }, "V Iron", knob (100), 0.0f, decimalsAttr (0, " %")));
    vg->addChild (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::vProgramIn, version }, "V Program In", true));
    vg->addChild (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::vMidIn, version }, "V Mid In", true));
    vg->addChild (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::vFilterIn, version }, "V Filter In", true));
    layout.add (std::move (vg));

    // Bandas 17 a 24: al final de la lista (el orden de los parametros de siempre no cambia)
    for (int b = kLegacyBands; b < kNumBands; ++b) addBand (b);

    // Dinamica espectral por banda (v0.21): tambien al final
    {
        auto sg = std::make_unique<AudioProcessorParameterGroup> ("spectral", "Spectral", "|");
        for (int b = 0; b < kNumBands; ++b)
            sg->addChild (std::make_unique<GauxBoolParameter> (ParameterID { ParamID::band (b, "spec"), version },
                                                                "Banda " + String (b + 1) + " Spectral", false));
        layout.add (std::move (sg));
    }

    return layout;
}

// Acceso rapido (sin bloqueos) a los valores crudos, usado por audio e interfaz
struct ParamRefs
{
    std::atomic<float>* mode = nullptr;
    std::atomic<float>* output = nullptr;
    std::atomic<float>* quality = nullptr;
    std::atomic<float>* analyzer = nullptr;
    std::atomic<float>* bypass = nullptr;
    std::atomic<float>* autoGain = nullptr;
    std::atomic<float>* gainScale = nullptr;
    std::atomic<float>* rangeOn = nullptr;
    std::atomic<float>* rangeLo = nullptr;
    std::atomic<float>* rangeHi = nullptr;
    std::atomic<float> *scOn = nullptr, *scDuck = nullptr, *scThr = nullptr, *scRel = nullptr, *phase = nullptr;

    struct Band { std::atomic<float> *on, *type, *freq, *gain, *q, *slope, *satType, *drive, *mix,
                                     *stereo, *dyn, *thr, *range, *atk, *rel, *autoThr, *ext, *spec; };
    std::array<Band, kNumBands> bands {};

    std::atomic<float> *vLowFreq, *vLowBoost, *vLowAtten, *vHighFreq, *vHighBoost, *vHighBw,
                       *vAttenFreq, *vHighAtten, *vAirFreq, *vAirGain, *vTube,
                       *vLmFreq, *vLmBoost, *vDipFreq, *vDip, *vHmFreq, *vHmBoost, *vLowCut, *vHighCut, *vIron,
                       *vProgramIn, *vMidIn, *vFilterIn;

    void init (juce::AudioProcessorValueTreeState& s)
    {
        auto get = [&s] (const juce::String& id) { auto* p = s.getRawParameterValue (id); jassert (p != nullptr); return p; };
        mode = get (ParamID::mode); output = get (ParamID::output);
        quality = get (ParamID::quality); analyzer = get (ParamID::analyzer);
        bypass = get (ParamID::bypass); autoGain = get (ParamID::autoGain); gainScale = get (ParamID::gainScale);
        rangeOn = get (ParamID::rangeOn); rangeLo = get (ParamID::rangeLo); rangeHi = get (ParamID::rangeHi);
        scOn = get (ParamID::scOn); scDuck = get (ParamID::scDuck); scThr = get (ParamID::scThr); scRel = get (ParamID::scRel); phase = get (ParamID::phase);
        for (int b = 0; b < kNumBands; ++b)
            bands[(size_t) b] = { get (ParamID::band (b, "on")), get (ParamID::band (b, "type")), get (ParamID::band (b, "freq")),
                                  get (ParamID::band (b, "gain")), get (ParamID::band (b, "q")), get (ParamID::band (b, "slope")),
                                  get (ParamID::band (b, "satType")), get (ParamID::band (b, "drive")), get (ParamID::band (b, "mix")),
                                  get (ParamID::band (b, "stereo")), get (ParamID::band (b, "dyn")), get (ParamID::band (b, "thr")),
                                  get (ParamID::band (b, "range")), get (ParamID::band (b, "atk")), get (ParamID::band (b, "rel")),
                                  get (ParamID::band (b, "autoThr")), get (ParamID::band (b, "ext")), get (ParamID::band (b, "spec")) };
        vLowFreq = get (ParamID::vLowFreq); vLowBoost = get (ParamID::vLowBoost); vLowAtten = get (ParamID::vLowAtten);
        vHighFreq = get (ParamID::vHighFreq); vHighBoost = get (ParamID::vHighBoost); vHighBw = get (ParamID::vHighBw);
        vAttenFreq = get (ParamID::vAttenFreq); vHighAtten = get (ParamID::vHighAtten);
        vAirFreq = get (ParamID::vAirFreq); vAirGain = get (ParamID::vAirGain); vTube = get (ParamID::vTube);
        vLmFreq = get (ParamID::vLmFreq); vLmBoost = get (ParamID::vLmBoost); vDipFreq = get (ParamID::vDipFreq);
        vDip = get (ParamID::vDip); vHmFreq = get (ParamID::vHmFreq); vHmBoost = get (ParamID::vHmBoost);
        vLowCut = get (ParamID::vLowCut); vHighCut = get (ParamID::vHighCut); vIron = get (ParamID::vIron);
        vProgramIn = get (ParamID::vProgramIn); vMidIn = get (ParamID::vMidIn); vFilterIn = get (ParamID::vFilterIn);
    }

    // Ganancia tal como la ve el usuario (sin Gain Scale), util para la interfaz
    BandSettings bandRaw (int b) const { return band (b, false); }

    BandSettings band (int b, bool applyScale = true) const
    {
        const auto& p = bands[(size_t) b];
        const double scale = applyScale ? gainScale->load() / 100.0 : 1.0;
        BandSettings s;
        s.on = p.on->load() > 0.5f;
        s.type = (int) p.type->load();
        s.freq = p.freq->load();
        s.gainDb = p.gain->load() * scale;
        s.q = p.q->load();
        s.slope = (int) p.slope->load();
        s.satType = (int) p.satType->load();
        s.driveDb = p.drive->load();
        s.mix = p.mix->load() / 100.0;
        s.stereo = (int) p.stereo->load();
        s.dynOn = p.dyn->load() > 0.5f;
        s.thresholdDb = p.thr->load();
        s.rangeDb = p.range->load() * scale;
        s.attackMs = p.atk->load();
        s.releaseMs = p.rel->load();
        s.autoThr = p.autoThr->load() > 0.5f;
        s.extSc = p.ext->load() > 0.5f;
        s.spectral = p.spec->load() > 0.5f;
        return s;
    }

    VintageSettings vintage() const
    {
        VintageSettings v;
        v.lowFreq = (int) vLowFreq->load(); v.lowBoost = vLowBoost->load(); v.lowAtten = vLowAtten->load();
        v.highFreq = (int) vHighFreq->load(); v.highBoost = vHighBoost->load(); v.highBandwidth = vHighBw->load();
        v.attenFreq = (int) vAttenFreq->load(); v.highAtten = vHighAtten->load();
        v.airFreq = (int) vAirFreq->load(); v.airGain = vAirGain->load(); v.tube = vTube->load();
        v.lmFreq = (int) vLmFreq->load(); v.lmBoost = vLmBoost->load(); v.dipFreq = (int) vDipFreq->load();
        v.dip = vDip->load(); v.hmFreq = (int) vHmFreq->load(); v.hmBoost = vHmBoost->load();
        v.lowCut = (int) vLowCut->load(); v.highCut = (int) vHighCut->load(); v.iron = vIron->load();
        v.programIn = vProgramIn->load() > 0.5f; v.midIn = vMidIn->load() > 0.5f; v.filterIn = vFilterIn->load() > 0.5f;
        return v;
    }
};
} // namespace gaux
