#pragma once
#include "AudioSimConfig.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <cmath>

namespace AudioSim
{
inline double moduleNumber (const juce::var& value, double fallback, double minimum, double maximum)
{
    const double number = (double) value;
    return (value.isInt() || value.isInt64() || value.isDouble()) && std::isfinite (number)
        ? juce::jlimit (minimum, maximum, number) : fallback;
}
inline int muxPortCount (const juce::var& settings)
{
    return (int) moduleNumber (settings["portCount"], 2, 1, maxAudioChannels);
}
inline std::vector<int> demuxWidths (const juce::var& settings)
{
    std::vector<int> result;
    if (const auto* widths = settings["widths"].getArray())
        for (const auto& width : *widths)
        {
            if (result.size() == maxAudioChannels) break;
            result.push_back ((int) moduleNumber (width, 1, 1, maxAudioChannels));
        }
    if (result.empty()) result = { 1, 1 };
    return result;
}
inline juce::var intArray (const std::vector<int>& values)
{
    juce::Array<juce::var> array;
    for (int value : values) array.add (value);
    return array;
}
}
