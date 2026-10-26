#include "AudioDeviceDiscovery.h"
#include <deque>
#include <thread>
#include <vector>

namespace
{
    // Completion is pumped explicitly by each test. No message loop, device API,
    // or timing-dependent sleep is needed to control a driver's completion.
    struct DiscoveryPostQueue
    {
        void post (std::function<void()> fn)
        {
            {
                std::lock_guard<std::mutex> lock (mutex);
                callbacks.push_back (std::move (fn));
            }
            available.signal();
        }

        bool waitForCallback()
        {
            {
                std::lock_guard<std::mutex> lock (mutex);
                if (! callbacks.empty()) return true;
            }
            return available.wait (3000);
        }

        void drain()
        {
            std::deque<std::function<void()>> ready;
            {
                std::lock_guard<std::mutex> lock (mutex);
                ready.swap (callbacks);
                available.reset();
            }
            for (auto& fn : ready) fn();
        }

        std::mutex mutex;
        std::deque<std::function<void()>> callbacks;
        juce::WaitableEvent available { true };
    };

    juce::var fakeCapabilities()
    {
        juce::Array<juce::var> devices;
        for (const auto rate : { 44100.0, 48000.0 })
        {
            auto* device = new juce::DynamicObject();
            device->setProperty ("id", juce::String ((int) rate));
            device->setProperty ("channelCount", 8);
            device->setProperty ("sampleRates", juce::Array<juce::var> { juce::var (rate) });
            device->setProperty ("bufferSizes", juce::Array<juce::var> { juce::var (64), juce::var (256) });
            device->setProperty ("supportsSampleRate", rate == 44100.0);
            devices.add (device);
        }
        auto* root = new juce::DynamicObject();
        root->setProperty ("audioInDevices", devices);
        root->setProperty ("audioOutDevices", devices);
        return root;
    }

    struct ControlledScanner : std::enable_shared_from_this<ControlledScanner>
    {
        AudioDeviceDiscovery::Scanner callback()
        {
            return [self = shared_from_this()] (bool, double, const AudioDeviceDiscovery::Cancelled& cancelled)
            {
                ++self->calls;
                const auto running = ++self->active;
                auto previous = self->maxActive.load();
                while (running > previous && ! self->maxActive.compare_exchange_weak (previous, running)) {}
                self->ranOffOwnerThread.store (std::this_thread::get_id() != self->ownerThread);
                {
                    std::lock_guard<std::mutex> lock (self->mutex);
                    self->lastCancelled = cancelled;
                }
                self->started.signal();
                self->release.wait (5000);
                --self->active;
                self->finished.signal();
                // Deliberately return a valid result even after cancellation:
                // the coordinator must reject stale driver completions itself.
                return fakeCapabilities();
            };
        }

        AudioDeviceDiscovery::Cancelled currentCancellation()
        {
            std::lock_guard<std::mutex> lock (mutex);
            return lastCancelled;
        }

        std::mutex mutex;
        AudioDeviceDiscovery::Cancelled lastCancelled;
        std::atomic<int> calls { 0 }, active { 0 }, maxActive { 0 };
        std::atomic<bool> ranOffOwnerThread { false };
        std::thread::id ownerThread = std::this_thread::get_id();
        juce::WaitableEvent started { true }, release { true }, finished { true };
    };

    juce::String status (const juce::var& result)
    {
        return result.getProperty ("discoveryState", {}).toString();
    }

    bool supports (const juce::var& result, int device)
    {
        const auto list = result.getProperty ("audioOutDevices", {});
        const auto* devices = list.getArray();
        return devices != nullptr && juce::isPositiveAndBelow (device, devices->size())
               && (bool) (*devices)[device].getProperty ("supportsSampleRate", false);
    }
}

class AudioDeviceDiscoveryTests final : public juce::UnitTest
{
public:
    AudioDeviceDiscoveryTests() : UnitTest ("Audio device discovery is asynchronous and generation safe", "AudioSim") {}

    void runTest() override
    {
        testAsynchronousScanAndCache();
        testCancellationAndSerialRestart();
        testQueuedCompletionCannotOverwriteNewRate();
        testDestructionDoesNotWaitOrDeliver();
        testErrorsAreReported();
    }

private:
    void testAsynchronousScanAndCache()
    {
        beginTest ("Blocked driver does not block request; capabilities are cached across hardware rates");
        auto scanner = std::make_shared<ControlledScanner>();
        auto queue = std::make_shared<DiscoveryPostQueue>();
        std::vector<juce::var> delivered;
        AudioDeviceDiscovery discovery (scanner->callback(), [&] (const juce::var& value) { delivered.push_back (value); },
                                        [queue] (auto fn) { queue->post (std::move (fn)); });

        discovery.request (true, 8000.0);
        expectEquals (scanner->calls.load(), 0);
        expectEquals (status (delivered.back()), juce::String ("disabled"));
        expect (! (bool) delivered.back()["hardwareEnabled"]);

        const auto before = juce::Time::getMillisecondCounterHiRes();
        discovery.request (true, 44100.0);
        expect (juce::Time::getMillisecondCounterHiRes() - before < 1000.0,
                "request must not wait for the five-second fake driver timeout");
        expectEquals (status (delivered.back()), juce::String ("scanning"));
        expect (scanner->started.wait (3000));
        expect (scanner->ranOffOwnerThread.load());
        const auto notCancelled = scanner->currentCancellation();
        discovery.request (true, 44100.0);
        expect (notCancelled && ! notCancelled(), "Repeated requests must share the ongoing scan");
        expectEquals (scanner->calls.load(), 1);

        scanner->release.signal();
        expect (queue->waitForCallback());
        queue->drain();
        expectEquals (status (delivered.back()), juce::String ("ready"));
        expect (supports (delivered.back(), 0));
        expect (! supports (delivered.back(), 1));
        const auto firstReady = delivered.back();

        discovery.request (true, 44100.0);
        discovery.request (true, 48000.0);
        expectEquals (scanner->calls.load(), 1);
        expectEquals (status (delivered.back()), juce::String ("ready"));
        expect (! supports (delivered.back(), 0));
        expect (supports (delivered.back(), 1));
        expect (supports (firstReady, 0), "Reprojecting cached rates must not mutate earlier UI payloads");
        expectEquals ((double) delivered.back()["sampleRate"], 48000.0);

        discovery.request (true, 48000.0, true);
        expectEquals (status (delivered.back()), juce::String ("scanning"));
        expect (queue->waitForCallback());
        queue->drain();
        expectEquals (scanner->calls.load(), 2);
        expectEquals (status (delivered.back()), juce::String ("ready"));
        expectEquals (scanner->maxActive.load(), 1);
    }

    void testCancellationAndSerialRestart()
    {
        beginTest ("Simulation rates cancel driver work and a later hardware request runs serially");
        auto scanner = std::make_shared<ControlledScanner>();
        auto queue = std::make_shared<DiscoveryPostQueue>();
        std::vector<juce::var> delivered;
        AudioDeviceDiscovery discovery (scanner->callback(), [&] (const juce::var& value) { delivered.push_back (value); },
                                        [queue] (auto fn) { queue->post (std::move (fn)); });
        discovery.request (true, 44100.0);
        expect (scanner->started.wait (3000));
        const auto oldCancellation = scanner->currentCancellation();
        discovery.request (true, 1500.0);
        expect (oldCancellation && oldCancellation());
        expectEquals (status (delivered.back()), juce::String ("disabled"));
        expectEquals (delivered.back()["audioInDevices"].getArray()->size(), 0);
        expectEquals (delivered.back()["audioOutDevices"].getArray()->size(), 0);
        expectEquals (scanner->calls.load(), 1);
        discovery.request (true, 48000.0);
        expectEquals (status (delivered.back()), juce::String ("scanning"));
        expectEquals (scanner->calls.load(), 1, "A second driver scan must wait off-thread for the first to finish");
        scanner->release.signal();
        expect (queue->waitForCallback());
        queue->drain();
        expectEquals (scanner->calls.load(), 2);
        expectEquals (scanner->maxActive.load(), 1);
        expectEquals (status (delivered.back()), juce::String ("ready"));
        expectEquals ((double) delivered.back()["sampleRate"], 48000.0);
        for (const auto& value : delivered)
            expect (status (value) != "ready" || (double) value["sampleRate"] == 48000.0);
    }

    void testQueuedCompletionCannotOverwriteNewRate()
    {
        beginTest ("A completion already queued for the UI cannot overwrite a newer sample rate");
        auto scanner = std::make_shared<ControlledScanner>();
        auto queue = std::make_shared<DiscoveryPostQueue>();
        std::vector<juce::var> delivered;
        AudioDeviceDiscovery discovery (scanner->callback(), [&] (const juce::var& value) { delivered.push_back (value); },
                                        [queue] (auto fn) { queue->post (std::move (fn)); });
        discovery.request (true, 44100.0);
        scanner->release.signal();
        expect (queue->waitForCallback());
        discovery.request (true, 48000.0); // Read the completed cache before the old UI callback runs.
        const auto count = delivered.size();
        queue->drain();
        expectEquals ((int) delivered.size(), (int) count);
        expectEquals (scanner->calls.load(), 1);
        expectEquals ((double) delivered.back()["sampleRate"], 48000.0);

        discovery.request (true, 16000.0);
        expectEquals (status (delivered.back()), juce::String ("disabled"));
        expectEquals (scanner->calls.load(), 1);
    }

    void testDestructionDoesNotWaitOrDeliver()
    {
        beginTest ("Closing while a driver is blocked returns immediately and cancels the task");
        auto scanner = std::make_shared<ControlledScanner>();
        auto queue = std::make_shared<DiscoveryPostQueue>();
        int deliveries = 0;
        auto discovery = std::make_unique<AudioDeviceDiscovery>
            (scanner->callback(), [&] (const juce::var&) { ++deliveries; },
             [queue] (auto fn) { queue->post (std::move (fn)); });
        discovery->request (true, 44100.0);
        expect (scanner->started.wait (3000));
        const auto cancellation = scanner->currentCancellation();
        const auto before = juce::Time::getMillisecondCounterHiRes();
        discovery.reset();
        expect (juce::Time::getMillisecondCounterHiRes() - before < 1000.0);
        expect (cancellation && cancellation());
        scanner->release.signal();
        expect (scanner->finished.wait (3000));
        queue->drain();
        expectEquals (deliveries, 1);

        beginTest ("Closing after completion was posted prevents delivery into a released UI");
        scanner = std::make_shared<ControlledScanner>();
        scanner->release.signal();
        discovery = std::make_unique<AudioDeviceDiscovery>
            (scanner->callback(), [&] (const juce::var&) { ++deliveries; },
             [queue] (auto fn) { queue->post (std::move (fn)); });
        discovery->request (true, 48000.0);
        expect (queue->waitForCallback());
        discovery.reset();
        queue->drain();
        expectEquals (deliveries, 2, "Only the two synchronous scanning notifications may be delivered");
    }

    void testErrorsAreReported()
    {
        beginTest ("Scanner errors produce an error state and allow a fresh request");
        auto queue = std::make_shared<DiscoveryPostQueue>();
        std::vector<juce::var> delivered;
        int scans = 0;
        AudioDeviceDiscovery discovery ([&] (bool, double, const auto&) -> juce::var { ++scans; return {}; },
                                        [&] (const juce::var& value) { delivered.push_back (value); },
                                        [queue] (auto fn) { queue->post (std::move (fn)); });
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            discovery.request (true, 44100.0);
            expect (queue->waitForCallback());
            queue->drain();
            expectEquals (status (delivered.back()), juce::String ("error"));
        }
        expectEquals (scans, 2);
    }
};

static AudioDeviceDiscoveryTests audioDeviceDiscoveryTests;
