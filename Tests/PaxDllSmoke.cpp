#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "PaxAPI.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <array>
#include <vector>

template <typename Function>
Function resolve(HMODULE library, const char* name)
{
    auto address = GetProcAddress(library, name);
    if (address == nullptr)
        throw std::runtime_error(name);
    return reinterpret_cast<Function>(address);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    HMODULE library = LoadLibraryW(argv[1]);
    if (library == nullptr) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return 3;
    }
    PAX_Instance* instance = nullptr;
    decltype(&PAX_destroy) destroy = nullptr;
    try {
        const auto describe = resolve<decltype(&PAX_getDescriptor)>(library, "PAX_getDescriptor");
        const auto create = resolve<decltype(&PAX_create)>(library, "PAX_create");
        destroy = resolve<decltype(&PAX_destroy)>(library, "PAX_destroy");
        const auto prepare = resolve<decltype(&PAX_prepare)>(library, "PAX_prepare");
        const auto process = resolve<decltype(&PAX_process)>(library, "PAX_process");
        const auto parameterCount = resolve<decltype(&PAX_getParameterCount)>(library, "PAX_getParameterCount");
        const auto parameterInfo = resolve<decltype(&PAX_getParameterInfo)>(library, "PAX_getParameterInfo");
        const auto get = resolve<decltype(&PAX_getParameter)>(library, "PAX_getParameter");
        const auto set = resolve<decltype(&PAX_setParameter)>(library, "PAX_setParameter");
        const auto* descriptor = describe();
        if (descriptor == nullptr || descriptor->apiVersion != PAX_API_VERSION)
            throw std::runtime_error("Pax API version mismatch");
        instance = create();
        if (instance == nullptr) throw std::runtime_error("Pax create failed");
        prepare(instance, 48000.0, 4);
        PAX_ParameterInfo info {};
        parameterInfo(instance, 0, &info);
        if (parameterCount(instance) != 1 || info.minValue != -60.0f || info.maxValue != 24.0f
            || info.defaultValue != 0.0f || info.step != 0.1f || get(instance, 0) != 0.0f)
            throw std::runtime_error("Unexpected gain parameter metadata");

        float left[] = {0.25f, -0.5f, 1.0f, 0.125f};
        float right[] = {-0.25f, 0.5f, -1.0f, -0.125f};
        float outputLeft[4] {}, outputRight[4] {};
        float* inputs[] = {left, right};
        float* outputs[] = {outputLeft, outputRight};
        PAX_ProcessContext context {};
        context.audioIn = inputs;
        context.audioOut = outputs;
        context.numChannels = 2;
        context.numSamples = 4;
        struct GainCase { float requestedDb, expectedDb, expectedGain; };
        for (const auto& gain : { GainCase{-100.f, -60.f, 0.001f},
                                 GainCase{-60.f, -60.f, 0.001f},
                                 GainCase{-20.f, -20.f, 0.1f},
                                 GainCase{-6.020599913f, -6.020599913f, 0.5f},
                                 GainCase{0.f, 0.f, 1.f},
                                 GainCase{6.020599913f, 6.020599913f, 2.f},
                                 GainCase{24.f, 24.f, 15.848932f},
                                 GainCase{100.f, 24.f, 15.848932f} }) {
            set(instance, 0, gain.requestedDb);
            if (std::abs(get(instance, 0) - gain.expectedDb) > 0.00001f)
                throw std::runtime_error("Parameter range or clamping is incorrect");
            process(instance, &context);
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < 4; ++sample)
                    if (std::abs(outputs[channel][sample] - inputs[channel][sample] * gain.expectedGain) > 0.000005f)
                        throw std::runtime_error("Attenuation, unity or amplification is incorrect");
        }
        const auto maxChannels = resolve<decltype(&PAX_getMaxAudioChannels)>(library, "PAX_getMaxAudioChannels");
        if (maxChannels() < 64) throw std::runtime_error("Multichannel capability not exported");
        for (double rate : {1500., 2000., 8000., 16000., 44100., 48000.})
            for (int frames : {16, 24, 32, 64})
                for (int channels : {1, 2, 8, 64}) {
                    prepare(instance, rate, frames);
                    std::vector<float> source((size_t) channels * frames), result(source.size());
                    std::array<float*, PAX_MAX_AUDIO_CHANNELS> ins {}, outs {};
                    for (int ch = 0; ch < channels; ++ch) {
                        ins[ch] = source.data() + (size_t) ch * frames;
                        outs[ch] = result.data() + (size_t) ch * frames;
                        for (int n = 0; n < frames; ++n) ins[ch][n] = (float) (ch + n + 1) / 256.f;
                    }
                    PAX_ProcessContext block {};
                    block.audioIn = ins.data(); block.audioOut = outs.data();
                    block.numChannels = channels; block.numSamples = frames;
                    for (const auto& gain : { GainCase{-6.020599913f, -6.020599913f, 0.5f},
                                             GainCase{6.020599913f, 6.020599913f, 2.f} }) {
                        set(instance, 0, gain.requestedDb); process(instance, &block);
                        for (size_t n = 0; n < result.size(); ++n)
                            if (std::abs(result[n] - source[n] * gain.expectedGain) > 0.000001f)
                                throw std::runtime_error("Multichannel/low-rate/small-frame gain failed");
                    }
                }
        std::cout << "PASS: " << descriptor->name << " DLL, API " << descriptor->apiVersion
                  << ", parameter limits, attenuation, unity, boost, multichannel gain, low rates and small frames\n";
        destroy(instance);
        FreeLibrary(library);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (instance != nullptr && destroy != nullptr) destroy(instance);
        FreeLibrary(library);
        return 1;
    }
}
