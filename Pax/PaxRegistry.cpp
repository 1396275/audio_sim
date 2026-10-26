#include "PaxRegistry.h"
#include <algorithm>

PaxValueType paxValueTypeFromTag (int tag)
{
    switch (tag)
    {
        case PAX_VALUETYPE_MQTT:   return PaxValueType::Mqtt;
        case PAX_VALUETYPE_OSC:    return PaxValueType::Osc;
        case PAX_VALUETYPE_DMX:    return PaxValueType::Dmx;
        case PAX_VALUETYPE_UDP:    return PaxValueType::Udp;
        case PAX_VALUETYPE_ARTNET: return PaxValueType::ArtNet;
        case PAX_VALUETYPE_MIDI:   return PaxValueType::Midi;
        default:                   return PaxValueType::Generic;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  PaxRegistry
// ─────────────────────────────────────────────────────────────────────────────

void PaxRegistry::load (const std::vector<PaxScanner::ScanResult>& results)
{
    entries.clear();
    nameToIndex.clear();

    for (const auto& r : results)
    {
        if (! r.valid) continue;

        // The scanner already validated and opened this DLL. Retain that
        // handle so its static initialization runs only once. A caller that
        // supplies an older/manually assembled ScanResult can still load it.
        auto lib = r.lib;
        if (! lib)
        {
            lib = std::make_shared<juce::DynamicLibrary>();
            if (! lib->open (r.file.getFullPathName()))
            {
                juce::Logger::writeToLog ("Registry: failed to open " + r.file.getFileName());
                continue;
            }
        }

        Entry e;
        e.name     = r.name;
        e.vendor   = r.vendor;
        e.version  = r.version;
        e.nodeType = r.nodeType;
        e.file     = r.file;
        e.lib      = lib;

        // Read optional port counts from descriptor
        if (auto* getDesc = (const PAX_Descriptor*(*)()) lib->getFunction ("PAX_getDescriptor"))
        {
            if (auto* desc = getDesc())
            {
                e.audioInputs  = desc->audioInputs;
                e.audioOutputs = desc->audioOutputs;
                e.midiInputs   = desc->midiInputs;
                e.midiOutputs  = desc->midiOutputs;
            }
        }

        if (auto getChannels = (int(*)()) lib->getFunction ("PAX_getMaxAudioChannels"))
        {
            e.hasChannelCapability = true;
            e.maxAudioChannels = juce::jlimit (1, PAX_MAX_AUDIO_CHANNELS, getChannels());
        }

        // Value port counts — separate optional exports, not descriptor fields
        // (avoids an ABI-breaking struct change for already-compiled Pax
        // binaries; see PaxAPI.h's PAX_Descriptor nodeType=4 comment).
        // Static, no instance needed — called once here at scan time.
        if (auto* getValIn = (int(*)()) lib->getFunction ("PAX_getValueInputCount"))
            e.valueInputs = getValIn();
        if (auto* getValOut = (int(*)()) lib->getFunction ("PAX_getValueOutputCount"))
            e.valueOutputs = getValOut();

        // Per-port value types — one more optional export beyond the counts
        // above, indexed by port position. Not exporting this (or an older
        // Pax that only has the count functions) means every value port
        // defaults to PAX_VALUETYPE_GENERIC (0), same as before per-port
        // typing existed.
        //
        // Always populate with exactly one entry per declared port, even
        // when the optional export isn't present at all — a Pax that
        // deliberately relies on the generic default (like MqttToValuePax's
        // output) must still get e.g. valueOutputTypes = [0], not an empty
        // array. An empty array here was a real bug: anything reconstructing
        // ports from this array (rather than from a live process() context,
        // which has its own separate in-code default) would see zero
        // output ports at all for such a Pax, not one generic one —
        // found via the sidebar showing MqttToValuePax as MQTT's own
        // colour instead of Converter/fuchsia, because with an empty
        // valueOutputTypes it looked like a pure-MQTT-source with no
        // output side to compare against at all.
        auto* getValInType  = (int(*)(int)) lib->getFunction ("PAX_getValueInputType");
        for (int i = 0; i < e.valueInputs; ++i)
            e.valueInputTypes.push_back (getValInType ? getValInType (i) : PAX_VALUETYPE_GENERIC);
        auto* getValOutType = (int(*)(int)) lib->getFunction ("PAX_getValueOutputType");
        for (int i = 0; i < e.valueOutputs; ++i)
            e.valueOutputTypes.push_back (getValOutType ? getValOutType (i) : PAX_VALUETYPE_GENERIC);

        // Colour category override — see PaxAPI.h's PAX_getColourCategory
        // doc for the full rule (only consulted for the one case the
        // auto-detection genuinely can't resolve: a pure source/sink Pax).
        if (auto* getColourCat = (int(*)()) lib->getFunction ("PAX_getColourCategory"))
            e.colourCategory = getColourCat();

        e.create  = (Entry::CreateFn)  lib->getFunction ("PAX_create");
        e.destroy = (Entry::DestroyFn) lib->getFunction ("PAX_destroy");
        e.prepare       = (Entry::PrepareFn)       lib->getFunction ("PAX_prepare");
        e.process       = (Entry::ProcessFn)       lib->getFunction ("PAX_process");
        e.getParamCount = (Entry::GetParamCountFn) lib->getFunction ("PAX_getParameterCount");
        e.getParamInfo  = (Entry::GetParamInfoFn)  lib->getFunction ("PAX_getParameterInfo");
        e.getParam      = (Entry::GetParamFn)      lib->getFunction ("PAX_getParameter");
        e.setParam          = (Entry::SetParamFn)          lib->getFunction ("PAX_setParameter");
        e.getAudioOutCount  = (Entry::GetAudioOutCountFn) lib->getFunction ("PAX_getAudioOutputCount");
        e.isParamReadOnly   = (Entry::IsParamReadOnlyFn)  lib->getFunction ("PAX_isParameterReadOnly");
        e.isParamLiveSynced = (Entry::IsParamLiveSyncedFn) lib->getFunction ("PAX_isParameterLiveSynced");
        e.audioPassthroughInput = (Entry::AudioPassthroughInputFn) lib->getFunction ("PAX_getAudioPassthroughInput");
        e.midiPassthrough = (Entry::MidiPassthroughFn) lib->getFunction ("PAX_getMidiPassthrough");

        if (! e.create || ! e.destroy || ! e.prepare || ! e.process)
        {
            juce::Logger::writeToLog ("Registry: symbol missing in " + r.file.getFileName());
            continue;
        }

        // UI-ready notifications only need immutable defaults, not a new
        // plugin instance each time. Query them here on the loading thread;
        // active processors continue querying their own live parameters.
        if (e.getParamCount && e.getParamInfo)
        {
            auto destroy = [fn = e.destroy](PAX_Instance* instance) { fn (instance); };
            std::unique_ptr<PAX_Instance, decltype (destroy)> temporary (e.create(), destroy);
            if (temporary)
            {
                const int count = e.getParamCount (temporary.get());
                for (int index = 0; index < count; ++index)
                {
                    PAX_ParameterInfo info {};
                    e.getParamInfo (temporary.get(), index, &info);
                    ParameterDescriptor descriptor;
                    descriptor.index = index;
                    descriptor.name = info.name != nullptr ? juce::String (info.name) : juce::String{};
                    descriptor.minValue = info.minValue;
                    descriptor.maxValue = info.maxValue;
                    descriptor.defaultValue = info.defaultValue;
                    descriptor.step = info.step;
                    descriptor.readOnly = e.isParamReadOnly
                        && e.isParamReadOnly (temporary.get(), index) != 0;
                    e.parameters.push_back (std::move (descriptor));
                }
            }
            else
                juce::Logger::writeToLog ("Registry: could not read parameter defaults for '" + r.name + "'");
        }

        nameToIndex[e.name] = static_cast<int>(entries.size());
        entries.push_back (std::move (e));
        juce::Logger::writeToLog ("Registry: loaded Pax '" + r.name + "'");
    }
}

std::unique_ptr<NodeProcessor>
PaxRegistry::createNode (const juce::String& nodeId,
                                 const juce::String& paxName)
{
    auto it = nameToIndex.find (paxName);
    if (it == nameToIndex.end())
    {
        juce::Logger::writeToLog ("Registry: unknown Pax '" + paxName + "'");
        return nullptr;
    }
    return std::make_unique<DynamicPaxProcessor> (nodeId, entries[static_cast<size_t>(it->second)]);
}

std::vector<juce::String>
PaxRegistry::getPaxNames (int nodeType) const
{
    std::vector<juce::String> names;
    for (const auto& e : entries)
        if (nodeType == 0 || e.nodeType == nodeType)
            names.push_back (e.name);
    return names;
}

// ─────────────────────────────────────────────────────────────────────────────
//  DynamicPaxProcessor
// ─────────────────────────────────────────────────────────────────────────────

DynamicPaxProcessor::DynamicPaxProcessor (const juce::String&             nodeId,
                                             const PaxRegistry::Entry& e)
    : NodeProcessor (nodeId, static_cast<NodeProcessor::Type> (e.nodeType)),
      lib             (e.lib),
      fnCreate        (e.create),
      fnDestroy       (e.destroy),
      fnPrepare       (e.prepare),
      fnProcess       (e.process),
      fnGetParamCount (e.getParamCount),
      fnGetParamInfo  (e.getParamInfo),
      fnGetParam      (e.getParam),
      fnSetParam          (e.setParam),
      fnGetAudioOutCount  (e.getAudioOutCount),
      fnIsParamReadOnly   (e.isParamReadOnly),
      fnIsParamLiveSynced (e.isParamLiveSynced),
      fnAudioPassthroughInput (e.audioPassthroughInput),
      fnMidiPassthrough (e.midiPassthrough),
      paxName       (e.name),
      valueOutputTypes (e.valueOutputTypes),
      lastGenericValueValues (e.valueOutputTypes.size(), 0.0f)
{
    maxAudioChannels = e.maxAudioChannels;
    hasChannelCapability = e.hasChannelCapability;
    jassert (fnCreate != nullptr);
    instance = fnCreate();
    jassert (instance != nullptr);

    // Set effective audio port counts
    int paxType = e.nodeType;
    audioInputCount  = e.audioInputs  > 0 ? e.audioInputs  : (paxType == 2 || paxType == 3 ? 1 : 0);
    audioOutputCount = e.audioOutputs > 0 ? e.audioOutputs : (paxType == 2 || paxType == 3 ? 1 : 0);

    // Allocate per-port audio buffers (size set properly in prepare())
    allocatePortBuffers (audioInputCount, audioOutputCount, 512);
}

DynamicPaxProcessor::~DynamicPaxProcessor()
{
    if (instance != nullptr && fnDestroy != nullptr)
    {
        fnDestroy (instance);
        instance = nullptr;
    }
    // lib shared_ptr releases here — .dylib unloaded when last processor is gone
}

void DynamicPaxProcessor::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    compatibilityChannel.setSize (16, maxBlockSize);
    allocatePortBuffers (audioInputCount, audioOutputCount, maxBlockSize);
    if (instance != nullptr && fnPrepare != nullptr)
        fnPrepare (instance, sampleRate, maxBlockSize);
}

void DynamicPaxProcessor::process (int numSamples)
{
    if (instance == nullptr) return;

    // Disable/Enable feature, 2026-09-11 — see PaxAPI.h's own
    // PAX_getAudioPassthroughInput doc comment for the full reasoning.
    // Never calls this Pax's own fnProcess() while disabled — for each
    // declared audio output port, either copies the input this Pax
    // itself declared as that output's pass-through source, or leaves it
    // silently cleared (the default "cut" — already correctly empty from
    // resetBuffers(), nothing more to do). Handles both the per-port and
    // single-port buffer storage styles, matching the same "prefer
    // per-port, fall back to single-buffer" pattern the normal,
    // non-disabled path below already establishes.
    if (disabled)
    {
        for (int p = 0; p < audioOutputCount; ++p)
        {
            const int inIdx = getAudioPassthroughInput (p);
            if (inIdx < 0) continue;   // no mapping for this output — stays cut

            const bool perPortOut = ! outputAudioBuffers.empty() && p < (int) outputAudioBuffers.size();
            const bool perPortIn  = ! inputAudioBuffers.empty()  && inIdx < (int) inputAudioBuffers.size();

            juce::AudioBuffer<float>& out = perPortOut ? outputAudioBuffers[(size_t) p]    : outputAudio;
            juce::AudioBuffer<float>& in  = perPortIn  ? inputAudioBuffers[(size_t) inIdx] : inputAudio;

            const int ch = std::min (in.getNumChannels(), out.getNumChannels());

            for (int c = 0; c < ch; ++c)
                out.copyFrom (c, 0, in, c, 0, numSamples);
        }

        // MIDI side — deliberately simple/single-port (see
        // PAX_getMidiPassthrough's own doc comment in PaxAPI.h): either
        // the whole MIDI stream passes through unchanged, or it stays cut
        // (already correctly empty from resetBuffers()).
        if (getMidiPassthrough())
            outputMidi = inputMidi;

        return;
    }

    // ── Convert juce::MidiBuffer → PAX_MidiEvent array ───────────────────
    int inCount = 0;
    for (const auto& meta : inputMidi)
    {
        if (inCount >= kMaxMidiEvents) break;
        const auto& msg = meta.getMessage();
        auto& ev = midiInBuf[inCount++];
        ev.sampleOffset = meta.samplePosition;
        ev.byteCount    = (uint8_t) std::min (msg.getRawDataSize(), 3);
        std::memcpy (ev.bytes, msg.getRawData(), ev.byteCount);
    }

    // Port-major layout. Older DLLs keep a two-channel stride; updated
    // DLLs explicitly advertise their maximum channels without changing v4 ABI.
    constexpr int maxPorts = 8;
    float* audioInPtrs[maxPorts * PAX_MAX_AUDIO_CHANNELS] {};
    float* audioOutPtrs[maxPorts * PAX_MAX_AUDIO_CHANNELS] {};
    const int channels = ! hasChannelCapability
        ? 2
        : getAudioChannelCount();
    compatibilityChannel.clear();
    if (nodeType != NodeProcessor::Type::Midi)
    {
        for (int p = 0; p < std::min (maxPorts, audioInputCount); ++p)
        {
            auto& buffer = inputAudioBuffers.empty() ? inputAudio : inputAudioBuffers[(size_t) p];
            for (int ch = 0; ch < channels; ++ch)
                audioInPtrs[p * channels + ch] = ch < buffer.getNumChannels()
                    ? buffer.getWritePointer (ch) : compatibilityChannel.getWritePointer (p * 2);
        }
        for (int p = 0; p < std::min (maxPorts, audioOutputCount); ++p)
        {
            auto& buffer = outputAudioBuffers.empty() ? outputAudio : outputAudioBuffers[(size_t) p];
            for (int ch = 0; ch < channels; ++ch)
                audioOutPtrs[p * channels + ch] = ch < buffer.getNumChannels()
                    ? buffer.getWritePointer (ch) : compatibilityChannel.getWritePointer (p * 2 + 1);
        }
    }

    // ── Call into the Pax ──────────────────────────────────────────────
    int outCount      = 0;
    int valueOutCount = 0;
    PAX_ProcessContext ctx {};
    ctx.audioIn        = audioInPtrs;
    ctx.audioOut       = audioOutPtrs;
    ctx.numChannels    = channels;
    ctx.numSamples     = numSamples;
    ctx.midiIn         = midiInBuf;
    ctx.midiInCount    = inCount;
    ctx.midiOut        = midiOutBuf;
    ctx.midiOutCount   = &outCount;
    ctx.midiMaxCount   = kMaxMidiEvents;
    // Value buffers — now live
    // Reset before each block so an unset PAX_Value::portIndex (or any
    // other field a Pax doesn't explicitly write) always reads as a safe
    // zero, never a stale value left over from a previous block's write
    // to that same array slot. Matches the guarantee documented in
    // PaxAPI.h's PAX_Value comment.
    outputValues.fill (PAX_Value {});
    ctx.valuesIn       = inputValues.data();
    ctx.valueInCount   = inputValueCount;
    ctx.valuesOut      = outputValues.data();
    ctx.valueOutCount  = &valueOutCount;
    ctx.valueMaxCount  = kMaxValueEvents;

    // DMX universe (API v4) — separate wide-payload path from Values above.
    // Zero the output frame and clear the valid flag before each call, same
    // reasoning as outputValues.fill() just above: a Pax that only writes
    // one or two channels shouldn't need to zero the other 510 itself, and
    // an unset flag must never read as "holds real data" left over from a
    // previous block.
    outputDmxFrame.fill (0);
    outputDmxFrameValid = false;
    ctx.dmxFrameIn        = inputDmxFrameValid ? inputDmxFrame.data() : nullptr;
    ctx.dmxFrameInValid   = inputDmxFrameValid;
    ctx.dmxFrameOut       = outputDmxFrame.data();
    ctx.dmxFrameOutValid  = &outputDmxFrameValid;

    fnProcess (instance, &ctx);
    outputValueCount = valueOutCount;

    // Capture any freshly-written generic value into the persistent
    // lastGenericValueValues mirror, before outputValues[] itself gets
    // wiped again at the top of the NEXT call to this function (see
    // outputValues.fill() above). Without this, Phase 5's own live
    // readout-on-hover feature reads 0 almost all the time even for a
    // Pax sending continuously-changing values — found via a user's own
    // real-world OSC test, 2026-09-10. Deliberately bounded to
    // outputValues[0 .. valueOutCount-1] — the genuinely-written prefix
    // this specific block — rather than scanning the whole (fixed-size,
    // 256-slot) array: every untouched slot beyond valueOutCount also
    // defaults to type=GENERIC(0)/portIndex=0 after the fill above,
    // indistinguishable from a genuine write at index 0 — scanning the
    // whole array (an earlier, wrong version of this same fix) would
    // "match" an untouched slot just as readily as a real one, silently
    // overwriting the persisted value with 0 every single block and
    // defeating this fix's own entire purpose.
    for (size_t p = 0; p < valueOutputTypes.size(); ++p)
    {
        if (valueOutputTypes[p] != PAX_VALUETYPE_GENERIC) continue;
        for (int i = 0; i < valueOutCount; ++i)
        {
            if (outputValues[(size_t) i].type == PAX_TYPE_GENERIC
                && outputValues[(size_t) i].portIndex == (uint8_t) p)
            {
                lastGenericValueValues[p] = outputValues[(size_t) i].value;
                break;
            }
        }
    }

    // Value events also count as activity for flash purposes — this was
    // missing until now, which meant a Pax that only ever writes value
    // events (e.g. MqttToValuePax) never registered any activity at all,
    // not even with the wrong colour. recordMidiActivity() is already the
    // de facto generic "this node had discrete-event activity" signal in
    // practice — every non-MIDI protocol node (DMX/ArtNet/OSC/UDP/MQTT)
    // already calls this exact same counter for its own activity, despite
    // none of them being MIDI either, so this is consistent with existing
    // precedent, not a special case.
    if (valueOutCount > 0)
        recordMidiActivity (valueOutCount);

    // ── Convert PAX_MidiEvent array → juce::MidiBuffer ───────────────────
    outputMidi.clear();
    for (int i = 0; i < outCount; ++i)
    {
        const auto& ev = midiOutBuf[i];

        // Guard: skip empty or obviously invalid events
        if (ev.byteCount == 0 || ev.bytes[0] == 0)
            continue;

        const int clampedLen = std::max (1, std::min ((int) ev.byteCount, 3));

        // Use createRaw so JUCE builds the message from the exact bytes
        // without the 3-byte constructor asserting on 1/2-byte messages
        outputMidi.addEvent (
            juce::MidiMessage (ev.bytes, clampedLen, 0.0),
            ev.sampleOffset);
    }
    if (outCount > 0)
        recordMidiActivity (outCount);

}

int DynamicPaxProcessor::getParameterCount() const
{
    return (fnGetParamCount && instance) ? fnGetParamCount (instance) : 0;
}

void DynamicPaxProcessor::getParameterInfo (int index, PAX_ParameterInfo& info) const
{
    if (fnGetParamInfo && instance)
        fnGetParamInfo (instance, index, &info);
}

float DynamicPaxProcessor::getParameter (int index) const
{
    return (fnGetParam && instance) ? fnGetParam (instance, index) : 0.f;
}

void DynamicPaxProcessor::setParameter (int index, float value)
{
    if (fnSetParam && instance)
        fnSetParam (instance, index, value);

    // Check if audio output count changed (e.g. band count in Spectrumyser)
    if (fnGetAudioOutCount && instance)
    {
        int newCount = fnGetAudioOutCount (instance);
        if (newCount > 0 && newCount != audioOutputCount)
        {
            audioOutputCount = newCount;
            if (onPortCountChanged) onPortCountChanged();
        }
    }
}

bool DynamicPaxProcessor::isParameterReadOnly (int index) const
{
    return (fnIsParamReadOnly && instance) ? (fnIsParamReadOnly (instance, index) != 0) : false;
}

bool DynamicPaxProcessor::isParameterLiveSynced (int index) const
{
    return (fnIsParamLiveSynced && instance) ? (fnIsParamLiveSynced (instance, index) != 0) : false;
}

int DynamicPaxProcessor::getAudioPassthroughInput (int outputIndex) const
{
    return (fnAudioPassthroughInput && instance) ? fnAudioPassthroughInput (instance, outputIndex) : -1;
}

bool DynamicPaxProcessor::getMidiPassthrough() const
{
    return (fnMidiPassthrough && instance) ? (fnMidiPassthrough (instance) != 0) : false;
}
