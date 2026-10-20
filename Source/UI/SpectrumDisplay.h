// Gaux Equalizer - Pantalla de espectro con curva de EQ y puntos arrastrables
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "../PluginProcessor.h"
#include "GauxLookAndFeel.h"
#include "Widgets.h"
#include "FrameClock.h"
#include "../DSP/SketchCurve.h"

namespace gaux
{
class SpectrumDisplay : public juce::Component
{
public:
    explicit SpectrumDisplay (GauxEqProcessor& p);
    ~SpectrumDisplay() override;

    void setVintageMode (bool v) { vintageMode = v; dbRange = v ? 20.0 : 24.0; repaint(); }
    void setSelectedBand (int b)
    {
        selectedBand = b;
        // elegir una banda que no esta en el grupo seleccionado deja solo esa
        if (b < 0 || b >= kNumBands || ! multiSel[(size_t) b]) { multiSel.fill (false); if (b >= 0 && b < kNumBands) multiSel[(size_t) b] = true; }
        repaint();
    }

    std::function<void (int)> onBandSelected;

    void paint (juce::Graphics&) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    juce::Point<float> nodePosition (int band) const;
    float dynamicGain (int band) const;
    void resized() override {}

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    // Opciones de la pantalla (clic derecho en la pantalla vacia; se guardan con la sesion)
    struct ViewOpts { int speed = 1, tilt = 2, range = 2; bool freeze = false, pre = true, piano = false, bandCurves = true; };
    ViewOpts viewOptions() const;
    void setViewOption (const char* key, int value);
    static constexpr double kRanges[4] = { 6.0, 12.0, 24.0, 36.0 };
    static constexpr double kTilts[4] = { 0.0, 3.0, 4.5, 6.0 };

    // Seleccion de varias bandas (Shift + clic, o arrastrar un recuadro en la pantalla vacia)
    bool isBandInSelection (int b) const { return b >= 0 && b < kNumBands && multiSel[(size_t) b]; }
    int selectionCount() const { int n = 0; for (bool v : multiSel) n += v ? 1 : 0; return n; }
    void selectAll();
    void deleteSelection();

    // Un cuadro de animacion (lo llama el reloj de la pantalla; publico para las pruebas)
    void advanceFrame (float dt);
    const FrameClock& getClock() const { return clock; }
    bool lastFrameDrew() const { return lastDirty; }

    // Para pruebas automaticas
    int getDragBand() const { return dragBand; }
    juce::Rectangle<float> analyzerShapeBounds() const { return post.fill.getBounds(); }
    juce::Point<float> deleteBadgePosition (int band) const;
    juce::Point<float> rangeHandlePosition (int band) const;
    float zoneX (bool high) const;
    // SKETCH (dibujar la curva)
    bool isSketchMode() const { return sketchMode; }
    void setSketchMode (bool on);
    juce::Rectangle<float> sketchButtonBounds() const;
    int lastSketchPlaced() const { return sketchPlaced; }
    bool sketchPreviewVisible() const { return inking && ! sketchFit.bands.empty(); }
    juce::Point<float> pointFor (double f, double db) const { return { freqToX (f), dbToY (db) }; }
    float xForFreq (double f) const { return freqToX (f); }   // (el teclado de abajo usa la misma escala)

    // CAZA DE PICOS (agarrar del analizador): resonancias que sobresalen de su entorno
    struct GrabPeak { double f = 0, q = 4; float db = -140, prom = 0, alpha = 0; bool alive = false; };
    const std::vector<GrabPeak>& grabPeaks() const { return peaks; }
    juce::Point<float> peakMarker (const GrabPeak& p) const;

    // CHOQUES con otra pista (otro Gaux del proyecto)
    juce::Rectangle<float> clashButtonBounds() const;
    void setClashTarget (int sourceId);           // -1 = ninguno
    int getClashTarget() const { return clashTarget; }
    const std::vector<ClashZone>& getClashZones() const { return clashZonesNow; }
    juce::Point<float> clashZoneHandle (const ClashZone& z) const;
    void showClashMenu();

private:
    struct Analyzer
    {
        std::vector<float> ring, smoothed;
        int pos = 0;
        bool fresh = false, visible = false;
        juce::Path fill, outline;
    };

    bool updateAnalyzer (Analyzer&, AnalyzerFifo&, float dt);   // true si cambio lo que se ve

    // Curvas en cache: cada banda se recalcula solo cuando cambia su ajuste
    struct Curve { BandSettings s; bool valid = false; std::vector<float> db; juce::int64 version = 0; };
    // Trazo ya generado de una curva: se reutiliza mientras la curva no cambie (version)
    struct StrokeCache
    {
        juce::int64 key = -1;
        juce::Path path;
        template <typename Fn> const juce::Path& get (juce::int64 k, Fn&& build)
        {
            if (k != key) { key = k; path.clear(); build (path); }
            return path;
        }
    };
    bool refreshCurves();
    bool updateCurve (Curve&, const BandSettings&);
    void pathFromDb (const std::vector<float>& db, std::vector<juce::Point<float>>& pts) const;
    juce::int64 stateSignature() const;

    juce::Rectangle<float> plotArea() const;
    float freqToX (double f) const;
    double xToFreq (float x) const;
    float dbToY (double db) const;
    double yToDb (float y) const;
    float analyzerDbToY (float db) const;

    void paintGrid (juce::Graphics&, juce::Rectangle<float>, juce::Colour lineCol, juce::Colour labelCol);
    int hitTestNode (juce::Point<float> p) const;
    int createBandAt (juce::Point<float> p);
    void applyDragPosition();
    bool hasRangeHandle (int band) const;
    int hitTestZone (juce::Point<float> p) const;   // 0 = ninguno, 1 = LO, 2 = HI
    void paintZone (juce::Graphics&, juce::Rectangle<float>);
    juce::Colour colourOf (int band) const;

    juce::RangedAudioParameter* param (int band, const char* name) const;
    void setParam (juce::RangedAudioParameter* p, float value);
    void beginGesture (int band);
    void endGesture();
    void showBandMenu (int band);

    GauxEqProcessor& proc;
    // Curvas
    std::vector<double> gridFreqs;
    std::vector<float> gridX;
    double gridFs = 0, gridBrickRes = 0; bool gridLin = false, gridNat = false; juce::Rectangle<float> gridArea;
    std::array<Curve, kNumBands> liveCurve, statCurve, fullCurve;
    // Dinamica espectral: curva en vivo = estatica + accion por frecuencia (la exporta el motor)
    std::array<std::vector<float>, kNumBands> specDb;
    std::array<juce::int64, kNumBands> specVersion {}, specBase {};
    std::array<bool, kNumBands> specOn {};
    std::array<std::array<float, kSpectralExportPoints>, kNumBands> specLast {};
    std::vector<float> gridExIdx;   // posicion de cada punto de la grilla en la curva exportada
    const std::vector<float>& liveDb (int b) const { return specOn[(size_t) b] ? specDb[(size_t) b] : liveCurve[(size_t) b].db; }
    juce::int64 liveVersion (int b) const { return specOn[(size_t) b] ? specVersion[(size_t) b] : liveCurve[(size_t) b].version; }
    bool isSpectral (int b) const;
    std::vector<float> totalDb;
    juce::int64 curveSerial = 0, totalVersion = 0;
    double curvesDbRange = 0;
    StrokeCache totalGlowWide, totalGlow, totalLine, focusFill, focusLine;
    std::array<std::array<StrokeCache, 5>, kNumBands> dynStrokes;
    std::array<StrokeCache, kNumBands> bandFill, bandLine;   // curva de cada banda siempre visible en su color
    VintageSettings vintageCached; bool vintageValid = false;
    std::vector<float> vintageDb;
    juce::int64 lastSignature = 0;
    double animTime = 0;
    float fpsAcc = 0;
    int lastMatchVersion = -1;
    bool lastDirty = false;
    bool hoverFrozen = false;   // con el mouse quieto sobre la pantalla el espectro y los picos se congelan
    static inline bool showFps = false;   // clic derecho en la pantalla vacia   // reloj de las animaciones (se congela en silencio: sin audio no se gasta CPU)
    // Capas en cache (fondo, resplandor, grilla, degradado del analizador)
    LayerCache bgLayer, glowLayer, glassLayer;
    bool vintageMode = false;
    int selectedBand = 0;
    int hoverBand = -1;
    int dragBand = -1;
    int justCreated = -1;
    enum class Drag { None, Node, DynRange, ZoneLo, ZoneHi, Ink, Lasso };
    Drag dragMode = Drag::None;
    int hoverZone = 0;
    bool hoverDelete = false, hoverRange = false;
    bool altSolo = false;
    float bloom = 0.0f;
    double dbRange = 24.0;

    // SKETCH: el trazo ("tinta"), el ajuste en vivo (fantasma) y la animacion al soltar
    SketchCurve sketch;
    bool sketchMode = false, inking = false, hoverSketchButton = false;
    std::vector<double> sketchFixed;     // curva que ya suena (bandas existentes) en la grilla del trazo
    int sketchFree = 0, sketchPlaced = 0;
    FitResult sketchFit;
    double lastSketchFitMs = 0;
    bool sketchFitStale = false;
    float inkFade = 0.0f;                // la tinta se desvanece al soltar
    struct Burst { juce::Point<float> p; juce::Colour c; float age = 0; };
    std::vector<Burst> bursts;           // anillos que nacen donde aparecen las bandas nuevas
    void startInk (juce::Point<float>);
    void addInk (juce::Point<float>);
    void finishInk();
    void refitSketch();
    void paintSketch (juce::Graphics&, juce::Rectangle<float>);

    // Caza de picos: promedio lento del analizador (para que las marcas no tiemblen) y deteccion ~10 veces/s
    std::vector<float> longDb;
    std::vector<GrabPeak> peaks;
    float peakTimer = 0.0f;
    int hoverPeak = -1, grabbedPeak = -1;
    bool grabMoved = false;
    bool detectPeaks (float dt);
    int hitTestPeak (juce::Point<float>) const;
    void paintPeaks (juce::Graphics&, juce::Rectangle<float>);

    // Opciones, seleccion multiple y recuadro
    ViewOpts opts;
    double optsTilt = -1;
    std::array<bool, kNumBands> multiSel {};
    struct GroupStart { double logF = 0, gain = 0; };
    std::array<GroupStart, kNumBands> groupStart {};
    juce::Rectangle<float> lasso;
    juce::Point<float> lassoStart;
    void applyGroupDrag();
    void paintPiano (juce::Graphics&, juce::Rectangle<float>);
    float bottomInset() const { return 0.0f; }

    // Choques
    int clashTarget = -1, lastClashTick = -1, hoverClash = -1;
    bool clashSubscribed = false, hoverClashButton = false;
    juce::String clashName;
    juce::Colour clashColour;
    std::array<float, kClashPoints> clashMine {}, clashOther {}, clashOtherSlow {}, clashMineSlow {}, clashSt {};
    std::vector<ClashZone> clashZonesNow;
    bool updateClash();
    int hitTestClash (juce::Point<float>) const;
    void paintClash (juce::Graphics&, juce::Rectangle<float>);
    void openClashSpace (const ClashZone&);

    // Arrastre estable: el punto "virtual" se mueve con el delta del mouse (Shift = fino)
    juce::Point<float> dragNode, lastMouse;

    // Analizadores (antes y despues del EQ)
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize = 1 << kFftOrder;
    juce::dsp::FFT fft { kFftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) kFftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> fftData, pullBuffer, tilt;
    double tiltFs = 0;
    Analyzer post, pre;

    FrameClock clock { *this, [this] (float dt) { advanceFrame (dt); } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumDisplay)
};
} // namespace gaux
