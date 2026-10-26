#include "WelchChannelAnalysis.h"
#include <set>

class WelchChannelAnalysisTests final : public juce::UnitTest
{
public:
    WelchChannelAnalysisTests() : UnitTest ("Multichannel Welch subplots", "AudioRouting") {}
    void runTest() override
    {
        constexpr int size = 1024, channels = 16, count = 2048;
        auto buffer = std::make_unique<AudioMonitorBuffer>();
        buffer->prepare (channels);
        juce::AudioBuffer<float> input (channels, count);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < count; ++i)
                input.setSample (ch, i, (float) (0.25 * std::sin (juce::MathConstants<double>::twoPi * (ch + 10) * i / size)));
        buffer->push (input, channels, count, 48000.0);
        AudioSim::WelchChannelAnalysis analyser;
        auto settings = juce::JSON::parse (R"({"layout":"stacked","fftSize":1024,"averages":2})");

        beginTest ("Distinct input tones produce distinct channel spectra over bounded refreshes");
        std::set<int> received;
        for (int cycle = 0; cycle < 2; ++cycle)
        {
            const auto frames = analyser.snapshot (*buffer, settings, 48000.0);
            expectEquals ((int) frames.size(), 8);
            for (const auto& frame : frames)
            {
                expect (received.insert (frame.channel).second);
                expectEquals (frame.inputChannels, channels);
                expectEquals (frame.averages, 2);
                expectEquals ((int) frame.decibels.size(), size / 2 + 1);
                if (! frame.decibels.empty())
                    expectEquals ((int) (std::max_element (frame.decibels.begin(), frame.decibels.end()) - frame.decibels.begin()), frame.channel + 10);
            }
        }
        expectEquals ((int) received.size(), channels);
        expect (analyser.snapshot (*buffer, settings, 48000.0).empty(), "A stopped source must not recalculate unchanged channels");

        beginTest ("Selected channels replay a stopped capture and retain their original identities");
        settings.getDynamicObject()->setProperty ("selectedChannels", juce::Array<juce::var> { 2, 15 });
        const auto selected = analyser.snapshot (*buffer, settings, 48000.0);
        expectEquals ((int) selected.size(), 2);
        if (selected.size() == 2) { expectEquals (selected[0].channel, 2); expectEquals (selected[1].channel, 15); }

        beginTest ("Returning to legacy single channel replays only that channel");
        settings.getDynamicObject()->setProperty ("layout", "single");
        settings.getDynamicObject()->setProperty ("channel", 7);
        const auto single = analyser.snapshot (*buffer, settings, 48000.0);
        expectEquals ((int) single.size(), 1);
        if (! single.empty()) expectEquals (single.front().channel, 7);

        beginTest ("Deselecting all and disconnecting still publish clearing metadata");
        settings.getDynamicObject()->setProperty ("layout", "stacked");
        settings.getDynamicObject()->setProperty ("selectedChannels", juce::Array<juce::var>{});
        const auto none = analyser.snapshot (*buffer, settings, 48000.0);
        expectEquals ((int) none.size(), 1);
        if (! none.empty()) { expectEquals (none.front().channel, -1); expect (none.front().decibels.empty()); }
        buffer->push (input, 0, 0, 1500.0);
        const auto disconnected = analyser.snapshot (*buffer, settings, 48000.0);
        expectEquals ((int) disconnected.size(), 1);
        if (! disconnected.empty()) { expectEquals (disconnected.front().inputChannels, 0); expectEquals (disconnected.front().sampleRate, 1500.0); }

        beginTest ("All 64 channels refresh without stale caches after input growth and FFT changes");
        buffer->prepare (64);
        juce::AudioBuffer<float> wide (64, count); wide.clear();
        buffer->push (wide, 64, count, 48000.0);
        settings.getDynamicObject()->removeProperty ("selectedChannels");
        settings.getDynamicObject()->setProperty ("fftSize", 2048);
        received.clear();
        for (int cycle = 0; cycle < 8; ++cycle)
            for (const auto& frame : analyser.snapshot (*buffer, settings, 48000.0))
            {
                received.insert (frame.channel);
                expectEquals ((int) frame.decibels.size(), 1025);
                expectEquals (frame.averages, 1);
            }
        expectEquals ((int) received.size(), 64);
    }
};
static WelchChannelAnalysisTests welchChannelAnalysisTests;
