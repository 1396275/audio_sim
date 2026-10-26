#pragma once
#include "NodeProcessor.h"
#include <atomic>
#include <array>
#include <memory>
#include <vector>

// Shared across graph rebuilds. Atomic samples keep asynchronous snapshots
// well-defined even while the producer wraps. The message thread downsamples
// before JSON encoding; all signal channels are captured independently.
struct AudioMonitorBuffer
{
    static constexpr int kRingSize = 65536;
    // Called while preparing a graph, before it reaches the audio thread.
    // Each channel is published once and retained for this buffer's lifetime:
    // a live old graph can safely keep pushing while a new graph grows it.
    // Repeated prepare calls (including the swap-time call) allocate nothing.
    void prepare (int channels)
    {
        channels = juce::jlimit (0, AudioSim::maxAudioChannels, channels);
        if (channels > allocatedChannels.load (std::memory_order_acquire))
        {
            const juce::SpinLock::ScopedLockType lock (allocationLock);
            for (int ch = allocatedChannels.load(); ch < channels; ++ch)
            {
                // C++20 atomic<float>'s value initialisation supplies silence
                // once; no second full-ring clear is needed.
                storage[(size_t) ch] = std::make_unique<std::atomic<float>[]> (kRingSize);
                data[(size_t) ch].store (storage[(size_t) ch].get(), std::memory_order_release);
                allocatedChannels.store (ch + 1, std::memory_order_release);
            }
        }
        preparedChannels.store (channels, std::memory_order_release);
    }
    int getAllocatedChannelCount() const { return allocatedChannels.load (std::memory_order_acquire); }
    void push (const juce::AudioBuffer<float>& input, int channels, int count, double sampleRate = 0.0)
    {
        revision.fetch_add (1, std::memory_order_acq_rel);
        channels = juce::jlimit (0, std::min (input.getNumChannels(), preparedChannels.load (std::memory_order_acquire)), channels);
        count = juce::jlimit (0, input.getNumSamples(), count);
        if (channels != activeChannels.load (std::memory_order_relaxed)
            || sampleRate != captureSampleRate.load (std::memory_order_relaxed))
        {
            validSamples.store (0); writePos.store (0); activeChannels.store (channels);
            captureSampleRate.store (sampleRate, std::memory_order_relaxed);
        }
        const int wp = writePos.load (std::memory_order_relaxed);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dest = data[(size_t) ch].load (std::memory_order_acquire);
            const auto* source = input.getReadPointer (ch);
            for (int i = 0; i < count; ++i) dest[(wp + i) % kRingSize].store (source[i], std::memory_order_relaxed);
        }
        validSamples.store (std::min (kRingSize, validSamples.load() + count), std::memory_order_relaxed);
        writePos.store ((wp + count) % kRingSize, std::memory_order_release);
        revision.fetch_add (1, std::memory_order_release);
    }
    // Full-resolution selected-channel history for PSD analysis. Reject a
    // snapshot if the producer changed it while copying, including ring wrap.
    // Only complete, actually captured samples are returned (no zero padding).
    bool snapshotChannel (int channel, int maximumSamples, std::vector<float>& out,
                          int& channels, double& sampleRate, uint64_t& version) const
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto before = revision.load (std::memory_order_acquire);
            if ((before & 1) != 0) continue;
            channels = activeChannels.load();
            sampleRate = captureSampleRate.load();
            const int count = juce::jlimit (0, kRingSize, std::min (maximumSamples, validSamples.load()));
            const int wp = writePos.load();
            out.clear();
            if (channel >= 0 && channel < channels)
            {
                out.resize ((size_t) count);
                const auto* source = data[(size_t) channel].load (std::memory_order_acquire);
                for (int i = 0; i < count; ++i)
                    out[(size_t) i] = source[(wp - count + i + kRingSize) % kRingSize].load (std::memory_order_relaxed);
            }
            std::atomic_thread_fence (std::memory_order_acquire);
            if (before == revision.load (std::memory_order_acquire)) { version = before; return true; }
        }
        out.clear();
        return false;
    }
    void snapshotChannels (int count, int points, std::vector<std::vector<float>>& out,
                           bool preservePeaks = false, int triggerMode = 0,
                           float triggerLevel = 0.f, int triggerChannel = 0) const
    {
        const int wp = writePos.load (std::memory_order_acquire);
        const int channels = activeChannels.load(), valid = validSamples.load();
        count = juce::jlimit (1, kRingSize, count);
        points = juce::jlimit (1, count, points);
        const bool envelope = preservePeaks && count > points;
        out.assign ((size_t) channels, std::vector<float> ((size_t) points * (envelope ? 2 : 1), 0.f));
        if (channels == 0) return;
        int windowShift = 0;
        // Find a crossing in the raw history BEFORE downsampling. Keep a
        // complete following window, with the same alignment on every channel.
        if (triggerMode != 0)
        {
            triggerChannel = juce::jlimit (0, channels - 1, triggerChannel);
            const auto* triggerData = data[(size_t) triggerChannel].load (std::memory_order_acquire);
            const auto atOffset = [&](int offset) {
                return triggerData[(wp - offset - 1 + kRingSize) % kRingSize].load (std::memory_order_relaxed);
            };
            const int maximumShift = std::min ({ count, valid - count - 1, kRingSize - count - 1 });
            for (int shift = 0; shift <= maximumShift; ++shift)
            {
                const float before = atOffset (count + shift), after = atOffset (count + shift - 1);
                if ((triggerMode == 1 && before < triggerLevel && after >= triggerLevel)
                    || (triggerMode == 2 && before > triggerLevel && after <= triggerLevel))
                { windowShift = shift; break; }
            }
        }
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto* channelData = data[(size_t) ch].load (std::memory_order_acquire);
            const auto sampleAt = [&](int position) {
                const int offset = count - position - 1 + windowShift;
                return offset < valid
                    ? channelData[(wp - offset - 1 + kRingSize) % kRingSize].load (std::memory_order_relaxed) : 0.f;
            };
            for (int i = 0; i < points; ++i)
            {
                const int begin = (int) ((int64_t) i * count / points);
                if (! envelope) { out[(size_t) ch][(size_t) i] = sampleAt (begin); continue; }
                const int end = (int) ((int64_t) (i + 1) * count / points);
                float lo = sampleAt (begin), hi = lo;
                for (int pos = begin + 1; pos < end; ++pos)
                {
                    const float value = sampleAt (pos);
                    lo = std::min (lo, value); hi = std::max (hi, value);
                }
                out[(size_t) ch][(size_t) i * 2] = lo;
                out[(size_t) ch][(size_t) i * 2 + 1] = hi;
            }
        }
    }
    // Retained for the lightweight edge activity calculation.
    void snapshot (int count, std::vector<float>& left, std::vector<float>& right) const
    {
        count = juce::jlimit (1, kRingSize, count);
        left.assign ((size_t) count, 0.f); right.assign ((size_t) count, 0.f);
        const int wp = writePos.load (std::memory_order_acquire);
        const int channels = activeChannels.load(), valid = validSamples.load();
        const auto* leftData = channels > 0 ? data[0].load (std::memory_order_acquire) : nullptr;
        const auto* rightData = channels > 0 ? data[channels > 1 ? 1 : 0].load (std::memory_order_acquire) : nullptr;
        for (int i = 0; i < count; ++i)
        {
            const int offset = count - i - 1;
            if (offset >= valid || channels == 0) continue;
            const size_t index = (size_t) ((wp - offset - 1 + kRingSize) % kRingSize);
            left[(size_t) i] = leftData[index].load (std::memory_order_relaxed);
            right[(size_t) i] = rightData[index].load (std::memory_order_relaxed);
        }
    }
private:
    std::array<std::unique_ptr<std::atomic<float>[]>, AudioSim::maxAudioChannels> storage;
    std::array<std::atomic<std::atomic<float>*>, AudioSim::maxAudioChannels> data {};
    juce::SpinLock allocationLock;
    std::atomic<int> preparedChannels { 0 }, allocatedChannels { 0 };
    std::atomic<int> writePos { 0 }, activeChannels { 0 }, validSamples { 0 };
    std::atomic<double> captureSampleRate { 0.0 };
    std::atomic<uint64_t> revision { 0 };
};

class AudioMonitorNode : public NodeProcessor
{
public:
    AudioMonitorNode (const juce::String& nodeId, AudioMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Audio), buffer (sharedBuffer) {}
    void prepare (double sampleRate, int maxBlockSize) override
    {
        NodeProcessor::prepare (sampleRate, maxBlockSize);
        captureRate = sampleRate;
        if (buffer) buffer->prepare (connectedInputAudioChannels);
    }
    int getOutputAudioChannels (int = 0) const override { return 0; }
    void process (int numSamples) override
    {
        // An observer is a branch from the signal path, with no output port.
        outputAudio.clear();
        if (buffer) buffer->push (inputAudio, connectedInputAudioChannels, numSamples, captureRate);
    }
    bool passesThroughWhenDisabled() const override { return true; }
private:
    AudioMonitorBuffer* buffer = nullptr;
    double captureRate = 0.0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioMonitorNode)
};
