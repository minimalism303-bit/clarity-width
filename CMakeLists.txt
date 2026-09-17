cmake_minimum_required (VERSION 3.22)

project (ClarityWidth VERSION 1.0.0 LANGUAGES C CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_POSITION_INDEPENDENT_CODE ON)

# JUCE is expected either as a sibling checkout (../JUCE) or fetched via
# add_subdirectory if the user has already cloned it into ./JUCE. See
# README.md for exact clone instructions.
if (NOT DEFINED JUCE_PATH)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
        set (JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
        set (JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
    else()
        message (FATAL_ERROR "JUCE not found. Clone it (see README.md) into ./JUCE or pass -DJUCE_PATH=/path/to/JUCE")
    endif()
endif()

add_subdirectory (${JUCE_PATH} JUCE)

# AU is only buildable/valid on macOS; everywhere else we ship VST3 + a
# Standalone app for quick auditioning without a DAW.
if (APPLE)
    set (CLARITYWIDTH_FORMATS VST3 AU Standalone)
else()
    set (CLARITYWIDTH_FORMATS VST3 Standalone)
endif()

juce_add_plugin (ClarityWidth
    COMPANY_NAME "ExampleAudio"
    PLUGIN_MANUFACTURER_CODE Exau
    PLUGIN_CODE Clwd
    FORMATS ${CLARITYWIDTH_FORMATS}
    PRODUCT_NAME "ClarityWidth"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES "Fx" "EQ"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources (ClarityWidth PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

# JUCE's internal VST3 manifest-helper tool (created by juce_add_plugin,
# above) is marked WIN32_EXECUTABLE (expects a WinMain entry point / GUI
# subsystem). Under MinGW that conflicts with its actual console `main()`
# entry point, so force it back to a console-subsystem executable when
# cross-compiling with MinGW.
if (MINGW AND TARGET juce_vst3_helper)
    set_target_properties (juce_vst3_helper PROPERTIES WIN32_EXECUTABLE FALSE)
    # Steinberg's moduleinfotool defines a wide-character wmain() on
    # Windows, not main() - the console-unicode CRT entry point
    # (wmainCRTStartup) needs -municode alongside -mconsole to match it.
    target_link_options (juce_vst3_helper PRIVATE -mconsole -municode)
endif()

target_include_directories (ClarityWidth PRIVATE Source)

target_compile_definitions (ClarityWidth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries (ClarityWidth PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# --- Headless DSP test harness (Tests/main.cpp) ---------------------------
add_executable (ClarityWidthTests Tests/main.cpp)
target_link_libraries (ClarityWidthTests PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_dsp)
target_compile_definitions (ClarityWidthTests PRIVATE
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)
