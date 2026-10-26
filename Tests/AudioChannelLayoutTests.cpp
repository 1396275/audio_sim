#include <juce_core/juce_core.h>
#include "AudioChannelLayout.h"

class AudioChannelLayoutTests final : public juce::UnitTest
{
public:
    AudioChannelLayoutTests() : UnitTest ("Audio channel layout inference", "AudioSim") {}

    void runTest() override
    {
        using namespace AudioSim;
        const auto follows = [](int input) {
            return NodeAudioChannelLayout { input, std::max (1, input), { input } };
        };

        beginTest ("Unrelated source chains keep independent widths");
        const std::vector<AudioChannelRoute> separate { { 0, 2, 0 }, { 1, 3, 0 } };
        auto result = resolveAudioChannelLayouts (4, separate, [&](size_t node, int input) {
            if (node < 2) { const int width = node == 0 ? 1 : 8; return NodeAudioChannelLayout { 0, width, { width } }; }
            return follows (input);
        });
        expect (result.converged);
        expectEquals (result.nodes[2].bufferChannels, 1);
        expectEquals (result.nodes[3].bufferChannels, 8);

        beginTest ("Fan-in mixes matching indices and keeps the widest source");
        const std::vector<AudioChannelRoute> mixed { { 0, 2, 0 }, { 1, 2, 0 }, { 2, 3, 0 } };
        result = resolveAudioChannelLayouts (4, mixed, [&](size_t node, int input) {
            if (node < 2) { const int width = node == 0 ? 3 : 8; return NodeAudioChannelLayout { 0, width, { width } }; }
            return follows (input);
        });
        expectEquals (result.nodes[2].inputChannels, 8);
        expectEquals (result.nodes[3].outputChannels[0], 8);

        beginTest ("A legacy stereo processor limits only its own branch");
        const std::vector<AudioChannelRoute> branches { { 0, 1, 0 }, { 1, 2, 0 }, { 0, 3, 0 } };
        result = resolveAudioChannelLayouts (4, branches, [&](size_t node, int input) {
            if (node == 0) return NodeAudioChannelLayout { 0, 64, { 64 } };
            if (node == 1) return NodeAudioChannelLayout { input, 2, { std::min (2, input) } };
            return follows (input);
        });
        expectEquals (result.nodes[2].bufferChannels, 2);
        expectEquals (result.nodes[3].bufferChannels, 64);

        beginTest ("Multiple output ports carry their own logical width");
        const std::vector<AudioChannelRoute> ports { { 0, 1, 0 }, { 0, 2, 1 } };
        result = resolveAudioChannelLayouts (3, ports, [&](size_t node, int input) {
            if (node == 0) return NodeAudioChannelLayout { 0, 8, { 1, 8 } };
            return follows (input);
        });
        expectEquals (result.nodes[1].inputChannels, 1);
        expectEquals (result.nodes[2].inputChannels, 8);

        beginTest ("Missing sources remain logically empty with a safe scratch capacity");
        const std::vector<AudioChannelRoute> absent { { 0, 1, 0 }, { 0, 2, 7 } };
        result = resolveAudioChannelLayouts (3, absent, [&](size_t node, int input) {
            return node == 0 ? NodeAudioChannelLayout { 0, 1, { 0 } } : follows (input);
        });
        expectEquals (result.nodes[1].outputChannels[0], 0);
        expectEquals (result.nodes[1].bufferChannels, 1);
        expectEquals (result.nodes[2].inputChannels, 0);

        beginTest ("A source-fed cycle reaches a bounded fixed width");
        const std::vector<AudioChannelRoute> cyclic { { 2, 0, 0 }, { 0, 1, 0 }, { 1, 0, 0 } };
        result = resolveAudioChannelLayouts (3, cyclic, [&](size_t node, int input) {
            return node == 2 ? NodeAudioChannelLayout { 0, 6, { 6 } } : follows (input);
        });
        expect (result.converged);
        expectEquals (result.nodes[0].outputChannels[0], 6);
        expectEquals (result.nodes[1].outputChannels[0], 6);

        beginTest ("Nonconvergent policies terminate rather than blocking preparation");
        const std::vector<AudioChannelRoute> self { { 0, 0, 0 } };
        result = resolveAudioChannelLayouts (1, self, [](size_t, int input) {
            return NodeAudioChannelLayout { input, 2, { input == 1 ? 2 : 1 } };
        });
        expect (! result.converged);
    }
};

static AudioChannelLayoutTests audioChannelLayoutTests;
