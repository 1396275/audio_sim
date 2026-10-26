#pragma once
#include "AudioSimConfig.h"
#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>
#include <utility>

namespace AudioSim
{
struct AudioChannelRoute
{
    std::size_t source = 0, destination = 0;
    int sourcePort = 0;
};

struct NodeAudioChannelLayout
{
    int inputChannels = 0;
    int bufferChannels = 1;
    std::vector<int> outputChannels;

    bool operator== (const NodeAudioChannelLayout&) const = default;
};

struct AudioChannelLayoutResult
{
    std::vector<NodeAudioChannelLayout> nodes;
    bool converged = true;
};

// Runs only while preparing a graph. Channels on a shared destination port
// mix by index, so the required width is the maximum upstream width, rather
// than the sum. Port counts and channel widths are independent. The resolver
// describes each module's fixed/follow-input policy and per-output widths.
// A topological node order normally settles in two passes; bounded repeated
// passes also handle a cycle fed by a fixed-width source deterministically.
template <typename Resolve>
AudioChannelLayoutResult resolveAudioChannelLayouts (std::size_t nodeCount,
                                                      std::span<const AudioChannelRoute> routes,
                                                      Resolve&& resolve)
{
    AudioChannelLayoutResult result;
    result.nodes.resize (nodeCount);
    const std::size_t maximumPasses = std::max<std::size_t> (1, nodeCount * (maxAudioChannels + 1));
    for (std::size_t pass = 0; pass < maximumPasses; ++pass)
    {
        bool changed = false;
        for (std::size_t index = 0; index < nodeCount; ++index)
        {
            int inputChannels = 0;
            for (const auto& route : routes)
            {
                if (route.destination != index || route.source >= nodeCount || route.sourcePort < 0) continue;
                const auto& outputs = result.nodes[route.source].outputChannels;
                if ((std::size_t) route.sourcePort < outputs.size())
                    inputChannels = std::max (inputChannels, outputs[(std::size_t) route.sourcePort]);
            }
            auto layout = resolve (index, inputChannels);
            layout.inputChannels = inputChannels;
            layout.bufferChannels = std::clamp (layout.bufferChannels, 1, maxAudioChannels);
            for (auto& width : layout.outputChannels)
                width = std::clamp (width, 0, layout.bufferChannels);
            if (layout != result.nodes[index])
            {
                result.nodes[index] = std::move (layout);
                changed = true;
            }
        }
        if (! changed) return result;
    }
    result.converged = false;
    return result;
}
}
