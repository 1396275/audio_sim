#pragma once
#include "AudioSimConfig.h"
#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>
#include <utility>
#include <type_traits>

namespace AudioSim
{
struct AudioChannelRoute
{
    std::size_t source = 0, destination = 0;
    int sourcePort = 0;
    int destinationPort = 0;
};

struct NodeAudioChannelLayout
{
    int inputChannels = 0;
    int bufferChannels = 1;
    std::vector<int> outputChannels;
    std::vector<int> inputPortChannels;

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
            std::vector<int> inputPorts;
            for (const auto& route : routes)
            {
                if (route.destination != index || route.source >= nodeCount || route.sourcePort < 0
                    || route.destinationPort < 0 || route.destinationPort >= maxAudioChannels) continue;
                const auto& outputs = result.nodes[route.source].outputChannels;
                if ((std::size_t) route.sourcePort < outputs.size())
                {
                    const auto port = (size_t) route.destinationPort;
                    if (inputPorts.size() <= port) inputPorts.resize (port + 1);
                    inputPorts[port] = std::max (inputPorts[port], outputs[(size_t) route.sourcePort]);
                    inputChannels = std::max (inputChannels, inputPorts[port]);
                }
            }
            auto layout = [&] {
                if constexpr (std::is_invocable_v<Resolve, size_t, int, std::span<const int>>)
                    return resolve (index, inputChannels, std::span<const int> (inputPorts));
                else return resolve (index, inputChannels);
            }();
            layout.inputChannels = inputChannels;
            layout.inputPortChannels = std::move (inputPorts);
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
