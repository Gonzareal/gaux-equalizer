// Gaux Equalizer - Reloj de animacion sincronizado con la pantalla
//
// Llama a onFrame (dt) en cada refresco real del monitor (60, 120 o 144 Hz; en un MacBook Pro con
// ProMotion, 120). Las animaciones usan dt (segundos reales), asi se mueven igual a cualquier FPS.
//
// Gobernador de costo: cada componente informa cuanto tarda en dibujarse. En una maquina rapida va al
// maximo del monitor; si un cuadro cuesta mucho (PC lento, ventana enorme) baja la frecuencia lo justo
// para no quitarle el hilo de la interfaz al DAW (ver vblank).
// Si el componente no esta a la vista (ventana cerrada, otra pestana), no hace nada.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include <algorithm>

namespace gaux
{
class FrameClock
{
public:
    FrameClock (juce::Component& c, std::function<void (float)> onFrameIn)
        : owner (c), onFrame (std::move (onFrameIn)),
          attachment (&c, [this] (double ts) { vblank (ts); })
    {
        registry().push_back (this);
    }
    ~FrameClock() { auto& r = registry(); r.erase (std::remove (r.begin(), r.end(), this), r.end()); }

    // Solo para pruebas sin monitor: avanzar un cuadro en todos los relojes visibles
    static void pumpAllForTests (float dt)
    {
        auto copy = registry();
        for (auto* c : copy) if (c->owner.isVisible()) c->onFrame (dt);
    }

    // Medir el costo de dibujo (llamar al principio y al final de paint)
    void paintBegin() { paintStart = juce::Time::getMillisecondCounterHiRes(); }
    void paintEnd()
    {
        // un pico aislado (p. ej. el primer cuadro, que arma las caches) no cuenta entero
        const double ms = juce::jmin (juce::Time::getMillisecondCounterHiRes() - paintStart, paintMs * 2.0 + 4.0);
        paintMs += (ms - paintMs) * 0.15;
    }

    double averagePaintMs() const { return paintMs; }
    double refreshHz() const      { return vblankDt > 0 ? 1.0 / vblankDt : 0.0; }
    double framesPerSecond() const { return fps; }

    // Limite opcional (0 = sin limite, el del monitor)
    static inline int maxFps = 0;

    // Cada cuantos refrescos del monitor dibujar, segun lo que cuesta un cuadro (publico para las pruebas)
    static int chooseEvery (double costSec, double vblankDt, int fpsCap)
    {
        int every = 1;
        while (every < 8)
        {
            const double interval = every * vblankDt;
            const double budget = interval < 1.0 / 61.0 ? 0.45 : 0.6;
            const bool capOk = fpsCap <= 0 || interval >= 1.0 / fpsCap - vblankDt * 0.25;
            if (costSec <= budget * interval && capOk) break;
            ++every;
        }
        return every;
    }

private:
    static std::vector<FrameClock*>& registry() { static std::vector<FrameClock*> r; return r; }

    void vblank (double ts)
    {
        if (lastVblank > 0)
        {
            const double d = ts - lastVblank;
            if (d > 0.0 && d < 0.1) vblankDt += (d - vblankDt) * 0.1;
        }
        lastVblank = ts;
        if (! owner.isShowing()) { lastFrame = ts; return; }

        // Cada cuantos refrescos del monitor se dibuja (1 = todos: 120 FPS en ProMotion).
        // Mas rapido que 60 FPS solo si dibujar ocupa menos del 45 % del hilo de la interfaz (el mismo que
        // usa el DAW); para sostener 60 FPS se permite hasta el 60 %. Solo si ni eso alcanza baja de 60.
        const int every = chooseEvery (paintMs * 0.001, vblankDt, maxFps);
        const double minInterval = (every - 0.25) * vblankDt;   // tolerancia para el temblor del reloj
        if (ts - lastFrame < minInterval) return;

        const float dt = (float) juce::jlimit (0.0, 0.1, lastFrame > 0 ? ts - lastFrame : vblankDt);
        if (lastFrame > 0 && ts > lastFrame) fps += (1.0 / (ts - lastFrame) - fps) * 0.1;
        lastFrame = ts;
        onFrame (dt);
    }

    juce::Component& owner;
    std::function<void (float)> onFrame;
    double lastVblank = 0, lastFrame = 0, vblankDt = 1.0 / 60.0, fps = 0;
    double paintStart = 0, paintMs = 0;
    juce::VBlankAttachment attachment;   // ultimo: se destruye primero
};

// Imagen en cache a la resolucion fisica de la pantalla (Retina = 2x). Se regenera solo si cambia
// el tamano, la escala o la "clave" que el componente le pasa.
struct LayerCache
{
    juce::Image image;
    juce::Rectangle<int> bounds;
    float scale = 0.0f;
    juce::int64 key = -1;

    bool opaque = false;   // capa opaca: se copia sin mezclar (mucho mas rapido)

    template <typename Fn>
    void draw (juce::Graphics& g, juce::Rectangle<int> area, juce::int64 newKey, Fn&& render, float opacity = 1.0f)
    {
        const float sc = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        if (image.isNull() || area != bounds || std::abs (sc - scale) > 0.01f || newKey != key)
        {
            bounds = area; scale = sc; key = newKey;
            image = juce::Image (opaque ? juce::Image::RGB : juce::Image::ARGB, juce::jmax (1, juce::roundToInt (area.getWidth() * sc)),
                                 juce::jmax (1, juce::roundToInt (area.getHeight() * sc)), true);
            juce::Graphics gi (image);
            gi.addTransform (juce::AffineTransform::scale (sc).translated (-area.getX() * sc, -area.getY() * sc));
            render (gi);
        }
        g.setOpacity (opacity);
        g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale).translated ((float) area.getX(), (float) area.getY()));
        g.setOpacity (1.0f);
    }

    void clear() { image = {}; }
};
} // namespace gaux
