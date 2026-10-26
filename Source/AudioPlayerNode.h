#pragma once
#include "NodeProcessor.h"
#include <atomic>
#include <array>
#include <vector>
#include <cmath>

// Shared Source state survives graph edits. File changes and resampling run on
// the message thread while processing is suspended; process() only reads PCM.
struct AudioPlayerState
{
    enum class Mode { File, Sine, Noise, Square, Triangle, Saw };
    enum class Noise { White, Pink };
    static Mode modeFromString (const juce::String& value)
    {
        if (value == "sine") return Mode::Sine;
        if (value == "noise") return Mode::Noise;
        if (value == "square") return Mode::Square;
        if (value == "triangle") return Mode::Triangle;
        if (value == "saw") return Mode::Saw;
        return Mode::File;
    }
    juce::AudioBuffer<float> fileBuffer;
    juce::String fileName, loadedFilePath;
    double fileSampleRate = 0.0, loadedSampleRate = 0.0;
    std::vector<std::pair<float, float>> waveformPeaks;
    std::atomic<int64_t> playbackPosition { 0 }, seekRequest { -1 };
    std::atomic<bool> playing { false }, looping { false }, needsUIPush { false };
    std::atomic<Mode> mode { Mode::Sine };
    std::atomic<int> outputChannels { 1 };
    std::atomic<bool> layoutDirty { false }; // manual load awaits its persisted file path
    std::atomic<float> sineFrequency { 440.0f }, level { 0.1f };
    std::atomic<Noise> noiseType { Noise::White };
    double sinePhase = 0.0;
    struct PinkState { float b0 = 0, b1 = 0, b2 = 0; };
    std::array<PinkState, AudioSim::maxAudioChannels> pink {};
    juce::Random noiseRandom;
    bool loadFile (const juce::File& file, double targetSampleRate);
    void computeWaveformPeaks (int numBuckets = 600);
};

// Node type 26 is retained so existing Audio Player projects open as Source.
class AudioPlayerNode : public NodeProcessor
{
public:
    AudioPlayerNode (const juce::String& nodeId, AudioPlayerState* sharedState)
        : NodeProcessor (nodeId, Type::Audio), state (sharedState) {}
    double getPlaybackFraction() const
    {
        if (state == nullptr || state->fileBuffer.getNumSamples() == 0) return 0.0;
        return juce::jlimit (0.0, 1.0, (double) state->playbackPosition.load()
                                      / state->fileBuffer.getNumSamples());
    }
    int resolveAudioChannelCount (int) const override
    {
        if (! state) return 0;
        return state->mode.load() == AudioPlayerState::Mode::File
            ? std::min (AudioSim::maxAudioChannels, state->fileBuffer.getNumChannels())
            : juce::jlimit (1, AudioSim::maxAudioChannels, state->outputChannels.load());
    }
    int getOutputAudioChannels (int = 0) const override { return resolveAudioChannelCount (0); }
    bool isPlaying() const { return state != nullptr && state->playing.load(); }
    void process (int numSamples) override
    {
        if (state == nullptr) { outputAudio.clear(); return; }
        const float level = state->level.load (std::memory_order_relaxed);
        const auto mode = state->mode.load (std::memory_order_relaxed);
        if (mode == AudioPlayerState::Mode::File) { processFile (numSamples, level); return; }
        const bool playing = state->playing.load (std::memory_order_relaxed);
        const double increment = juce::jlimit (0.0, currentSampleRate * 0.49,
                                    (double) state->sineFrequency.load()) / currentSampleRate;
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
            {
                float sample = 0.0f;
                switch (mode)
                {
                    case AudioPlayerState::Mode::Sine:
                        sample = (float) std::sin (2.0 * juce::MathConstants<double>::pi * state->sinePhase); break;
                    case AudioPlayerState::Mode::Square:
                        sample = state->sinePhase < 0.5 ? 1.0f : -1.0f; break;
                    case AudioPlayerState::Mode::Triangle:
                        sample = (float) (1.0 - 4.0 * std::abs (state->sinePhase - 0.5)); break;
                    case AudioPlayerState::Mode::Saw:
                        sample = (float) (2.0 * state->sinePhase - 1.0); break;
                    case AudioPlayerState::Mode::Noise:
                    {
                        const float white = state->noiseRandom.nextFloat() * 2.0f - 1.0f;
                        if (state->noiseType.load() == AudioPlayerState::Noise::Pink)
                        {
                            auto& p = state->pink[(size_t) ch];
                            p.b0 = 0.99765f * p.b0 + white * 0.0990460f;
                            p.b1 = 0.96300f * p.b1 + white * 0.2965164f;
                            p.b2 = 0.57000f * p.b2 + white * 1.0526913f;
                            sample = (p.b0 + p.b1 + p.b2 + white * 0.1848f) * 0.2f;
                        }
                        else sample = white;
                        break;
                    }
                    default: break;
                }
                outputAudio.setSample (ch, i, playing ? sample * level : 0.0f);
            }
            state->sinePhase += increment;
            state->sinePhase -= std::floor (state->sinePhase);
        }
    }
private:
    void processFile (int count, float level)
    {
        const int channels = state->fileBuffer.getNumChannels();
        const int length = state->fileBuffer.getNumSamples();
        outputAudio.clear();
        if (channels == 0 || length == 0) return;
        const auto seek = state->seekRequest.exchange (-1);
        int64_t pos = juce::jlimit ((int64_t) 0, (int64_t) length,
                                    seek >= 0 ? seek : state->playbackPosition.load());
        if (state->playing.load())
        {
            for (int i = 0; i < count; ++i)
            {
                if (pos >= length)
                {
                    if (state->looping.load()) pos = 0;
                    else { state->playing.store (false); break; }
                }
                for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
                {
                    // Files map channels by index without changing their channel layout.
                    // Missing channels stay silent instead of repeating the last one.
                    const int source = ch;
                    if (source < channels)
                        outputAudio.setSample (ch, i, state->fileBuffer.getSample (source, (int) pos) * level);
                }
                ++pos;
            }
        }
        state->playbackPosition.store (pos);
    }
    AudioPlayerState* state = nullptr;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPlayerNode)
};
