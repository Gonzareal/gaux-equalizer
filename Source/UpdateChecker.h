// Gaux Equalizer - Aviso de version nueva (beta -> version final y siguientes)
//
// Como funciona (y lo que NO hace):
//  - Al abrir la ventana del plugin, como mucho una vez cada 24 h, lee un archivo publico pequeno
//    (update/latest.json del repositorio en GitHub). Es una lectura normal: no se envia ningun dato del
//    usuario, ni identificadores, ni que DAW usa.
//  - Si ese archivo anuncia una version mas nueva que la instalada, aparece un aviso discreto arriba con
//    "Descargar" (abre la pagina o el instalador en el navegador) y "Mas tarde". "Omitir esta version"
//    no vuelve a avisar de esa misma version.
//  - Nunca descarga ni instala nada por su cuenta (en Mac y Windows instalar un plugin pide la contrasena
//    del equipo; hacerlo en segundo plano dentro de un DAW seria inseguro). El instalador nuevo se instala
//    encima y conserva los presets.
//  - Sin internet, o si el archivo no existe o esta danado: no pasa nada, no hay aviso ni demora.
//  - Se lee en un hilo aparte, nunca en el hilo de audio.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>

namespace gaux
{
struct UpdateInfo
{
    juce::String version, channel, title, notes, url;
    bool valid() const { return version.isNotEmpty() && url.startsWithIgnoreCase ("https://"); }
};

struct UpdateLogic
{
    // "1.0.0" > "0.26.0", "1.0" == "1.0.0", "0.26.0-beta" < "0.26.0"
    static int compare (const juce::String& a, const juce::String& b)
    {
        auto split = [] (const juce::String& v, bool& pre)
        {
            const auto core = v.trim().upToFirstOccurrenceOf ("-", false, false).upToFirstOccurrenceOf (" ", false, false);
            pre = v.containsChar ('-') || v.containsIgnoreCase ("beta") || v.containsIgnoreCase ("rc");
            juce::StringArray parts = juce::StringArray::fromTokens (core, ".", "");
            std::array<int, 4> n { 0, 0, 0, 0 };
            for (int i = 0; i < juce::jmin (4, parts.size()); ++i) n[(size_t) i] = juce::jmax (0, parts[i].getIntValue());
            return n;
        };
        bool pa = false, pb = false;
        const auto na = split (a, pa), nb = split (b, pb);
        for (size_t i = 0; i < 4; ++i) if (na[i] != nb[i]) return na[i] < nb[i] ? -1 : 1;
        if (pa != pb) return pa ? -1 : 1;   // la beta de una version es anterior a la version final
        return 0;
    }

    // Lee el JSON publicado. Campos: version, channel ("beta" | "final"), title, notes, url_mac, url_win, url
    static UpdateInfo parse (const juce::String& json)
    {
        UpdateInfo u;
        const auto v = juce::JSON::parse (json);
        if (! v.isObject()) return u;
        auto str = [&] (const char* k) { return v.getProperty (k, {}).toString().trim().substring (0, 400); };
        u.version = str ("version"); u.channel = str ("channel"); u.title = str ("title"); u.notes = str ("notes");
       #if JUCE_MAC
        u.url = str ("url_mac");
       #elif JUCE_WINDOWS
        u.url = str ("url_win");
       #endif
        if (u.url.isEmpty()) u.url = str ("url");
        if (! u.version.containsOnly ("0123456789.-betaBETArc ")) u.version = {};
        return u;
    }

    // Hay que avisar? (version mas nueva, no omitida por el usuario)
    static bool shouldNotify (const UpdateInfo& u, const juce::String& installed, const juce::String& skipped)
    {
        return u.valid() && compare (u.version, installed) > 0 && u.version != skipped;
    }
};

class UpdateChecker : private juce::Thread
{
public:
    static constexpr const char* kDefaultUrl = "https://raw.githubusercontent.com/Gonzareal/gaux-equalizer/main/update/latest.json";

    explicit UpdateChecker (juce::String installedVersion)
        : juce::Thread ("Gaux update check"), installed (std::move (installedVersion)) {}
    ~UpdateChecker() override { stopThread (6000); }

    // callback en el hilo de mensajes cuando hay una version nueva
    std::function<void (const UpdateInfo&)> onUpdateAvailable;

    void start()
    {
        if (! isThreadRunning() && ! started) { started = true; startThread (juce::Thread::Priority::low); }
    }

    void skip (const juce::String& version)
    {
        if (auto s = openSettings()) { s->setValue ("skipVersion", version); s->saveIfNeeded(); }
    }

    // Ajustes del aviso (un archivo pequeno en la carpeta de datos del usuario). Se abre y se cierra en cada uso:
    // nada queda vivo cuando el DAW descarga el plugin.
    static std::unique_ptr<juce::PropertiesFile> openSettings()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Gaux EQ"; o.filenameSuffix = ".settings"; o.folderName = "Gaux EQ";
        o.osxLibrarySubFolder = "Application Support"; o.storageFormat = juce::PropertiesFile::storeAsXML;
        o.millisecondsBeforeSaving = -1;   // se guarda solo con saveIfNeeded (sin temporizadores)
        return std::make_unique<juce::PropertiesFile> (o);
    }

    // Lee el anuncio (o usa el de hoy si ya se leyo) y decide si hay que avisar. Publico para las pruebas.
    static bool fetchAndDecide (const juce::String& url, const juce::String& installed, juce::PropertiesFile& s,
                                UpdateInfo& out, std::function<bool()> shouldExit = {})
    {
        if (s.getBoolValue ("updatesOff", false)) return false;
        const auto now = juce::Time::currentTimeMillis();
        juce::String json;
        if (url == kDefaultUrl && now - s.getValue ("lastCheck", "0").getLargeIntValue() < 24LL * 3600 * 1000)
            json = s.getValue ("lastJson");   // ya se leyo hoy: se usa lo guardado, sin conexion
        else
        {
            if (url.startsWith ("file://"))
                json = juce::URL (url).getLocalFile().loadFileAsString().substring (0, 8192);
            else if (url.startsWith ("https://"))
                if (auto in = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                                    .withConnectionTimeoutMs (4000).withNumRedirectsToFollow (3)))
                    json = in->readEntireStreamAsString().substring (0, 8192);
            if (shouldExit && shouldExit()) return false;
            if (json.isNotEmpty()) { s.setValue ("lastJson", json); s.setValue ("lastCheck", juce::String (now)); s.saveIfNeeded(); }
        }
        out = UpdateLogic::parse (json);
        return UpdateLogic::shouldNotify (out, installed, s.getValue ("skipVersion"));
    }

private:
    void run() override
    {
        auto s = openSettings();
        if (s == nullptr) return;
        UpdateInfo info;
        const auto url = juce::SystemStats::getEnvironmentVariable ("GAUX_UPDATE_URL", kDefaultUrl);
        if (fetchAndDecide (url, installed, *s, info, [this] { return threadShouldExit(); }))
            juce::MessageManager::callAsync ([w = juce::WeakReference<UpdateChecker> (this), info]
            {
                if (auto* c = w.get(); c != nullptr && c->onUpdateAvailable) c->onUpdateAvailable (info);
            });
    }

    juce::String installed;
    bool started = false;
    JUCE_DECLARE_WEAK_REFERENCEABLE (UpdateChecker)
};
} // namespace gaux
