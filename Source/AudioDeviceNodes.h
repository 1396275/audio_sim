#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "NodeProcessor.h"
#include "DeviceAudioFifo.h"
#include "GraphModel.h"
#include "StartupFadeRegistry.h"
#include "SleepGuard.h"
#include "DiagLog.h"   // TEMPORARY diagnostic (2026-09-25)
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <array>
#include <functional>

class ProcessingGraph;

// ─────────────────────────────────────────────────────────────────────────────
/**
 * AudioOutDeviceNode  (nodeType 4)
 *
 * One audio input port — its channel width follows the connected signal.
 * Sends that audio to the selected physical audio output device.
 */
class AudioOutDeviceNode : public NodeProcessor,
                            private juce::AudioIODeviceCallback
{
public:
    explicit AudioOutDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Audio) {}

    ~AudioOutDeviceNode() override { closeDevice(); }  // closeDevice() is idempotent

    // v0.0.915 (Side finding 1, 2026-09-27) — the device now opens at the
    // graph's TARGET config (host rate/block, passed in by Patchy's own
    // AudioDeviceManager) instead of this node's currentSampleRate/
    // currentBlockSize, which are still NodeProcessor's 44100/512 defaults
    // at open time (openDevice() runs before the new graph's prepare()).
    void openDevice (const juce::String& deviceName,
                     juce::AudioDeviceManager& deviceManager,
                     double targetSampleRate, int targetBlockSize);
    void closeDevice();
    // v0.0.915 (Side finding 1) — for a TRANSFERRED device (never reopened):
    // reconfigure it to the target config if that differs from what was last
    // requested for it. See the definition for the full reasoning.
    void syncDeviceConfig (double targetSampleRate, int targetBlockSize);

    juce::String getSelectedDeviceName() const { return registeredDeviceName; }
    void markTransferred() { transferred = true; }
    bool wasTransferred()  const { return transferred; }

    bool getIsDawDevice()  const { return isDawDevice.load (std::memory_order_acquire); }
    template <typename NodeT>
    void transferCallbackTo (NodeT& dst)
    {
        if (devManager == nullptr || audioFifo.get() == nullptr) return;

        // Real bug found and fixed 2026-09-03, revised twice more the
        // same day as deeper root causes kept surfacing — see this
        // project's own SessionLog.md for the full, multi-stage story.
        // This method now handles ONLY the callback registration itself
        // — register the new callback FIRST, then remove the old one,
        // per JUCE's own documented support for briefly overlapping
        // callbacks, avoiding any "nothing registered" window at all.
        //
        // [Superseded 2026-09-24 by the shared-fifo fix further down —
        // the paragraphs below are kept for history.]
        // The audio content itself — this node's own audioFifo — is
        // deliberately NOT transferred here anymore. An earlier version
        // of this fix did it right here, on the message thread, but that
        // left a genuine, if small, gap: the OLD node keeps being
        // actively processed (and keeps writing fresh audio into its own
        // fifo) for a real stretch of this same rebuild's own remaining
        // work, happening entirely AFTER this transfer runs — any audio
        // written during that window was simply lost, never making it
        // into the new fifo's own snapshot. Doing the fifo transfer here
        // also risked a genuine cross-thread race: this call runs on the
        // message thread, while the OLD node — still live, still part of
        // the graph the audio thread is actively processing until the
        // swap — could be concurrently writing to that exact same fifo
        // via its own process() call, violating AbstractFifo's own
        // single-reader/single-writer contract.
        //
        // Both problems are solved by doing the fifo transfer separately,
        // on the audio thread itself, immediately after the swap in
        // processBlock() — see that function's own comment for where and
        // why. At that exact moment, the audio thread is the only thread
        // touching either fifo at all, with the old graph no longer live
        // and the message thread's own rebuild long since finished —
        // eliminating both the lost-audio window and the cross-thread
        // race entirely, rather than merely shrinking either one.
        dst.devManager           = devManager;
        dst.registeredDeviceName = registeredDeviceName;
        dst.currentSampleRate    = currentSampleRate;
        dst.currentBlockSize     = currentBlockSize;
        dst.deviceChannelCount   = deviceChannelCount;
        dst.setSelectedChannels (getSelectedChannels());
        // v0.0.915 — the device's last requested config and the idle-sleep
        // hold follow the device to its new node (the device stays open).
        dst.requestedSampleRate  = requestedSampleRate;
        dst.requestedBlockSize   = requestedBlockSize;
        dst.holdsSleepGuard      = holdsSleepGuard;
        holdsSleepGuard          = false;
        // Shared-fifo fix, 2026-09-24 — the new node takes over THIS
        // node's own fifo (shared, not copied) before its callback is
        // registered. From here until processBlock()'s swap, the device
        // talks to the new node while the graph still runs this old one —
        // with separate fifos that window was a real gap (the device read
        // an empty fifo / wrote into one nobody read). Sharing one fifo
        // makes the handover seamless by construction: old graph and
        // device keep meeting in the same fifo, and after the swap the new
        // node simply carries on in it. Still single-producer/single-
        // consumer: the graph side is one audio thread processing either
        // the old node or (after the swap) the new one, never both.
        // Replaces the old swap-time transferFifoFrom() copy, which
        // allocated on the audio thread and was immediately wiped by the
        // swap's own prepare() anyway.
        dst.audioFifo.shareFrom (audioFifo);
        // TEMPORARY diagnostic (2026-09-25) — start the new node's counters
        // from the shared fifo's current totals, so it only logs new events.
        dst.diagLastStarved = audioFifo->starvedCount.load (std::memory_order_relaxed);
        dst.diagLastTrims   = audioFifo->trimCount.load (std::memory_order_relaxed);
        devManager->addAudioCallback (&dst);
        // Shared-fifo fix, 2026-09-24 — JUCE may call both callbacks for
        // a block while they overlap (add-then-remove, above/below). With
        // one shared fifo, the old callback must stop touching it the
        // moment the new one is live, or that block would be consumed
        // (Out) or written (In) twice.
        callbackHandedOver.store (true, std::memory_order_release);
        devManager->removeAudioCallback (this);
        devManager = nullptr; registeredDeviceName = {};
    }

    // NodeProcessor
    void prepare (double sampleRate, int maxBlockSize) override;
    int resolveAudioChannelCount (int routedInputChannels) const override
    {
        return juce::jlimit (0, AudioSim::maxAudioChannels, routedInputChannels);
    }
    int getOutputAudioChannels (int = 0) const override { return 0; }
    void process (int numSamples) override;

    juce::String selectedDeviceName;

private:
    // AudioIODeviceCallback — called by the output device thread
    void audioDeviceIOCallbackWithContext (const float* const*,
                                           int,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        juce::ignoreUnused (device);
    }
    void audioDeviceStopped()                            override {}

    // Lock-free multi-channel audio FIFO (graph thread writes, device thread reads)
    static constexpr int kFifoFrames    = 8192;
    static constexpr int kMaxFifoChans  = 256;

    SharedDeviceAudioFifo audioFifo;
    // Shared-fifo fix, 2026-09-24 — set in transferCallbackTo() once the
    // new node's callback is registered; this node's own device callback
    // then leaves the shared fifo alone (see that function's comment).
    std::atomic<bool> callbackHandedOver { false };

    // TEMPORARY diagnostic (2026-09-25) — random-click investigation, see
    // DiagLog.h. Last-seen counter values, so each new starved/trim event
    // and each device xrun is logged once, with a timestamp, when it
    // happens. lastXruns starts at -2 ("not read yet") so a node taking
    // over an already-running device doesn't report its whole history.
    int diagLastStarved  = 0;
    int diagLastTrims    = 0;
    int diagLastXruns    = -2;
    int diagXrunSamples  = 0;

    juce::AudioDeviceManager* devManager = nullptr;
    juce::String              registeredDeviceName;
    bool                      transferred = false;
    std::atomic<bool>         isDawDevice { false };
    // v0.0.915 (Side finding 1, 2026-09-27) — the rate/block last REQUESTED
    // for this device (not what it reports back: a device that can't honour
    // a block size exactly must not be re-requested, i.e. restarted, on
    // every rebuild). 0 = nothing requested yet.
    double                    requestedSampleRate = 0.0;
    int                       requestedBlockSize  = 0;
    // v0.0.915 — true while this node owns one SleepGuard hold (acquired on
    // a successful open, released in closeDevice(), moved on transfer).
    bool                      holdsSleepGuard     = false;

    // selectedChannels is written from the message thread and read from the
    // device callback thread — protect with a SpinLock.
    std::vector<int>    selectedChannels;
    juce::SpinLock      channelLock;
    int                 deviceChannelCount = 0;

public:
    void setSelectedChannels (std::vector<int> chans)
    {
        juce::SpinLock::ScopedLockType sl (channelLock);
        chans.erase (std::remove_if (chans.begin(), chans.end(), [](int ch) { return ch < 0; }), chans.end());
        std::sort (chans.begin(), chans.end());
        chans.erase (std::unique (chans.begin(), chans.end()), chans.end());
        chans.resize (std::min (chans.size(), (size_t) AudioSim::maxAudioChannels));
        selectedChannels = std::move (chans);
    }
    std::vector<int> getSelectedChannels() const
    {
        juce::SpinLock::ScopedLockType sl (channelLock);
        return selectedChannels;
    }
    int getDeviceChannelCount() const { return deviceChannelCount; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioOutDeviceNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * AudioInDeviceNode  (nodeType 3)
 *
 * One audio output port — one logical channel per selected physical input.
 * Receives audio asynchronously from a physical audio input device via a
 * lock-free FIFO; drains it into outputAudio each process() call.
 */
class AudioInDeviceNode : public NodeProcessor,
                           private juce::AudioIODeviceCallback
{
public:
    explicit AudioInDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Audio) {}

    ~AudioInDeviceNode() override { closeDevice(); }   // closeDevice() is idempotent

    // v0.0.915 (Side finding 1, 2026-09-27) — the device now opens at the
    // graph's TARGET config (host rate/block, passed in by Patchy's own
    // AudioDeviceManager) instead of this node's currentSampleRate/
    // currentBlockSize, which are still NodeProcessor's 44100/512 defaults
    // at open time (openDevice() runs before the new graph's prepare()).
    void openDevice (const juce::String& deviceName,
                     juce::AudioDeviceManager& deviceManager,
                     double targetSampleRate, int targetBlockSize);
    void closeDevice();
    // v0.0.915 (Side finding 1) — for a TRANSFERRED device (never reopened):
    // reconfigure it to the target config if that differs from what was last
    // requested for it. See the definition for the full reasoning.
    void syncDeviceConfig (double targetSampleRate, int targetBlockSize);

    juce::String getSelectedDeviceName() const { return registeredDeviceName; }
    void markTransferred() { transferred = true; }
    bool wasTransferred()  const { return transferred; }

    bool getIsDawDevice()  const { return isDawDevice.load (std::memory_order_acquire); }
    template <typename NodeT>
    void transferCallbackTo (NodeT& dst)
    {
        if (devManager == nullptr || audioFifo.get() == nullptr) return;

        // Real bug found and fixed 2026-09-03, revised twice more the
        // same day as deeper root causes kept surfacing — see this
        // project's own SessionLog.md for the full, multi-stage story.
        // This method now handles ONLY the callback registration itself
        // — register the new callback FIRST, then remove the old one,
        // per JUCE's own documented support for briefly overlapping
        // callbacks, avoiding any "nothing registered" window at all.
        //
        // [Superseded 2026-09-24 by the shared-fifo fix further down —
        // the paragraphs below are kept for history.]
        // The audio content itself — this node's own audioFifo — is
        // deliberately NOT transferred here anymore. An earlier version
        // of this fix did it right here, on the message thread, but that
        // left a genuine, if small, gap: the OLD node keeps being
        // actively processed (and keeps writing fresh audio into its own
        // fifo) for a real stretch of this same rebuild's own remaining
        // work, happening entirely AFTER this transfer runs — any audio
        // written during that window was simply lost, never making it
        // into the new fifo's own snapshot. Doing the fifo transfer here
        // also risked a genuine cross-thread race: this call runs on the
        // message thread, while the OLD node — still live, still part of
        // the graph the audio thread is actively processing until the
        // swap — could be concurrently writing to that exact same fifo
        // via its own process() call, violating AbstractFifo's own
        // single-reader/single-writer contract.
        //
        // Both problems are solved by doing the fifo transfer separately,
        // on the audio thread itself, immediately after the swap in
        // processBlock() — see that function's own comment for where and
        // why. At that exact moment, the audio thread is the only thread
        // touching either fifo at all, with the old graph no longer live
        // and the message thread's own rebuild long since finished —
        // eliminating both the lost-audio window and the cross-thread
        // race entirely, rather than merely shrinking either one.
        dst.devManager           = devManager;
        dst.registeredDeviceName = registeredDeviceName;
        dst.currentSampleRate    = currentSampleRate;
        dst.currentBlockSize     = currentBlockSize;
        dst.deviceChannelCount   = deviceChannelCount;
        dst.setSelectedChannels (getSelectedChannels());
        // v0.0.915 — the device's last requested config and the idle-sleep
        // hold follow the device to its new node (the device stays open).
        dst.requestedSampleRate  = requestedSampleRate;
        dst.requestedBlockSize   = requestedBlockSize;
        dst.holdsSleepGuard      = holdsSleepGuard;
        holdsSleepGuard          = false;
        // Fix, 2026-09-24 — a fade-in armed by openDevice() but not yet
        // started (no real audio read yet) must follow the device to its
        // new node, or a rebuild landing in that window would skip it.
        dst.fadeInMuteMs.store (fadeInMuteMs.load (std::memory_order_relaxed), std::memory_order_relaxed);
        dst.fadeInArmed.store (fadeInArmed.load (std::memory_order_relaxed), std::memory_order_relaxed);
        dst.fadeListed.store  (fadeListed.load  (std::memory_order_relaxed), std::memory_order_relaxed);   // v0.0.923
        // Shared-fifo fix, 2026-09-24 — same as AudioOutDeviceNode's own
        // transferCallbackTo(); see its comment.
        dst.audioFifo.shareFrom (audioFifo);
        // TEMPORARY diagnostic (2026-09-25) — start the new node's counters
        // from the shared fifo's current totals, so it only logs new events.
        dst.diagLastStarved = audioFifo->starvedCount.load (std::memory_order_relaxed);
        dst.diagLastTrims   = audioFifo->trimCount.load (std::memory_order_relaxed);
        dst.streamAvailable.store (streamAvailable.load (std::memory_order_acquire), std::memory_order_release);
        devManager->addAudioCallback (&dst);
        // Shared-fifo fix, 2026-09-24 — JUCE may call both callbacks for
        // a block while they overlap (add-then-remove, above/below). With
        // one shared fifo, the old callback must stop touching it the
        // moment the new one is live, or that block would be consumed
        // (Out) or written (In) twice.
        callbackHandedOver.store (true, std::memory_order_release);
        devManager->removeAudioCallback (this);
        devManager = nullptr; registeredDeviceName = {};
    }

    // NodeProcessor
    void prepare (double sampleRate, int maxBlockSize) override;
    int resolveAudioChannelCount (int routedInputChannels) const override
    {
        if (isDawDevice.load (std::memory_order_acquire))
            return juce::jlimit (0, AudioSim::maxAudioChannels, routedInputChannels);
        if (! streamAvailable.load (std::memory_order_acquire)) return 0;
        juce::SpinLock::ScopedLockType sl (channelLock);
        return std::min (AudioSim::maxAudioChannels, (int) selectedChannels.size());
    }
    int getOutputAudioChannels (int = 0) const override { return resolveAudioChannelCount (getAudioChannelCount()); }
    void setFeedbackMuted (bool muted) { feedbackMuted.store (muted); }
    void process (int numSamples) override;

    // Real bug found 2026-09-15 (same root cause as MidiInDeviceNode's own
    // identical fix — see that class's own process() comment for the full
    // story): the hardware audio callback below keeps writing into
    // audioFifo regardless of `disabled`, entirely independent of whether
    // process() is being called. Skipping process() entirely while
    // disabled (this project's default "cut" behaviour) left audioFifo's
    // own 8192-frame ring buffer silently filling with real, unread audio
    // the whole time this node stayed disabled — capped at ~186ms of
    // backlog (juce::AbstractFifo never overwrites unread data), but
    // never self-draining either, so the very next process() call after
    // re-enabling would start reading audio from before this node was
    // disabled, at a de-synced ~186ms delay that never recovers on its
    // own. Fixed by keeping process() running unconditionally (still
    // draining audioFifo, so it never backlogs at all) while disabled,
    // clearing outputAudio instead of populating it from the fifo — see
    // process()'s own implementation for the actual logic.
    bool passesThroughWhenDisabled() const override { return true; }

    juce::String selectedDeviceName;

private:
    // Independent of manager ownership: an old graph still drains its shared
    // queue after callback transfer and remains available until it retires.
    std::atomic<bool> streamAvailable { false };
    std::atomic<bool> feedbackMuted { false };
    void audioDeviceIOCallbackWithContext (const float* const*,
                                           int,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        juce::ignoreUnused (device);
    }
    void audioDeviceStopped()                           override {}

    static constexpr int kFifoFrames    = 8192;
    static constexpr int kMaxFifoChans  = 256;

    SharedDeviceAudioFifo audioFifo;
    // Shared-fifo fix, 2026-09-24 — set in transferCallbackTo() once the
    // new node's callback is registered; this node's own device callback
    // then leaves the shared fifo alone (see that function's comment).
    std::atomic<bool> callbackHandedOver { false };

    // TEMPORARY diagnostic (2026-09-25) — random-click investigation, see
    // DiagLog.h. Last-seen counter values, so each new starved/trim event
    // and each device xrun is logged once, with a timestamp, when it
    // happens. lastXruns starts at -2 ("not read yet") so a node taking
    // over an already-running device doesn't report its whole history.
    int diagLastStarved  = 0;
    int diagLastTrims    = 0;
    int diagLastXruns    = -2;
    int diagXrunSamples  = 0;

    // TEMPORARY diagnostic (2026-09-25) — click detector on the RAW input
    // (device thread, before the fifo). A click is a sample-to-sample jump
    // above kClickMinJump AND well above the largest jump seen on that
    // channel over the last ~100 ms (decaying peak) — so a loud square or
    // saw wave, whose edges are big but regular, doesn't keep firing,
    // while an isolated jump in otherwise smooth audio does. Rate-limited
    // to one line per ~250 ms per node.
    static constexpr float kClickMinJump   = 0.3f;
    static constexpr float kClickPeakRatio = 3.0f;
    static constexpr float kClickPeakDecay = 0.9995f;   // per sample, ~-19 dB/100 ms at 44.1 kHz
    std::array<float, kMaxFifoChans> diagPrevSample {};
    std::array<float, kMaxFifoChans> diagPeakJump   {};
    bool diagDetectorPrimed    = false;
    int  diagSamplesSinceClick = 1 << 30;

    juce::AudioDeviceManager* devManager = nullptr;
    juce::String              registeredDeviceName;
    bool                      transferred = false;
    std::atomic<bool>         isDawDevice { false };
    // v0.0.915 (Side finding 1, 2026-09-27) — the rate/block last REQUESTED
    // for this device (not what it reports back: a device that can't honour
    // a block size exactly must not be re-requested, i.e. restarted, on
    // every rebuild). 0 = nothing requested yet.
    double                    requestedSampleRate = 0.0;
    int                       requestedBlockSize  = 0;
    // v0.0.915 — true while this node owns one SleepGuard hold (acquired on
    // a successful open, released in closeDevice(), moved on transfer).
    bool                      holdsSleepGuard     = false;

    // Fix, 2026-09-24 — open-time click. Some interfaces (confirmed: the
    // Behringer FCA1616) emit a sharp pop in their own input stream when it
    // starts — audible even on the interface's own headphone out, so it's
    // hardware-side. Patchy can't stop it at the source, but can keep it
    // out of its own audio path: after each FRESH open (never after a mere
    // transfer between graphs, which doesn't restart the stream), the first
    // real audio read is held silent for fadeInMuteMs, then ramped
    // up linearly over kFadeInRampSeconds. Counting starts at the first
    // successful read, not at open, so a slow-starting device can't
    // silently use up the window before its pop arrives. fadeInArmed is
    // set on the message thread (openDevice) and consumed on the audio
    // thread (process); fadeInPos is audio-thread-only (-1 = inactive).
    // Measured 2026-09-24 (pop-timing diagnostic): the FCA1616's pop lands
    // ~987 ms after the first audio (peak 0.914), a second spike at ~1045 ms
    // (0.878), then a decaying tail (0.053 -> 0.010 by ~1335 ms) — likely
    // the interface unmuting its inputs ~1 s after the stream starts. The
    // mute covers both spikes with margin; the ramp swallows the tail.
    //
    // Targeted, 2026-09-24: the fade is now only armed for devices listed
    // in StartupFadeRegistry (see that header), and the mute duration comes
    // from the registry per device (FCA1616's measured default: 1250 ms)
    // instead of a fixed constant. The ramp stays fixed — it only smooths
    // the return. fadeInMuteMs is set together with fadeInArmed in
    // openDevice() (message thread) and read when the fade starts in
    // process() (audio thread).
    static constexpr double kFadeInRampSeconds = 0.150;
    std::atomic<int>          fadeInMuteMs { 0 };
    std::atomic<bool>         fadeInArmed { false };
    int                       fadeInPos   = -1;

    // v0.0.923 (2026-10-04) — dropout fade. The FCA1616 was seen (user log,
    // 2026-10-04) stopping its stream for ~250–280 ms every few minutes
    // (~20–24 consecutive STARVED blocks, nothing reopened by Patchy) and
    // producing its usual stream-start pop (jump 0.807) ~1 s after the audio
    // came back. For a device on the startup-fade list, a gap of at least
    // kDropoutSeconds without audio re-runs the same mute-then-ramp when the
    // audio returns, so that pop is silenced too. fadeListed: is this
    // device on the list (set by armStartupFade(), refreshed from the
    // message thread by refreshStartupFadeListing() so the panel's tick box
    // applies at once). starvedSamples: audio thread only.
    static constexpr double   kDropoutSeconds = 0.100;
    std::atomic<bool>         fadeListed { false };
    juce::int64               starvedSamples = 0;
    // Arms (listed device) or clears (unlisted) the fade for deviceName.
    // Message thread. Used by openDevice() and, v0.0.915, syncDeviceConfig().
    void armStartupFade (const juce::String& deviceName);

public:
    /** v0.0.923 — message thread (WebBridge activity timer): re-reads whether
     *  this device is on the startup-fade list (and its mute length), for
     *  the dropout fade. See fadeListed. */
    void refreshStartupFadeListing();

private:

    // selectedChannels is written from the message thread and read from the
    // device callback thread — protect with a SpinLock.
    std::vector<int>  selectedChannels;
    juce::SpinLock    channelLock;
    int               deviceChannelCount = 0;

public:
    void setSelectedChannels (std::vector<int> chans)
    {
        juce::SpinLock::ScopedLockType sl (channelLock);
        chans.erase (std::remove_if (chans.begin(), chans.end(), [](int ch) { return ch < 0; }), chans.end());
        std::sort (chans.begin(), chans.end());
        chans.erase (std::unique (chans.begin(), chans.end()), chans.end());
        chans.resize (std::min (chans.size(), (size_t) AudioSim::maxAudioChannels));
        selectedChannels = std::move (chans);
    }
    std::vector<int> getSelectedChannels() const
    {
        juce::SpinLock::ScopedLockType sl (channelLock);
        return selectedChannels;
    }
    int getDeviceChannelCount() const { return deviceChannelCount; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioInDeviceNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * AudioDeviceManager
 *
 * Owns a juce::AudioDeviceManager shared between all audio device nodes.
 * Handles device open/close and persists selections.
 */
class AudioDeviceManager
{
public:
    AudioDeviceManager();

    void storeSelection (const juce::String& nodeId,
                         const juce::String& deviceName)
    {
        selections[nodeId] = deviceName;
    }

    juce::String getSelection (const juce::String& nodeId) const
    {
        auto it = selections.find (nodeId);
        return it != selections.end() ? it->second : juce::String{};
    }

    bool applyToGraph (const juce::String& nodeId,
                       const juce::String& deviceName,
                       ProcessingGraph& graph);

    /** v0.0.915 (Side finding 1, 2026-09-27) — the host's current rate/block,
     *  set by PatchyProcessor::rebuildProcessingGraph() before device
     *  selections are applied. Fresh opens use it, and transferred devices
     *  are reconfigured to it when it changes (host buffer/rate change). */
    void setTargetAudioConfig (double sampleRate, int blockSize)
    {
        targetSampleRate = sampleRate;
        targetBlockSize  = blockSize;
    }

    void applyDeviceSelections (ProcessingGraph& graph);
    void setMuteFeedback (bool enabled) { muteFeedback = enabled; }
    void updateFeedbackProtection (ProcessingGraph& graph);

    void setDevice (const juce::String& nodeId,
                    const juce::String& deviceName,
                    ProcessingGraph& graph)
    {
        storeSelection (nodeId, deviceName);
        applyToGraph   (nodeId, deviceName, graph);
    }

    /** Store and apply a channel selection for an audio device node.
     *  Returns the new deviceChannelCount so the caller can validate. */
    void storeChannelSelection (const juce::String& nodeId, const std::vector<int>& channels)
    {
        channelSelections[nodeId] = channels;
    }

    void setAudioDeviceChannels (const juce::String& nodeId,
                                  const std::vector<int>& channels,
                                  ProcessingGraph& graph)
    {
        channelSelections[nodeId] = channels;
        applyChannelsToGraph (nodeId, channels, graph);
    }

    void applyChannelsToGraph (const juce::String& nodeId,
                                const std::vector<int>& channels,
                                ProcessingGraph& graph);

    void applyAllChannelSelections (ProcessingGraph& graph);

    const std::vector<int>* getChannelSelection (const juce::String& nodeId) const
    {
        auto it = channelSelections.find (nodeId);
        return it != channelSelections.end() ? &it->second : nullptr;
    }

    static juce::var getAvailableDevicesVar (bool isStandalone, double sampleRate,
                                           const std::function<bool()>& isCancelled = {});
    double getTargetSampleRate() const { return targetSampleRate; }
    // Detach every node callback before calling this.
    void closeAllHardware()
    {
        outputManagers.clear();
        inputManagers.clear();
    }

    /** Root-cause fix, 2026-09-11 — see this class's own outputManagers/
     *  inputManagers members below for the full story. Lazily creates and
     *  returns THIS specific node's own, independent juce::AudioDeviceManager
     *  — never a single, application-wide shared one. */
    juce::AudioDeviceManager& getOrCreateOutputManager (const juce::String& nodeId)
    {
        return getOrCreateManager (outputManagers, nodeId, 0, 2);
    }
    juce::AudioDeviceManager& getOrCreateInputManager (const juce::String& nodeId)
    {
        return getOrCreateManager (inputManagers, nodeId, 2, 0);
    }

    /** Closes and discards the per-node manager (and whatever real hardware
     *  device it may still hold open) for any nodeId whose own node no
     *  longer exists in the given (already fully rebuilt) graph — called
     *  once per rebuild, right after applyDeviceSelections(), so it only
     *  ever runs once the new graph's own node list is final and accurate.
     *  Without this, a genuinely deleted node's own device would otherwise
     *  stay open forever — unlike the lightweight, harmless string/int
     *  entries left behind in selections/channelSelections above (which
     *  this method deliberately leaves untouched, matching that existing,
     *  established, accepted-as-harmless pattern), an abandoned
     *  juce::AudioDeviceManager can hold a real, exclusive hardware
     *  connection, blocking every other node or application from using
     *  that same device even though the user has already deleted the node
     *  that opened it. */
    void pruneDeletedNodeManagers (ProcessingGraph& graph);

private:
    // v0.0.922 — handed to nodes set to "DAW" (their openDevice() returns
    // before using it). NEVER initialised, so it never opens a device.
    juce::AudioDeviceManager dawPlaceholderManager;

    juce::AudioDeviceManager& getOrCreateManager (
        std::unordered_map<juce::String, std::unique_ptr<juce::AudioDeviceManager>>& managers,
        const juce::String& nodeId, int numInputChannels, int numOutputChannels)
    {
        auto it = managers.find (nodeId);
        if (it != managers.end()) return *it->second;
        auto newManager = std::make_unique<juce::AudioDeviceManager>();
        // Fix, v0.0.940 (2026-10-17) — initialise with NO channels needed so
        // no device is opened here: (2, 0) / (0, 2) used to open and start the
        // system's DEFAULT input / output at JUCE's default config the moment
        // a new Audio node arrived, before openDevice() switched it to the
        // node's own device — disturbing that default device (often the very
        // one already playing, e.g. BlackHole). Same lesson as v0.0.922's
        // device listing. openDevice() sets explicit channels, which JUCE
        // takes as the channel need (setAudioDeviceSetup).
        juce::ignoreUnused (numInputChannels, numOutputChannels);
        // openDevice initialises one exact device/config.
        auto& ref = *newManager;
        managers[nodeId] = std::move (newManager);
        return ref;
    }

    // Root cause of the "last device wins" bug (found and fixed
    // 2026-09-11): every AudioInDeviceNode/AudioOutDeviceNode used to
    // share these same two, single, application-wide juce::AudioDeviceManager
    // instances — a real juce::AudioDeviceManager represents ONE active
    // device connection at a time, so each node's own openDevice() call
    // simply replaced whatever the previous node had already set, with
    // every node's own callback staying registered but all of them then
    // receiving audio from whichever device was opened last. Now one
    // independent manager per node, keyed by nodeId (not by C++ object
    // instance — see transferCallbackTo()'s own comment in this file for
    // why: the map entry must persist stably across a graph rebuild, the
    // same way the old, single, PatchyProcessor-owned managers used to,
    // for that existing transfer mechanism to keep working unchanged).
    std::unordered_map<juce::String, std::unique_ptr<juce::AudioDeviceManager>> outputManagers;
    std::unordered_map<juce::String, std::unique_ptr<juce::AudioDeviceManager>> inputManagers;
    std::unordered_map<juce::String, juce::String>       selections;
    std::unordered_map<juce::String, std::vector<int>>   channelSelections;
    double targetSampleRate = 0.0;   // v0.0.915 — see setTargetAudioConfig()
    int    targetBlockSize  = 512;
    bool muteFeedback = true;
};
