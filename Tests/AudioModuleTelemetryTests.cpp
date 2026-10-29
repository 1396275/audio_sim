#include "PatchyProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>

// Exercise the application's real polling path, not just node snapshots:
// a valid measurement must reach the UI even when there is no audio output.
class AudioModuleTelemetryTests final : public juce::UnitTest
{
public:
    AudioModuleTelemetryTests() : UnitTest ("Audio module UI telemetry", "AudioRouting") {}

    void runTest() override
    {
        beginTest ("WAV Source -> RMS -> Value Display reaches UI polling without an audio output device");
        constexpr double rate = 48000;
        constexpr int frames = 24, samples = 4800;
        juce::TemporaryFile wav (".wav");
        juce::AudioBuffer<float> pcm (3, samples);
        pcm.clear();
        for (int sample = 0; sample < samples; ++sample)
        {
            pcm.setSample (0, sample, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * sample / 48));
            pcm.setSample (2, sample, 0.00001f);
        }
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = wav.getFile().createOutputStream();
        auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
            .withSampleRate (rate).withNumChannels (3).withBitsPerSample (24));
        expect (writer != nullptr);
        if (! writer) return;
        expect (writer->writeFromAudioSampleBuffer (pcm, 0, samples));
        writer.reset();

        PatchyProcessor processor;
        processor.isStandalone = true;
        processor.configureSimulation (rate, frames);
        auto& model = processor.getGraphModel();
        const auto source = model.addNode (26, 0, 0).id;
        const auto rms = model.addNode (30, 100, 0).id;
        const auto display = model.addNode (34, 200, 0).id;
        auto* settings = new juce::DynamicObject();
        settings->setProperty ("mode", "file");
        settings->setProperty ("filePath", wav.getFile().getFullPathName());
        settings->setProperty ("level", 1.0);
        settings->setProperty ("loop", true);
        model.setNodeSettings (source, juce::JSON::toString (juce::var (settings)));
        model.setNodeSettings (rms, R"({"windowMs":100})");
        const auto* audioConnection = model.addConnection (source, source + "_Audio Out_out", rms, rms + "_Audio In_in");
        expect (audioConnection != nullptr);
        const auto audioConnectionId = audioConnection ? audioConnection->id : juce::String();
        expect (model.addConnection (rms, rms + "_Value Out_out", display, display + "_Value In_in") != nullptr);
        auto* state = processor.getOrCreateAudioPlayerState (source);
        expectEquals (state->fileBuffer.getNumChannels(), 3);
        expectEquals (state->fileBuffer.getNumSamples(), samples);
        state->playing.store (true);

        juce::AudioBuffer<float> host (1, frames);
        juce::MidiBuffer midi;
        const auto process = [&](int blocks) {
            for (int block = 0; block < blocks; ++block)
            {
                host.clear(); midi.clear(); processor.processBlock (host, midi);
            }
        };
        const auto poll = [&](const juce::String& id) -> juce::var {
            for (const auto& activity : processor.getPortActivity())
                if (activity.nodeId == id)
                {
                    // These nodes emit no audio: audio meter activity cannot
                    // be the condition for delivering their numeric status.
                    expectEquals (activity.audioRmsL, 0.f);
                    expectEquals (activity.audioRmsR, 0.f);
                    return activity.audioModule;
                }
            expect (false, "UI polling dropped module " + id);
            return {};
        };

        process (1);
        const auto collecting = poll (rms);
        expectEquals ((int) collecting["inputChannels"], 3);
        expect (! (bool) collecting["rmsReady"]);
        expectEquals (poll (display)["displayValues"].size(), 3);
        process (samples / frames - 1);
        const auto measured = poll (rms);
        const auto received = poll (display)["displayValues"];
        expect ((bool) measured["rmsReady"]);
        expectEquals (measured["rms"].size(), 3);
        expectEquals (received.size(), 3);
        const double expected[] { std::sqrt (0.125), 0.0, 0.00001 };
        for (int channel = 0; channel < std::min (3, std::min (measured["rms"].size(), received.size())); ++channel)
        {
            expectWithinAbsoluteError ((double) measured["rms"][channel], expected[channel], 0.0000002);
            expectEquals ((int) received[channel]["key"], channel + 1);
            expectWithinAbsoluteError ((double) received[channel]["value"], expected[channel], 0.0000002);
        }

        beginTest ("Silent measurements remain visible and repeated UI polls retain the latest values");
        state->level.store (0.f);
        process (samples / frames);
        for (int push = 0; push < 2; ++push)
        {
            const auto silent = poll (rms);
            const auto displayed = poll (display)["displayValues"];
            expect ((bool) silent["rmsReady"]);
            expectEquals (silent["rms"].size(), 3);
            expectEquals (displayed.size(), 3);
            for (int channel = 0; channel < std::min (3, std::min (silent["rms"].size(), displayed.size())); ++channel)
            {
                expectEquals ((double) silent["rms"][channel], 0.0);
                expectEquals ((double) displayed[channel]["value"], 0.0);
            }
        }

        beginTest ("Disconnecting the audio input pushes empty status to clear the previous readings");
        model.removeConnection (audioConnectionId);
        process (1);
        const auto disconnected = poll (rms);
        expectEquals ((int) disconnected["inputChannels"], 0);
        expect (! (bool) disconnected["rmsReady"]);
        expectEquals (disconnected["rms"].size(), 0);
        expectEquals (poll (display)["displayValues"].size(), 0);
    }
};
static AudioModuleTelemetryTests audioModuleTelemetryTests;
