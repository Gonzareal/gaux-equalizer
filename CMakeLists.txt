cmake_minimum_required(VERSION 3.22)

project(GauxEqualizer VERSION 0.21.0)

# Windows: runtime de C++ estatico en TODOS los objetivos (JUCE, el plugin y sus envoltorios VST3/Standalone),
# asi el plugin no depende de que el PC tenga instalado el runtime de Visual Studio y todo enlaza igual
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# En Mac: compilar para Apple Silicon (M1-M5). Cambia a "arm64;x86_64" para un binario universal.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Version minima de macOS")

# JUCE: si existe la carpeta JUCE junto al proyecto se usa; si no, se descarga.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
elseif(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# En Mac el plugin se copia solo a ~/Library/Audio/Plug-Ins. En Windows la carpeta VST3 del sistema
# necesita permisos de administrador: lo copia build_windows.bat.
# (los scripts de empaquetado lo apagan con -DGAUX_COPY_AFTER_BUILD=FALSE)
if(NOT DEFINED GAUX_COPY_AFTER_BUILD)
    if(APPLE)
        set(GAUX_COPY_AFTER_BUILD TRUE)
    else()
        set(GAUX_COPY_AFTER_BUILD FALSE)
    endif()
endif()

set(GAUX_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND GAUX_FORMATS AU)
endif()

juce_add_plugin(GauxEqualizer
    COMPANY_NAME "Gaux"
    BUNDLE_ID "com.gaux.equalizer"
    PLUGIN_MANUFACTURER_CODE Gaux
    PLUGIN_CODE Gxeq
    FORMATS ${GAUX_FORMATS}
    PRODUCT_NAME "Gaux Equalizer"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${GAUX_COPY_AFTER_BUILD}
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_compile_features(GauxEqualizer PRIVATE cxx_std_17)

# Windows (Visual Studio): runtime estatico (el plugin no depende de que el PC tenga instalado el
# "Visual C++ Redistributable"), compilacion en paralelo y sin avisos de min/max.
if(MSVC)
    set_property(TARGET GauxEqualizer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(GauxEqualizer PRIVATE /MP /bigobj)
    target_compile_definitions(GauxEqualizer PRIVATE NOMINMAX _USE_MATH_DEFINES)
endif()

# Tipografias incrustadas en el plugin (licencia OFL, ver Resources/Fonts)
juce_add_binary_data(GauxFonts SOURCES
    Resources/Fonts/SpaceGrotesk-Regular.ttf
    Resources/Fonts/SpaceGrotesk-SemiBold.ttf
    Resources/Fonts/Oswald-Medium.ttf
    Resources/Fonts/Oswald-Bold.ttf
    Resources/Fonts/JetBrainsMono-Medium.ttf
    Resources/Fonts/LogoFont.ttf)

target_sources(GauxEqualizer PRIVATE
    Source/ThirdParty/pffft/pffft.c
    Source/ThirdParty/pffft/pffft_common.c
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/UI/SpectrumDisplay.cpp)

target_include_directories(GauxEqualizer PRIVATE Source)

target_compile_definitions(GauxEqualizer PUBLIC
    PFFFT_STATIC_DEFINE=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(GauxEqualizer
    PRIVATE
        GauxFonts
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Pruebas del motor de sonido (no se compilan con el plugin; las usa la compilacion automatica)
add_executable(GauxDspTests EXCLUDE_FROM_ALL Tests/DspTests.cpp)
target_include_directories(GauxDspTests PRIVATE Source)
target_compile_features(GauxDspTests PRIVATE cxx_std_17)
if(MSVC)
    target_compile_definitions(GauxDspTests PRIVATE _USE_MATH_DEFINES NOMINMAX)
    target_compile_options(GauxDspTests PRIVATE /bigobj)
endif()
