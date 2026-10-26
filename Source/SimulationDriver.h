#pragma once
#include "PatchyProcessor.h"
#include <cmath>

// Each DSP call receives exactly frameSize samples at the logical rate.
// Small frames may be processed in short batches when the OS wakes this
// worker. At 44.1/48 kHz, same-rate FIFOs bridge physical callback sizes.
class SimulationDriver final : private juce::Thread
{
public:
    explicit SimulationDriver (PatchyProcessor& p) : juce::Thread ("AudioSim DSP"), processor (p) {}
    ~SimulationDriver() override { stop(); }
    void start (double rate, int frames)
    {
        stop();
        sampleRate = rate; frameSize = frames;
        // The standalone host buffer is only a DSP clock. Graph nodes own
        // their independently resolved audio buffers; this is not a channel limit.
        block.setSize (1, frames);
        midi.ensureSize (2048);
        startThread (juce::Thread::Priority::high);
    }
    void stop() { signalThreadShouldExit(); notify(); stopThread (10000); }
private:
    void run() override
    {
        const double period = 1000.0 * frameSize / sampleRate;
        double next = juce::Time::getMillisecondCounterHiRes();
        while (! threadShouldExit())
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (now < next) { wait (std::max (1, (int) std::ceil (next - now))); continue; }
            if (now - next > 100.0) next = now;
            block.clear(); midi.clear();
            const juce::ScopedLock lock (processor.getCallbackLock());
            if (! processor.isSuspended()) processor.processBlock (block, midi);
            next += period;
        }
    }
    PatchyProcessor& processor;
    juce::AudioBuffer<float> block;
    juce::MidiBuffer midi;
    double sampleRate = 44100.0;
    int frameSize = 512;
};
