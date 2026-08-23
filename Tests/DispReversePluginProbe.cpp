#include "../Source/PluginProcessor.h"
#include "../Source/ReverseDispersionFirRuntime.h"
#include "../../TR-Shared/Modulation/Tests/TRRandomHoldRuntimeAssertions.h"

#include <cmath>
#include <chrono>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (! condition)
        throw std::runtime_error(message);
}

void setValue(DisperserAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.apvts.getParameter(id);
    require(parameter != nullptr, "DISP reverse probe parameter is missing");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

std::vector<float> render(double sampleRate, int blockSize, bool reverse, bool jitter)
{
    constexpr int length = 8192;
    auto processor = std::make_unique<DisperserAudioProcessor>();
    processor->prepareToPlay(sampleRate, blockSize);
    setValue(*processor, DisperserAudioProcessor::kParamAmount, 16.0f);
    setValue(*processor, DisperserAudioProcessor::kParamSeries, 1.0f);
    setValue(*processor, DisperserAudioProcessor::kParamFreq, 1000.0f);
    setValue(*processor, DisperserAudioProcessor::kParamShape, 0.65f);
    setValue(*processor, DisperserAudioProcessor::kParamFeedback, 0.0f);
    setValue(*processor, DisperserAudioProcessor::kParamMix, 1.0f);
    setValue(*processor, DisperserAudioProcessor::kParamJitter, jitter ? 0.75f : 0.0f);
    setValue(*processor, DisperserAudioProcessor::kParamReverse, reverse ? 1.0f : 0.0f);

    std::vector<float> output(static_cast<std::size_t>(length), 0.0f);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    // Let the normal parameter smoothing/initialisation settle before the
    // impulse. The reverse comparison must use a stationary direct response,
    // not the processor's startup transition.
    for (int warmup = 0; warmup < 256; ++warmup)
    {
        buffer.clear();
        processor->processBlock(buffer, midi);
    }
    for (int offset = 0; offset < length; offset += blockSize)
    {
        const auto count = std::min(blockSize, length - offset);
        buffer.clear();
        if (offset == 0)
        {
            buffer.setSample(0, 0, 1.0f);
            buffer.setSample(1, 0, 1.0f);
        }
        processor->processBlock(buffer, midi);
        for (int sample = 0; sample < count; ++sample)
            output[static_cast<std::size_t>(offset + sample)] = buffer.getSample(0, sample);
    }
    processor->releaseResources();
    return output;
}

std::size_t referenceTapCount(double sampleRate)
{
    DISP::DSP::ReverseDispersionFirRuntime runtime;
    runtime.prepare(sampleRate);
    runtime.setParameters(16, 1, 1000.0f, 0.65f, false, false, 0.0f);
    return static_cast<std::size_t> (runtime.tapCount());
}

double benchmarkCpu(double sampleRate, int blockSize, bool reverse, bool jitter, int style)
{
    constexpr int benchmarkSamples = 480'000;
    auto processor = std::make_unique<DisperserAudioProcessor>();
    processor->prepareToPlay(sampleRate, blockSize);
    setValue(*processor, DisperserAudioProcessor::kParamAmount, 64.0f);
    setValue(*processor, DisperserAudioProcessor::kParamSeries, 4.0f);
    setValue(*processor, DisperserAudioProcessor::kParamFreq, 730.0f);
    setValue(*processor, DisperserAudioProcessor::kParamShape, 0.55f);
    setValue(*processor, DisperserAudioProcessor::kParamFeedback, 0.42f);
    setValue(*processor, DisperserAudioProcessor::kParamMix, 1.0f);
    setValue(*processor, DisperserAudioProcessor::kParamJitter, jitter ? 0.75f : 0.0f);
    setValue(*processor, DisperserAudioProcessor::kParamReverse, reverse ? 1.0f : 0.0f);
    setValue(*processor, DisperserAudioProcessor::kParamStyle, static_cast<float>(style));

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    for (int warmup = 0; warmup < 16; ++warmup)
        processor->processBlock(buffer, midi);

    const auto start = std::chrono::steady_clock::now();
    for (int offset = 0; offset < benchmarkSamples; offset += blockSize)
    {
        const auto count = std::min(blockSize, benchmarkSamples - offset);
        buffer.clear();
        for (int sample = 0; sample < count; ++sample)
        {
            const auto phase = static_cast<float>(offset + sample) * 0.0137f;
            buffer.setSample(0, sample, 0.2f * std::sin(phase));
            buffer.setSample(1, sample, 0.17f * std::cos(phase * 0.73f));
        }
        processor->processBlock(buffer, midi);
        for (int sample = 0; sample < count; ++sample)
            require(std::isfinite(buffer.getSample(0, sample)) && std::isfinite(buffer.getSample(1, sample)),
                    "DISP CPU benchmark produced non-finite audio");
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    processor->releaseResources();
    const auto audioSeconds = static_cast<double>(benchmarkSamples) / sampleRate;
    return 100.0 * elapsed / audioSeconds;
}

double impulseEnergyCentroid(const std::vector<float>& signal)
{
    double weighted = 0.0;
    double energy = 0.0;
    for (std::size_t index = 0; index < signal.size(); ++index)
    {
        const double sampleEnergy = static_cast<double> (signal[index]) * signal[index];
        weighted += static_cast<double> (index) * sampleEnergy;
        energy += sampleEnergy;
    }
    return energy > 1.0e-20 ? weighted / energy : 0.0;
}

double mirroredImpulseCorrelation(const std::vector<float>& direct,
                                  const std::vector<float>& reverse,
                                  std::size_t boundedLength = 0)
{
    // The production FIR is deliberately bounded. Compare the same bounded
    // direct-IR window, rather than reversing the whole rendered buffer (which
    // would move the reference impulse to the far end of the capture).
    const auto count = std::min ({ direct.size(), reverse.size(),
                                   boundedLength > 0 ? boundedLength
                                                     : static_cast<std::size_t> (DISP::DSP::ReverseDispersionFirRuntime::maxTaps) });
    double dot = 0.0;
    double directEnergy = 0.0;
    double reverseEnergy = 0.0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto directSample = static_cast<double> (direct[count - 1 - index]);
        const auto reverseSample = static_cast<double> (reverse[index]);
        dot += directSample * reverseSample;
        directEnergy += directSample * directSample;
        reverseEnergy += reverseSample * reverseSample;
    }
    return directEnergy > 1.0e-20 && reverseEnergy > 1.0e-20
        ? dot / std::sqrt (directEnergy * reverseEnergy) : 0.0;
}

std::pair<std::size_t, float> peakLocation(const std::vector<float>& signal)
{
    std::size_t index = 0;
    float peak = 0.0f;
    for (std::size_t i = 0; i < signal.size(); ++i)
        if (std::abs(signal[i]) > peak)
        {
            peak = std::abs(signal[i]);
            index = i;
        }
    return { index, peak };
}
}

int main()
{
    try
    {
        TR::Modulation::Tests::verifyRandomHoldStateContinuity();
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        for (const auto sampleRate : { 44'100.0, 48'000.0, 96'000.0 })
        {
            for (const auto blockSize : { 64, 128, 512 })
            {
                const auto released = render(sampleRate, blockSize, false, false);
                const auto legacyStateEnabled = render(sampleRate, blockSize, true, false);
                float maximumDelta = 0.0f;
                for (std::size_t index = 0; index < released.size(); ++index)
                {
                    require(std::isfinite(released[index])
                                && std::isfinite(legacyStateEnabled[index]),
                            "DISP retired reverse state produced non-finite audio");
                    maximumDelta = std::max(
                        maximumDelta,
                        std::abs(released[index] - legacyStateEnabled[index]));
                }
                require(maximumDelta == 0.0f,
                        "DISP retired reverse parameter still changes release audio");
                std::cout << "sample_rate=" << sampleRate
                          << " block_size=" << blockSize
                          << " retired_reverse_delta=" << maximumDelta << '\n';
            }
        }

        auto stateSource = std::make_unique<DisperserAudioProcessor>();
        setValue(*stateSource, DisperserAudioProcessor::kParamReverse, 1.0f);
        juce::MemoryBlock state;
        stateSource->getStateInformation(state);
        auto stateRestored = std::make_unique<DisperserAudioProcessor>();
        stateRestored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        require(stateRestored->apvts.getRawParameterValue(DisperserAudioProcessor::kParamReverse)->load() > 0.5f,
                "DISP reverse parameter did not survive preset round-trip");

        auto released = std::make_unique<DisperserAudioProcessor>();
        auto legacyStateEnabled = std::make_unique<DisperserAudioProcessor>();
        setValue(*legacyStateEnabled, DisperserAudioProcessor::kParamReverse, 1.0f);
        released->prepareToPlay(48'000.0, 128);
        legacyStateEnabled->prepareToPlay(48'000.0, 128);
        require(released->getLatencySamples() == legacyStateEnabled->getLatencySamples(),
                "DISP retired reverse parameter changes reported latency");
        std::cout << "DISP reverse retirement probe passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "DISP reverse plugin probe failed: " << error.what() << '\n';
        return 1;
    }
}
