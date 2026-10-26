#include "WelchSpectrum.h"
#include "AudioMonitorNode.h"
#include <numeric>

class WelchSpectrumTests final : public juce::UnitTest
{
public:
    WelchSpectrumTests() : UnitTest ("Welch PSD normalisation and raw capture", "AudioRouting") {}
    void runTest() override
    {
        constexpr int size = 1024, count = size + 7 * size / 2;
        constexpr double rate = 48000.0;
        AudioSim::WelchSpectrum analyser;
        std::vector<float> samples (count), db;
        const auto power = [&] {
            double sum = 0;
            for (float value : db) sum += std::pow (10.0, value / 10.0);
            return sum * rate / size;
        };
        for (const auto* window : { "hann", "hamming", "blackman", "blackmanHarris", "flatTop", "rectangular" })
        {
            beginTest (juce::String (window) + " integrates a bin-centred sine to its mean square power");
            analyser.configure (size, window);
            for (int i = 0; i < count; ++i) samples[(size_t) i] = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 64 * i / size));
            expectEquals (analyser.calculate (samples, rate, 8, db), 8);
            expectEquals ((int) db.size(), size / 2 + 1);
            expectWithinAbsoluteError (power(), 0.125, 0.00001);
            const int peak = (int) (std::max_element (db.begin(), db.end()) - db.begin());
            expectEquals (peak, 64);
            const float original = db[64];
            for (auto& value : samples) value *= 0.0001f;
            analyser.calculate (samples, rate, 8, db);
            expectWithinAbsoluteError (db[64] - original, -80.f, 0.002f, "Quiet signals must not pass through the scope PCM quantisation");
        }

        beginTest ("DC and Nyquist are retained and never doubled");
        analyser.configure (size, "rectangular");
        for (bool nyquist : { false, true })
        {
            for (int i = 0; i < count; ++i) samples[(size_t) i] = nyquist && (i & 1) ? -0.25f : 0.25f;
            analyser.calculate (samples, rate, 8, db);
            expectWithinAbsoluteError (power(), 0.0625, 0.000001);
            expectWithinAbsoluteError (db[nyquist ? size / 2 : 0], (float) (10.0 * std::log10 (0.0625 * size / rate)), 0.001f);
        }

        beginTest ("Welch averages linear power of overlapping segments, rather than dB");
        std::vector<float> unequal (size * 3 / 2, 1.f);
        std::fill (unequal.begin() + size, unequal.end(), 3.f);
        expectEquals (analyser.calculate (unequal, rate, 2, db), 2);
        expectWithinAbsoluteError (db[0], (float) (10.0 * std::log10 (2.5 * size / rate)), 0.001f);

        beginTest ("Length, available segment count, silence and nonfinite data are handled");
        analyser.configure (16384, "hann");
        samples.assign (65536, 0.f);
        expectEquals (analyser.calculate (samples, rate, 16, db), 7);
        expect (std::all_of (db.begin(), db.end(), [](float value) { return value == -200.f; }));
        samples[1000] = std::numeric_limits<float>::quiet_NaN();
        samples[2000] = std::numeric_limits<float>::infinity();
        analyser.calculate (samples, rate, 16, db);
        expect (std::all_of (db.begin(), db.end(), [](float value) { return std::isfinite (value); }));
        samples.resize (16383);
        expectEquals (analyser.calculate (samples, rate, 8, db), 0);
        expect (db.empty());
        expectEquals (AudioSim::WelchSpectrum::sanitiseFftSize (777), 2048);

        beginTest ("Selected raw channel preserves quiet samples and resets after a sample rate change");
        auto buffer = std::make_unique<AudioMonitorBuffer>();
        buffer->prepare (8);
        juce::AudioBuffer<float> input (8, 24);
        for (int ch = 0; ch < 8; ++ch)
            for (int i = 0; i < 24; ++i) input.setSample (ch, i, (ch + 1) * 0.00001f);
        int channels = 0; double sr = 0; uint64_t version = 0;
        for (int block = 0; block < 2800; ++block) buffer->push (input, 8, 24, rate);
        expect (buffer->snapshotChannel (7, 65536, samples, channels, sr, version));
        expectEquals (channels, 8); expectEquals (sr, rate); expectEquals ((int) samples.size(), 65536);
        for (float value : samples) if (value != 0.00008f) { expect (false, "Channel selection or ring wrap corrupted raw PCM"); break; }
        buffer->push (input, 8, 24, 1500.0);
        expect (buffer->snapshotChannel (7, 65536, samples, channels, sr, version));
        expectEquals ((int) samples.size(), 24); expectEquals (sr, 1500.0);
        expect (buffer->snapshotChannel (8, 65536, samples, channels, sr, version)); expect (samples.empty());
        buffer->prepare (0); buffer->push (input, 8, 24, 1500.0);
        expect (buffer->snapshotChannel (0, 65536, samples, channels, sr, version));
        expectEquals (channels, 0); expect (samples.empty());
    }
};
static WelchSpectrumTests welchSpectrumTests;
