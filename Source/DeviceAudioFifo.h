#pragma once
#include "AudioSimConfig.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

// Same-rate, single-producer/single-consumer queue. Only 44.1/48 kHz
// hardware may attach. The queue bridges callback sizes without resampling.
// Storage is allocated before callbacks start, independent of DSP frame size.
class DeviceAudioFifo
{
    static constexpr int chunkSize = 64, capacity = 32768;
    struct Ring
    {
        juce::AbstractFifo fifo { capacity };
        juce::AudioBuffer<float> data { AudioSim::maxAudioChannels, capacity };
        std::atomic<int> channels { 0 }, lastWrite { 0 };
        Ring() { data.clear(); }
        void write (const float* const* source, int count, int available,
                    const std::vector<int>* selection = nullptr)
        {
            const int slots = juce::jlimit (0, AudioSim::maxAudioChannels,
                                selection ? (int) selection->size() : available);
            int a, na, b, nb;
            fifo.prepareToWrite (count, a, na, b, nb);
            for (int ch = 0; ch < slots; ++ch)
            {
                const int input = selection ? (*selection)[(size_t) ch] : ch;
                const float* in = input >= 0 && input < available ? source[input] : nullptr;
                auto* out = data.getWritePointer (ch);
                if (in != nullptr)
                {
                    std::memcpy (out + a, in, (size_t) na * sizeof (float));
                    std::memcpy (out + b, in + na, (size_t) nb * sizeof (float));
                }
                else
                {
                    juce::FloatVectorOperations::clear (out + a, na);
                    juce::FloatVectorOperations::clear (out + b, nb);
                }
            }
            channels.store (slots, std::memory_order_relaxed);
            lastWrite.store (count, std::memory_order_relaxed);
            fifo.finishedWrite (na + nb);
        }
        int read (juce::AudioBuffer<float>& output, int start, int count)
        {
            output.clear (start, count);
            int a, na, b, nb;
            fifo.prepareToRead (count, a, na, b, nb);
            for (int ch = 0; ch < std::min (output.getNumChannels(), channels.load()); ++ch)
            {
                output.copyFrom (ch, start, data, ch, a, na);
                output.copyFrom (ch, start + na, data, ch, b, nb);
            }
            fifo.finishedRead (na + nb);
            return na + nb;
        }
        bool trim (int requested)
        {
            const int target = 2 * (requested + std::max (1, lastWrite.load()));
            const int ready = fifo.getNumReady();
            if (ready <= target + std::max (requested, lastWrite.load())) return false;
            int a, na, b, nb;
            fifo.prepareToRead (ready - target, a, na, b, nb);
            fifo.finishedRead (na + nb);
            return true;
        }
    };
public:
    void prepareFor (int, int rate, int block)
    {
        configuredRate = rate;
        configuredBlock = block;
    }
    void write (const juce::AudioBuffer<float>& input, int count, const std::vector<int>&)
    {
        queue.write (input.getArrayOfReadPointers(), count, input.getNumChannels());
    }
    void read (float* const* output, int channels, int count, const std::vector<int>& selection)
    {
        for (int ch = 0; ch < channels; ++ch)
            if (output[ch]) juce::FloatVectorOperations::clear (output[ch], count);
        if (queue.trim (count)) ++trimCount;
        for (int start = 0; start < count; start += chunkSize)
        {
            const int n = std::min (chunkSize, count - start);
            if (queue.read (scratch, 0, n) < n) ++starvedCount;
            for (int slot = 0; slot < std::min ((int) selection.size(), AudioSim::maxAudioChannels); ++slot)
            {
                const int physical = selection[(size_t) slot];
                if (physical >= 0 && physical < channels && output[physical])
                    std::memcpy (output[physical] + start, scratch.getReadPointer (slot), (size_t) n * sizeof (float));
            }
        }
    }
    void write (const float* const* input, int channels, int count, const std::vector<int>& selection)
    {
        queue.write (input, count, channels, &selection);
    }
    bool read (juce::AudioBuffer<float>& output, int count)
    {
        if (queue.trim (count)) ++trimCount;
        if (queue.fifo.getNumReady() < count)
        {
            output.clear(); ++starvedCount; return false;
        }
        return queue.read (output, 0, count) == count;
    }
    int configuredRate = 0, configuredBlock = 0;
    std::atomic<int> starvedCount { 0 }, trimCount { 0 };
private:
    Ring queue;
    juce::AudioBuffer<float> scratch { AudioSim::maxAudioChannels, chunkSize };
};

// The message thread owns storage; graph/device threads only read a stable
// published pointer. Closing hardware keeps the queue alive until its graph
// nodes retire, so no callback can observe freed storage. A freshly rebuilt
// node adopts the existing queue without first allocating a replacement.
class SharedDeviceAudioFifo
{
public:
    DeviceAudioFifo* get() const { return published.load (std::memory_order_acquire); }
    DeviceAudioFifo* operator->() const { return get(); }
    void prepareFor (int channels, int rate, int block)
    {
        if (! owner) owner = std::make_shared<DeviceAudioFifo>();
        owner->prepareFor (channels, rate, block);
        published.store (owner.get(), std::memory_order_release);
    }
    // Message thread only, before the destination joins the live graph.
    void shareFrom (const SharedDeviceAudioFifo& source)
    {
        jassert (get() == nullptr);
        owner = source.owner;
        published.store (owner.get(), std::memory_order_release);
    }
private:
    std::shared_ptr<DeviceAudioFifo> owner;
    std::atomic<DeviceAudioFifo*> published { nullptr };
};
