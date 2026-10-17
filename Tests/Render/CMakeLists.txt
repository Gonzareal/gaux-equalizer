# Prueba interna: renderiza la interfaz a PNG y procesa audio sin DAW (solo Linux/CI)
cmake_minimum_required(VERSION 3.22)
project(GauxRenderTest VERSION 0.1.0)
add_subdirectory(${JUCE_DIR} JUCE)
juce_add_console_app(GauxRenderTest PRODUCT_NAME "GauxRenderTest")
target_sources(GauxRenderTest PRIVATE
    RenderTest.cpp
    ../../Source/ThirdParty/pffft/pffft.c
    ../../Source/ThirdParty/pffft/pffft_common.c
    ../../Source/PluginProcessor.cpp
    ../../Source/PluginEditor.cpp
    ../../Source/UI/SpectrumDisplay.cpp)
target_include_directories(GauxRenderTest PRIVATE ../../Source ${XSTUB_DIR})
target_compile_definitions(GauxRenderTest PRIVATE PFFFT_STATIC_DEFINE=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JucePlugin_Name="Gaux Equalizer" JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
juce_add_binary_data(GauxFonts SOURCES
    ../../Resources/Fonts/SpaceGrotesk-Regular.ttf
    ../../Resources/Fonts/SpaceGrotesk-SemiBold.ttf
    ../../Resources/Fonts/Oswald-Medium.ttf
    ../../Resources/Fonts/Oswald-Bold.ttf
    ../../Resources/Fonts/JetBrainsMono-Medium.ttf
    ../../Resources/Fonts/LogoFont.ttf)
target_link_libraries(GauxRenderTest PRIVATE GauxFonts juce::juce_gui_basics juce::juce_gui_extra
    juce::juce_audio_processors juce::juce_dsp juce::juce_recommended_config_flags)

# Estudio virtual de mezcla (pruebas por genero)
juce_add_console_app(GauxMixLab PRODUCT_NAME "GauxMixLab")
target_sources(GauxMixLab PRIVATE
    MixLab.cpp
    ../../Source/ThirdParty/pffft/pffft.c
    ../../Source/ThirdParty/pffft/pffft_common.c
    ../../Source/PluginProcessor.cpp
    ../../Source/PluginEditor.cpp
    ../../Source/UI/SpectrumDisplay.cpp)
target_include_directories(GauxMixLab PRIVATE ../../Source ${XSTUB_DIR})
target_compile_definitions(GauxMixLab PRIVATE PFFFT_STATIC_DEFINE=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JucePlugin_Name="Gaux Equalizer" JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(GauxMixLab PRIVATE GauxFonts juce::juce_gui_basics juce::juce_gui_extra
    juce::juce_audio_processors juce::juce_dsp juce::juce_recommended_config_flags)
