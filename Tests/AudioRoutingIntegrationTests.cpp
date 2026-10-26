#include <juce_core/juce_core.h>
#include "ProcessingGraph.h"
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace
{
    // These tests exercise the real graph builder, compiled routes, node
    // processors and monitor ring. No soundcard or dynamic library is opened.
    struct AudioRoutingFixture
    {
        static constexpr int frameSize = 24;
        static constexpr int fileLength = 5;
        static constexpr double sampleRate = 1500.0;

        GraphModel model;
        std::unordered_map<juce::String, std::unique_ptr<AudioPlayerState>> sources;
        std::unordered_map<juce::String, std::unique_ptr<AudioMonitorBuffer>> monitors;
        // Destroy the graph before the shared source and monitor states.
        ProcessingGraph graph;

        static float fileSample (int channel, int sample, float offset)
        {
            return offset + 0.01f * (channel + 1) + 0.001f * (sample % fileLength);
        }

        juce::String addFile (int channels, float offset = 0.f)
        {
            const auto id = model.addNode (26, 0.f, 0.f).id;
            auto state = std::make_unique<AudioPlayerState>();
            state->mode.store (AudioPlayerState::Mode::File);
            state->playing.store (true);
            state->looping.store (true);
            state->level.store (1.f);
            state->fileSampleRate = state->loadedSampleRate = sampleRate;
            state->fileBuffer.setSize (channels, fileLength);
            for (int channel = 0; channel < channels; ++channel)
                for (int sample = 0; sample < fileLength; ++sample)
                    state->fileBuffer.setSample (channel, sample, fileSample (channel, sample, offset));
            sources[id] = std::move (state);
            return id;
        }

        juce::String addGenerator (int channels)
        {
            const auto id = model.addNode (26, 0.f, 0.f).id;
            auto state = std::make_unique<AudioPlayerState>();
            state->mode.store (AudioPlayerState::Mode::Square);
            state->playing.store (true);
            state->outputChannels.store (channels);
            state->sineFrequency.store (1.f);
            state->level.store (0.25f);
            sources[id] = std::move (state);
            return id;
        }

        juce::String addMonitor (int type = 6)
        {
            const auto id = model.addNode (type, 100.f, 0.f).id;
            monitors[id] = std::make_unique<AudioMonitorBuffer>();
            return id;
        }

        juce::String connect (const juce::String& source, const juce::String& monitor)
        {
            if (auto* connection = model.addConnection (source, source + "_Audio Out_out",
                                                        monitor, monitor + "_Audio In_in"))
                return connection->id;
            return {};
        }

        void rebuild()
        {
            graph.isStandaloneMode = true;
            graph.graphModel = &model;
            graph.rebuild (model,
                nullptr, // No registry or dynamic-library instances.
                {},
                [this](const juce::String& id) { return monitors.at (id).get(); },
                {}, {}, {}, {}, {}, {}, {}, {},
                [this](const juce::String& id) { return sources.at (id).get(); });
            // The host buffer is only the simulation clock. Its one channel
            // must not cap the widths inferred inside the independent graph.
            graph.prepare (sampleRate, frameSize, 1);
        }

        void restartFiles()
        {
            for (auto& entry : sources)
            {
                entry.second->playbackPosition.store (0);
                entry.second->seekRequest.store (-1);
            }
        }

        void process()
        {
            juce::AudioBuffer<float> host (1, frameSize);
            juce::FloatVectorOperations::fill (host.getWritePointer (0), 0.75f, frameSize);
            juce::MidiBuffer midi;
            graph.process (host, midi);
        }

        std::vector<std::vector<float>> snapshot (const juce::String& monitor)
        {
            std::vector<std::vector<float>> channels;
            monitors.at (monitor)->snapshotChannels (frameSize, frameSize, channels);
            return channels;
        }
    };
}

class AudioRoutingIntegrationTests final : public juce::UnitTest
{
public:
    AudioRoutingIntegrationTests() : UnitTest ("Audio graph routes intrinsic channel widths", "AudioRouting") {}

    void runTest() override
    {
        testWideFilesIgnoreHostWidth();
        testMixingAndMissingChannels();
        testDisconnectedMonitor();
        testGeneratorWidthRebuild();
        testIndependentBranchWidths();
        testFrequencyAnalysisObserver();
    }

private:
    void testFrequencyAnalysisObserver()
    {
        beginTest ("Frequency analysis is a saved input-only observer with full channel routing");
        AudioRoutingFixture fixture;
        const auto source = fixture.addFile (8);
        const auto analyser = fixture.addMonitor (29);
        expect (AudioSim::isBuiltinNodeType (29));
        const auto* modelNode = fixture.model.findNode (analyser);
        expectEquals ((int) modelNode->ports.size(), 1);
        expect (modelNode->ports[0].type == PortType::Audio && modelNode->ports[0].direction == PortDirection::Input);
        const juce::String settings = R"({"fftSize":4096,"window":"hamming","channel":7,"freqMin":20,"freqMax":700,"held":{"sr":1500,"fftSize":256,"decibels":[-12,-34]}})";
        fixture.model.setNodeSettings (analyser, settings);
        fixture.connect (source, analyser);
        fixture.rebuild(); fixture.process();
        expectMonitorWidth (fixture, analyser, 8);
        const auto traces = fixture.snapshot (analyser);
        expectEquals ((int) traces.size(), 8);
        for (int channel = 0; channel < (int) traces.size(); ++channel)
            for (int i = 0; i < AudioRoutingFixture::frameSize; ++i)
                expectWithinAbsoluteError (traces[(size_t) channel][(size_t) i], AudioRoutingFixture::fileSample (channel, i, 0.f), 0.000001f);
        GraphModel restored;
        restored.pushExistingSnapshot (juce::JSON::parse (juce::JSON::toString (fixture.model.toVar())));
        expect (restored.undo());
        expect (restored.findNode (analyser) != nullptr);
        if (const auto* saved = restored.findNode (analyser)) expectEquals (saved->settingsJson, settings);
        expectEquals ((int) restored.getConnections().size(), 1);
        expect (fixture.model.addConnection (analyser, analyser + "_Audio Out_out", source, source + "_Audio In_in") == nullptr);
    }

    void expectMonitorWidth (AudioRoutingFixture& fixture, const juce::String& monitor, int channels)
    {
        auto* node = fixture.graph.findNode (monitor);
        expect (node != nullptr);
        if (node == nullptr) return;
        expectEquals (node->connectedInputAudioChannels, channels);
        expectEquals (node->getAudioChannelCount(), std::max (1, channels));
        expectEquals (node->getOutputAudioChannels(), 0, "An observer has no output signal");
        for (int channel = 0; channel < node->outputAudio.getNumChannels(); ++channel)
            for (int sample = 0; sample < AudioRoutingFixture::frameSize; ++sample)
                expectEquals (node->outputAudio.getSample (channel, sample), 0.f);
    }

    void testWideFilesIgnoreHostWidth()
    {
        for (int width : { 8, 64 })
        {
            beginTest (juce::String (width) + "-channel file survives a mono simulation clock and 24-sample frames");
            AudioRoutingFixture fixture;
            // Put the observer first to exercise topology rather than insertion order.
            const auto monitor = fixture.addMonitor();
            const auto source = fixture.addFile (width);
            expect (fixture.connect (source, monitor).isNotEmpty());
            fixture.rebuild();
            auto* sourceNode = fixture.graph.findNode (source);
            expect (sourceNode != nullptr);
            if (sourceNode == nullptr) return;
            expectEquals (sourceNode->getAudioChannelCount(), width);
            expectEquals (sourceNode->getOutputAudioChannels(), width);
            for (int block = 0; block < 3; ++block)
            {
                fixture.process();
                expectMonitorWidth (fixture, monitor, width);
                const auto traces = fixture.snapshot (monitor);
                expectEquals ((int) traces.size(), width);
                if ((int) traces.size() != width) continue;
                for (int channel = 0; channel < width; ++channel)
                {
                    expectEquals ((int) traces[(size_t) channel].size(), AudioRoutingFixture::frameSize);
                    for (int sample = 0; sample < (int) traces[(size_t) channel].size(); ++sample)
                        expectWithinAbsoluteError (traces[(size_t) channel][(size_t) sample],
                            AudioRoutingFixture::fileSample (channel, block * AudioRoutingFixture::frameSize + sample, 0.f), 0.000001f);
                }
            }
        }
    }

    void testMixingAndMissingChannels()
    {
        beginTest ("Two- and eight-channel sources mix by channel index with missing channels silent");
        AudioRoutingFixture fixture;
        const auto narrow = fixture.addFile (2, 0.1f);
        const auto wide = fixture.addFile (8, 0.3f);
        const auto monitor = fixture.addMonitor();
        fixture.connect (narrow, monitor);
        fixture.connect (wide, monitor);
        fixture.rebuild();
        fixture.process();
        expectMonitorWidth (fixture, monitor, 8);
        auto traces = fixture.snapshot (monitor);
        expectEquals ((int) traces.size(), 8);
        if (traces.size() != 8) return;
        for (int channel = 0; channel < 8; ++channel)
            for (int sample = 0; sample < AudioRoutingFixture::frameSize; ++sample)
            {
                const float expected = AudioRoutingFixture::fileSample (channel, sample, 0.3f)
                    + (channel < 2 ? AudioRoutingFixture::fileSample (channel, sample, 0.1f) : 0.f);
                expectWithinAbsoluteError (traces[(size_t) channel][(size_t) sample], expected, 0.000001f);
            }

        beginTest ("A disabled wide source contributes silence without copying narrow channels into its missing channels");
        fixture.model.findNode (wide)->disabled = true;
        fixture.rebuild();
        fixture.restartFiles();
        fixture.process();
        expectMonitorWidth (fixture, monitor, 8);
        traces = fixture.snapshot (monitor);
        expectEquals ((int) traces.size(), 8);
        if (traces.size() != 8) return;
        for (int channel = 0; channel < 8; ++channel)
            for (int sample = 0; sample < AudioRoutingFixture::frameSize; ++sample)
                expectWithinAbsoluteError (traces[(size_t) channel][(size_t) sample],
                    channel < 2 ? AudioRoutingFixture::fileSample (channel, sample, 0.1f) : 0.f, 0.000001f);
    }

    void testDisconnectedMonitor()
    {
        beginTest ("Removing a route clears old monitor history and reports zero connected channels");
        AudioRoutingFixture fixture;
        const auto source = fixture.addFile (8);
        const auto monitor = fixture.addMonitor();
        const auto route = fixture.connect (source, monitor);
        expect (route.isNotEmpty());
        fixture.rebuild();
        fixture.process();
        expectEquals ((int) fixture.snapshot (monitor).size(), 8);
        expect (fixture.model.removeConnection (route));
        fixture.rebuild();
        fixture.process();
        expectMonitorWidth (fixture, monitor, 0);
        expect (fixture.snapshot (monitor).empty());
    }

    void testGeneratorWidthRebuild()
    {
        beginTest ("Generator output width is local and updates downstream monitors after a rebuild");
        AudioRoutingFixture fixture;
        const auto generator = fixture.addGenerator (2);
        const auto monitor = fixture.addMonitor();
        fixture.connect (generator, monitor);
        for (int width : { 2, 8, 1 })
        {
            fixture.sources.at (generator)->outputChannels.store (width);
            fixture.rebuild();
            fixture.process();
            expectMonitorWidth (fixture, monitor, width);
            const auto traces = fixture.snapshot (monitor);
            expectEquals ((int) traces.size(), width);
            if ((int) traces.size() != width) continue;
            for (const auto& channel : traces)
                for (float value : channel) expectWithinAbsoluteError (value, 0.25f, 0.000001f);
        }
    }

    void testIndependentBranchWidths()
    {
        beginTest ("Independent two- and eight-channel branches retain their own widths");
        AudioRoutingFixture fixture;
        const auto narrowMonitor = fixture.addMonitor();
        const auto wide = fixture.addFile (8, 0.2f);
        const auto narrow = fixture.addFile (2, 0.4f);
        const auto wideMonitor = fixture.addMonitor();
        fixture.connect (narrow, narrowMonitor);
        fixture.connect (wide, wideMonitor);
        fixture.rebuild();
        fixture.process();
        expectMonitorWidth (fixture, narrowMonitor, 2);
        expectMonitorWidth (fixture, wideMonitor, 8);
        for (const auto& branch : { std::make_pair (narrowMonitor, 2), std::make_pair (wideMonitor, 8) })
        {
            const auto traces = fixture.snapshot (branch.first);
            expectEquals ((int) traces.size(), branch.second);
            if ((int) traces.size() != branch.second) continue;
            const float offset = branch.second == 2 ? 0.4f : 0.2f;
            for (int channel = 0; channel < branch.second; ++channel)
                for (int sample = 0; sample < AudioRoutingFixture::frameSize; ++sample)
                    expectWithinAbsoluteError (traces[(size_t) channel][(size_t) sample],
                        AudioRoutingFixture::fileSample (channel, sample, offset), 0.000001f);
        }
    }
};

static AudioRoutingIntegrationTests audioRoutingIntegrationTests;
