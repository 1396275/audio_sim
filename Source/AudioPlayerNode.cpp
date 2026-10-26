#include "AudioPlayerNode.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>

bool AudioPlayerState::loadFile (const juce::File& file, double targetSampleRate)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();   // WAV/AIFF always available
    // FLAC/OggVorbis/MP3 registered here too, IF the relevant JUCE_USE_*
    // flags are enabled at compile time (see CMakeLists.txt) — codecs
    // simply aren't compiled in otherwise, registerBasicFormats() alone
    // never includes them.

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        juce::Logger::writeToLog ("AudioPlayerState: failed to open " + file.getFullPathName());
        return false;
    }

    const int   numFileChannels = (int) reader->numChannels;
    const int64_t numFileSamples  = reader->lengthInSamples;
    const double sourceSampleRate = reader->sampleRate;

    if (numFileChannels <= 0 || numFileChannels > AudioSim::maxAudioChannels || numFileSamples <= 0
        || numFileSamples > std::numeric_limits<int>::max() || sourceSampleRate <= 0.0 || targetSampleRate <= 0.0)
    {
        juce::Logger::writeToLog ("AudioPlayerState: " + file.getFullPathName() + " has no readable audio data");
        return false;
    }

    // Read the whole file, at its own native rate, into a temporary buffer —
    // resampling (below) needs the complete signal available up front, not
    // streamed block by block, since this is a one-shot, whole-file load,
    // not a real-time read.
    juce::AudioBuffer<float> raw (numFileChannels, (int) numFileSamples);
    if (! reader->read (&raw, 0, (int) numFileSamples, 0, true, true)) return false;

    juce::AudioBuffer<float> resampled;

    // Plain, direct comparison rather than juce::approximatelyEqual() — a
    // relatively recent JUCE addition, avoided here since this project's
    // own exact JUCE version isn't directly confirmed; a simple epsilon
    // check needs no version assumption at all. Sample rates in practice
    // are always exact values (44100.0, 48000.0, etc), so an exact `==`
    // would likely work too, but a small tolerance costs nothing and
    // guards against any stray floating-point noise.
    if (std::abs (sourceSampleRate - targetSampleRate) < 0.01)
    {
        resampled = std::move (raw);
    }
    else
    {
        const double speedRatio = sourceSampleRate / targetSampleRate;
        const double desiredLength = std::ceil ((double) numFileSamples / speedRatio);
        if (desiredLength > std::numeric_limits<int>::max()) return false;
        const int outLength = (int) desiredLength;
        resampled.setSize (numFileChannels, outLength);
        juce::MemoryAudioSource input (raw, false, false);
        juce::ResamplingAudioSource converter (&input, false, numFileChannels);
        converter.setResamplingRatio (speedRatio);
        converter.prepareToPlay (4096, targetSampleRate);
        for (int start = 0; start < outLength; start += 4096)
        {
            juce::AudioSourceChannelInfo block (&resampled, start, std::min (4096, outLength - start));
            converter.getNextAudioBlock (block);
        }
        converter.releaseResources();
    }

    fileBuffer      = std::move (resampled);
    fileName        = file.getFileName();
    loadedFilePath = file.getFullPathName();
    fileSampleRate  = sourceSampleRate;
    loadedSampleRate = targetSampleRate;

    playbackPosition.store (0, std::memory_order_relaxed);
    seekRequest.store (-1, std::memory_order_relaxed);
    playing.store (false, std::memory_order_relaxed);

    computeWaveformPeaks();
    needsUIPush.store (true, std::memory_order_relaxed);

    juce::Logger::writeToLog ("AudioPlayerState: loaded " + fileName
                              + " (" + juce::String (sourceSampleRate, 0) + "Hz -> "
                              + juce::String (targetSampleRate, 0) + "Hz, "
                              + juce::String (fileBuffer.getNumSamples()) + " samples)");
    return true;
}

void AudioPlayerState::computeWaveformPeaks (int numBuckets)
{
    waveformPeaks.clear();

    const int numSamples = fileBuffer.getNumSamples();
    if (numSamples <= 0) return;

    numBuckets = juce::jmax (1, numBuckets);
    waveformPeaks.reserve ((size_t) numBuckets);

    const int numChannels = fileBuffer.getNumChannels();

    for (int b = 0; b < numBuckets; ++b)
    {
        int64_t startSample = ((int64_t) b * numSamples) / numBuckets;
        int64_t endSample   = ((int64_t) (b + 1) * numSamples) / numBuckets;
        if (endSample <= startSample) endSample = startSample + 1;
        endSample = juce::jmin (endSample, (int64_t) numSamples);

        float mn = 0.0f, mx = 0.0f;
        for (int64_t s = startSample; s < endSample; ++s)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float v = fileBuffer.getSample (ch, (int) s);
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
            }
        }
        waveformPeaks.push_back ({ mn, mx });
    }
}
