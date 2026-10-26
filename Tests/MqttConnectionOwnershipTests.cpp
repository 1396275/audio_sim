// Patchy — MQTT connection ownership/lifetime tests (JUCE UnitTest framework)
//
// Doesn't touch mosquitto or the network at all. These tests validate the
// exact shared_ptr ownership pattern MqttConnection/PublishConnection
// (Source/MqttDeviceNodes.h) rely on for thread-safety, using a lightweight
// stand-in object instead of a real mosquitto connection — the real bug
// (mosquitto_connect_async()'s blocking DNS resolution) isn't practical to
// exercise reliably in a unit test, but the correctness property the whole
// fix depends on — does a shared connection object survive a background
// thread being mid-work when its owner drops its reference, and get
// cleaned up exactly once once that thread actually finishes — is pure
// C++ object lifetime under concurrency, and doesn't need a broker at all.
//
// See Architecture.md's Phase 6 table entry ("Move MQTT connect off the
// message thread", 2026-08-13) for the full design this validates.
//
// Timing note: these tests use real sleeps/polling to coordinate with
// background threads, since there's no way to synchronise on "has the
// thread reached this point yet" without adding synchronisation the real
// production code doesn't have either. Generous margins are used
// throughout to keep this robust under normal load; treat a rare failure
// here as worth re-running once before assuming a real regression.

#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Stand-in for MqttConnection/PublishConnection — something a background
// thread can hold via shared_ptr while "doing work" (standing in for the
// one blocking mosquitto_connect_async() call), while an "owner" might
// drop its own reference at any point. liveCount tracks how many instances
// currently exist, the same way a leak/double-free would show up as a
// count that never reaches zero, or goes negative.
struct FakeConnection
{
    explicit FakeConnection (std::atomic<int>& liveCountRef) : liveCount (liveCountRef)
    {
        ++liveCount;
    }
    ~FakeConnection()
    {
        --liveCount;
    }

    // Stands in for the one blocking mosquitto_connect_async() call.
    void simulateSlowWork (int millisToBlock)
    {
        juce::Thread::sleep (millisToBlock);
        workCompleted = true;
    }

    std::atomic<int>& liveCount;
    std::atomic<bool> workCompleted { false };

    JUCE_DECLARE_NON_COPYABLE (FakeConnection)
};

// ─────────────────────────────────────────────────────────────────────────────
class MqttConnectionOwnershipTests : public juce::UnitTest
{
public:
    MqttConnectionOwnershipTests() : juce::UnitTest ("MqttConnectionOwnership", "Patchy") {}

    void runTest() override
    {
        testConnectionOutlivesDroppedOwnerReference();
        testConnectionCleansUpOnceThreadFinishes();
        testRapidReconfigureDoesNotLeakOrDoubleFree();
        testConcurrentReadWriteOfGuardedPointer();
    }

private:
    // Polls a condition up to timeoutMs, sleeping briefly between checks —
    // used throughout instead of a flat sleep, so these tests take only as
    // long as they actually need to, rather than always waiting a fixed
    // worst-case duration.
    template <typename Predicate>
    bool waitUntil (Predicate pred, int timeoutMs)
    {
        int waited = 0;
        while (! pred() && waited < timeoutMs) { juce::Thread::sleep (5); waited += 5; }
        return pred();
    }

    void testConnectionOutlivesDroppedOwnerReference()
    {
        beginTest ("Dropping the owner's reference does not destroy a connection a background thread still holds");

        std::atomic<int> liveCount { 0 };
        auto conn = std::make_shared<FakeConnection> (liveCount);
        expectEquals (liveCount.load(), 1);

        std::atomic<bool> threadStarted { false };
        juce::WaitableEvent mayFinish;
        std::jthread worker ([conn, &threadStarted, &mayFinish]
        {
            threadStarted = true;
            mayFinish.wait(); // Keep the connection alive until the owner checks it.
            conn->simulateSlowWork (0);
        });

        expect (waitUntil ([&] { return threadStarted.load(); }, 1000),
                "background thread should have started");

        // This is the exact moment the real teardown() does the equivalent
        // of — drop our own reference while the worker still owns its copy.
        // An explicit gate avoids a scheduler-dependent 200 ms assumption.
        conn.reset();

        // The object must still be alive — the thread's own copy keeps it so.
        expectEquals (liveCount.load(), 1);

        // FakeConnection's destructor writes to liveCount. The worker must
        // release its capture before this stack-owned counter is destroyed.
        mayFinish.signal();
        worker.join();
        expectEquals (liveCount.load(), 0);
    }

    void testConnectionCleansUpOnceThreadFinishes()
    {
        beginTest ("Connection is destroyed exactly once, after the last (thread-held) reference releases it");

        std::atomic<int> liveCount { 0 };
        std::atomic<bool> threadFinished { false };

        auto conn = std::make_shared<FakeConnection> (liveCount);
        std::jthread worker ([conn, &threadFinished]
        {
            conn->simulateSlowWork (100);
            threadFinished = true;
            // The thread's own shared_ptr copy (conn, captured by value)
            // goes out of scope here, at the end of this lambda — this is
            // the point that should actually trigger destruction, if this
            // was the last reference.
        });

        conn.reset();   // owner drops its reference immediately, same as teardown()

        expect (waitUntil ([&] { return threadFinished.load(); }, 2000),
                "background thread should have finished");
        worker.join(); // Also joins on a timeout: no captures may outlive this test.

        // Joining guarantees that the lambda and its last shared_ptr capture
        // have actually been destroyed, beyond merely setting the done flag.
        expectEquals (liveCount.load(), 0);
    }

    void testRapidReconfigureDoesNotLeakOrDoubleFree()
    {
        beginTest ("Many rapid reconfigurations do not leak or double-free connections");

        std::atomic<int> liveCount { 0 };
        constexpr int kIterations = 50;
        std::vector<std::jthread> workers;
        workers.reserve (kIterations);

        for (int i = 0; i < kIterations; ++i)
        {
            auto conn = std::make_shared<FakeConnection> (liveCount);
            workers.emplace_back ([conn]
            {
                conn->simulateSlowWork (2);   // short — mirrors settings
                                              // changing again before the
                                              // previous attempt could
                                              // possibly finish
            });
            // conn (this iteration's local copy) goes out of scope here,
            // immediately — the next iteration's configure doesn't wait for
            // this one's launched thread at all, same as the real
            // configure()/teardown() never blocking on an in-flight attempt.
        }

        expect (waitUntil ([&] { return liveCount.load() == 0; }, 5000),
                "all connections should eventually be cleaned up, none leaked");

        for (auto& worker : workers) worker.join();

        expectEquals (liveCount.load(), 0);
    }

    void testConcurrentReadWriteOfGuardedPointer()
    {
        beginTest ("Concurrent reassignment (message thread) and reads (audio thread) of the guarded pointer don't crash or corrupt state");

        // Mirrors MqttSubscribeNode/MqttPublishNode's own connectionLock +
        // connection member pattern directly, rather than just testing
        // FakeConnection's lifetime in isolation — this exercises the
        // actual SpinLock-guarded access pattern under real concurrent
        // pressure between two threads, the same shape as configure()
        // (message thread) racing against process() (audio thread).
        struct Owner
        {
            juce::SpinLock connectionLock;
            std::shared_ptr<FakeConnection> connection;

            void reconfigure (std::atomic<int>& liveCount)
            {
                auto fresh = std::make_shared<FakeConnection> (liveCount);
                const juce::SpinLock::ScopedLockType sl (connectionLock);
                connection = fresh;
            }

            bool readIsAlive()
            {
                std::shared_ptr<FakeConnection> conn;
                { const juce::SpinLock::ScopedLockType sl (connectionLock); conn = connection; }
                return conn != nullptr;
            }
        };

        std::atomic<int> liveCount { 0 };
        Owner owner;
        std::atomic<bool> reconfigureThreadDone { false };

        // "Message thread" — reconfigures repeatedly, same as rapid settings changes.
        std::jthread worker ([&owner, &liveCount, &reconfigureThreadDone]
        {
            for (int i = 0; i < 200; ++i)
                owner.reconfigure (liveCount);
            reconfigureThreadDone = true;
        });

        // "Audio thread" — this test itself, reading concurrently at a
        // tight cadence, same shape as process() being called every block.
        for (int i = 0; i < 500; ++i)
            owner.readIsAlive();

        expect (waitUntil ([&] { return reconfigureThreadDone.load(); }, 2000),
                "reconfigure thread should finish its loop");
        worker.join();

        // Once the worker has joined, only the final owner-held connection
        // remains. It must release exactly once when the owner lets it go.
        expect (true, "completed without crashing");
        expectEquals (liveCount.load(), 1, "only the final owner-held connection remains");
        owner.connection.reset(); // The joined worker can no longer access owner.
        expectEquals (liveCount.load(), 0, "the final connection is released exactly once");
    }
};

// Register the test suite
static MqttConnectionOwnershipTests mqttConnectionOwnershipTests;
