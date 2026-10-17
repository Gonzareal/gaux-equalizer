// Gaux Equalizer - La calavera GAUX: reacciona a la senal
//   dormida -> tranquila -> calentando -> saturada -> CLIP
//   Cabecea con los golpes, mueve la mandibula al ritmo, se le levanta la tapa al saturar
//   (con CLIP le sale volando y se ve el cerebro en llamas). Solo expresiva, sin texto.
#pragma once

#include "GauxLookAndFeel.h"
#include "Logo.h"
#include "FrameClock.h"
#include "../PluginProcessor.h"

namespace gaux
{
class Mascot : public juce::Component
{
public:
    enum class Mood { Sleep, Chill, Warm, Angry, Clip };

    explicit Mascot (GauxEqProcessor& p) : proc (p) { setInterceptsMouseClicks (false, false); }


    bool vintageStyle = false;

    // Para pruebas: forzar un estado
    void forceMood (Mood m, float lvl)
    {
        forced = true; mood = m; level = lvl;
        lid = m == Mood::Clip ? 1.0f : m == Mood::Angry ? 0.35f : 0.0f;
        kick = m == Mood::Chill ? 0.6f : 0.0f;
        peakHoldDb = m == Mood::Clip ? 2.4f : m == Mood::Warm ? -1.8f : -7.5f;
        hotBand = 1; hotDrive = 20.0f;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float S = juce::jmin (r.getWidth(), r.getHeight());
        auto face = r.withSizeKeepingCentre (S, S);
        const double t = juce::Time::getMillisecondCounterHiRes() * 0.001;

        juce::Point<float> shake;
        if (mood == Mood::Clip)
            shake = { (float) std::sin (t * 63.0) * S * 0.035f, (float) std::cos (t * 71.0) * S * 0.025f };
        else if (mood == Mood::Angry)
            shake = { (float) std::sin (t * 41.0) * S * 0.012f, 0.0f };

        {
            juce::Graphics::ScopedSaveState ss (g);
            const auto neck = face.getCentre().translated (0, S * 0.45f);
            g.addTransform (juce::AffineTransform::rotation (kick * 0.13f, neck.x, neck.y).translated (shake.x, shake.y + kick * S * 0.05f));
            drawSkull (g, face, t);
        }
    }

private:
    // Un cuadro del monitor: la logica avanza con el tiempo real y solo se redibuja si algo se mueve
    void frame (float dt)
    {
        if (forced) return;
        const float before = level + kick * 3.0f + lid * 2.0f + (float) mood * 10.0f;
        tick (dt);
        const float after = level + kick * 3.0f + lid * 2.0f + (float) mood * 10.0f;
        if (mood == Mood::Sleep)
        {
            // dormida: solo la "z" flotando, basta con ~15 cuadros por segundo
            sleepAcc += dt;
            if (sleepAcc >= 1.0f / 15.0f || std::abs (after - before) > 1.0e-3f) { sleepAcc = 0; repaint(); }
        }
        else repaint();   // despierta siempre hay algo vivo (mandibula, humo, temblor)
    }

    // Coeficiente "por paso a 30 Hz" convertido a cualquier dt
    static float rate (float perTick30, float dt) { return 1.0f - std::pow (1.0f - perTick30, dt * 30.0f); }

public:
    // Un paso de la animacion (por defecto 1/30 s). Publico para las pruebas.
    void tick (float dt = 1.0f / 30.0f)
    {
        if (forced) { repaint(); return; }
        const float rms = proc.outputRms.load();
        const float lvlDb = juce::Decibels::gainToDecibels (rms, -100.0f);
        const float target = juce::jlimit (0.0f, 1.0f, (lvlDb + 48.0f) / 42.0f);
        level += (target - level) * rate (target > level ? 0.6f : 0.15f, dt);

        // Golpes reales detectados en el audio (kick / bajo): cabeceo y mandibula
        const int bc = proc.beatCount.load();
        if (bc != lastBeat)
        {
            lastBeat = bc;
            kick = juce::jmax (kick, 0.55f + 0.45f * proc.beatStrength.load());
            ++beatsSeen;
        }
        kick *= std::pow (0.80f, dt * 30.0f);

        const float pDb = juce::Decibels::gainToDecibels (proc.outputPeak.load(), -100.0f);
        peakHoldDb = juce::jmax (pDb, peakHoldDb - 12.0f * dt);

        // Calor = saturacion que de verdad esta sonando (no la posicion de la perilla)
        // (promediada ~0.6 s para que no siga cada golpe del kick)
        const float sDb = lvlDb > -60.0f ? proc.satDb.load() : -100.0f;
        satSmooth += (std::pow (10.0f, sDb / 10.0f) - satSmooth) * rate (0.05f, dt);          // promedio en energia
        const float satAvgDb = 10.0f * std::log10 (juce::jmax (1.0e-10f, satSmooth));
        // Mapa de "calor": -36 dB o menos = limpio, ~-22 dB = calido (color de valvula/transformador),
        // ~-8 dB = calido fuerte (TUBE con drive), 0 dB o mas = saturacion fuerte (enojada)
        static constexpr float px[] = { -36.0f, -22.0f, -8.0f, -2.0f, 2.0f, 8.0f };
        static constexpr float py[] = {   0.0f,  0.33f, 0.45f, 0.62f, 0.75f, 1.0f };
        float heat = satAvgDb <= px[0] ? 0.0f : 1.0f;
        for (int k = 0; k < 5; ++k)
            if (satAvgDb > px[k] && satAvgDb <= px[k + 1])
                heat = py[k] + (py[k + 1] - py[k]) * (satAvgDb - px[k]) / (px[k + 1] - px[k]);
        const float clip = proc.clipLevel.load();
        const float st = juce::jmax (heat * 0.9f, clip);
        stress += (st - stress) * rate (st > stress ? 0.15f : 0.05f, dt);

        // Animo con histeresis y tiempo minimo: nada de parpadeos entre estados
        if (lvlDb < -70.0f) quietTime += dt; else quietTime = 0;
        Mood want = mood;
        if (proc.params.bypass->load() > 0.5f || quietTime > 2.0f + 1.0e-3f) want = Mood::Sleep;
        else
        {
            const int cur = mood == Mood::Sleep ? 1 : (int) mood;
            int lvl = cur;
            const float up[] = { 0, 0, 0.26f, 0.56f, 0.88f }, down[] = { 0, 0, 0.18f, 0.44f, 0.74f };
            while (lvl < 4 && (stress > up[lvl + 1] || (lvl + 1 == 4 && clip > 0.9f))) ++lvl;
            while (lvl > 1 && stress < down[lvl] && ! (lvl == 4 && clip > 0.5f)) --lvl;
            want = (Mood) lvl;
        }
        moodTime += dt;
        const bool goingUp = (int) want > (int) mood;
        if (want != mood && (goingUp || mood == Mood::Sleep || want == Mood::Sleep || moodTime > 0.4f + 1.0e-4f))
        {
            mood = want;
            moodTime = 0;
        }

        const float lidTarget = mood == Mood::Clip ? 1.0f : mood == Mood::Angry ? 0.35f : 0.0f;
        lid += (lidTarget - lid) * rate (lidTarget > lid ? 0.35f : 0.12f, dt);
    }

    Mood getMood() const { return mood; }
    int getBeatsSeen() const { return beatsSeen; }

private:
    // Colores del brillo de los ojos segun el animo
    juce::Colour eyeColour() const
    {
        switch (mood)
        {
            case Mood::Sleep: return juce::Colour (0xff4a3a3a);
            case Mood::Chill: return juce::Colour (0xffb3122c);
            case Mood::Warm:  return juce::Colour (0xffff6a1a);
            case Mood::Angry: return juce::Colour (0xffff2442);
            case Mood::Clip:  return juce::Colour (0xffffe6d0);
        }
        return Colours::accent;
    }

    // Poligono irregular "a mano" a partir de puntos base
    static juce::Path roughShape (const std::vector<juce::Point<float>>& pts, float jitter, int seed)
    {
        juce::Path p;
        for (int i = 0; i < (int) pts.size(); ++i)
        {
            const auto j = juce::Point<float> ((Logo::hashNoise (i, seed) - 0.5f) * jitter, (Logo::hashNoise (i + 31, seed) - 0.5f) * jitter);
            if (i == 0) p.startNewSubPath (pts[(size_t) i] + j); else p.lineTo (pts[(size_t) i] + j);
        }
        p.closeSubPath();
        return p.createPathWithRoundedCorners (2.5f);
    }

    void drawSkull (juce::Graphics& g, juce::Rectangle<float> f, double t)
    {
        const float S = f.getHeight();
        const auto c = f.getCentre().translated (0, S * 0.04f);
        const float ink = juce::jmax (1.6f, S * 0.035f);
        const auto inkCol = juce::Colour (0xff0b0607);

        float jaw = 0.0f;
        switch (mood)
        {
            case Mood::Sleep: jaw = 0.0f; break;
            case Mood::Chill: jaw = 0.12f * level + 0.45f * kick; break;
            case Mood::Warm:  jaw = 0.12f + 0.15f * level + 0.45f * kick; break;
            case Mood::Angry: jaw = 0.45f + 0.15f * (float) std::abs (std::sin (t * 18.0)); break;
            case Mood::Clip:  jaw = 0.9f; break;
        }

        // Humo cuando esta saturada o clipeando
        if (mood == Mood::Angry || mood == Mood::Clip)
            for (int i = 0; i < 4; ++i)
            {
                const float ph = (float) std::fmod (t * 0.8 + i * 0.25, 1.0);
                const float side = (i % 2 == 0 ? -1.0f : 1.0f) * (0.08f + 0.05f * i);
                const auto pp = juce::Point<float> (c.x + side * S + std::sin ((float) t * 3.0f + (float) i) * S * 0.04f, c.y - S * (0.42f + ph * 0.45f));
                g.setColour ((mood == Mood::Clip ? juce::Colour (0xffff6a3a) : juce::Colours::white).withAlpha (0.28f * (1.0f - ph)));
                g.fillEllipse (juce::Rectangle<float> (S * (0.08f + ph * 0.14f), S * (0.08f + ph * 0.14f)).withCentre (pp));
            }

        // Diadema de los audifonos (detras del craneo)
        {
            juce::Path band;
            band.addCentredArc (c.x, c.y - S * 0.06f, S * 0.43f, S * 0.42f, 0, -1.25f, 1.25f, true);
            g.setColour (inkCol);
            g.strokePath (band, juce::PathStrokeType (S * 0.09f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (juce::Colour (0xff3a3436));
            g.strokePath (band, juce::PathStrokeType (S * 0.045f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Mandibula inferior (se abre)
        const float jy = c.y + S * 0.20f + jaw * S * 0.12f;
        std::vector<juce::Point<float>> jawPts {
            { c.x - S * 0.17f, jy - S * 0.02f }, { c.x + S * 0.17f, jy - S * 0.02f }, { c.x + S * 0.15f, jy + S * 0.09f },
            { c.x + S * 0.06f, jy + S * 0.15f }, { c.x - S * 0.06f, jy + S * 0.15f }, { c.x - S * 0.15f, jy + S * 0.09f } };
        auto jawPath = roughShape (jawPts, S * 0.02f, 3);
        g.setColour (inkCol);
        g.strokePath (jawPath, juce::PathStrokeType (ink * 2.0f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe9dfca), c.x, jy, juce::Colour (0xffa69679), c.x, jy + S * 0.15f, false));
        g.fillPath (jawPath);
        g.setColour (inkCol);
        for (int i = -2; i <= 2; ++i)
            g.drawLine (c.x + i * S * 0.055f, jy - S * 0.02f, c.x + i * S * 0.055f + S * 0.006f, jy + S * 0.045f, ink * 0.6f);

        // Boca abierta (oscuridad entre mandibulas)
        if (jaw > 0.05f)
        {
            g.setColour (juce::Colour (0xff120607));
            g.fillRect (juce::Rectangle<float> (c.x - S * 0.15f, c.y + S * 0.19f, S * 0.30f, jy - c.y - S * 0.19f));
            if (mood == Mood::Clip)
            {
                g.setColour (Colours::accent.withAlpha (0.6f));
                g.fillRect (juce::Rectangle<float> (c.x - S * 0.10f, c.y + S * 0.21f, S * 0.20f, jy - c.y - S * 0.23f));
            }
        }

        // Craneo (con la tapa separable por una grieta en zigzag)
        std::vector<juce::Point<float>> skull {
            { c.x - S * 0.06f, c.y - S * 0.40f }, { c.x + S * 0.10f, c.y - S * 0.40f }, { c.x + S * 0.27f, c.y - S * 0.31f },
            { c.x + S * 0.34f, c.y - S * 0.14f }, { c.x + S * 0.33f, c.y + S * 0.02f }, { c.x + S * 0.25f, c.y + S * 0.10f },
            { c.x + S * 0.20f, c.y + S * 0.22f }, { c.x - S * 0.20f, c.y + S * 0.22f }, { c.x - S * 0.25f, c.y + S * 0.10f },
            { c.x - S * 0.34f, c.y + S * 0.02f }, { c.x - S * 0.35f, c.y - S * 0.15f }, { c.x - S * 0.26f, c.y - S * 0.32f } };
        auto skullPath = roughShape (skull, S * 0.03f, 11);

        const float lidY = c.y - S * 0.19f;
        juce::Path zig;
        {
            const int teeth = 7;
            zig.startNewSubPath (c.x - S * 0.6f, lidY);
            for (int i = 0; i <= teeth; ++i)
                zig.lineTo (c.x - S * 0.36f + S * 0.72f * i / teeth, lidY + (i % 2 == 0 ? -1.0f : 1.0f) * S * 0.025f);
            zig.lineTo (c.x + S * 0.6f, lidY);
        }
        juce::Path below (zig), above (zig);
        below.lineTo (c.x + S * 0.6f, c.y + S * 0.6f); below.lineTo (c.x - S * 0.6f, c.y + S * 0.6f); below.closeSubPath();
        above.lineTo (c.x + S * 0.6f, c.y - S * 0.8f); above.lineTo (c.x - S * 0.6f, c.y - S * 0.8f); above.closeSubPath();

        const int cracks = mood == Mood::Clip ? 3 : mood == Mood::Angry ? 2 : mood == Mood::Warm ? 1 : 0;
        auto drawCranium = [&]
        {
            g.setColour (inkCol);
            g.strokePath (skullPath, juce::PathStrokeType (ink * 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff3ebd8), c.x - S * 0.1f, c.y - S * 0.35f, juce::Colour (0xffa99a7d), c.x + S * 0.2f, c.y + S * 0.2f, true));
            g.fillPath (skullPath);
            {
                juce::Graphics::ScopedSaveState ss2 (g);
                g.reduceClipRegion (skullPath);
                juce::Random rnd (19);
                for (int i = 0; i < 26; ++i)
                {
                    const float rr = S * (0.01f + rnd.nextFloat() * 0.04f);
                    g.setColour (juce::Colour (0xff5a4a36).withAlpha (0.12f + rnd.nextFloat() * 0.18f));
                    g.fillEllipse (c.x - S * 0.35f + rnd.nextFloat() * S * 0.7f, c.y - S * 0.4f + rnd.nextFloat() * S * 0.62f, rr, rr * 0.7f);
                }
                g.setColour (juce::Colours::black.withAlpha (0.18f));
                g.fillEllipse (juce::Rectangle<float> (S * 0.3f, S * 0.7f).withCentre ({ c.x + S * 0.34f, c.y - S * 0.05f }));
            }
            g.setColour (inkCol);
            if (cracks >= 2)
            {
                juce::Path k; k.startNewSubPath (c.x - S * 0.30f, c.y - S * 0.12f);
                k.lineTo (c.x - S * 0.24f, c.y - S * 0.10f); k.lineTo (c.x - S * 0.22f, c.y - S * 0.04f);
                g.strokePath (k, juce::PathStrokeType (ink * 0.7f));
            }
            if (cracks >= 3)
            {
                juce::Path k; k.startNewSubPath (c.x + S * 0.32f, c.y - S * 0.10f);
                k.lineTo (c.x + S * 0.26f, c.y - S * 0.08f); k.lineTo (c.x + S * 0.27f, c.y - S * 0.02f);
                g.strokePath (k, juce::PathStrokeType (ink * 0.7f));
            }
        };

        // Parte de abajo del craneo
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (below);
            drawCranium();
        }

        // Cerebro al rojo vivo (se ve cuando la tapa se levanta)
        if (lid > 0.15f)
        {
            const auto brainC = juce::Point<float> (c.x, lidY + S * 0.01f);
            auto brain = juce::Rectangle<float> (S * 0.56f, S * 0.16f).withCentre (brainC);
            const auto hot = mood == Mood::Clip ? juce::Colour (0xffffd060) : Colours::accent;
            g.setGradientFill (juce::ColourGradient (hot, brainC.x, brainC.y, juce::Colour (0xff6a0610), brain.getRight(), brain.getBottom(), true));
            g.fillEllipse (brain);
            g.setColour (inkCol);
            for (int i = 0; i < 3; ++i)
                g.drawLine (brain.getX() + brain.getWidth() * (0.3f + 0.2f * i), brain.getY() + 2, brain.getX() + brain.getWidth() * (0.25f + 0.2f * i), brain.getBottom() - 2, ink * 0.5f);
            if (mood == Mood::Clip)
                for (int i = 0; i < 5; ++i)
                {
                    // Llamas que salen del craneo
                    const float fx = brain.getX() + brain.getWidth() * (0.1f + 0.2f * i);
                    const float fh = S * (0.18f + 0.12f * (float) std::abs (std::sin (t * 9.0 + i * 1.7)));
                    juce::Path flame;
                    flame.startNewSubPath (fx - S * 0.05f, brainC.y);
                    flame.quadraticTo (fx - S * 0.04f, brainC.y - fh * 0.6f, fx + (float) std::sin (t * 7.0 + i) * S * 0.03f, brainC.y - fh);
                    flame.quadraticTo (fx + S * 0.05f, brainC.y - fh * 0.5f, fx + S * 0.05f, brainC.y);
                    flame.closeSubPath();
                    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffffe08a), fx, brainC.y, juce::Colour (0x00ff3010), fx, brainC.y - fh, false));
                    g.fillPath (flame);
                }
        }

        // La tapa: se levanta con bisagra a la izquierda; con CLIP sale volando
        {
            juce::Graphics::ScopedSaveState ss (g);
            const auto hinge = juce::Point<float> (c.x - S * 0.34f, lidY);
            const float wob = mood == Mood::Clip ? (float) std::sin (t * 13.0) * 0.08f : 0.0f;
            const auto lidT = juce::AffineTransform::rotation (-lid * 0.75f + wob, hinge.x, hinge.y)
                                  .translated (lid * S * 0.04f, -lid * S * 0.22f);
            g.addTransform (lidT);
            g.reduceClipRegion (above);
            drawCranium();
            g.setColour (inkCol);
            if (cracks >= 1)
            {
                juce::Path k; k.startNewSubPath (c.x + S * 0.04f, c.y - S * 0.40f);
                k.lineTo (c.x + S * 0.02f, c.y - S * 0.31f); k.lineTo (c.x + S * 0.07f, c.y - S * 0.26f);
                g.strokePath (k, juce::PathStrokeType (ink * 0.7f));
            }
            if (lid > 0.02f)
            {
                // Borde de la grieta
                g.strokePath (zig, juce::PathStrokeType (ink * 0.9f));
            }
        }

        // Cuencas de los ojos con X cruda brillante
        const auto eyeCol = eyeColour();
        const float flicker = mood == Mood::Clip ? (std::fmod (t, 0.16) < 0.08 ? 1.0f : 0.6f) : 1.0f;
        for (int side = -1; side <= 1; side += 2)
        {
            const auto e = juce::Point<float> (c.x + side * S * 0.145f, c.y - S * 0.06f);
            std::vector<juce::Point<float>> sock {
                { e.x - S * 0.10f, e.y - S * 0.05f }, { e.x - S * 0.02f, e.y - S * 0.09f }, { e.x + S * 0.09f, e.y - S * 0.06f },
                { e.x + S * 0.10f, e.y + S * 0.04f }, { e.x + S * 0.02f, e.y + S * 0.09f }, { e.x - S * 0.08f, e.y + S * 0.06f } };
            g.setColour (juce::Colour (0xff100809));
            g.fillPath (roughShape (sock, S * 0.025f, 40 + side));

            if (mood != Mood::Sleep)
            {
                g.setGradientFill (juce::ColourGradient (eyeCol.withAlpha (0.55f * flicker), e.x, e.y, eyeCol.withAlpha (0.0f), e.x + S * 0.12f, e.y, true));
                g.fillEllipse (juce::Rectangle<float> (S * 0.26f, S * 0.24f).withCentre (e));
            }
            const float xr = S * 0.06f;
            juce::Path xp;
            xp.addPath (Logo::roughStroke ({ e.x - xr, e.y - xr }, { e.x + xr, e.y + xr }, S * 0.045f, 60 + side));
            xp.addPath (Logo::roughStroke ({ e.x + xr, e.y - xr * 0.9f }, { e.x - xr * 0.9f, e.y + xr }, S * 0.04f, 70 + side));
            g.setColour (eyeCol.withMultipliedAlpha (flicker));
            g.fillPath (xp);
            if (mood == Mood::Clip)
            {
                g.setColour (Colours::accent.withAlpha (0.8f));
                g.strokePath (xp, juce::PathStrokeType (1.0f));
            }
        }

        // Nariz
        std::vector<juce::Point<float>> nose { { c.x, c.y + S * 0.03f }, { c.x - S * 0.045f, c.y + S * 0.12f }, { c.x + S * 0.045f, c.y + S * 0.12f } };
        g.setColour (juce::Colour (0xff100809));
        g.fillPath (roughShape (nose, S * 0.012f, 5));

        // Dientes superiores
        g.setColour (inkCol);
        for (int i = -2; i <= 2; ++i)
            g.drawLine (c.x + i * S * 0.055f, c.y + S * 0.165f, c.x + i * S * 0.055f, c.y + S * 0.22f, ink * 0.6f);
        g.drawLine (c.x - S * 0.15f, c.y + S * 0.165f, c.x + S * 0.15f, c.y + S * 0.165f, ink * 0.6f);

        // Audifonos (copas rojas a los lados)
        for (int side = -1; side <= 1; side += 2)
        {
            auto cup = juce::Rectangle<float> (S * 0.13f, S * 0.27f).withCentre ({ c.x + side * S * 0.39f, c.y - S * 0.06f });
            g.setColour (inkCol);
            g.fillRoundedRectangle (cup.expanded (ink), S * 0.05f);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffff3a4e), cup.getX(), cup.getY(), juce::Colour (0xff8a0c1c), cup.getRight(), cup.getBottom(), false));
            g.fillRoundedRectangle (cup, S * 0.045f);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.fillRoundedRectangle (cup.withWidth (cup.getWidth() * 0.3f).reduced (0, cup.getHeight() * 0.15f).translated (cup.getWidth() * 0.12f, 0), 2.0f);
        }

        if (mood == Mood::Sleep)
        {
            g.setColour (Colours::text.withAlpha (0.75f));
            g.setFont (monoFont (10.0f));
            g.drawText ("z", juce::Rectangle<float> (12, 12).withCentre ({ f.getRight() - 4, f.getY() + 8 - (float) std::fmod (t * 6.0, 6.0) }), juce::Justification::centred);
        }
        if (mood == Mood::Clip)
        {
            g.setColour (juce::Colours::white);
            g.setFont (displayFont (13.0f));
            g.drawText ("!!", juce::Rectangle<float> (20, 16).withCentre ({ f.getRight() - 2, f.getY() + 6 }), juce::Justification::centred);
        }
    }

    GauxEqProcessor& proc;
    Mood mood = Mood::Sleep;
    float level = 0.0f, stress = 0.0f, kick = 0.0f, prevTarget = 0.0f, lid = 0.0f, peakHoldDb = -100.0f, hotDrive = 0.0f;
    int hotBand = -1;
    float quietTime = 999.0f, moodTime = 0.0f, sleepAcc = 0.0f;
    bool forced = false;
    int lastBeat = 0, beatsSeen = 0;
    float satSmooth = 0.0f;
    FrameClock clock { *this, [this] (float dt) { frame (dt); } };
};
} // namespace gaux
