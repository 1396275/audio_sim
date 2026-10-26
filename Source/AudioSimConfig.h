#pragma once
#include <array>

// Identity and built-in policy for this fork. Keep upstream type IDs and the
// Pax API unchanged so existing audio projects and algorithm DLLs still load.
namespace AudioSim
{
    inline constexpr auto appName = "AudioSim";
    inline constexpr auto paxFolder = "AudioSimPax";
    inline constexpr int maxAudioChannels = 64;
    inline constexpr int minBlockSize = 16;
    inline constexpr int maxBlockSize = 4096;
    inline constexpr double minSampleRate = 1000.0;
    inline constexpr double maxSampleRate = 192000.0;
    inline constexpr std::array<int, 5> builtinNodeTypes { 3, 4, 6, 26, 29 };

    // All other rates are strictly internal simulation: no device discovery/open.
    constexpr bool isHardwareSampleRate (double rate)
    {
        return rate == 44100.0 || rate == 48000.0;
    }

    constexpr bool isBuiltinNodeType (int type)
    {
        for (const auto enabled : builtinNodeTypes)
            if (enabled == type) return true;
        return false;
    }
}
