cmake_minimum_required(VERSION 3.22)
project(PatchyPax)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Output directory ──────────────────────────────────────────────────────────
# Built Pax land in build/pax/ for easy testing.
# Copy them to your Patchy Pax folder to load them.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pax")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pax")  # Windows .dll

# ── Helper macro ─────────────────────────────────────────────────────────────
macro(add_pax TARGET_NAME SOURCE_FILE)
    add_library(${TARGET_NAME} SHARED ${SOURCE_FILE})

    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

    # No JUCE, no external dependencies — just the C++ standard library.
    # Remove the default "lib" prefix on Linux/macOS so the file is
    # "LevelPax.dylib" not "libLevelPax.dylib".
    set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "")
    if(WIN32)
        set_target_properties(${TARGET_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
        target_compile_definitions(${TARGET_NAME} PRIVATE _USE_MATH_DEFINES)
    endif()

    # On macOS, produce .dylib (not .so)
    if(APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX ".dylib")
    endif()
endmacro()

# Audio examples included with the framework.
set(PAX_TARGETS LevelPax AmpPax StereoSplitterPax SpectrumyserPax)
add_pax(LevelPax LevelPax/LevelPax.cpp)
add_pax(AmpPax AmpPax/AmpPax.cpp)
add_pax(StereoSplitterPax StereoSplitterPax/StereoSplitterPax.cpp)
add_pax(SpectrumyserPax SpectrumyserPax/SpectrumyserPax.cpp)

option(AUDIOSIM_BUILD_EXTRA_PAX "Build upstream MIDI/network/converter examples" OFF)
if(AUDIOSIM_BUILD_EXTRA_PAX)
    add_pax(TransposePax TransposePax/TransposePax.cpp)
    add_pax(ChannelFilterPax ChannelFilterPax/ChannelFilterPax.cpp)
    add_pax(EnvelopePax EnvelopePax/EnvelopePax.cpp)
    add_pax(MqttToValuePax MqttToValuePax/MqttToValuePax.cpp)
    add_pax(OscToValuePax OscToValuePax/OscToValuePax.cpp)
    add_pax(ValueToDMXPax ValueToDMXPax/ValueToDMXPax.cpp)
    add_pax(AudioToDmxPax AudioToDmxPax/AudioToDmxPax.cpp)
    add_pax(MidiToDmxPax MidiToDmxPax/MidiToDmxPax.cpp)
    add_pax(UdpValueToMidiCCPax UdpValueToMidiCCPax/UdpValueToMidiCCPax.cpp)
    add_pax(AudioPeakToOscPax AudioPeakToOscPax/AudioPeakToOscPax.cpp)
    list(APPEND PAX_TARGETS TransposePax ChannelFilterPax EnvelopePax MqttToValuePax
         OscToValuePax ValueToDMXPax AudioToDmxPax MidiToDmxPax UdpValueToMidiCCPax AudioPeakToOscPax)
endif()
# ── Install target (optional) ─────────────────────────────────────────────────
# cmake --install build --prefix ~ will copy Pax to the right folder.
if(APPLE)
    set(PAX_INSTALL_DIR "$ENV{HOME}/Library/AudioSim/Pax")
elseif(WIN32)
    set(PAX_INSTALL_DIR "$ENV{APPDATA}/AudioSim/Pax")
else()
    set(PAX_INSTALL_DIR "$ENV{HOME}/.audiosim/pax")
endif()


install(TARGETS ${PAX_TARGETS}
        LIBRARY DESTINATION "${PAX_INSTALL_DIR}"
        RUNTIME DESTINATION "${PAX_INSTALL_DIR}")

# ── Pax_Install — v0.0.922 (2026-10-03) ──────────────────────────────────────
# Builds every Pax and copies them to the Pax folder Patchy scans, in one go
# (part of the main project's Patchy_Everything). Needed since the universal
# build: Patchy running as Intel code (Logic under Rosetta) can only load
# Intel dylibs, so the installed Pax must be rebuilt with the same
# architectures as Patchy — this keeps them in step automatically.
set(_pax_files "")
foreach(_t IN LISTS PAX_TARGETS)
    list(APPEND _pax_files "$<TARGET_FILE:${_t}>")
endforeach()
add_custom_target(Pax_Install
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PAX_INSTALL_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pax_files} "${PAX_INSTALL_DIR}"
    COMMENT "Installing Pax to ${PAX_INSTALL_DIR}"
    VERBATIM)
add_dependencies(Pax_Install ${PAX_TARGETS})
