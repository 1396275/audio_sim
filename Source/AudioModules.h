#pragma once
#include "NodeProcessor.h"
#include "AudioModuleConfig.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <complex>
#include <numeric>

class ConfiguredAudioNode : public NodeProcessor
{
public:
    explicit ConfiguredAudioNode (const juce::String& id) : NodeProcessor (id, Type::Audio) {}
    virtual void configure (const juce::var&) = 0;
    void prepare (double rate, int frames) override
    {
        NodeProcessor::prepare (rate, frames);
        signalChannels = inputPortAudioChannels.empty() ? 0 : inputPortAudioChannels.front();
    }
protected:
    int signalChannels = 0;
    std::unique_ptr<juce::DynamicObject> status (int outputs) const
    {
        auto s = std::make_unique<juce::DynamicObject>();
        s->setProperty ("sampleRate", currentSampleRate);
        s->setProperty ("inputChannels", signalChannels);
        s->setProperty ("inputWidths", AudioSim::intArray (inputPortAudioChannels));
        std::vector<int> widths;
        for (int port = 0; port < outputs; ++port) widths.push_back (getOutputAudioChannels (port));
        s->setProperty ("outputWidths", AudioSim::intArray (widths));
        return s;
    }
};

class RmsNode final : public ConfiguredAudioNode
{
public:
    explicit RmsNode (const juce::String& id) : ConfiguredAudioNode (id) {}
    void configure (const juce::var& settings) override
    {
        windowMs = AudioSim::moduleNumber (settings["windowMs"], 100, 1, 2000);
    }
    int getOutputAudioChannels (int = 0) const override { return 0; }
    void prepare (double rate, int frames) override
    {
        ConfiguredAudioNode::prepare (rate, frames);
        windowSamples = std::max (1, (int) std::llround (rate * windowMs / 1000.0));
        accumulated = 0; sums.fill (0.0); ready.store (false);
        for (auto& value : values) value.store (0.f);
    }
    void process (int frames) override
    {
        for (int sample = 0; sample < frames; ++sample)
        {
            for (int ch = 0; ch < signalChannels; ++ch)
            {
                const double value = inputAudio.getSample (ch, sample);
                if (std::isfinite (value)) sums[(size_t) ch] += value * value;
            }
            if (++accumulated == windowSamples)
            {
                for (int ch = 0; ch < signalChannels; ++ch)
                {
                    values[(size_t) ch].store ((float) std::sqrt (sums[(size_t) ch] / windowSamples), std::memory_order_relaxed);
                    sums[(size_t) ch] = 0.0;
                }
                accumulated = 0; ready.store (true, std::memory_order_release);
            }
        }
        // One generic numeric event per input channel; keys are 1-based.
        outputValueCount = signalChannels;
        for (int ch = 0; ch < signalChannels; ++ch)
        {
            auto& value = outputValues[(size_t) ch]; value = {};
            value.key = (uint32_t) ch + 1; value.type = PAX_TYPE_GENERIC;
            value.dataType = PAX_DATA_FLOAT; value.value = values[(size_t) ch].load (std::memory_order_relaxed);
        }
    }
    juce::var getAudioModuleStatus() const override
    {
        auto s = status (0);
        juce::Array<juce::var> rms;
        const bool valid = ! disabled && signalChannels > 0 && ready.load (std::memory_order_acquire);
        for (int ch = 0; ch < signalChannels; ++ch) rms.add (valid ? values[(size_t) ch].load (std::memory_order_relaxed) : 0.f);
        s->setProperty ("rms", rms); s->setProperty ("rmsReady", valid);
        s->setProperty ("windowSamples", windowSamples);
        return s.release();
    }
private:
    double windowMs = 100;
    int windowSamples = 4800, accumulated = 0;
    std::array<double, AudioSim::maxAudioChannels> sums {};
    std::array<std::atomic<float>, AudioSim::maxAudioChannels> values {};
    std::atomic<bool> ready { false };
};

class MuxNode final : public ConfiguredAudioNode
{
public:
    explicit MuxNode (const juce::String& id) : ConfiguredAudioNode (id) {}
    void configure (const juce::var& settings) override { ports = AudioSim::muxPortCount (settings); }
    int totalChannels() const
    {
        int total = 0;
        for (int port = 0; port < std::min (ports, (int) inputPortAudioChannels.size()); ++port)
            total += inputPortAudioChannels[(size_t) port];
        return total;
    }
    int resolveAudioChannelCount (int) const override { return juce::jlimit (1, AudioSim::maxAudioChannels, totalChannels()); }
    int getOutputAudioChannels (int = 0) const override { return totalChannels() <= AudioSim::maxAudioChannels ? totalChannels() : 0; }
    void prepare (double rate, int frames) override
    {
        ConfiguredAudioNode::prepare (rate, frames);
        inputAudioBuffers.resize ((size_t) ports);
        for (int port = 0; port < ports; ++port)
            inputAudioBuffers[(size_t) port].setSize (std::max (1, port < (int) inputPortAudioChannels.size() ? inputPortAudioChannels[(size_t) port] : 0), frames);
    }
    void process (int frames) override
    {
        if (totalChannels() > AudioSim::maxAudioChannels) return;
        int offset = 0;
        for (int port = 0; port < std::min (ports, (int) inputPortAudioChannels.size()); ++port)
            for (int ch = 0; ch < inputPortAudioChannels[(size_t) port]; ++ch)
                outputAudio.copyFrom (offset++, 0, inputAudioBuffers[(size_t) port], ch, 0, frames);
    }
    juce::var getAudioModuleStatus() const override
    {
        auto s = status (1);
        auto widths = inputPortAudioChannels; widths.resize ((size_t) ports);
        s->setProperty ("inputWidths", AudioSim::intArray (widths));
        s->setProperty ("inputChannels", totalChannels());
        s->setProperty ("valid", totalChannels() <= AudioSim::maxAudioChannels);
        return s.release();
    }
private:
    int ports = 2;
};

class DemuxNode final : public ConfiguredAudioNode
{
public:
    explicit DemuxNode (const juce::String& id) : ConfiguredAudioNode (id) {}
    void configure (const juce::var& settings) override { widths = AudioSim::demuxWidths (settings); }
    int totalChannels() const { return std::accumulate (widths.begin(), widths.end(), 0); }
    bool valid() const { return ! inputPortAudioChannels.empty() && totalChannels() == inputPortAudioChannels.front(); }
    int getOutputAudioChannels (int port = 0) const override
    {
        return valid() && port >= 0 && port < (int) widths.size() ? widths[(size_t) port] : 0;
    }
    void prepare (double rate, int frames) override
    {
        ConfiguredAudioNode::prepare (rate, frames);
        outputAudioBuffers.resize (widths.size());
        for (size_t port = 0; port < widths.size(); ++port) outputAudioBuffers[port].setSize (widths[port], frames);
    }
    void process (int frames) override
    {
        if (! valid()) return;
        int offset = 0;
        for (size_t port = 0; port < widths.size(); ++port)
            for (int ch = 0; ch < widths[port]; ++ch)
                outputAudioBuffers[port].copyFrom (ch, 0, inputAudio, offset++, 0, frames);
    }
    juce::var getAudioModuleStatus() const override
    {
        auto s = status ((int) widths.size());
        s->setProperty ("valid", valid()); s->setProperty ("requiredChannels", totalChannels());
        return s.release();
    }
private:
    std::vector<int> widths { 1, 1 };
};

class EqualizerNode final : public ConfiguredAudioNode
{
public:
    explicit EqualizerNode (const juce::String& id) : ConfiguredAudioNode (id) {}
    void configure (const juce::var& settings) override { config = settings; }
    bool passesThroughWhenDisabled() const override { return true; }
    int getOutputAudioChannels (int = 0) const override { return inputPortAudioChannels.empty() ? 0 : inputPortAudioChannels.front(); }
    void prepare (double rate, int frames) override
    {
        ConfiguredAudioNode::prepare (rate, frames);
        bypass = (bool) config["bypass"];
        outputGain = std::pow (10.0, AudioSim::moduleNumber (config["outputGainDb"], 0, -24, 24) / 20.0);
        const auto* saved = config["bands"].getArray();
        const std::array<double, 5> defaults { 100, 400, 1000, 4000, 10000 };
        for (size_t band = 0; band < 5; ++band)
        {
            const auto s = saved && band < (size_t) saved->size() ? (*saved)[(int) band] : juce::var();
            const bool enabled = ! s.hasProperty ("enabled") || (bool) s["enabled"];
            const double f = juce::jlimit (5.0, rate * 0.49, AudioSim::moduleNumber (s["frequency"], defaults[band], 5, 95000));
            const double q = AudioSim::moduleNumber (s["q"], 0.70710678, 0.1, 20);
            const double gain = std::pow (10.0, AudioSim::moduleNumber (s["gainDb"], 0, -24, 24) / 20.0);
            auto type = s["type"].toString();
            if (type != "peak" && type != "lowShelf" && type != "highShelf" && type != "lowPass" && type != "highPass")
                type = band == 0 ? "lowShelf" : band == 4 ? "highShelf" : "peak";
            using Coefficients = juce::dsp::IIR::ArrayCoefficients<double>;
            auto c = type == "lowPass" ? Coefficients::makeLowPass (rate, f, q)
                   : type == "highPass" ? Coefficients::makeHighPass (rate, f, q)
                   : type == "lowShelf" ? Coefficients::makeLowShelf (rate, f, q, gain)
                   : type == "highShelf" ? Coefficients::makeHighShelf (rate, f, q, gain)
                   : Coefficients::makePeakFilter (rate, f, q, gain);
            coefficients[band] = enabled ? std::array<double, 5>{ c[0]/c[3], c[1]/c[3], c[2]/c[3], c[4]/c[3], c[5]/c[3] }
                                         : std::array<double, 5>{ 1, 0, 0, 0, 0 };
        }
        states = {};
        juce::Array<juce::var> curve;
        for (int i = 0; i < 160; ++i)
        {
            const double f = 5 * std::pow (rate * 0.49 / 5, i / 159.0);
            const auto z = std::polar (1.0, -juce::MathConstants<double>::twoPi * f / rate);
            double magnitude = bypass ? 1.0 : outputGain;
            if (! bypass)
                for (const auto& c : coefficients) magnitude *= std::abs ((c[0] + c[1]*z + c[2]*z*z) / (1.0 + c[3]*z + c[4]*z*z));
            curve.add (juce::Array<juce::var>{ f, 20.0 * std::log10 (std::max (1e-10, magnitude)) });
        }
        response = curve;
    }
    void process (int frames) override
    {
        for (int ch = 0; ch < signalChannels; ++ch)
            for (int sample = 0; sample < frames; ++sample)
            {
                double value = inputAudio.getSample (ch, sample);
                if (! std::isfinite (value)) value = 0;
                if (! bypass && ! disabled)
                {
                    for (size_t band = 0; band < 5; ++band)
                    {
                        const auto& c = coefficients[band]; auto& s = states[(size_t) ch][band];
                        const double out = c[0] * value + s[0];
                        s[0] = c[1] * value - c[3] * out + s[1];
                        s[1] = c[2] * value - c[4] * out;
                        value = out;
                    }
                    value *= outputGain;
                }
                outputAudio.setSample (ch, sample, (float) value);
            }
    }
    juce::var getAudioModuleStatus() const override
    {
        auto s = status (1); s->setProperty ("response", response); s->setProperty ("bypass", bypass || disabled);
        return s.release();
    }
private:
    juce::var config, response;
    bool bypass = false;
    double outputGain = 1;
    std::array<std::array<double, 5>, 5> coefficients {};
    std::array<std::array<std::array<double, 2>, 5>, AudioSim::maxAudioChannels> states {};
};
