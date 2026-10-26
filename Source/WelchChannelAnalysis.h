#pragma once
#include "WelchSpectrum.h"
#include "AudioMonitorNode.h"
#include <array>

namespace AudioSim
{
struct WelchChannelSpectrum
{
    juce::String window, layout = "single";
    double sampleRate = 48000.0;
    int fftSize = 2048, channel = 0, inputChannels = 0, averages = 0, requestedAverages = 8;
    std::vector<float> decibels;
};

// Message-thread analysis, sharing one FFT plan and scratch buffer across
// channels. Bound each refresh so a 64-channel large FFT cannot monopolise UI.
class WelchChannelAnalysis
{
public:
    static constexpr int channelsPerRefresh = 8;

    std::vector<WelchChannelSpectrum> snapshot (const AudioMonitorBuffer& buffer,
                                               const juce::String& json, double fallbackRate)
    {
        if (json != savedJson) { savedJson = json; parsedSettings = juce::JSON::parse (json); }
        return snapshot (buffer, parsedSettings, fallbackRate);
    }

    std::vector<WelchChannelSpectrum> snapshot (const AudioMonitorBuffer& buffer,
                                               const juce::var& settings, double fallbackRate)
    {
        WelchChannelSpectrum base;
        base.fftSize = WelchSpectrum::sanitiseFftSize ((int) settings["fftSize"]);
        base.window = WelchSpectrum::sanitiseWindow (settings["window"].toString());
        base.layout = settings["layout"].toString() == "stacked" ? "stacked" : "single";
        const int requested = (int) settings["averages"];
        base.requestedAverages = requested == 2 || requested == 4 || requested == 8 || requested == 16 ? requested : 8;
        uint64_t metadataVersion = 0;
        if (! buffer.snapshotChannel (0, 0, samples, base.inputChannels, base.sampleRate, metadataVersion)) return {};
        if (base.sampleRate <= 0) base.sampleRate = fallbackRate > 0 ? fallbackRate : 48000.0;

        std::vector<int> channels;
        if (base.layout == "single")
            channels.push_back (juce::jlimit (0, maxAudioChannels - 1, (int) settings["channel"]));
        else
        {
            const auto* selected = settings["selectedChannels"].getArray();
            for (int ch = 0; ch < base.inputChannels; ++ch)
                if (selected == nullptr || selected->contains (juce::var (ch))) channels.push_back (ch);
            // Metadata must still clear obsolete curves on disconnect or when
            // all channels are deselected. -1 never denotes an audio channel.
            if (channels.empty()) channels.push_back (-1);
        }

        const auto key = juce::String (base.fftSize) + ":" + base.window + ":" + base.layout + ":"
            + juce::String (base.requestedAverages) + ":" + juce::String (base.sampleRate) + ":"
            + juce::String (base.inputChannels) + ":" + juce::JSON::toString (settings["selectedChannels"])
            + ":" + juce::String ((int) settings["channel"]);
        if (key != configuration)
        {
            configuration = key;
            versions.fill (~uint64_t (0));
            nextChannel = 0;
        }
        analyser.configure (base.fftSize, base.window);
        const int count = base.fftSize + (base.requestedAverages - 1) * (base.fftSize / 2);
        std::vector<WelchChannelSpectrum> result;
        for (int visited = 0; visited < std::min ((int) channels.size(), channelsPerRefresh); ++visited)
        {
            const int channel = channels[nextChannel++ % channels.size()];
            auto snap = base;
            snap.channel = channel;
            uint64_t version = 0;
            if (! buffer.snapshotChannel (channel, count, samples, snap.inputChannels, snap.sampleRate, version)) continue;
            if (snap.sampleRate <= 0) snap.sampleRate = base.sampleRate;
            auto& previous = versions[(size_t) (channel + 1)];
            if (previous == version) continue;
            snap.averages = analyser.calculate (samples, snap.sampleRate, base.requestedAverages, snap.decibels);
            previous = version;
            result.push_back (std::move (snap));
        }
        return result;
    }
private:
    WelchSpectrum analyser;
    std::vector<float> samples;
    juce::String configuration;
    juce::String savedJson;
    juce::var parsedSettings;
    std::array<uint64_t, maxAudioChannels + 1> versions {};
    size_t nextChannel = 0;
};
}
