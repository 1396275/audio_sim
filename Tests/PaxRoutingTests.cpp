#include <juce_core/juce_core.h>
#include "ProcessingGraph.h"
#include "PaxRegistry.h"
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#if JUCE_WINDOWS
namespace
{
    struct PaxRoutingFixture
    {
        static constexpr int frames = 24, fileLength = 7;
        static constexpr double rate = 1500.0;
        PaxRegistry registry;
        GraphModel model;
        std::unordered_map<juce::String, std::unique_ptr<AudioPlayerState>> sources;
        std::unordered_map<juce::String, std::unique_ptr<AudioMonitorBuffer>> monitors;
        ProcessingGraph graph; // Destroy DLL instances before registry/state.

        static juce::File buildPaxDirectory()
        {
            const auto workingDirectory = juce::File::getCurrentWorkingDirectory();
            const auto working = workingDirectory.getChildFile ("pax");
            if (workingDirectory.getFileName() == "windows-release" && working.isDirectory()) return working;
            // Direct execution also finds only the DLLs beside the built test
            // executable. Never use Run/AudioSimPax, which may be an older copy.
            return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                .getParentDirectory().getChildFile ("pax");
        }

        static float sample (int channel, int index, float offset)
        {
            return offset + 0.01f * (channel + 1) + 0.001f * (index % fileLength);
        }

        juce::String addFile (int width, float offset = 0.f)
        {
            const auto id = model.addNode (26, 0.f, 0.f).id;
            auto state = std::make_unique<AudioPlayerState>();
            state->mode.store (AudioPlayerState::Mode::File);
            state->playing.store (true);
            state->looping.store (true);
            state->level.store (1.f);
            state->loadedSampleRate = state->fileSampleRate = rate;
            state->fileBuffer.setSize (width, fileLength);
            for (int channel = 0; channel < width; ++channel)
                for (int index = 0; index < fileLength; ++index)
                    state->fileBuffer.setSample (channel, index, sample (channel, index, offset));
            sources[id] = std::move (state);
            return id;
        }

        juce::String addPax (const juce::String& name, const juce::Array<juce::var>& parameters = {})
        {
            for (const auto& entry : registry.getEntries())
                if (entry.name == name)
                {
                    PaxPortSpec ports;
                    ports.audioIn = entry.audioInputs;
                    ports.audioOut = entry.audioOutputs;
                    const auto id = model.addNode (100 + entry.nodeType, 50.f, 0.f, name, ports).id;
                    if (! parameters.isEmpty()) model.setNodeSettings (id, juce::JSON::toString (parameters));
                    return id;
                }
            return {};
        }

        juce::String addMonitor()
        {
            const auto id = model.addNode (6, 100.f, 0.f).id;
            monitors[id] = std::make_unique<AudioMonitorBuffer>();
            return id;
        }

        juce::String audioPort (const juce::String& id, PortDirection direction, int ordinal)
        {
            if (const auto* node = model.findNode (id))
                for (const auto& port : node->ports)
                    if (port.type == PortType::Audio && port.direction == direction && ordinal-- == 0)
                        return port.id;
            return {};
        }

        bool connect (const juce::String& source, const juce::String& destination, int output = 0)
        {
            const auto from = audioPort (source, PortDirection::Output, output);
            const auto to = audioPort (destination, PortDirection::Input, 0);
            return from.isNotEmpty() && to.isNotEmpty()
                && model.addConnection (source, from, destination, to) != nullptr;
        }

        void rebuild()
        {
            graph.isStandaloneMode = true;
            graph.graphModel = &model;
            graph.rebuild (model, &registry, {},
                [this](const juce::String& id) { return monitors.at (id).get(); },
                {}, {}, {}, {}, {}, {}, {}, {},
                [this](const juce::String& id) { return sources.at (id).get(); });
            graph.prepare (rate, frames, 1);
        }

        void process()
        {
            juce::AudioBuffer<float> host (1, frames);
            juce::FloatVectorOperations::fill (host.getWritePointer (0), 0.75f, frames);
            juce::MidiBuffer midi;
            graph.process (host, midi);
        }

        DynamicPaxProcessor* pax (const juce::String& id)
        {
            return dynamic_cast<DynamicPaxProcessor*> (graph.findNode (id));
        }

        std::vector<std::vector<float>> snapshot (const juce::String& id)
        {
            std::vector<std::vector<float>> traces;
            monitors.at (id)->snapshotChannels (frames, frames, traces);
            return traces;
        }
    };
}
#endif

class PaxRoutingTests final : public juce::UnitTest
{
public:
    PaxRoutingTests() : UnitTest ("Built Pax DLLs follow routed channel layouts", "AudioRouting") {}

    void runTest() override
    {
       #if JUCE_WINDOWS
        testGainDllWidths();
        testIndependentBranches();
        testDynamicOutputPorts();
       #else
        beginTest ("Windows DLL routing fixture");
        logMessage ("Skipped: these integration fixtures load Windows DLL build outputs.");
       #endif
    }

private:
   #if JUCE_WINDOWS
    bool load (PaxRoutingFixture& fixture, std::initializer_list<const char*> names)
    {
        std::vector<PaxScanner::ScanResult> results;
        for (const auto* name : names)
        {
            PaxScanner::ScanResult result;
            result.file = PaxRoutingFixture::buildPaxDirectory().getChildFile (juce::String (name) + ".dll");
            expect (result.file.existsAsFile(), "Build output missing: " + result.file.getFullPathName());
            if (! result.file.existsAsFile()) return false;
            result.valid = true;
            result.name = name;
            result.nodeType = 2;
            results.push_back (std::move (result));
        }
        // Manual scan results exercise real registry loading without scanning
        // user folders or consulting audio devices.
        fixture.registry.load (results);
        expectEquals ((int) fixture.registry.getEntries().size(), (int) names.size());
        if (fixture.registry.getEntries().size() != names.size()) return false;
        for (const auto& entry : fixture.registry.getEntries())
        {
            expect (entry.hasChannelCapability, "Fresh multichannel DLL capability must be present");
            expectEquals (entry.maxAudioChannels, 64);
        }
        return true;
    }

    void expectWidth (PaxRoutingFixture& fixture, const juce::String& id, int width, int output = 0)
    {
        auto* node = fixture.graph.findNode (id);
        expect (node != nullptr);
        if (! node) return;
        expectEquals (node->getAudioChannelCount(), width);
        expectEquals (node->connectedInputAudioChannels, width);
        expectEquals (node->getOutputAudioChannels (output), width);
    }

    void expectSignal (PaxRoutingFixture& fixture, const juce::String& monitor,
                       int width, int block, float offset, float gain)
    {
        const auto traces = fixture.snapshot (monitor);
        expectEquals ((int) traces.size(), width);
        if ((int) traces.size() != width) return;
        for (int channel = 0; channel < width; ++channel)
        {
            expectEquals ((int) traces[(size_t) channel].size(), PaxRoutingFixture::frames);
            for (int index = 0; index < (int) traces[(size_t) channel].size(); ++index)
                expectWithinAbsoluteError (traces[(size_t) channel][(size_t) index],
                    gain * PaxRoutingFixture::sample (channel, block * PaxRoutingFixture::frames + index, offset),
                    0.000002f);
        }
    }

    void testGainDllWidths()
    {
        for (const auto* name : { "LevelPax", "AmpPax" })
            for (int width : { 8, 64 })
            {
                beginTest (juce::String (name) + " preserves " + juce::String (width)
                           + " channels with a mono host clock and 24-sample frames");
                PaxRoutingFixture fixture;
                if (! load (fixture, { name })) continue;
                const bool level = juce::String (name) == "LevelPax";
                const auto monitor = fixture.addMonitor();
                const auto algorithm = fixture.addPax (name, { juce::var (level ? -6.020599913 : 6.020599913) });
                const auto source = fixture.addFile (width);
                expect (fixture.connect (source, algorithm));
                expect (fixture.connect (algorithm, monitor));
                fixture.rebuild();
                expectWidth (fixture, algorithm, width);
                const auto* processor = fixture.pax (algorithm);
                expect (processor != nullptr);
                if (! processor) continue;
                expectEquals ((int) processor->inputAudioBuffers.size(), 1);
                expectEquals ((int) processor->outputAudioBuffers.size(), 1);
                for (int block = 0; block < 3; ++block)
                {
                    fixture.process();
                    expectSignal (fixture, monitor, width, block, 0.f, level ? 0.5f : 2.f);
                }
            }
    }

    void testIndependentBranches()
    {
        beginTest ("Independent Level and Amp branches keep two- and eight-channel layouts");
        PaxRoutingFixture fixture;
        if (! load (fixture, { "LevelPax", "AmpPax" })) return;
        const auto narrowMonitor = fixture.addMonitor();
        const auto wide = fixture.addFile (8, 0.2f);
        const auto narrow = fixture.addFile (2, 0.4f);
        const auto wideMonitor = fixture.addMonitor();
        const auto level = fixture.addPax ("LevelPax", { juce::var (-6.020599913) });
        const auto amp = fixture.addPax ("AmpPax", { juce::var (6.020599913) });
        expect (fixture.connect (narrow, level));
        expect (fixture.connect (level, narrowMonitor));
        expect (fixture.connect (wide, amp));
        expect (fixture.connect (amp, wideMonitor));
        fixture.rebuild();
        expectWidth (fixture, level, 2);
        expectWidth (fixture, amp, 8);
        fixture.process();
        expectSignal (fixture, narrowMonitor, 2, 0, 0.4f, 0.5f);
        expectSignal (fixture, wideMonitor, 8, 0, 0.2f, 2.f);
    }

    void testDynamicOutputPorts()
    {
        beginTest ("Spectrumyser restores five declared output ports and routes its last eight-channel bus");
        PaxRoutingFixture fixture;
        if (! load (fixture, { "SpectrumyserPax" })) return;
        const auto source = fixture.addFile (8);
        const auto spectrum = fixture.addPax ("SpectrumyserPax");
        const auto monitor = fixture.addMonitor();
        expect (fixture.connect (source, spectrum));
        fixture.rebuild();
        auto* processor = fixture.pax (spectrum);
        expect (processor != nullptr);
        if (! processor) return;
        expectEquals (processor->audioOutputCount, 3);
        expectEquals ((int) processor->outputAudioBuffers.size(), 3);

        processor->setParameter (0, 5.f);
        processor->setParameter (9, 100.f);
        processor->setParameter (10, 500.f);
        expectEquals (processor->audioOutputCount, 5);
        juce::Array<juce::var> parameters;
        for (int index = 0; index < processor->getParameterCount(); ++index)
            parameters.add ((double) processor->getParameter (index));
        fixture.model.setNodeSettings (spectrum, juce::JSON::toString (parameters));
        fixture.model.updateNodeAudioOutputCount (spectrum, processor->audioOutputCount);
        expect (fixture.connect (spectrum, monitor, 4));
        int declaredOutputs = 0;
        for (const auto& port : fixture.model.findNode (spectrum)->ports)
            if (port.type == PortType::Audio && port.direction == PortDirection::Output) ++declaredOutputs;
        expectEquals (declaredOutputs, 5);
        fixture.rebuild(); // Restore saved parameter array before allocating ports.
        processor = fixture.pax (spectrum);
        expect (processor != nullptr);
        if (! processor) return;
        expectEquals (processor->audioOutputCount, 5);
        expectEquals ((int) processor->outputAudioBuffers.size(), 5);
        expectEquals (processor->getParameter (0), 5.f);
        expectWidth (fixture, spectrum, 8, 4);
        if (processor->outputAudioBuffers.size() != 5) return;
        for (const auto& buffer : processor->outputAudioBuffers)
            expectEquals (buffer.getNumChannels(), 8);

        for (int block = 0; block < 3; ++block)
        {
            fixture.process();
            const auto traces = fixture.snapshot (monitor);
            expectEquals ((int) traces.size(), 8);
            if (traces.size() != 8) return;
            for (int channel = 0; channel < 8; ++channel)
            {
                bool hasSignal = false;
                for (int index = 0; index < PaxRoutingFixture::frames; ++index)
                {
                    const auto value = traces[(size_t) channel][(size_t) index];
                    expect (std::isfinite (value));
                    hasSignal = hasSignal || std::abs (value) > 1.0e-12f;
                    expectWithinAbsoluteError (value, processor->outputAudioBuffers[4].getSample (channel, index), 0.000001f);
                }
                expect (hasSignal, "The final output must process every routed channel");
            }
        }
    }
   #endif
};

static PaxRoutingTests paxRoutingTests;
