#include "AudioModules.h"
#include "ProcessingGraph.h"
#include <unordered_map>

namespace
{
struct ModuleGraph
{
    GraphModel model;
    std::unordered_map<juce::String, std::unique_ptr<AudioPlayerState>> sources;
    ProcessingGraph graph;
    static constexpr int frames = 24;
    static constexpr double rate = 1500;
    juce::String source (int channels, float offset = 0)
    {
        const auto id = model.addNode (26, 0, 0).id;
        auto state = std::make_unique<AudioPlayerState>();
        state->mode.store (AudioPlayerState::Mode::File); state->playing.store (true);
        state->looping.store (true); state->level.store (1);
        state->fileSampleRate = state->loadedSampleRate = rate;
        state->fileBuffer.setSize (channels, 5);
        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::fill (state->fileBuffer.getWritePointer (ch), offset + (ch + 1) * 0.1f, 5);
        sources[id] = std::move (state);
        return id;
    }
    juce::String module (int type, const char* settings = "{}")
    {
        const auto id = model.addNode (type, 100, 0).id;
        model.setNodeSettings (id, settings);
        return id;
    }
    juce::String port (const juce::String& id, bool output, int index)
    {
        for (const auto& p : model.findNode (id)->ports)
            if (p.type == PortType::Audio && (p.direction == PortDirection::Output) == output && index-- == 0) return p.id;
        return {};
    }
    bool connect (const juce::String& from, const juce::String& to, int out = 0, int in = 0)
    {
        return model.addConnection (from, port (from, true, out), to, port (to, false, in)) != nullptr;
    }
    void rebuild()
    {
        graph.isStandaloneMode = true; graph.graphModel = &model;
        graph.rebuild (model, nullptr, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
            [this](const juce::String& id) { return sources.at (id).get(); });
        graph.prepare (rate, frames, 1);
    }
    void process()
    {
        juce::AudioBuffer<float> host (1, frames); host.clear(); juce::MidiBuffer midi;
        graph.process (host, midi);
    }
};
}

class AudioModuleTests final : public juce::UnitTest
{
public:
    AudioModuleTests() : UnitTest ("RMS, Mux, Demux and EQ", "AudioRouting") {}
    void runTest() override
    {
        beginTest ("Mux concatenates unequal vectors; Demux restores every channel and port width");
        ModuleGraph f;
        const auto a = f.source (2), b = f.source (3, 0.2f);
        const auto mux = f.module (31), demux = f.module (32, R"({"widths":[2,3]})");
        const auto rms = f.module (30, R"({"windowMs":32})");
        expect (f.connect (a, mux, 0, 0)); expect (f.connect (b, mux, 0, 1));
        expect (f.connect (mux, demux)); expect (f.connect (mux, rms));
        f.rebuild(); f.process();
        auto* combined = f.graph.findNode (mux); auto* split = f.graph.findNode (demux);
        expectEquals (combined->getOutputAudioChannels(), 5);
        expectEquals (split->getOutputAudioChannels (0), 2); expectEquals (split->getOutputAudioChannels (1), 3);
        for (int ch = 0; ch < 5; ++ch)
            for (int sample = 0; sample < ModuleGraph::frames; ++sample)
            {
                const float value = (ch + 1) * 0.1f;
                expectWithinAbsoluteError (combined->outputAudio.getSample (ch, sample), value, 0.000001f);
                expectWithinAbsoluteError (split->outputAudioBuffers[ch < 2 ? 0 : 1].getSample (ch < 2 ? ch : ch - 2, sample), value, 0.000001f);
            }
        expect (! (bool) f.graph.findNode (rms)->getAudioModuleStatus()["rmsReady"]);
        f.process();
        const auto levels = f.graph.findNode (rms)->getAudioModuleStatus();
        expect ((bool) levels["rmsReady"]); expectEquals ((int) levels["windowSamples"], 48);
        for (int ch = 0; ch < 5; ++ch)
        {
            expectWithinAbsoluteError ((float) (double) levels["rms"][ch], (ch + 1) * 0.1f, 0.000001f);
            expectEquals ((int) f.graph.findNode (rms)->outputValues[(size_t) ch].key, ch + 1);
        }

        beginTest ("Mux handles fan-in on one port, gaps and 64-channel overflow explicitly");
        ModuleGraph mix;
        const auto one = mix.source (1), two = mix.source (1, 0.2f), three = mix.source (2, 0.5f), m = mix.module (31, R"({"portCount":3})");
        expect (mix.connect (one, m)); expect (mix.connect (two, m)); expect (mix.connect (three, m, 0, 2));
        mix.rebuild(); mix.process();
        expectEquals (mix.graph.findNode (m)->getOutputAudioChannels(), 3);
        expectWithinAbsoluteError (mix.graph.findNode (m)->outputAudio.getSample (0, 0), 0.4f, 0.000001f);
        expectWithinAbsoluteError (mix.graph.findNode (m)->outputAudio.getSample (1, 0), 0.6f, 0.000001f);
        ModuleGraph wide;
        const auto w1 = wide.source (64), w2 = wide.source (1), wm = wide.module (31);
        expect (wide.connect (w1, wm)); expect (wide.connect (w2, wm, 0, 1));
        wide.rebuild(); wide.process();
        expectEquals (wide.graph.findNode (wm)->getOutputAudioChannels(), 0);
        expect (! (bool) wide.graph.findNode (wm)->getAudioModuleStatus()["valid"]);

        beginTest ("Port edits remove only deleted-port wires and undo restores settings and connections");
        const auto connections = mix.model.getConnections().size();
        mix.model.pushSnapshot(); mix.model.setNodeSettings (m, R"({"portCount":2})");
        expectEquals ((int) mix.model.getConnections().size(), (int) connections - 1);
        expect (mix.model.undo());
        expectEquals ((int) mix.model.getConnections().size(), (int) connections);
        expectEquals ((int) mix.model.findNode (m)->ports.size(), 4);
        expect (mix.model.redo());
        expectEquals ((int) mix.model.findNode (m)->ports.size(), 3);

        beginTest ("Demux rejects mismatched partitions and resumes after reconfiguration");
        f.model.setNodeSettings (demux, R"({"widths":[1,1]})"); f.rebuild(); f.process();
        expectEquals (f.graph.findNode (demux)->getOutputAudioChannels(), 0);
        expect (! (bool) f.graph.findNode (demux)->getAudioModuleStatus()["valid"]);
        f.model.setNodeSettings (demux, R"({"widths":[1,1,3]})"); f.rebuild(); f.process();
        expectEquals (f.graph.findNode (demux)->getOutputAudioChannels (2), 3);
        expectWithinAbsoluteError (f.graph.findNode (demux)->outputAudioBuffers[2].getSample (2, 0), 0.5f, 0.000001f);

        testRms(); testEq();
    }
private:
    void testRms()
    {
        beginTest ("RMS measures an exact full-scale sine window across non-dividing frame boundaries");
        RmsNode node ("rms"); node.configure (juce::JSON::parse (R"({"windowMs":100})"));
        node.inputPortAudioChannels = { 2 }; node.setAudioChannelCount (2); node.prepare (1000, 24);
        for (int block = 0; block < 5; ++block)
        {
            node.resetBuffers (24);
            for (int i = 0; i < 24; ++i)
            {
                node.inputAudio.setSample (0, i, (float) std::sin (juce::MathConstants<double>::twoPi * (block * 24 + i) / 10));
                node.inputAudio.setSample (1, i, 0.00001f);
            }
            node.process (24);
        }
        const auto values = node.getAudioModuleStatus()["rms"];
        expectWithinAbsoluteError ((double) values[0], std::sqrt (0.5), 0.000001);
        expectWithinAbsoluteError ((double) values[1], 0.00001, 0.000000001);
        expectWithinAbsoluteError (20 * std::log10 ((double) values[0]), -3.01029996, 0.00001);
    }
    void testEq()
    {
        beginTest ("EQ bell gain is correct, channels are isolated, bypass is exact");
        EqualizerNode node ("eq");
        node.configure (juce::JSON::parse (R"({"bands":[{"enabled":false},{"enabled":false},{"type":"peak","frequency":1000,"gainDb":6,"q":1},{"enabled":false},{"enabled":false}]})"));
        node.inputPortAudioChannels = { 8 }; node.setAudioChannelCount (8); node.prepare (48000, 24);
        double inputPower = 0, outputPower = 0;
        for (int block = 0; block < 300; ++block)
        {
            node.resetBuffers (24);
            for (int i = 0; i < 24; ++i) node.inputAudio.setSample (0, i, (float) (0.2 * std::sin (juce::MathConstants<double>::twoPi * (block * 24 + i) / 48)));
            node.process (24);
            if (block > 200)
                for (int i = 0; i < 24; ++i)
                {
                    inputPower += std::pow (node.inputAudio.getSample (0, i), 2);
                    outputPower += std::pow (node.outputAudio.getSample (0, i), 2);
                    expectEquals (node.outputAudio.getSample (7, i), 0.f);
                }
        }
        expectWithinAbsoluteError (10 * std::log10 (outputPower / inputPower), 6.0, 0.001);
        node.disabled = true; node.process (24);
        for (int i = 0; i < 24; ++i) expectEquals (node.outputAudio.getSample (0, i), node.inputAudio.getSample (0, i));

        beginTest ("All EQ filter types remain finite at supported rate and frequency limits");
        for (double rate : { 1000., 1500., 48000., 192000. })
            for (const auto* type : { "peak", "lowShelf", "highShelf", "lowPass", "highPass" })
            {
                auto config = juce::JSON::parse (R"({"bands":[{"frequency":95000,"gainDb":24,"q":20},{"enabled":false},{"enabled":false},{"enabled":false},{"enabled":false}]})");
                config["bands"][0].getDynamicObject()->setProperty ("type", type);
                node.disabled = false; node.configure (config); node.prepare (rate, 24);
                node.resetBuffers (24); node.inputAudio.setSample (0, 0, 1); node.process (24);
                for (int i = 0; i < 24; ++i) expect (std::isfinite (node.outputAudio.getSample (0, i)));
                expectEquals ((int) node.getAudioModuleStatus()["response"].getArray()->size(), 160);
            }
    }
};
static AudioModuleTests audioModuleTests;
