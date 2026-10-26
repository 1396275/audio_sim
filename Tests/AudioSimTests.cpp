#include <juce_core/juce_core.h>
#include "AudioPlayerNode.h"
#include "AudioMonitorNode.h"
#include "DeviceAudioFifo.h"
#include <cmath>
#include <thread>

class AudioSimTests : public juce::UnitTest
{
public:
    AudioSimTests() : UnitTest ("AudioSim source, channels and frame boundaries", "AudioSim") {}
    void runTest() override
    {
        beginTest ("Only exact 44.1/48 kHz permit hardware");
        for (double rate : { 1000., 1500., 2000., 8000., 16000., 32000., 44100.1, 48000.1, 96000., 192000. })
            expect (! AudioSim::isHardwareSampleRate (rate));
        expect (AudioSim::isHardwareSampleRate (44100.));
        expect (AudioSim::isHardwareSampleRate (48000.));

        beginTest ("Sine phase survives non-power-of-two frames and channel expansion");
        for (int frame : { 16, 24, 32, 64 })
        {
            AudioPlayerState state;
            state.outputChannels.store (8);
            state.playing.store (true); state.sineFrequency.store (125.f); state.level.store (0.25f);
            AudioPlayerNode node ("source", &state);
            expectEquals (node.resolveAudioChannelCount (0), 8);
            node.setAudioChannelCount (8); node.prepare (1500., frame);
            for (int block = 0; block < 3; ++block)
            {
                node.process (frame);
                for (int ch = 0; ch < 8; ++ch)
                    for (int i = 0; i < frame; ++i)
                    {
                        const auto expected = (float) (0.25 * std::sin (2.0 * juce::MathConstants<double>::pi * 125.0 * (block * frame + i) / 1500.0));
                        expectWithinAbsoluteError (node.outputAudio.getSample (ch, i), expected, 0.00001f);
                    }
            }
        }

        beginTest ("File channel layout and looping survive a partial frame");
        AudioPlayerState file;
        file.mode.store (AudioPlayerState::Mode::File); file.level.store (1.f);
        file.playing.store (true); file.looping.store (true); file.fileBuffer.setSize (3, 5);
        for (int ch = 0; ch < 3; ++ch)
            for (int i = 0; i < 5; ++i) file.fileBuffer.setSample (ch, i, (float) (10 * ch + i));
        AudioPlayerNode player ("file", &file); player.setAudioChannelCount (8); player.prepare (2000., 24);
        expectEquals (player.resolveAudioChannelCount (0), 3);
        expectEquals (player.getOutputAudioChannels(), 3);
        for (int block = 0; block < 2; ++block)
        {
            player.process (24);
            for (int ch = 0; ch < 8; ++ch)
                for (int i = 0; i < 24; ++i)
                    expectEquals (player.outputAudio.getSample (ch, i), ch < 3 ? (float) (10 * ch + (block * 24 + i) % 5) : 0.f);
        }

        beginTest ("Monitor captures actual channels through ring wrap and disconnect");
        auto monitor = std::make_unique<AudioMonitorBuffer>();
        expectEquals (monitor->getAllocatedChannelCount(), 0);
        monitor->prepare (8);
        expectEquals (monitor->getAllocatedChannelCount(), 8);
        juce::AudioBuffer<float> input (8, 4096);
        for (int ch = 0; ch < 8; ++ch)
            juce::FloatVectorOperations::fill (input.getWritePointer (ch), 0.1f * (ch + 1), 4096);
        for (int block = 0; block < 20; ++block) monitor->push (input, 3, 4096);
        std::vector<std::vector<float>> traces;
        monitor->snapshotChannels (2000, 128, traces);
        expectEquals ((int) traces.size(), 3);
        for (int ch = 0; ch < (int) traces.size(); ++ch)
            for (float value : traces[(size_t) ch]) expectWithinAbsoluteError (value, 0.1f * (ch + 1), 0.00001f);
        monitor->push (input, 1, 2); monitor->snapshotChannels (24, 24, traces);
        expectEquals ((int) traces.size(), 1);
        if (! traces.empty())
        {
            expectEquals (traces[0][0], 0.f);
            expectWithinAbsoluteError (traces[0][23], 0.1f, 0.00001f);
        }
        monitor->push (input, 0, 24); monitor->snapshotChannels (24, 24, traces);
        expect (traces.empty());

        beginTest ("Downsampled monitor retains narrow positive and negative peaks");
        input.clear();
        input.setSample (0, 7, 0.9f);
        input.setSample (0, 23, -0.8f);
        monitor->push (input, 1, 64);
        monitor->snapshotChannels (64, 4, traces);
        expectEquals ((int) traces.size(), 1);
        expectEquals ((int) traces[0].size(), 4);
        // Ordinary point sampling reads positions 0/16/32/48 and misses both pulses.
        for (float value : traces[0]) expectEquals (value, 0.f);
        monitor->snapshotChannels (64, 4, traces, true);
        expectEquals ((int) traces[0].size(), 8);
        expectWithinAbsoluteError (traces[0][1], 0.9f, 0.00001f);
        expectWithinAbsoluteError (traces[0][2], -0.8f, 0.00001f);

        beginTest ("Monitor prepare supports 64 channels without reallocating during push");
        monitor->prepare (64);
        const int allocated = monitor->getAllocatedChannelCount();
        expectEquals (allocated, 64);
        juce::AudioBuffer<float> wideInput (64, 24);
        for (int ch = 0; ch < 64; ++ch)
            juce::FloatVectorOperations::fill (wideInput.getWritePointer (ch), (float) (ch + 1), 24);
        for (int block = 0; block < 100; ++block) monitor->push (wideInput, 64, 24);
        monitor->prepare (64); // Graph swaps prepare again; history must survive.
        monitor->snapshotChannels (24, 24, traces);
        expectEquals ((int) traces.size(), 64);
        for (int ch = 0; ch < (int) traces.size(); ++ch)
            for (float value : traces[(size_t) ch]) expectEquals (value, (float) (ch + 1));
        monitor->prepare (2);
        monitor->prepare (64);
        expectEquals (monitor->getAllocatedChannelCount(), allocated);

        beginTest ("Preparing additional monitor channels is safe while audio is pushed");
        auto growingMonitor = std::make_unique<AudioMonitorBuffer>();
        growingMonitor->prepare (2);
        std::atomic<bool> start { false }, finished { false };
        std::thread producer ([&] {
            while (! start.load (std::memory_order_acquire)) std::this_thread::yield();
            while (! finished.load (std::memory_order_acquire)) growingMonitor->push (wideInput, 64, 24);
        });
        start.store (true, std::memory_order_release);
        for (int channels : { 8, 32, 64, 2, 64 }) growingMonitor->prepare (channels);
        finished.store (true, std::memory_order_release);
        producer.join();
        growingMonitor->push (wideInput, 64, 24);
        growingMonitor->snapshotChannels (24, 24, traces);
        expectEquals (growingMonitor->getAllocatedChannelCount(), 64);
        expectEquals ((int) traces.size(), 64);
        if (traces.size() == 64) expectEquals (traces[63][23], 64.f);

        beginTest ("FIFO bridges frames and transfers existing queued data without replacement");
        SharedDeviceAudioFifo fifo;
        expect (fifo.get() == nullptr, "Unopened devices must not allocate queue storage");
        fifo.prepareFor (8, 48000, 24);
        const auto* preparedFifo = fifo.get();
        fifo.prepareFor (8, 48000, 24);
        expect (fifo.get() == preparedFifo, "Repeated preparation reuses the queue");
        juce::AudioBuffer<float> frame (8, 24), physical (8, 64);
        const std::vector<int> mapping { 2, 5, 7 };
        for (int block = 0; block < 3; ++block)
        {
            for (int ch = 0; ch < 8; ++ch)
                for (int i = 0; i < 24; ++i) frame.setSample (ch, i, (float) (ch * 100 + block * 24 + i));
            fifo->write (frame, 24, mapping);
        }
        SharedDeviceAudioFifo transferred;
        expect (transferred.get() == nullptr);
        transferred.shareFrom (fifo);
        expect (transferred.get() == preparedFifo);
        transferred->read (physical.getArrayOfWritePointers(), 8, 64, mapping);
        for (int ch = 0; ch < 8; ++ch)
            for (int i = 0; i < 64; ++i)
            {
                const auto it = std::find (mapping.begin(), mapping.end(), ch);
                const float expected = it == mapping.end() ? 0.f : (float) ((int) (it - mapping.begin()) * 100 + i);
                expectEquals (physical.getSample (ch, i), expected);
            }
        beginTest ("Transferred FIFO retains 64-channel data after its old node retires");
        SharedDeviceAudioFifo surviving;
        {
            SharedDeviceAudioFifo oldNode;
            oldNode.prepareFor (64, 44100, 24);
            oldNode->write (wideInput, 24, {});
            surviving.shareFrom (oldNode);
        }
        juce::AudioBuffer<float> recovered (64, 24);
        expect (surviving->read (recovered, 24));
        for (int ch = 0; ch < 64; ++ch)
            for (int i = 0; i < 24; ++i) expectEquals (recovered.getSample (ch, i), (float) (ch + 1));
    }
};
static AudioSimTests audioSimTests;
