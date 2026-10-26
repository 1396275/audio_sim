#include "AudioDeviceNodes.h"

#include "ProcessingGraph.h"


// ─────────────────────────────────────────────────────────────────────────────
//  AudioDeviceManager
// ─────────────────────────────────────────────────────────────────────────────

AudioDeviceManager::AudioDeviceManager()
{
    // Deliberately empty now — see this class's own outputManagers/
    // inputManagers member comments in AudioDeviceNodes.h for the full
    // story. Each node's own manager is created lazily, on first use, by
    // getOrCreateOutputManager()/getOrCreateInputManager() below — there
    // is no longer a single, shared manager to eagerly initialise here.
}

bool AudioDeviceManager::applyToGraph (const juce::String& nodeId,
                                        const juce::String& deviceName,
                                        ProcessingGraph& graph)
{
    if (! AudioSim::isHardwareSampleRate (targetSampleRate))
    {
        if (auto* n = graph.findAudioOutNode (nodeId)) n->closeDevice();
        if (auto* n = graph.findAudioInNode (nodeId)) n->closeDevice();
        return false;
    }
    // v0.0.915 (Side finding 1, 2026-09-27) — fresh opens get the host's
    // target config; transferred devices (previously skipped entirely, so
    // they never followed a host buffer/rate change) are synced to it.
    if (auto* n = graph.findAudioOutNode (nodeId))
    {
        if (deviceName.isEmpty())
            n->closeDevice();           // always close, even if transferred
        else if (! n->wasTransferred() || n->getSelectedDeviceName() != deviceName)
            // Fix, v0.0.922 — "DAW" never touches hardware: give it the
            // never-initialised placeholder instead of creating a per-node
            // manager, whose initialiseWithDefaultDevices() OPENED the default
            // devices for nothing (in the host's process under Rosetta: Logic's
            // own output — see getAvailableDevicesVar()).
            n->openDevice (deviceName, deviceName == "DAW" ? dawPlaceholderManager : getOrCreateOutputManager (nodeId),
                           targetSampleRate, targetBlockSize);
        else
            n->syncDeviceConfig (targetSampleRate, targetBlockSize);
        if (const auto* channels = getChannelSelection (nodeId))
            applyChannelsToGraph (nodeId, *channels, graph);
        updateFeedbackProtection (graph);
        return true;
    }
    if (auto* n = graph.findAudioInNode (nodeId))
    {
        if (deviceName.isEmpty())
            n->closeDevice();           // always close, even if transferred
        else if (! n->wasTransferred() || n->getSelectedDeviceName() != deviceName)
            n->openDevice (deviceName, deviceName == "DAW" ? dawPlaceholderManager : getOrCreateInputManager (nodeId),
                           targetSampleRate, targetBlockSize);   // see the Out branch above
        else
            n->syncDeviceConfig (targetSampleRate, targetBlockSize);
        if (const auto* channels = getChannelSelection (nodeId))
            applyChannelsToGraph (nodeId, *channels, graph);
        updateFeedbackProtection (graph);
        return true;
    }
    return false;
}

void AudioDeviceManager::pruneDeletedNodeManagers (ProcessingGraph& graph)
{
    // See this method's own declaration comment in AudioDeviceNodes.h for
    // the full reasoning — called once per rebuild, after
    // applyDeviceSelections(), once the new graph's own node list is
    // final. erase() on a std::unique_ptr-valued map entry destroys the
    // juce::AudioDeviceManager, which closes whatever real device it may
    // still hold open.
    for (auto it = outputManagers.begin(); it != outputManagers.end(); )
        it = (graph.findAudioOutNode (it->first) == nullptr) ? outputManagers.erase (it) : std::next (it);
    for (auto it = inputManagers.begin(); it != inputManagers.end(); )
        it = (graph.findAudioInNode (it->first) == nullptr) ? inputManagers.erase (it) : std::next (it);
}

void AudioDeviceManager::updateFeedbackProtection (ProcessingGraph& graph)
{
    for (const auto& node : graph.getNodes())
        if (auto* input = dynamic_cast<AudioInDeviceNode*> (node.get()))
        {
            bool muted = false;
            const auto name = input->getSelectedDeviceName();
            if (muteFeedback && name.isNotEmpty())
                for (const auto& candidate : graph.getNodes())
                    if (auto* output = dynamic_cast<AudioOutDeviceNode*> (candidate.get()))
                        if (output->getSelectedDeviceName() == name) { muted = true; break; }
            input->setFeedbackMuted (muted);
        }
}

void AudioDeviceManager::applyDeviceSelections (ProcessingGraph& graph)
{
    for (const auto& [nodeId, deviceName] : selections)
        applyToGraph (nodeId, deviceName, graph);
    updateFeedbackProtection (graph);
}

void AudioDeviceManager::applyChannelsToGraph (const juce::String& nodeId,
                                                const std::vector<int>& channels,
                                                ProcessingGraph& graph)
{
    std::vector<int> mapped = channels;
    int deviceChannels = 0;
    if (auto* n = graph.findAudioOutNode (nodeId)) deviceChannels = n->getDeviceChannelCount();
    else if (auto* n = graph.findAudioInNode (nodeId)) deviceChannels = n->getDeviceChannelCount();
    mapped.erase (std::remove_if (mapped.begin(), mapped.end(), [deviceChannels](int ch) {
        return ch < 0 || (deviceChannels > 0 && ch >= deviceChannels);
    }), mapped.end());
    std::sort (mapped.begin(), mapped.end());
    mapped.erase (std::unique (mapped.begin(), mapped.end()), mapped.end());
    mapped.resize (std::min ((int) mapped.size(), AudioSim::maxAudioChannels));
    if (auto* n = graph.findAudioOutNode (nodeId)) n->setSelectedChannels (mapped);
    else if (auto* n = graph.findAudioInNode (nodeId)) n->setSelectedChannels (mapped);
}

void AudioDeviceManager::applyAllChannelSelections (ProcessingGraph& graph)
{
    for (const auto& [nodeId, channels] : channelSelections)
        applyChannelsToGraph (nodeId, channels, graph);
}

juce::var AudioDeviceManager::getAvailableDevicesVar (bool isStandalone, double sampleRate,
                                                    const std::function<bool()>& isCancelled)
{
    const auto cancelled = [&] { return isCancelled && isCancelled(); };
    if (cancelled()) return {};
    juce::Array<juce::var> outputs, inputs;
    const bool hardwareEnabled = AudioSim::isHardwareSampleRate (sampleRate);
    // No device manager, enumeration or capability query at simulation rates.
    if (hardwareEnabled)
    {
        if (! isStandalone)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id", "DAW"); obj->setProperty ("name", "DAW");
            obj->setProperty ("channelCount", 2); obj->setProperty ("supportsSampleRate", true);
            outputs.add (obj); inputs.add (obj);
        }
        if (cancelled()) return {};
        juce::AudioDeviceManager probeManager;
        if (cancelled()) return {};
        // JUCE's first getAvailableDeviceTypes() already scans each type.
        const auto& types = probeManager.getAvailableDeviceTypes();
        if (cancelled()) return {};
        for (auto* type : types)
        {
            if (cancelled()) return {};
            const auto typeName = type->getTypeName();
            if (cancelled()) return {};
            for (bool input : { false, true })
            {
                if (cancelled()) return {};
                const auto names = type->getDeviceNames (input);
                if (cancelled()) return {};
                for (const auto& name : names)
                {
                    if (cancelled()) return {};
                    // JUCE takes OUTPUT name first, INPUT name second.
                    std::unique_ptr<juce::AudioIODevice> dev (type->createDevice (input ? juce::String{} : name,
                                                                              input ? name : juce::String{}));
                    if (cancelled()) return {};
                    if (! dev) continue;
                    const int channels = input ? dev->getInputChannelNames().size() : dev->getOutputChannelNames().size();
                    if (cancelled()) return {};
                    const auto rates = dev->getAvailableSampleRates();
                    if (cancelled()) return {};
                    const auto sizes = dev->getAvailableBufferSizes();
                    if (cancelled()) return {};
                    auto obj = std::make_unique<juce::DynamicObject>();
                    obj->setProperty ("id", typeName + "::" + name);
                    obj->setProperty ("name", name + " (" + typeName + ")");
                    obj->setProperty ("channelCount", channels);
                    juce::Array<juce::var> rateValues, bufferValues;
                    for (auto rate : rates) { if (cancelled()) return {}; rateValues.add (rate); }
                    for (auto size : sizes) { if (cancelled()) return {}; bufferValues.add (size); }
                    obj->setProperty ("sampleRates", rateValues);
                    obj->setProperty ("bufferSizes", bufferValues);
                    obj->setProperty ("supportsSampleRate", rates.contains (sampleRate));
                    (input ? inputs : outputs).add (obj.release());
                }
            }
        }
    }
    if (cancelled()) return {};
    auto* root = new juce::DynamicObject();
    root->setProperty ("hardwareEnabled", hardwareEnabled);
    root->setProperty ("sampleRate", sampleRate);
    root->setProperty ("audioOutDevices", outputs);
    root->setProperty ("audioInDevices", inputs);
    return root;
}

// ─────────────────────────────────────────────────────────────────────────────
//  v0.0.915 (Side finding 1, 2026-09-27) — shared device-config helpers
// ─────────────────────────────────────────────────────────────────────────────

/** Falls back to the old NodeProcessor-style defaults only if no valid
 *  target was passed (should not happen once the processor is set up). */
static double validRate  (double sr) { return sr > 0.0 ? sr : 44100.0; }
static int    validBlock (int bs)    { return bs > 0   ? bs : 512; }

static juce::String describeDeviceConfig (juce::AudioDeviceManager& manager)
{
    if (auto* d = manager.getCurrentAudioDevice())
        return " at " + juce::String ((int) d->getCurrentSampleRate()) + " Hz / "
               + juce::String (d->getCurrentBufferSizeSamples());
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioOutDeviceNode
// ─────────────────────────────────────────────────────────────────────────────

// Probe before opening: JUCE otherwise silently chooses another sample rate.
// Frame size is logical; choose a driver-supported callback size independently.
static std::vector<int> defaultDeviceChannels (int deviceChannelCount)
{
    std::vector<int> channels;
    const int count = juce::jlimit (0, AudioSim::maxAudioChannels, deviceChannelCount);
    channels.reserve ((size_t) count);
    for (int ch = 0; ch < count; ++ch) channels.push_back (ch);
    return channels;
}

static juce::String openExactDevice (juce::AudioDeviceManager& manager,
                                     const juce::String& deviceId, bool input,
                                     double rate, int block, int& channelCount)
{
    if (! AudioSim::isHardwareSampleRate (rate)) return "Hardware disabled at simulation sample rate";
    for (auto* type : manager.getAvailableDeviceTypes())
    {
        for (const auto& name : type->getDeviceNames (input))
        {
            if (deviceId != name && deviceId != type->getTypeName() + "::" + name) continue;
            std::unique_ptr<juce::AudioIODevice> dev (type->createDevice (input ? juce::String{} : name,
                                                                      input ? name : juce::String{}));
            if (! dev || ! dev->getAvailableSampleRates().contains (rate))
                return "Device does not support " + juce::String (rate) + " Hz";
            channelCount = input ? dev->getInputChannelNames().size() : dev->getOutputChannelNames().size();
            if (channelCount <= 0) return "Device has no channels in the selected direction";
            const auto sizes = dev->getAvailableBufferSizes();
            int callbackSize = dev->getDefaultBufferSize();
            for (auto size : sizes)
                if (size > 0 && (callbackSize <= 0 || std::abs (size - block) < std::abs (callbackSize - block))) callbackSize = size;
            dev.reset();
            juce::BigInteger mask;
            mask.setRange (0, channelCount, true);
            juce::XmlElement config ("DEVICESETUP");
            config.setAttribute ("deviceType", type->getTypeName());
            config.setAttribute ("audioInputDeviceName", input ? name : juce::String{});
            config.setAttribute ("audioOutputDeviceName", input ? juce::String{} : name);
            config.setAttribute ("audioDeviceRate", rate);
            config.setAttribute ("audioDeviceBufferSize", callbackSize);
            config.setAttribute ("audioDeviceInChans", input ? mask.toString (2) : juce::String ("0"));
            config.setAttribute ("audioDeviceOutChans", input ? juce::String ("0") : mask.toString (2));
            auto error = manager.initialise (0, 0, &config, false);
            auto* actual = manager.getCurrentAudioDevice();
            if (error.isEmpty() && (! actual || actual->getCurrentSampleRate() != rate))
                error = "Device did not open at the exact requested sample rate";
            if (error.isNotEmpty()) manager.closeAudioDevice();
            return error;
        }
    }
    return "Device not found: " + deviceId;
}

void AudioOutDeviceNode::openDevice (const juce::String& deviceName,
                                      juce::AudioDeviceManager& manager,
                                      double targetSampleRate, int targetBlockSize)
{
    closeDevice();
    selectedDeviceName = deviceName;
    isDawDevice.store (deviceName == "DAW", std::memory_order_release);
    if (! AudioSim::isHardwareSampleRate (targetSampleRate) || deviceName.isEmpty() || isDawDevice) return;
    const auto error = openExactDevice (manager, deviceName, false, targetSampleRate, targetBlockSize, deviceChannelCount);
    if (error.isNotEmpty())
    {
        deviceChannelCount = 0;
        juce::Logger::writeToLog ("AudioOutDeviceNode: " + error);
        return;
    }
    setSelectedChannels (defaultDeviceChannels (deviceChannelCount));
    audioFifo.prepareFor (deviceChannelCount, (int) targetSampleRate, targetBlockSize);
    callbackHandedOver.store (false, std::memory_order_release);
    devManager = &manager;
    registeredDeviceName = deviceName;
    requestedSampleRate = targetSampleRate;
    requestedBlockSize = targetBlockSize;
    
    if (! holdsSleepGuard) { SleepGuard::acquire(); holdsSleepGuard = true; }
    manager.addAudioCallback (this);
    juce::Logger::writeToLog ("AudioOutDeviceNode: opened " + deviceName + describeDeviceConfig (manager));
}

void AudioOutDeviceNode::closeDevice()
{
    isDawDevice.store (false, std::memory_order_release);
    if (devManager != nullptr)
    {
        devManager->removeAudioCallback (this);
        devManager->closeAudioDevice();
        devManager = nullptr;
    }
    if (holdsSleepGuard) { SleepGuard::release(); holdsSleepGuard = false; }   // v0.0.915
    requestedSampleRate = 0.0;   // v0.0.915 — next open starts clean
    requestedBlockSize  = 0;
    registeredDeviceName.clear();
    transferred = false;  // allow openDevice() to work after close
}

void AudioOutDeviceNode::syncDeviceConfig (double targetSampleRate, int targetBlockSize)
{
    // v0.0.915 (Side finding 1, 2026-09-27) — a transferred device was never
    // reconfigured, so after a host buffer/rate change it kept its old
    // config (studio: SYSTEM-8 stayed at 44100/512 with the graph at 32).
    // Only acts when the target differs from what was last REQUESTED, so an
    // ordinary graph edit (same config) never touches the device, and a
    // device that can't honour a block size exactly isn't restarted on
    // every rebuild.
    if (devManager == nullptr || isDawDevice) return;
    const double sr = validRate  (targetSampleRate);
    const int    bs = validBlock (targetBlockSize);
    if (std::abs (sr - requestedSampleRate) < 0.01 && bs == requestedBlockSize) return;
    requestedSampleRate = sr;
    requestedBlockSize  = bs;
    if (auto* fifo = audioFifo.get()) fifo->prepareFor (deviceChannelCount, (int) sr, bs);
}

void AudioOutDeviceNode::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    if (auto* fifo = audioFifo.get()) fifo->prepareFor (deviceChannelCount, (int) sampleRate, maxBlockSize);
}

void AudioOutDeviceNode::process (int numSamples)
{
    for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
        outputAudio.copyFrom (ch, 0, inputAudio, ch, 0, numSamples);

    if (auto* fifo = audioFifo.get())
    {
        // Output mapping is applied by the hardware reader, not this writer.
        fifo->write (inputAudio, numSamples, {});
    }
}

void AudioOutDeviceNode::audioDeviceIOCallbackWithContext (
    const float* const*, int,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Shared-fifo fix, 2026-09-24 — handed over: output silence, never
    // touch the shared fifo (JUCE sums overlapping callbacks' outputs).
    if (callbackHandedOver.load (std::memory_order_acquire) || audioFifo.get() == nullptr)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
        return;
    }

    std::vector<int> chans;
    { juce::SpinLock::ScopedLockType sl (channelLock); chans = selectedChannels; }
    audioFifo->read (outputChannelData, numOutputChannels, numSamples, chans);

    // TEMPORARY diagnostic (2026-09-25) — log each new starved/trim event.
    {
        const int st = audioFifo->starvedCount.load (std::memory_order_relaxed);
        if (st != diagLastStarved)
        {
            diagLastStarved = st;
            diagLog ("AudioOut[" + registeredDeviceName + "] STARVED — underrun, silent block + re-prime (total " + juce::String (st) + ")");
        }
        const int tr = audioFifo->trimCount.load (std::memory_order_relaxed);
        if (tr != diagLastTrims)
        {
            diagLastTrims = tr;
            diagLog ("AudioOut[" + registeredDeviceName + "] TRIM — surplus discarded (total " + juce::String (tr) + ")");
        }
    }

    // TEMPORARY diagnostic (2026-09-25) — driver-level dropouts, polled
    // about every 250 ms on the device thread; logged only when the
    // device's own xrun count goes up.
    diagXrunSamples += numSamples;
    if (diagXrunSamples >= (int) (currentSampleRate * 0.25))
    {
        diagXrunSamples = 0;
        if (auto* mgr = devManager)
            if (auto* dev = mgr->getCurrentAudioDevice())
            {
                const int x = dev->getXRunCount();
                if (x >= 0)
                {
                    if (diagLastXruns >= 0 && x > diagLastXruns)
                        diagLog ("AudioOut[" + registeredDeviceName + "] XRUN — driver dropout (+"
                                 + juce::String (x - diagLastXruns) + ", total " + juce::String (x) + ")");
                    diagLastXruns = x;
                }
            }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioInDeviceNode
// ─────────────────────────────────────────────────────────────────────────────

void AudioInDeviceNode::openDevice (const juce::String& deviceName,
                                      juce::AudioDeviceManager& manager,
                                      double targetSampleRate, int targetBlockSize)
{
    closeDevice();
    streamAvailable.store (false, std::memory_order_release);
    selectedDeviceName = deviceName;
    isDawDevice.store (deviceName == "DAW", std::memory_order_release);
    if (! AudioSim::isHardwareSampleRate (targetSampleRate) || deviceName.isEmpty() || isDawDevice) return;
    const auto error = openExactDevice (manager, deviceName, true, targetSampleRate, targetBlockSize, deviceChannelCount);
    if (error.isNotEmpty())
    {
        deviceChannelCount = 0;
        juce::Logger::writeToLog ("AudioInDeviceNode: " + error);
        return;
    }
    setSelectedChannels (defaultDeviceChannels (deviceChannelCount));
    audioFifo.prepareFor (deviceChannelCount, (int) targetSampleRate, targetBlockSize);
    callbackHandedOver.store (false, std::memory_order_release);
    devManager = &manager;
    registeredDeviceName = deviceName;
    requestedSampleRate = targetSampleRate;
    requestedBlockSize = targetBlockSize;
    armStartupFade (deviceName);
    if (! holdsSleepGuard) { SleepGuard::acquire(); holdsSleepGuard = true; }
    streamAvailable.store (true, std::memory_order_release);
    manager.addAudioCallback (this);
    juce::Logger::writeToLog ("AudioInDeviceNode: opened " + deviceName + describeDeviceConfig (manager));
}

void AudioInDeviceNode::closeDevice()
{
    isDawDevice.store (false, std::memory_order_release);
    // transferAudioDevicesFrom also closes the old graph's nodes. A handed-
    // over node still feeds that graph from the shared queue until the swap.
    if (! callbackHandedOver.load (std::memory_order_acquire))
        streamAvailable.store (false, std::memory_order_release);
    if (devManager != nullptr)
    {
        devManager->removeAudioCallback (this);
        devManager->closeAudioDevice();
        devManager = nullptr;
    }
    if (holdsSleepGuard) { SleepGuard::release(); holdsSleepGuard = false; }   // v0.0.915
    requestedSampleRate = 0.0;   // v0.0.915 — next open starts clean
    requestedBlockSize  = 0;
    registeredDeviceName.clear();
    transferred = false;  // allow openDevice() to work after close
}

void AudioInDeviceNode::armStartupFade (const juce::String& deviceName)
{
    // Moved here unchanged from openDevice() (v0.0.915) so a reconfigure
    // restart can arm it too — see syncDeviceConfig().
    const int muteMs = StartupFadeRegistry::getMuteMs (deviceName);
    fadeListed.store (muteMs >= 0, std::memory_order_relaxed);   // v0.0.923 — dropout fade
    if (muteMs >= 0)
    {
        fadeInMuteMs.store (muteMs, std::memory_order_relaxed);
        fadeInArmed.store (true, std::memory_order_relaxed);
    }
    else
    {
        // Not listed: clear any arm left over from a previous,
        // listed device on this node that never got to start.
        fadeInArmed.store (false, std::memory_order_relaxed);
    }
}

void AudioInDeviceNode::refreshStartupFadeListing()
{
    const int muteMs = registeredDeviceName.isEmpty() ? -1 : StartupFadeRegistry::getMuteMs (registeredDeviceName);
    fadeListed.store (muteMs >= 0, std::memory_order_relaxed);
    if (muteMs >= 0)
        fadeInMuteMs.store (muteMs, std::memory_order_relaxed);
}

void AudioInDeviceNode::syncDeviceConfig (double targetSampleRate, int targetBlockSize)
{
    // v0.0.915 (Side finding 1, 2026-09-27) — same as AudioOutDeviceNode's
    // own syncDeviceConfig(); see its comment. Plus: reconfiguring restarts
    // the device's stream, and a listed device (FCA1616) pops on every
    // stream start, so the startup fade is re-armed when a restart actually
    // happened. Armed after the restart; the pop lands ~1 s after the first
    // audio, and the fade only starts counting at the first real read.
    if (devManager == nullptr || isDawDevice) return;
    const double sr = validRate  (targetSampleRate);
    const int    bs = validBlock (targetBlockSize);
    if (std::abs (sr - requestedSampleRate) < 0.01 && bs == requestedBlockSize) return;
    requestedSampleRate = sr;
    requestedBlockSize  = bs;
    if (auto* fifo = audioFifo.get()) fifo->prepareFor (deviceChannelCount, (int) sr, bs);
}

void AudioInDeviceNode::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    if (auto* fifo = audioFifo.get()) fifo->prepareFor (deviceChannelCount, (int) sampleRate, maxBlockSize);
}

void AudioInDeviceNode::process (int numSamples)
{
    // See passesThroughWhenDisabled()'s own comment in AudioDeviceNodes.h
    // for the full story. Always drains audioFifo (never backlogs), but
    // only actually populates outputAudio when enabled — while disabled,
    // drained audio is simply discarded, keeping this node correctly
    // "cut" (silent) rather than accidentally pass-through. Reuses the
    // already-allocated outputAudio member as the drain destination even
    // while disabled (rather than a new scratch buffer) — this project's
    // own established rule against heap allocation on the audio thread.
    auto* fifo = audioFifo.get();
    if (fifo == nullptr)
    {
        outputAudio.clear();
        return;
    }
    const bool gotAudio = fifo->read (outputAudio, numSamples);
    if (disabled || feedbackMuted.load())
        outputAudio.clear();

    // Fix, 2026-09-24 — open-time click: mute-then-ramp after a fresh open.
    // See fadeInArmed's own comment in AudioDeviceNodes.h.
    if (gotAudio && fadeInArmed.exchange (false, std::memory_order_relaxed))
        fadeInPos = 0;

    // v0.0.923 — dropout fade: audio back after a gap of at least
    // kDropoutSeconds → the same mute-then-ramp, for listed devices. See
    // fadeListed's own comment in AudioDeviceNodes.h.
    if (! gotAudio)
        starvedSamples += numSamples;
    else
    {
        if (fadeInPos < 0
            && fadeListed.load (std::memory_order_relaxed)
            && starvedSamples >= (juce::int64) (currentSampleRate * kDropoutSeconds))
        {
            fadeInPos = 0;
            diagLog ("AudioIn[" + registeredDeviceName + "] dropout of "
                     + juce::String ((int) (1000.0 * (double) starvedSamples / currentSampleRate))
                     + " ms - fade re-armed");
        }
        starvedSamples = 0;
    }

    if (fadeInPos >= 0)
    {
        const int muteLen = (int) (currentSampleRate * 0.001 * fadeInMuteMs.load (std::memory_order_relaxed));
        const int rampLen = juce::jmax (1, (int) (currentSampleRate * kFadeInRampSeconds));
        for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
        {
            float* d = outputAudio.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const int pos = fadeInPos + i;
                if (pos < muteLen)                d[i] = 0.0f;
                else if (pos < muteLen + rampLen) d[i] *= (float) (pos - muteLen) / (float) rampLen;
            }
        }
        fadeInPos += numSamples;
        if (fadeInPos >= muteLen + rampLen)
            fadeInPos = -1;
    }

    // TEMPORARY diagnostic (2026-09-25) — log each new starved/trim event.
    {
        const int st = audioFifo->starvedCount.load (std::memory_order_relaxed);
        if (st != diagLastStarved)
        {
            diagLastStarved = st;
            diagLog ("AudioIn[" + registeredDeviceName + "] STARVED — short read, silent block (total " + juce::String (st) + ")");
        }
        const int tr = audioFifo->trimCount.load (std::memory_order_relaxed);
        if (tr != diagLastTrims)
        {
            diagLastTrims = tr;
            diagLog ("AudioIn[" + registeredDeviceName + "] TRIM — surplus discarded (total " + juce::String (tr) + ")");
        }
    }
}

void AudioInDeviceNode::audioDeviceIOCallbackWithContext (
    const float* const* inputChannelData, int numInputChannels,
    float* const*, int, int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    // Shared-fifo fix, 2026-09-24 — handed over: the new node's callback
    // is the only writer now (see transferCallbackTo()).
    if (callbackHandedOver.load (std::memory_order_acquire) || audioFifo.get() == nullptr)
        return;

    std::vector<int> chans;
    { juce::SpinLock::ScopedLockType sl (channelLock); chans = selectedChannels; }

    // TEMPORARY diagnostic (2026-09-25) — click detector on the RAW input,
    // before anything in Patchy touches it. See the diag members' comment.
    {
        float worstJump = 0.0f;
        int   worstCh   = -1;
        for (int ch : chans)
        {
            if (ch < 0 || ch >= numInputChannels || ch >= kMaxFifoChans || inputChannelData[ch] == nullptr)
                continue;
            const float* x = inputChannelData[ch];
            float prev = diagDetectorPrimed ? diagPrevSample[(size_t) ch] : x[0];
            float peak = diagPeakJump[(size_t) ch];
            for (int i = 0; i < numSamples; ++i)
            {
                const float jump = std::abs (x[i] - prev);
                if (jump > kClickMinJump && jump > kClickPeakRatio * peak && jump > worstJump)
                {
                    worstJump = jump;
                    worstCh   = ch;
                }
                peak = juce::jmax (peak * kClickPeakDecay, jump);
                prev = x[i];
            }
            diagPrevSample[(size_t) ch] = prev;
            diagPeakJump[(size_t) ch]   = peak;
        }
        diagDetectorPrimed = true;
        diagSamplesSinceClick = juce::jmin (diagSamplesSinceClick + numSamples, 1 << 30);
        if (worstCh >= 0 && diagSamplesSinceClick >= (int) (currentSampleRate * 0.25))
        {
            diagSamplesSinceClick = 0;
            diagLog ("AudioIn[" + registeredDeviceName + "] CLICK in raw input — ch "
                     + juce::String (worstCh + 1) + ", jump " + juce::String (worstJump, 3));
        }
    }

    // TEMPORARY diagnostic (2026-09-25) — driver-level dropouts, polled
    // about every 250 ms on the device thread; logged only when the
    // device's own xrun count goes up.
    diagXrunSamples += numSamples;
    if (diagXrunSamples >= (int) (currentSampleRate * 0.25))
    {
        diagXrunSamples = 0;
        if (auto* mgr = devManager)
            if (auto* dev = mgr->getCurrentAudioDevice())
            {
                const int x = dev->getXRunCount();
                if (x >= 0)
                {
                    if (diagLastXruns >= 0 && x > diagLastXruns)
                        diagLog ("AudioIn[" + registeredDeviceName + "] XRUN — driver dropout (+"
                                 + juce::String (x - diagLastXruns) + ", total " + juce::String (x) + ")");
                    diagLastXruns = x;
                }
            }
    }

    audioFifo->write (inputChannelData, numInputChannels, numSamples, chans);
}
