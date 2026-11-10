cmake_minimum_required(VERSION 3.22)

# macOS: universal binary (Apple Silicon + Intel), macOS 11 or newer.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
# Windows: link the runtime statically so no Visual C++ redistributable is needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(ReferenceAB VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(ReferenceAB
    PRODUCT_NAME "Reference AB"
    COMPANY_NAME "ReferenceAB"
    BUNDLE_ID "com.referenceab.plugin"
    PLUGIN_MANUFACTURER_CODE Rfab
    PLUGIN_CODE Rfa1
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Analyzer
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(ReferenceAB PRIVATE
    Source/LoudnessMeter.cpp
    Source/ReferenceTrack.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(ReferenceAB PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1)

target_link_libraries(ReferenceAB
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- tests
enable_testing()

juce_add_console_app(LoudnessTests PRODUCT_NAME "LoudnessTests")
target_sources(LoudnessTests PRIVATE
    Tests/LoudnessTests.cpp
    Source/LoudnessMeter.cpp)
target_compile_definitions(LoudnessTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(LoudnessTests
    PRIVATE
        juce::juce_dsp
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags)

add_test(NAME LoudnessTests COMMAND LoudnessTests)

# Runs the whole audio engine (file loading, A/B, level match, state) without a host.
juce_add_console_app(ProcessorTests PRODUCT_NAME "ProcessorTests")
target_sources(ProcessorTests PRIVATE
    Tests/ProcessorTests.cpp
    Source/LoudnessMeter.cpp
    Source/ReferenceTrack.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_compile_definitions(ProcessorTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JucePlugin_VersionString="${PROJECT_VERSION}")
target_link_libraries(ProcessorTests
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)

add_test(NAME ProcessorTests COMMAND ProcessorTests)
