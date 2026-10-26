#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

namespace AudioSim
{
// Used on the message thread, never in the simulation/audio callback.
// Periodic windows, 50% overlap, no detrending, one-sided PSD in FS^2/Hz.
// Average linear periodograms before converting to dBFS/Hz.
class WelchSpectrum
{
public:
    static int sanitiseFftSize (int size)
    {
        return size >= 256 && size <= 16384 && (size & (size - 1)) == 0 ? size : 2048;
    }
    static juce::String sanitiseWindow (const juce::String& name)
    {
        for (const auto* supported : { "hann", "hamming", "blackman", "blackmanHarris", "flatTop", "rectangular" })
            if (name == supported) return name;
        return "hann";
    }
    void configure (int requestedSize, const juce::String& requestedWindow)
    {
        const int size = sanitiseFftSize (requestedSize);
        const auto name = sanitiseWindow (requestedWindow);
        if (size == fftSize && name == windowName) return;
        fftSize = size; windowName = name;
        int order = 0;
        for (int n = size; n > 1; n >>= 1) ++order;
        fft = std::make_unique<juce::dsp::FFT> (order);
        work.resize ((size_t) size * 2);
        window.resize ((size_t) size);
        power.resize ((size_t) size / 2 + 1);
        windowEnergy = 0.0;
        for (int i = 0; i < size; ++i)
        {
            const double phase = juce::MathConstants<double>::twoPi * i / size;
            const double c1 = std::cos (phase), c2 = std::cos (2 * phase), c3 = std::cos (3 * phase);
            double value = 1.0;
            if (name == "hann") value = 0.5 - 0.5 * c1;
            else if (name == "hamming") value = 0.54 - 0.46 * c1;
            else if (name == "blackman") value = 0.42 - 0.5 * c1 + 0.08 * c2;
            else if (name == "blackmanHarris") value = 0.35875 - 0.48829 * c1 + 0.14128 * c2 - 0.01168 * c3;
            else if (name == "flatTop") value = 0.21557895 - 0.41663158 * c1 + 0.277263158 * c2
                                                - 0.083578947 * c3 + 0.006947368 * std::cos (4 * phase);
            window[(size_t) i] = (float) value;
            windowEnergy += value * value;
        }
    }
    int calculate (std::span<const float> samples, double sampleRate, int requestedAverages,
                   std::vector<float>& decibels)
    {
        decibels.clear();
        if (! fft || ! std::isfinite (sampleRate) || sampleRate <= 0 || (int) samples.size() < fftSize) return 0;
        const int hop = fftSize / 2;
        const int segments = std::min (std::clamp (requestedAverages, 2, 16), 1 + ((int) samples.size() - fftSize) / hop);
        const int first = (int) samples.size() - fftSize - (segments - 1) * hop;
        std::fill (power.begin(), power.end(), 0.0);
        for (int segment = 0; segment < segments; ++segment)
        {
            std::fill (work.begin(), work.end(), 0.f);
            for (int i = 0; i < fftSize; ++i)
            {
                const float sample = samples[(size_t) (first + segment * hop + i)];
                work[(size_t) i] = (std::isfinite (sample) ? sample : 0.f) * window[(size_t) i];
            }
            fft->performRealOnlyForwardTransform (work.data(), true);
            for (int bin = 0; bin <= fftSize / 2; ++bin)
            {
                const double real = work[(size_t) bin * 2], imag = work[(size_t) bin * 2 + 1];
                power[(size_t) bin] += (real * real + imag * imag) * (bin == 0 || bin == fftSize / 2 ? 1.0 : 2.0);
            }
        }
        decibels.resize (power.size());
        const double normalisation = sampleRate * windowEnergy * segments;
        for (size_t bin = 0; bin < power.size(); ++bin)
            decibels[bin] = (float) (10.0 * std::log10 (std::max (1.0e-20, power[bin] / normalisation)));
        return segments;
    }
private:
    int fftSize = 0;
    juce::String windowName;
    double windowEnergy = 0.0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, work;
    std::vector<double> power;
};
}
