cmake_minimum_required(VERSION 3.22)

# ── Universal build (macOS) — v0.0.922 (2026-10-03) ─────────────────────────
# Apple Silicon + Intel in one binary, so Patchy also loads in hosts running
# under Rosetta (Logic "Open using Rosetta" only loads Intel plug-ins — the
# user needs that mode for Native Instruments). Must be set before project().
# Covers JUCE, Patchy, Patchy FX, the Pax and libmosquitto (built from source
# below). Turn off for faster local debug builds (roughly halves compile time):
#   -DPATCHY_UNIVERSAL=OFF
option(PATCHY_UNIVERSAL "macOS: build arm64 + x86_64 (universal)" ON)
if (CMAKE_HOST_APPLE AND PATCHY_UNIVERSAL)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(AudioSim VERSION 0.1.0)

# Some Windows CMake/MSVC combinations decode the localized /showIncludes
# prefix incorrectly during compiler detection. Use the UTF-8 probe from our
# build script so Ninja actually records headers and recompiles dependants.
if(MSVC AND DEFINED AUDIOSIM_MSVC_INCLUDE_PREFIX)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${AUDIOSIM_MSVC_INCLUDE_PREFIX}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# v0.0.929 — back up the git-ignored FYI/ notes at each configure (see the file)
include(${CMAKE_SOURCE_DIR}/cmake/FyiBackup.cmake)

# ── Options ───────────────────────────────────────────────────────────────────
option(NODEGRAPH_DEV_MODE "Skip UI build, use Vite dev server on localhost:5173" OFF)
option(PATCHY_BUILD_TESTS "Build Patchy unit tests" ON)

# ── Find npm ──────────────────────────────────────────────────────────────────
if(NOT NODEGRAPH_DEV_MODE)
    if(WIN32)
        find_program(NPM_EXE NAMES npm.cmd npm REQUIRED DOC "npm")
    else()
        find_program(NPM_EXE NAMES npm REQUIRED DOC "npm")
    endif()
    message(STATUS "npm: ${NPM_EXE}")
endif()

# ── Fetch JUCE 8 ──────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.12
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ── Build & zip React UI ──────────────────────────────────────────────────────
set(UI_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/UI")
set(UI_DIST  "${UI_DIR}/dist")
set(UI_ZIP   "${CMAKE_CURRENT_BINARY_DIR}/ui_assets.zip")
set(UI_STAMP "${CMAKE_CURRENT_BINARY_DIR}/ui.stamp")

if(NOT NODEGRAPH_DEV_MODE)
    file(GLOB_RECURSE UI_SOURCES
        "${UI_DIR}/src/*" "${UI_DIR}/index.html"
        "${UI_DIR}/package.json" "${UI_DIR}/vite.config.ts" "${UI_DIR}/tsconfig.json")

    set(_need_build FALSE)
    if(NOT EXISTS "${UI_STAMP}" OR NOT EXISTS "${UI_DIST}/index.html")
        set(_need_build TRUE)
    else()
        foreach(_f ${UI_SOURCES})
            if("${_f}" IS_NEWER_THAN "${UI_STAMP}")
                set(_need_build TRUE)
                break()
            endif()
        endforeach()
    endif()

    if(_need_build)
        message(STATUS "Patchy: npm install…")
        execute_process(COMMAND ${NPM_EXE} ci
            WORKING_DIRECTORY "${UI_DIR}" RESULT_VARIABLE r COMMAND_ECHO STDOUT)
        if(NOT r EQUAL 0)
            message(FATAL_ERROR "npm install failed")
        endif()
        message(STATUS "Patchy: npm run build…")
        execute_process(COMMAND ${NPM_EXE} run build
            WORKING_DIRECTORY "${UI_DIR}" RESULT_VARIABLE r COMMAND_ECHO STDOUT)
        if(NOT r EQUAL 0)
            message(FATAL_ERROR "npm run build failed")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar cf "${UI_ZIP}" --format=zip -- .
            WORKING_DIRECTORY "${UI_DIST}" RESULT_VARIABLE r)
        if(NOT r EQUAL 0)
            message(FATAL_ERROR "zip packaging failed")
        endif()
        file(WRITE "${UI_STAMP}" "")
        message(STATUS "Patchy: UI ready.")
    else()
        message(STATUS "Patchy: UI up-to-date.")
        if(NOT EXISTS "${UI_ZIP}")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E tar cf "${UI_ZIP}" --format=zip -- .
                WORKING_DIRECTORY "${UI_DIST}")
        endif()
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${UI_SOURCES})

    juce_add_binary_data(PatchyUI
        HEADER_NAME "BinaryData.h"
        NAMESPACE   BinaryData
        SOURCES     "${UI_ZIP}")
endif()

# ── Plugin target ─────────────────────────────────────────────────────────────
juce_add_plugin(Patchy
    VERSION                  "0.1.0"
    COMPANY_NAME             "AudioSim"
    PLUGIN_MANUFACTURER_CODE "ASim"
    PLUGIN_CODE              "ASim"
    FORMATS                  Standalone
    PRODUCT_NAME             "AudioSim"
    BUNDLE_ID                "com.audiosim.framework"
    IS_SYNTH                 TRUE          # produces audio from MIDI
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        TRUE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    NEEDS_WEBVIEW2            TRUE
    VST3_CATEGORIES          "Instrument" "Fx"
    # The AU of this target is the INSTRUMENT (aumu): instrument tracks in
    # Logic, fed by the track's MIDI. Its sibling PatchyFX (below) is the
    # MIDI-controlled EFFECT (aumf) — see there.
    AU_MAIN_TYPE             "kAudioUnitType_MusicDevice"
    COPY_PLUGIN_AFTER_BUILD  TRUE
    # Fix, 2026-09-11: standalone build was silently receiving all-zero
    # input data from any real audio input device — root cause was this
    # declaration's own total absence. Without it, macOS never even
    # prompts for microphone/audio-input access at all for the built
    # .app (unlike running from CLion's own debugger, which sidesteps
    # this check) — it just silently denies. See Architecture.md's own
    # roadmap entry for the full story, including a note on macOS's own
    # per-bundle-ID permission caching if a rebuild alone doesn't
    # immediately resolve it for an install that's run before.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "Patchy needs audio input access to route audio from your chosen input devices."
)

# Shared by Patchy and PatchyFX — see patchy_configure_target() below.
set(PATCHY_SOURCES
    Source/PatchyProcessor.cpp
    Source/PatchyEditor.cpp
    Source/ProcessingGraph.cpp
    Source/NodeProcessor.cpp
    Pax/PaxRegistry.cpp
    Pax/PaxScanner.cpp
    Source/GraphModel.cpp
    Source/WebBridge.cpp
    Source/WebBridge_Dispatch.cpp
    Source/WebBridge_Push.cpp
    Source/WebBridge_FileIO.cpp
    Source/WebBridge_Fragments.cpp
    Source/StandaloneApp.cpp
    Source/MidiDeviceNodes.cpp
    Source/AudioDeviceNodes.cpp
    Source/SleepGuard.cpp
    Source/PatchyLog.cpp
    Source/UdpDeviceNodes.cpp
    Source/OscDeviceNodes.cpp
    Source/OscMonitorNode.cpp
    Source/UdpMonitorNode.cpp
    Source/MqttDeviceNodes.cpp
    Source/ArtNetDeviceNodes.cpp
    Source/ArtNetReceiver.cpp
    Source/DmxDeviceNodes.cpp
    Source/DmxMonitorNode.cpp
    Source/DmxConsoleNode.cpp
    Source/ArtNetMonitorNode.cpp
    Source/ArtNetConsoleNode.cpp
    Source/MidiMonitorNode.cpp
    Source/AudioMonitorNode.cpp
    Source/AudioPlayerNode.cpp
    Source/MidiKeyboardNode.cpp
)


# ── libmosquitto (MQTT client library) — v0.0.922 (2026-10-03) ──────────────
if(WIN32)
    set(BUILD_TESTING OFF CACHE BOOL "Do not build dependency tests" FORCE)
    FetchContent_Declare(pthreads4w
        GIT_REPOSITORY https://github.com/jwinarske/pthreads4w.git
        GIT_TAG cmake)
    FetchContent_MakeAvailable(pthreads4w)
    target_include_directories(libpthreadVC3 INTERFACE "${pthreads4w_SOURCE_DIR}")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
endif()

# Built from source as a STATIC library and linked into Patchy, instead of the
# Homebrew dylib (/opt/homebrew/opt/mosquitto/lib): Homebrew's is arm64-only
# (no universal build possible) and a dynamic Homebrew dependency means
# Patchy can't load on a Mac without Homebrew mosquitto. Client library only:
# no broker, no command-line tools, no TLS (Patchy uses plain MQTT —
# connect/subscribe/publish/username+password — so no OpenSSL needed).
set(WITH_TLS               OFF CACHE BOOL "" FORCE)
set(WITH_TLS_PSK           OFF CACHE BOOL "" FORCE)
set(WITH_EC                OFF CACHE BOOL "" FORCE)
set(WITH_SRV               OFF CACHE BOOL "" FORCE)
set(WITH_CJSON             OFF CACHE BOOL "" FORCE)
set(WITH_BROKER            OFF CACHE BOOL "" FORCE)
set(WITH_APPS              OFF CACHE BOOL "" FORCE)
set(WITH_PLUGINS           OFF CACHE BOOL "" FORCE)
set(WITH_CLIENTS           OFF CACHE BOOL "" FORCE)
set(WITH_LIB_CPP           OFF CACHE BOOL "" FORCE)
set(DOCUMENTATION          OFF CACHE BOOL "" FORCE)
set(WITH_STATIC_LIBRARIES  ON  CACHE BOOL "" FORCE)
set(WITH_PIC               ON  CACHE BOOL "" FORCE)   # linked into a plug-in (shared module)
set(WITH_THREADING         ON  CACHE BOOL "" FORCE)   # mosquitto_loop_start()
# mosquitto's own CMakeLists declares an old minimum CMake version (3.5),
# which CMake 4 refuses unless told the minimum policy version to assume —
# 3.10 (v0.0.929) also silences the "Compatibility with CMake < 3.10 will be
# removed" deprecation warning. Scoped to mosquitto: unset right after.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_Declare(
    mosquitto
    GIT_REPOSITORY https://github.com/eclipse-mosquitto/mosquitto.git
    GIT_TAG        v2.0.20
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(mosquitto)
unset(CMAKE_POLICY_VERSION_MINIMUM)
if(WIN32)
    target_link_libraries(libmosquitto_static PThreads4W::PThreads4W)
endif()
set(MOSQUITTO_INCLUDE_DIR "${mosquitto_SOURCE_DIR}/include")
set(MOSQUITTO_LIBRARY     libmosquitto_static)
message(STATUS "libmosquitto: static, built from source (${MOSQUITTO_INCLUDE_DIR})")

# ── Shared target setup — v0.0.919 (2026-10-02) ──────────────────────────────
# Patchy and PatchyFX are the same program built twice: identical sources,
# UI bundle, libraries and defines. Everything per-target goes through here
# so the two can never drift apart.
function(patchy_configure_target tgt)
    target_sources(${tgt} PRIVATE ${PATCHY_SOURCES})

    if(WIN32)
        target_compile_definitions(${tgt} PRIVATE NOMINMAX JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
    endif()

    if(NOT NODEGRAPH_DEV_MODE)
        target_link_libraries(${tgt} PRIVATE PatchyUI)
        target_compile_definitions(${tgt} PRIVATE HAS_BUNDLED_UI=1)
    else()
        target_compile_definitions(${tgt} PRIVATE HAS_BUNDLED_UI=0)
    endif()

    target_link_libraries(${tgt}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_devices
            juce::juce_audio_formats
            juce::juce_gui_extra
            juce::juce_dsp
            ${MOSQUITTO_LIBRARY}
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
    target_include_directories(${tgt} PRIVATE ${MOSQUITTO_INCLUDE_DIR})
    target_compile_definitions(${tgt} PRIVATE LIBMOSQUITTO_STATIC)   # v0.0.922 — static libmosquitto

    # v0.0.915 (2026-09-27) — SleepGuard.cpp uses IOPMAssertionCreateWithName.
    # JUCE links IOKit on macOS already; explicit here so it never depends on that.
    if (APPLE)
        target_link_libraries(${tgt} PRIVATE "-framework IOKit")
    endif()

    target_compile_definitions(${tgt} PRIVATE
            JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_WIN_WEBVIEW2=1
        # For AudioPlayerNode's own file loading (2026-09-04) — WAV/AIFF are
        # already always available via juce::AudioFormatManager::
        # registerBasicFormats(), no flag needed for either. JUCE_USE_MP3AUDIOFORMAT
        # specifically carries its own official, explicit legal disclaimer
        # (patent/licensing risk is the enabling party's own responsibility,
        # not JUCE's/Raw Material Software's) — flagged to the user directly
        # before enabling this, not silently turned on.
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_MP3AUDIOFORMAT=1
    )
endfunction()

patchy_configure_target(Patchy)

# Build the standalone application and the selected algorithm DLLs.
add_custom_target(AudioSim_Everything)
add_dependencies(AudioSim_Everything Patchy_All)

# ── Example Pax (built separately) (optional, built separately)
add_subdirectory(Pax EXCLUDE_FROM_ALL)
# v0.0.922 — Patchy_Everything also builds and installs the Pax (universal,
# like Patchy — see Pax_Install in Pax/CMakeLists.txt).
add_dependencies(AudioSim_Everything Pax_Install)

# ── Convenience targets ───────────────────────────────────────────────────────
if(NPM_EXE)
    add_custom_target(ui_rebuild
        COMMAND ${NPM_EXE} ci
        COMMAND ${NPM_EXE} run build
        COMMAND ${CMAKE_COMMAND} -E tar cf "${UI_ZIP}" --format=zip -- .
        WORKING_DIRECTORY "${UI_DIR}"
        COMMENT "Rebuilding and repackaging React UI" VERBATIM)

    add_custom_target(ui_dev
        COMMAND ${NPM_EXE} run dev
        WORKING_DIRECTORY "${UI_DIR}"
        COMMENT "Starting Vite dev server" VERBATIM)
endif()

# ── Unit Tests ────────────────────────────────────────────────────────────────
if (PATCHY_BUILD_TESTS)
    add_executable (PatchyTests
        Tests/PatchyTestRunner.cpp
        Tests/GraphModelTests.cpp
        Tests/AudioSimTests.cpp
        Tests/WindowPlacementTests.cpp
        Tests/AudioDeviceDiscoveryTests.cpp
        Tests/AudioChannelLayoutTests.cpp
        Tests/MqttConnectionOwnershipTests.cpp
        Tests/WaitableEventCpuSpinTests.cpp
        Tests/MidiMorpherTests.cpp
        Source/GraphModel.cpp
    )

    target_include_directories (PatchyTests PRIVATE Source Pax)

    target_compile_features (PatchyTests PRIVATE cxx_std_20)

    target_link_libraries (PatchyTests PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_data_structures
        juce::juce_events
        juce::juce_gui_basics
    )

    target_compile_definitions (PatchyTests PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
    )

    enable_testing()
    add_test (NAME PatchyTests COMMAND PatchyTests)
    # Exercise the real graph implementation and sources, with the same
    # headers/definitions as shared code. Its JUCE implementation is already
    # linked into Patchy, so do not compile a second set of module sources.
    add_executable (AudioRoutingTests
        Tests/PatchyTestRunner.cpp
        Tests/AudioRoutingIntegrationTests.cpp
        Tests/AudioModuleTests.cpp
        Tests/AudioModuleTelemetryTests.cpp
        Tests/WelchSpectrumTests.cpp
        Tests/WelchChannelAnalysisTests.cpp
        Tests/PaxRoutingTests.cpp)
    target_compile_features (AudioRoutingTests PRIVATE cxx_std_20)
    target_include_directories (AudioRoutingTests PRIVATE
        Source Pax $<TARGET_PROPERTY:Patchy,INCLUDE_DIRECTORIES>)
    target_compile_definitions (AudioRoutingTests PRIVATE
        $<TARGET_PROPERTY:Patchy,COMPILE_DEFINITIONS>)
    target_link_libraries (AudioRoutingTests PRIVATE Patchy)
    add_dependencies (AudioRoutingTests AmpPax SpectrumyserPax)
    add_test (NAME AudioRoutingTests COMMAND AudioRoutingTests AudioRouting)
    if(WIN32)
        add_executable(PaxDllSmoke Tests/PaxDllSmoke.cpp)
        target_include_directories(PaxDllSmoke PRIVATE Pax)
        target_compile_definitions(PaxDllSmoke PRIVATE NOMINMAX)
        add_dependencies(PaxDllSmoke AmpPax)
        add_test(NAME PaxDllSmoke COMMAND PaxDllSmoke $<TARGET_FILE:AmpPax>)
    endif()
endif()
