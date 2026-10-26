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
        if (parameterCount(instance) != 1 || info.minValue != -60.0f || info.maxValue != 6.0f)
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
        set(instance, 0, -6.020599913f);
        if (std::abs(get(instance, 0) + 6.020599913f) > 0.00001f)
            throw std::runtime_error("Parameter did not update");
        process(instance, &context);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 4; ++sample)
                if (std::abs(outputs[channel][sample] - inputs[channel][sample] * 0.5f) > 0.000001f)
                    throw std::runtime_error("Gain processing result is incorrect");
        set(instance, 0, -60.0f);
        process(instance, &context);
        for (auto* output : outputs)
            for (int sample = 0; sample < 4; ++sample)
                if (output[sample] != 0.0f)
                    throw std::runtime_error("Silence parameter did not take effect");
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
                    set(instance, 0, -6.020599913f); process(instance, &block);
                    for (size_t n = 0; n < result.size(); ++n)
                        if (std::abs(result[n] - source[n] * 0.5f) > 0.000001f)
                            throw std::runtime_error("Multichannel/low-rate/small-frame gain failed");
                }
        std::cout << "PASS: " << descriptor->name << " DLL, API " << descriptor->apiVersion
                  << ", parameters, multichannel gain, low rates, small frames and silence\n";
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
