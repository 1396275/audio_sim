#pragma once

#include <juce_events/juce_events.h>
#include "AudioSimConfig.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#if JUCE_WINDOWS
 #include <objbase.h>
#endif

// Device capability queries can block in third-party drivers. Keep them off the
// message thread, cache the result, and never join that work while closing a UI.
// request(), destruction, and delivery belong to the same (normally UI) thread;
// an injected Post must queue completion onto that thread too.
class AudioDeviceDiscovery final
{
public:
    using Cancelled = std::function<bool()>;
    using Scanner = std::function<juce::var (bool standalone, double rate, const Cancelled&)>;
    using Deliver = std::function<void (const juce::var&)>;
    using Post = std::function<void (std::function<void()>)>;

    AudioDeviceDiscovery (Scanner scanner, Deliver deliver,
                          Post post = [] (std::function<void()> fn)
                          {
                              juce::MessageManager::callAsync (std::move (fn));
                          })
        : state (std::make_shared<State> (std::move (scanner), std::move (deliver), std::move (post))) {}

    ~AudioDeviceDiscovery()
    {
        const auto shared = state;
        shared->closed.store (true);
        shared->generation.fetch_add (1);
        std::lock_guard<std::mutex> lock (shared->mutex);
        shared->pending.reset();
        shared->deliver = {};
    }

    // Never waits for a scanner, even if an earlier driver query is still busy.
    void request (bool standalone, double rate, bool forceRefresh = false)
    {
        const auto shared = state;
        juce::var cached;
        Deliver deliver;
        std::uint64_t generation = 0;
        bool startWorker = false;
        const bool enabled = AudioSim::isHardwareSampleRate (rate);
        {
            std::lock_guard<std::mutex> lock (shared->mutex);
            if (shared->closed.load()) return;
            deliver = shared->deliver;
            const auto index = standalone ? 1u : 0u;
            if (forceRefresh) shared->cache[index] = juce::var();

            // Repeated ready/list requests must not keep cancelling the very
            // scan they are waiting for (for example on every graph edit).
            const bool sameScan = enabled && ! forceRefresh && shared->scanGeneration.has_value()
                                  && shared->current.standalone == standalone
                                  && shared->current.rate == rate
                                  && *shared->scanGeneration == shared->generation.load()
                                  && shared->cache[index].isVoid();
            generation = sameScan ? shared->current.generation : shared->generation.fetch_add (1) + 1;
            shared->current = { standalone, rate, generation };

            if (! enabled)
            {
                shared->pending.reset();
            }
            else if (! shared->cache[index].isVoid())
            {
                cached = shared->cache[index];
                shared->pending.reset();
            }
            else if (! sameScan)
            {
                shared->pending = shared->current;
                shared->scanGeneration = generation;
                if (! shared->workerRunning)
                {
                    shared->workerRunning = true;
                    startWorker = true;
                }
            }
        }

        const auto status = ! enabled ? "disabled" : ! cached.isVoid() ? "ready" : "scanning";
        if (deliver) deliver (makeResult (cached, rate, status));
        if (startWorker && ! juce::Thread::launch ([shared] { runWorker (shared); }))
        {
            {
                std::lock_guard<std::mutex> lock (shared->mutex);
                shared->workerRunning = false;
                shared->pending.reset();
                shared->scanGeneration.reset();
            }
            deliverCompletion (shared, generation, makeResult ({}, rate, "error"));
        }
    }

private:
    struct Request
    {
        bool standalone = true;
        double rate = 0.0;
        std::uint64_t generation = 0;
    };

    struct State
    {
        State (Scanner scan, Deliver callback, Post dispatch)
            : scanner (std::move (scan)), deliver (std::move (callback)), post (std::move (dispatch)) {}

        std::mutex mutex;
        std::atomic<bool> closed { false };
        std::atomic<std::uint64_t> generation { 0 };
        Scanner scanner;
        Deliver deliver;
        Post post;
        juce::var cache[2]; // DAW and standalone have different virtual ports.
        Request current;
        std::optional<Request> pending;
        std::optional<std::uint64_t> scanGeneration;
        bool workerRunning = false;
    };

    static juce::var makeResult (const juce::var& capabilities, double rate, const char* status)
    {
        auto result = capabilities.clone();
        if (result.getDynamicObject() == nullptr) result = new juce::DynamicObject();
        auto* root = result.getDynamicObject();
        for (const auto* key : { "audioInDevices", "audioOutDevices" })
        {
            if (root->getProperty (key).getArray() == nullptr)
                root->setProperty (key, juce::Array<juce::var>());
            auto& devices = root->getProperty (key);
            for (auto& device : *devices.getArray())
            {
                if (auto* object = device.getDynamicObject())
                {
                    if (const auto* rates = object->getProperty ("sampleRates").getArray())
                    {
                        bool supports = false;
                        for (const auto& supported : *rates)
                            supports = supports || (double) supported == rate;
                        object->setProperty ("supportsSampleRate", supports);
                    }
                }
            }
        }
        root->setProperty ("discoveryState", status);
        root->setProperty ("hardwareEnabled", AudioSim::isHardwareSampleRate (rate));
        root->setProperty ("sampleRate", rate);
        return result;
    }

    static void deliverCompletion (const std::shared_ptr<State>& shared, std::uint64_t generation,
                                   const juce::var& result)
    {
        Deliver deliver;
        {
            std::lock_guard<std::mutex> lock (shared->mutex);
            if (shared->closed.load() || shared->generation.load() != generation) return;
            deliver = shared->deliver;
        }
        if (deliver) deliver (result);
    }

    static void runWorker (const std::shared_ptr<State>& shared)
    {
       #if JUCE_WINDOWS
        // WASAPI and other Windows audio APIs may require COM on the calling
        // thread. A driver query must never borrow the message thread's COM.
        struct ComScope
        {
            ComScope() : result (CoInitializeEx (nullptr, COINIT_MULTITHREADED)) {}
            ~ComScope() { if (SUCCEEDED (result)) CoUninitialize(); }
            HRESULT result;
        } comScope;
       #endif

        for (;;)
        {
            Request request;
            {
                std::lock_guard<std::mutex> lock (shared->mutex);
                if (shared->closed.load() || ! shared->pending.has_value())
                {
                    shared->workerRunning = false;
                    return;
                }
                request = *shared->pending;
                shared->pending.reset();
            }

            const Cancelled cancelled = [weak = std::weak_ptr<State> (shared), generation = request.generation]
            {
                const auto current = weak.lock();
                return current == nullptr || current->closed.load() || current->generation.load() != generation;
            };
            juce::var capabilities;
            bool failed = false;
            try
            {
                if (! cancelled() && shared->scanner)
                    capabilities = shared->scanner (request.standalone, request.rate, cancelled);
                failed = capabilities.getDynamicObject() == nullptr;
            }
            catch (...) { failed = true; }

            if (cancelled()) continue;
            {
                std::lock_guard<std::mutex> lock (shared->mutex);
                if (shared->closed.load() || shared->generation.load() != request.generation) continue;
                if (shared->scanGeneration == request.generation) shared->scanGeneration.reset();
                if (! failed) shared->cache[request.standalone ? 1u : 0u] = capabilities;
            }

            auto result = makeResult (capabilities, request.rate, failed ? "error" : "ready");
            // Only completed scans use Post. Neither Post nor user callbacks
            // execute with the state mutex held, including in synchronous tests.
            shared->post ([shared, generation = request.generation, result = std::move (result)]
            {
                deliverCompletion (shared, generation, result);
            });
        }
    }

    std::shared_ptr<State> state;
    JUCE_DECLARE_NON_COPYABLE (AudioDeviceDiscovery)
};
