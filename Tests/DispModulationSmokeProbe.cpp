#include "../Source/PluginProcessor.h"
#include "../Source/UIV2/DispBackendBindings.h"
#include "../Source/UIV2/DispUiDefinition.h"
#include "../Source/Modulation/DispModulationConfig.h"
#include "../../TR-Shared/Modulation/Tests/TRNativeSidechainBaseline.h"
#include "../../TR-Shared/Modulation/Tests/TRModulationJourneyAssertions.h"
#include "../../TR-Shared/Modulation/Tests/TRJitterMotionEvidence.h"
#include "../../TR-Shared/Modulation/Tests/TRMotionRecipeUiAssertions.h"
#include "../../TR-Shared/SimpleUIV2/Preset/TRPresetManager.h"
#include "../../TR-Shared/Testing/TRPluginCpuBenchmark.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

struct DispNativeSidechainTestAccess
{
    static void captureJitter(DisperserAudioProcessor& processor, float* series1,
                              float* series2, int capacity) noexcept
    {
        processor.jitterEvidenceNativeSeries1ForTests_ = series1;
        processor.jitterEvidenceNativeSeries2ForTests_ = series2;
        processor.jitterEvidenceCapacityForTests_ = capacity;
    }
    static void capturePeriod(DisperserAudioProcessor& processor, float* periodMs) noexcept
    {
        processor.jitterEvidenceNativePeriodMsForTests_ = periodMs;
    }
    static void captureRawFrequency(DisperserAudioProcessor& processor,
                                    float* series1, float* series2) noexcept
    {
        processor.jitterEvidenceNativeRawFreq1ForTests_ = series1;
        processor.jitterEvidenceNativeRawFreq2ForTests_ = series2;
    }
    static void captureAmount(DisperserAudioProcessor& processor, float* amount) noexcept
    {
        processor.jitterEvidenceNativeAmountForTests_ = amount;
    }
    static std::array<float, 3> nativeInitialState(const DisperserAudioProcessor& processor,
                                                   int lane) noexcept
    {
        const auto& state = processor.jitterLanes_[static_cast<std::size_t>(lane)].freq;
        return { state.slowNext, state.fastNext, state.tonePhase };
    }
    static std::array<float, 3> matrixInitialState(const DisperserAudioProcessor& processor,
                                                   int lane) noexcept
    {
        return processor.modulation.motionAdaptiveInitialState(0, lane);
    }
    static std::array<juce::int64, 2> nativeRandomSeeds(
        const DisperserAudioProcessor& processor, int lane) noexcept
    {
        const auto& state = processor.jitterLanes_[static_cast<std::size_t>(lane)].freq;
        return { state.slowRng.getSeed(), state.fastRng.getSeed() };
    }
    static std::array<juce::int64, 2> matrixRandomSeeds(
        const DisperserAudioProcessor& processor, int lane) noexcept
    {
        return processor.modulation.motionAdaptiveRandomSeeds(0, lane);
    }
    static float predictedPeriod(const DisperserAudioProcessor& processor, int sample) noexcept
    {
        return processor.jitterMotionReferencePeriods_.getNumSamples() > sample
            ? processor.jitterMotionReferencePeriods_.getSample(0, sample) : 0.0f;
    }
    static float matrixAdaptiveAmount(const DisperserAudioProcessor& processor) noexcept
    {
        return processor.modulation.motionAdaptiveAmount(0);
    }
    static void selectNative(DisperserAudioProcessor& processor)
    {
        processor.useNativeSidechainForTests_ = true;
    }
    static bool enableShared(DisperserAudioProcessor& processor)
    {
        return TR::Modulation::Tests::setNativeBaselineParameter(
            processor.apvts, DisperserAudioProcessor::kParamSidechain, 1.0f);
    }
    static void extract(const DisperserAudioProcessor& processor, int sample, float* values)
    {
        values[0] = processor.sidechainFrequencyOffsetNorm_[static_cast<std::size_t>(sample)];
        values[1] = processor.sidechainRmsEnv_;
        values[2] = processor.sidechainGateSmoothed_;
        values[3] = processor.sidechainDepthSmoothed_;
    }
    static void extractShared(const DisperserAudioProcessor& processor, int sample, float* values)
    {
        const auto control = processor.modulation.analysisControlSignal(1);
        values[0] = control.valid() ? control.samples[sample] : 0.0f;
        values[1] = processor.sidechainFrequencyOffsetNorm_[static_cast<std::size_t>(sample)];
    }
    static float matrixOffset(const DisperserAudioProcessor& processor, int sample)
    {
        return processor.sidechainFrequencyOffsetNorm_[static_cast<std::size_t>(sample)];
    }
    static float nativeFrequencyOctave(const DisperserAudioProcessor& processor,
                                       int series) noexcept
    {
        const auto lane = static_cast<std::size_t>(juce::jlimit(
            0, DisperserAudioProcessor::kSeriesMax - 1, series));
        const auto base = processor.smoothedFreqValue;
        const auto moved = processor.smoothedJitterSeriesFreq[lane];
        return base > 1.0e-6f && moved > 1.0e-6f ? std::log2(moved / base) : 0.0f;
    }
    static float matrixFrequencyOctave(const DisperserAudioProcessor& processor,
                                       int series, int sample) noexcept
    {
        return processor.modulation.effectiveNativeAtSample(
            TR::DispModulation::seriesFrequencyOffset(series), sample, 0.0f);
    }
};

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

juce::Component* findById(juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id) return &parent;
    for (auto* child : parent.getChildren())
        if (auto* found = findById(*child, id)) return found;
    return nullptr;
}

void process(DisperserAudioProcessor& processor, bool noteOn, float sidechain = 0.0f)
{
    constexpr int blockSize = 512;
    juce::AudioBuffer<float> audio(processor.getTotalNumInputChannels(), blockSize);
    for (int sample = 0; sample < blockSize; ++sample)
    {
        const auto value = 0.1f * std::sin(0.01f * static_cast<float>(sample));
        audio.setSample(0, sample, value);
        audio.setSample(1, sample, value);
        if (audio.getNumChannels() >= 4)
        {
            const auto sc = sidechain * std::sin(0.13f * static_cast<float>(sample));
            audio.setSample(2, sample, sc);
            audio.setSample(3, sample, sc);
        }
    }
    juce::MidiBuffer midi;
    if (noteOn)
        midi.addEvent(juce::MidiMessage::noteOn(1, 127, static_cast<juce::uint8>(127)), 16);
    processor.processBlock(audio, midi);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < blockSize; ++sample)
            require(std::isfinite(audio.getSample(channel, sample)), "DISP produced non-finite audio");
}

struct DispJitterRender
{
    std::vector<float> audio, control0, control1, reference, nativeReference;
    std::vector<float> raw0, raw1;
    std::vector<float> nativeAmount;
    std::vector<float> matrixAmountAtBlockEnd;
    std::array<std::array<float, 3>, 2> initialState {};
    std::array<std::array<juce::int64, 2>, 2> initialRandomSeeds {};
    bool feedbackInvariant = true;
};

DispJitterRender renderDispJitterEvidence(int path, float amount, float frequencyHz,
                                          double sampleRate = 48000.0, int blockSize = 257,
                                          bool automate = false, int durationSeconds = 0)
{
    const int seconds = durationSeconds > 0 ? durationSeconds : (automate ? 24 : 8);
    const auto totalSamples = static_cast<int>(sampleRate) * seconds;
    auto processor = std::make_unique<DisperserAudioProcessor>();
    processor->prepareToPlay(sampleRate, blockSize);
    using TR::Modulation::Tests::setNativeBaselineParameter;
    require(setNativeBaselineParameter(processor->apvts, DisperserAudioProcessor::kParamFreq,
                                       frequencyHz)
                && setNativeBaselineParameter(processor->apvts, DisperserAudioProcessor::kParamAmount, 48.0f)
                && setNativeBaselineParameter(processor->apvts, DisperserAudioProcessor::kParamSeries, 4.0f)
                && setNativeBaselineParameter(processor->apvts, DisperserAudioProcessor::kParamShape, 0.5f)
                && setNativeBaselineParameter(processor->apvts, DisperserAudioProcessor::kParamJitter,
                                               0.0f)
                && setNativeBaselineParameter(processor->apvts, DisperserAudioProcessor::kParamFeedback, 0.65f)
                && setNativeBaselineParameter(processor->apvts, DisperserAudioProcessor::kParamMix, 1.0f),
            "DISP Jitter evidence parameters rejected");
    if (path == 2)
    {
        auto recipe = TR::DispModulation::makeJitterParityRecipe(
            TR::Modulation::makeDefaultState());
        require(setNativeBaselineParameter(processor->apvts, "mod_macro_1", 0.0f)
                    && processor->setModulationState(recipe),
                "DISP Jitter evidence recipe rejected");
    }
    for (int remaining = static_cast<int>(sampleRate); remaining > 0;)
    {
        const auto count = juce::jmin(blockSize, remaining);
        juce::AudioBuffer<float> settle(
            juce::jmax(2, processor->getTotalNumInputChannels()), count);
        settle.clear();
        juce::MidiBuffer midi;
        processor->processBlock(settle, midi);
        remaining -= count;
    }
    require(setNativeBaselineParameter(processor->apvts,
                path == 1 ? DisperserAudioProcessor::kParamJitter : "mod_macro_1", amount),
            "DISP Jitter evidence activation rejected");
    DispJitterRender result;
    for (int lane = 0; lane < 2; ++lane)
    {
        result.initialState[static_cast<std::size_t>(lane)] = path == 2
            ? DispNativeSidechainTestAccess::matrixInitialState(*processor, lane)
            : DispNativeSidechainTestAccess::nativeInitialState(*processor, lane);
        result.initialRandomSeeds[static_cast<std::size_t>(lane)] = path == 2
            ? DispNativeSidechainTestAccess::matrixRandomSeeds(*processor, lane)
            : DispNativeSidechainTestAccess::nativeRandomSeeds(*processor, lane);
    }
    result.audio.resize(static_cast<std::size_t>(totalSamples * 2));
    result.control0.resize(static_cast<std::size_t>(totalSamples), 0.0f);
    result.control1.resize(static_cast<std::size_t>(totalSamples), 0.0f);
    result.nativeReference.resize(static_cast<std::size_t>(totalSamples), 0.0f);
    result.raw0.resize(static_cast<std::size_t>(totalSamples), 0.0f);
    result.raw1.resize(static_cast<std::size_t>(totalSamples), 0.0f);
    result.nativeAmount.resize(static_cast<std::size_t>(totalSamples), 0.0f);
    int offset = 0;
    while (offset < totalSamples)
    {
        const auto count = juce::jmin(blockSize, totalSamples - offset);
        if (automate)
        {
            constexpr float frequencies[] { 80.0f, 800.0f, 6000.0f, 160.0f };
            constexpr float amounts[] { 0.2f, 1.0f, 0.6f, 0.35f };
            const auto segment = static_cast<int>(offset / (sampleRate * 0.5)) & 3;
            require(setNativeBaselineParameter(processor->apvts,
                        DisperserAudioProcessor::kParamFreq, frequencies[segment])
                        && setNativeBaselineParameter(processor->apvts,
                            path == 1 ? DisperserAudioProcessor::kParamJitter : "mod_macro_1",
                            amounts[segment]),
                    "DISP Jitter automation target rejected");
        }
        juce::AudioBuffer<float> block(juce::jmax(2, processor->getTotalNumInputChannels()), count);
        for (int sample = 0; sample < count; ++sample)
        {
            const auto absolute = offset + sample;
            const auto saw = 0.14f * (2.0f * static_cast<float>(
                std::fmod(65.406 * absolute / sampleRate, 1.0)) - 1.0f);
            const auto transient = absolute % 12000 < 64
                ? 0.4f * std::exp(-static_cast<float>(absolute % 12000) / 16.0f) : 0.0f;
            block.setSample(0, sample, saw + transient);
            block.setSample(1, sample, 0.75f * saw + transient);
        }
        juce::MidiBuffer midi;
        if (path != 0)
        {
            DispNativeSidechainTestAccess::captureJitter(
                *processor, result.control0.data() + offset,
                result.control1.data() + offset, count);
            DispNativeSidechainTestAccess::capturePeriod(
                *processor, result.nativeReference.data() + offset);
            DispNativeSidechainTestAccess::captureRawFrequency(
                *processor, result.raw0.data() + offset, result.raw1.data() + offset);
            DispNativeSidechainTestAccess::captureAmount(
                *processor, result.nativeAmount.data() + offset);
        }
        processor->processBlock(block, midi);
        if (path == 2)
            result.matrixAmountAtBlockEnd.push_back(
                DispNativeSidechainTestAccess::matrixAdaptiveAmount(*processor));
        for (int sample = 0; sample < count; ++sample)
            for (int channel = 0; channel < 2; ++channel)
                result.audio[static_cast<std::size_t>((offset + sample) * 2 + channel)] =
                    block.getSample(channel, sample);
        for (int sample = 0; sample < count; ++sample)
        {
            result.reference.push_back(path == 2
                ? DispNativeSidechainTestAccess::predictedPeriod(*processor, sample) : 0.0f);
            if (path == 2)
            {
                result.raw0[static_cast<std::size_t>(offset + sample)]
                    = DispNativeSidechainTestAccess::matrixFrequencyOctave(
                        *processor, 0, sample);
                result.raw1[static_cast<std::size_t>(offset + sample)]
                    = DispNativeSidechainTestAccess::matrixFrequencyOctave(
                        *processor, 1, sample);
            }
        }
        DispNativeSidechainTestAccess::captureJitter(*processor, nullptr, nullptr, 0);
        DispNativeSidechainTestAccess::capturePeriod(*processor, nullptr);
        DispNativeSidechainTestAccess::captureRawFrequency(*processor, nullptr, nullptr);
        DispNativeSidechainTestAccess::captureAmount(*processor, nullptr);
        const auto feedback = processor->getPhaseContourTelemetry().feedbackMagnitude;
        result.feedbackInvariant = result.feedbackInvariant
            && std::isfinite(feedback) && feedback <= 1.0f;
        offset += count;
    }
    return result;
}

bool writeDispJitterHostMatrix(const juce::File& output)
{
    std::ofstream csv(output.getFullPathName().toStdString(), std::ios::trunc);
    csv << "sample_rate_hz,block_size,lane_1_rms_ratio,lane_2_rms_ratio,feedback_invariant\n";
    bool passed = true;
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto blockSize : { 64, 257, 2048 })
        {
            const auto native = renderDispJitterEvidence(1, 0.6f, 800.0f, sampleRate, blockSize);
            const auto matrix = renderDispJitterEvidence(2, 0.6f, 800.0f, sampleRate, blockSize);
            const auto skip = static_cast<std::size_t>(sampleRate);
            const auto lane1 = TR::Modulation::Tests::rmsRatio(
                native.control0, matrix.control0, skip);
            const auto lane2 = TR::Modulation::Tests::rmsRatio(
                native.control1, matrix.control1, skip);
            const auto feedback = native.feedbackInvariant && matrix.feedbackInvariant;
            const auto rowPassed = lane1 >= 0.97 && lane1 <= 1.03
                && lane2 >= 0.97 && lane2 <= 1.03 && feedback;
            passed = passed && rowPassed;
            csv << sampleRate << ',' << blockSize << ',' << lane1 << ',' << lane2
                << ',' << feedback << '\n';
        }
    return csv.good() && passed;
}

bool writeDispJitterAutomationMatrix(const juce::File& output)
{
    std::ofstream csv(output.getFullPathName().toStdString(), std::ios::trunc);
    csv << "sample_rate_hz,block_size,lane_1_rms_ratio,lane_2_rms_ratio,lane_1_max_window_rms_error,lane_2_max_window_rms_error,feedback_invariant\n";
    bool passed = true;
    for (const auto sampleRate : { 48000.0, 96000.0 })
        for (const auto blockSize : { 64, 2048 })
        {
            const auto native = renderDispJitterEvidence(1, 0.6f, 800.0f,
                sampleRate, blockSize, true);
            const auto matrix = renderDispJitterEvidence(2, 0.6f, 800.0f,
                sampleRate, blockSize, true);
            const auto skip = static_cast<std::size_t>(sampleRate);
            const auto window = static_cast<std::size_t>(sampleRate * 0.25);
            const auto lane1Ratio = TR::Modulation::Tests::rmsRatio(
                native.control0, matrix.control0, skip);
            const auto lane2Ratio = TR::Modulation::Tests::rmsRatio(
                native.control1, matrix.control1, skip);
            const auto lane1 = TR::Modulation::Tests::maximumWindowedRmsRatioError(
                native.control0, matrix.control0, skip, window);
            const auto lane2 = TR::Modulation::Tests::maximumWindowedRmsRatioError(
                native.control1, matrix.control1, skip, window);
            const auto feedback = native.feedbackInvariant && matrix.feedbackInvariant;
            passed = passed && lane1Ratio >= 0.95 && lane1Ratio <= 1.05
                && lane2Ratio >= 0.95 && lane2Ratio <= 1.05 && feedback;
            csv << sampleRate << ',' << blockSize << ',' << lane1Ratio << ',' << lane2Ratio
                << ',' << lane1 << ',' << lane2
                << ',' << feedback << '\n';
        }
    return csv.good() && passed;
}

bool writeDispJitterLongRun(const juce::File& output)
{
    std::ofstream csv(output.getFullPathName().toStdString(), std::ios::trunc);
    csv << "frequency_hz,amount,duration_seconds,lane_1_rms_ratio,lane_2_rms_ratio,lane_1_native_matrix_correlation,lane_2_native_matrix_correlation,raw_1_rms_ratio,raw_2_rms_ratio,raw_1_correlation,raw_2_correlation,initial_state_max_error,initial_random_seeds_equal,native_amount_max_error,native_matrix_amount_block_end_max_error,reference_min_ms,reference_max_ms,reference_rms_error_ms,reference_max_error_ms,feedback_invariant\n";
    bool passed = true;
    for (const auto test : { std::pair { 800.0f, 0.25f }, std::pair { 6000.0f, 1.0f } })
    {
        constexpr int durationSeconds = 180;
        const auto native = renderDispJitterEvidence(
            1, test.second, test.first, 48000.0, 257, false, durationSeconds);
        const auto matrix = renderDispJitterEvidence(
            2, test.second, test.first, 48000.0, 257, false, durationSeconds);
        const auto skip = static_cast<std::size_t>(48000);
        const auto lane1 = TR::Modulation::Tests::rmsRatio(
            native.control0, matrix.control0, skip);
        const auto lane2 = TR::Modulation::Tests::rmsRatio(
            native.control1, matrix.control1, skip);
        const auto correlation1 = TR::Modulation::Tests::correlation(
            native.control0, matrix.control0, skip);
        const auto correlation2 = TR::Modulation::Tests::correlation(
            native.control1, matrix.control1, skip);
        const auto rawRatio1 = TR::Modulation::Tests::rmsRatio(
            native.raw0, matrix.raw0, skip);
        const auto rawRatio2 = TR::Modulation::Tests::rmsRatio(
            native.raw1, matrix.raw1, skip);
        const auto rawCorrelation1 = TR::Modulation::Tests::correlation(
            native.raw0, matrix.raw0, skip);
        const auto rawCorrelation2 = TR::Modulation::Tests::correlation(
            native.raw1, matrix.raw1, skip);
        const auto feedback = native.feedbackInvariant && matrix.feedbackInvariant;
        float initialStateMaxError = 0.0f;
        bool initialRandomSeedsEqual = true;
        for (int lane = 0; lane < 2; ++lane)
            for (int value = 0; value < 3; ++value)
                initialStateMaxError = std::max(initialStateMaxError, std::abs(
                    native.initialState[static_cast<std::size_t>(lane)][static_cast<std::size_t>(value)]
                    - matrix.initialState[static_cast<std::size_t>(lane)][static_cast<std::size_t>(value)]));
        for (int lane = 0; lane < 2; ++lane)
            initialRandomSeedsEqual = initialRandomSeedsEqual
                && native.initialRandomSeeds[static_cast<std::size_t>(lane)]
                    == matrix.initialRandomSeeds[static_cast<std::size_t>(lane)];
        float amountMaxError = 0.0f;
        float nativeMatrixAmountMaxError = 0.0f;
        const auto rampSamples = static_cast<int>(std::round(48000.0 * 0.0325));
        for (std::size_t sample = 0; sample < native.nativeAmount.size(); ++sample)
        {
            const auto expected = test.second * juce::jmin(1.0f,
                static_cast<float>(sample + 1) / static_cast<float>(rampSamples));
            amountMaxError = std::max(amountMaxError,
                std::abs(native.nativeAmount[sample] - expected));
        }
        for (std::size_t block = 0; block < matrix.matrixAmountAtBlockEnd.size(); ++block)
        {
            const auto sample = std::min(native.nativeAmount.size() - 1,
                (block + 1) * static_cast<std::size_t>(257) - 1);
            nativeMatrixAmountMaxError = std::max(nativeMatrixAmountMaxError,
                std::abs(native.nativeAmount[sample] - matrix.matrixAmountAtBlockEnd[block]));
        }
        double referenceSquareError = 0.0;
        float referenceMaxError = 0.0f;
        float referenceMinimum = std::numeric_limits<float>::max();
        float referenceMaximum = 0.0f;
        for (std::size_t sample = skip; sample < native.nativeReference.size(); ++sample)
        {
            const auto error = std::abs(native.nativeReference[sample] - matrix.reference[sample]);
            referenceSquareError += static_cast<double>(error) * error;
            referenceMaxError = std::max(referenceMaxError, error);
            referenceMinimum = std::min(referenceMinimum, native.nativeReference[sample]);
            referenceMaximum = std::max(referenceMaximum, native.nativeReference[sample]);
        }
        const auto referenceRmsError = std::sqrt(referenceSquareError
            / static_cast<double>(native.nativeReference.size() - skip));
        passed = passed && lane1 >= 0.985 && lane1 <= 1.015
            && lane2 >= 0.985 && lane2 <= 1.015
            && correlation1 >= 0.999 && correlation2 >= 0.999
            && referenceMaxError <= 1.0e-5f && feedback;
        csv << test.first << ',' << test.second << ',' << durationSeconds << ','
            << lane1 << ',' << lane2 << ',' << correlation1 << ',' << correlation2 << ','
            << rawRatio1 << ',' << rawRatio2 << ',' << rawCorrelation1 << ','
            << rawCorrelation2 << ','
            << initialStateMaxError << ',' << initialRandomSeedsEqual << ','
            << amountMaxError << ','
            << nativeMatrixAmountMaxError << ','
            << referenceMinimum << ',' << referenceMaximum << ',' << referenceRmsError << ','
            << referenceMaxError << ',' << feedback << '\n';
        if (test.first == 6000.0f && test.second == 1.0f)
        {
            const auto count = std::min<std::size_t>(native.raw0.size(), 96000u);
            const std::vector<float> native0(native.raw0.begin(), native.raw0.begin() + count);
            const std::vector<float> native1(native.raw1.begin(), native.raw1.begin() + count);
            const std::vector<float> matrix0(matrix.raw0.begin(), matrix.raw0.begin() + count);
            const std::vector<float> matrix1(matrix.raw1.begin(), matrix.raw1.begin() + count);
            passed = TR::Modulation::Tests::writeFourLaneFloatTrace(
                output.getSiblingFile("raw-generator-diagnostic.f32"),
                native0, native1, matrix0, matrix1) && passed;
        }
    }
    return csv.good() && passed;
}

bool writeDispJitterEvidence(const juce::File& output)
{
    require(output.createDirectory(), "DISP Jitter evidence directory unavailable");
    std::ofstream metrics(output.getChildFile("metrics.csv").getFullPathName().toStdString());
    std::ofstream trace(output.getChildFile("control-trace.csv").getFullPathName().toStdString());
    std::ofstream manifest(output.getChildFile("control-manifest.csv").getFullPathName().toStdString());
    metrics << "frequency_hz,amount,native_deviation_rms,matrix_deviation_rms,ratio,series_correlation_native,series_correlation_matrix,feedback_invariant\n";
    trace << "frequency_hz,amount,frame,native_series_1,native_series_2,matrix_series_1,matrix_series_2,matrix_reference\n";
    manifest << "frequency_hz,amount,file,sample_rate_hz,lane_relationship\n";
    for (const auto frequency : { 80.0f, 800.0f, 6000.0f })
        for (const auto amount : { 0.25f, 0.6f, 1.0f })
        {
            const auto baseline = renderDispJitterEvidence(0, amount, frequency);
            const auto native = renderDispJitterEvidence(1, amount, frequency);
            const auto matrix = renderDispJitterEvidence(2, amount, frequency);
            const auto comparison = TR::Modulation::Tests::compareJitterRenders(
                baseline.audio, native.audio, matrix.audio, 48000u * 2u);
            metrics << frequency << ',' << amount << ',' << comparison.nativeDeviationRms << ','
                    << comparison.matrixDeviationRms << ',' << comparison.deviationRatio << ','
                    << TR::Modulation::Tests::correlation(native.control0, native.control1, 16) << ','
                    << TR::Modulation::Tests::correlation(matrix.control0, matrix.control1, 16) << ','
                    << (native.feedbackInvariant && matrix.feedbackInvariant) << '\n';
            const auto traceName = "control-" + juce::String((int) frequency) + "hz-"
                + juce::String((int) std::round(amount * 100.0f)) + "pct.f32";
            require(TR::Modulation::Tests::writeFourLaneFloatTrace(
                        output.getChildFile(traceName), native.control0, native.control1,
                        matrix.control0, matrix.control1),
                    "DISP full-rate control trace export failed");
            manifest << frequency << ',' << amount << ',' << traceName
                     << ",48000,independent\n";
            if (frequency == 800.0f && amount == 1.0f)
                for (std::size_t frame = 0; frame < native.control0.size(); frame += 64)
                    trace << frequency << ',' << amount << ',' << frame << ','
                          << native.control0[frame] << ',' << native.control1[frame] << ','
                          << matrix.control0[frame] << ',' << matrix.control1[frame] << ','
                          << matrix.reference[frame] << '\n';
            if (frequency == 800.0f && amount == 1.0f)
            {
                using TR::Modulation::Tests::differenceRender;
                using TR::Modulation::Tests::writeStereoWav;
                require(writeStereoWav(output.getChildFile("baseline.wav"), baseline.audio, 48000.0)
                            && writeStereoWav(output.getChildFile("native.wav"), native.audio, 48000.0)
                            && writeStereoWav(output.getChildFile("matrix.wav"), matrix.audio, 48000.0)
                            && writeStereoWav(output.getChildFile("native-minus-baseline.wav"),
                                              differenceRender(native.audio, baseline.audio), 48000.0)
                            && writeStereoWav(output.getChildFile("matrix-minus-baseline.wav"),
                                              differenceRender(matrix.audio, baseline.audio), 48000.0),
                        "DISP Jitter evidence WAV export failed");
            }
        }
    auto presetProcessor = std::make_unique<DisperserAudioProcessor>();
    auto presetState = TR::DispModulation::makeJitterParityRecipe(
        TR::Modulation::makeDefaultState());
    require(TR::Modulation::Tests::setNativeBaselineParameter(
                presetProcessor->apvts, DisperserAudioProcessor::kParamJitter, 0.0f)
                && TR::Modulation::Tests::setNativeBaselineParameter(
                    presetProcessor->apvts, "mod_macro_1", 1.0f)
                && presetProcessor->setModulationState(presetState),
            "DISP Jitter MATRIX preset state rejected");
    const auto staging = output.getChildFile("preset-staging");
    TR::DispUIV2::DispBackendBindings presetBackend(*presetProcessor);
    TR::SimpleUIV2::TRPresetManager presetManager(
        TR::DispUIV2::definition(), presetBackend, staging);
    constexpr const char* presetName = "DISP Jitter MATRIX 100";
    require(presetManager.saveAs(presetName, true).wasOk(),
            "DISP Jitter MATRIX preset could not be saved");
    const auto savedPreset = presetManager.libraryFolder().getChildFile(
        juce::String(presetName) + ".trpreset");
    const auto evidencePreset = output.getChildFile(savedPreset.getFileName());
    require(savedPreset.existsAsFile() && savedPreset.copyFileTo(evidencePreset),
            "DISP Jitter MATRIX preset could not be copied to evidence");

    auto restored = std::make_unique<DisperserAudioProcessor>();
    TR::DispUIV2::DispBackendBindings restoredBackend(*restored);
    TR::SimpleUIV2::TRPresetManager restoredManager(
        TR::DispUIV2::definition(), restoredBackend, staging);
    require(restoredManager.load(presetName).wasOk()
                && restored->modulationState() == presetState
                && std::abs(restored->apvts.getRawParameterValue(
                    DisperserAudioProcessor::kParamJitter)->load()) <= 1.0e-7f
                && std::abs(restored->apvts.getRawParameterValue("mod_macro_1")->load()
                            - 1.0f) <= 1.0e-7f,
            "DISP Jitter MATRIX preset did not round-trip exactly");
    std::ofstream presetProof(output.getChildFile("preset-verification.csv")
                                  .getFullPathName().toStdString());
    presetProof << "preset,native_jitter,macro_1,route_count,round_trip\n"
                << presetName << ",0,1," << presetState.routes.size() << ",1\n";
    return metrics.good() && trace.good() && manifest.good() && presetProof.good();
}
}

int main(int argc, char** argv)
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        if (argc == 3 && juce::String(argv[1]) == "--qualify-jitter-host-matrix")
            return writeDispJitterHostMatrix(juce::File(argv[2])) ? 0 : 3;
        if (argc == 3 && juce::String(argv[1]) == "--qualify-jitter-automation")
            return writeDispJitterAutomationMatrix(juce::File(argv[2])) ? 0 : 4;
        if (argc == 3 && juce::String(argv[1]) == "--qualify-jitter-long-run")
            return writeDispJitterLongRun(juce::File(argv[2])) ? 0 : 5;
        if (argc == 3 && juce::String(argv[1]) == "--export-jitter-motion-evidence")
            return writeDispJitterEvidence(juce::File(argv[2])) ? 0 : 2;
        if (argc == 3 && juce::String(argv[1]) == "--export-native-sidechain-baseline")
        {
            const auto ok = TR::Modulation::Tests::exportNativeSidechainBaseline<DisperserAudioProcessor>(
                juce::File(argv[2]), "DISP-TR", "offset_norm,rms,gate,depth", 4,
                [](auto& processor) -> auto& { return processor.apvts; },
                [](auto& processor, auto& state)
                {
                    DispNativeSidechainTestAccess::selectNative(processor);
                    using namespace TR::Modulation::Tests;
                    return setNativeBaselineParameter(state, DisperserAudioProcessor::kParamSidechain, 1.0f)
                        && setNativeBaselineParameter(state, DisperserAudioProcessor::kParamSidechainGain, 0.0f)
                        && setNativeBaselineParameter(state, DisperserAudioProcessor::kParamSidechainSmooth, 0.5f)
                        && setNativeBaselineParameter(state, DisperserAudioProcessor::kParamSidechainPol, 1.0f);
                },
                [](const auto& processor, int sample, int, float* values)
                { DispNativeSidechainTestAccess::extract(processor, sample, values); });
            return ok ? 0 : 2;
        }
        if (argc == 3 && juce::String(argv[1]) == "--export-shared-sidechain-baseline")
        {
            const auto ok = TR::Modulation::Tests::exportNativeSidechainBaseline<DisperserAudioProcessor>(
                juce::File(argv[2]), "DISP-TR", "control,offset_norm", 2,
                [](auto& processor) -> auto& { return processor.apvts; },
                [](auto&, auto& state)
                {
                    using namespace TR::Modulation::Tests;
                    return setNativeBaselineParameter(state, DisperserAudioProcessor::kParamSidechain, 1.0f)
                        && setNativeBaselineParameter(state, DisperserAudioProcessor::kParamSidechainGain, 0.0f)
                        && setNativeBaselineParameter(state, DisperserAudioProcessor::kParamSidechainSmooth, 0.5f)
                        && setNativeBaselineParameter(state, DisperserAudioProcessor::kParamSidechainPol, 1.0f);
                },
                [](const auto& processor, int sample, int, float* values)
                { DispNativeSidechainTestAccess::extractShared(processor, sample, values); });
            return ok ? 0 : 2;
        }
        {
            auto auditProcessor = std::make_unique<DisperserAudioProcessor>();
            TR::DispUIV2::DispBackendBindings auditBackend(*auditProcessor);
            require(TR::Modulation::Tests::auditMotionRecipeBackend(
                        auditBackend, auditProcessor->apvts,
                        DisperserAudioProcessor::kParamJitter, "native-jitter", 3, 8, 1).passed(),
                    "DISP Jitter recipe UI/backend contract failed");
        }
        auto processor = std::make_unique<DisperserAudioProcessor>();
        require(processor->acceptsMidi(), "DISP does not advertise MIDI input");
        auto layout = processor->getBusesLayout();
        layout.inputBuses.set(1, juce::AudioChannelSet::stereo());
        require(processor->setBusesLayout(layout), "DISP shared sidechain layout rejected");
        processor->prepareToPlay(48000.0, 512);

        auto state = TR::Modulation::makeDefaultState();
        state.midiSources[static_cast<std::size_t>(TR::Modulation::MidiSourceType::note)]
            .smoothingSeconds = 0.0f;
        require(TR::Modulation::appendRoute(state, TR::Modulation::Route {
            0, 0, true, TR::Modulation::SourceId::midi(TR::Modulation::MidiSourceType::note),
            TR::Modulation::Polarity::unipolar, 1.0f, "macro:1",
            TR::Modulation::SourceId::none(), TR::Modulation::Polarity::unipolar,
            TR::Modulation::makeLinearCurve(), TR::Modulation::makeLinearCurve() }),
            "DISP MIDI -> Macro route rejected");
        require(TR::Modulation::appendRoute(state, TR::Modulation::Route {
            0, 0, true, TR::Modulation::SourceId::macro(1),
            TR::Modulation::Polarity::unipolar, 1.0f, "core:frequency",
            TR::Modulation::SourceId::none(), TR::Modulation::Polarity::unipolar,
            TR::Modulation::makeLinearCurve(), TR::Modulation::makeLinearCurve() }),
            "DISP Macro -> Frequency route rejected");
        require(processor->setModulationState(state), "DISP modulation state rejected");

        TR::DispUIV2::DispBackendBindings presetBackend(*processor);
        const auto presetMusicalState = presetBackend.readMusicalState();
        require(presetBackend.validateMusicalState(presetMusicalState)
                    && presetMusicalState.textValues.count(
                           TR::Modulation::Integration::presetStateId) == 1,
                "DISP internal preset state omitted modulation XML");
        require(presetBackend.parameterSnapshot().count("mod_macro_1") == 1,
                "DISP internal preset state omitted Macro parameters");

        std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
        editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
        editor->setVisible(true);
        juce::Timer::callPendingTimersSynchronously();
        auto* macrosButton = dynamic_cast<juce::Button*>(findById(*editor, "macros-panel-button"));
        auto* matrixButton = dynamic_cast<juce::Button*>(findById(*editor, "matrix-workspace-button"));
        auto* workspace = findById(*editor, "auxiliary-workspace");
        require(macrosButton != nullptr && matrixButton != nullptr
                    && workspace != nullptr && !workspace->isVisible(),
                "DISP MACROS/MATRIX controls are missing");
        const auto productSize = juce::Point<int> { editor->getWidth(), editor->getHeight() };
        TR::Modulation::Tests::clickButton(*macrosButton);
        auto* compactPanel = findById(*editor, "macro-panel");
        require(compactPanel != nullptr && compactPanel->isShowing()
                    && !workspace->isVisible()
                    && editor->getWidth() == productSize.x + 200
                    && editor->getHeight() == productSize.y,
                "First MACROS click did not open the compact Macro panel");
        TR::Modulation::Tests::clickButton(*matrixButton);
        require(workspace->isVisible() && matrixButton->getToggleState(),
                "DISP MATRIX workspace did not open");
        require(editor->getWidth() == 1040 && editor->getHeight() == 680,
                "DISP MATRIX workspace did not request its canonical size");
        const auto journey = TR::Modulation::Tests::auditMacroJourney(workspace);
        require(journey.workspaceFound && journey.visible && journey.hasAllMacroCards
                    && journey.hasFocusTargets && journey.containerHasNoFocusRing
                    && journey.nameEditingContract,
                "DISP MATRIX journey has complete cards and control-local focus");
        TR::Modulation::Tests::clickButton(*matrixButton);
        require(compactPanel->isShowing()
                    && editor->getWidth() == productSize.x + 200
                    && editor->getHeight() == productSize.y,
                "DISP MATRIX did not restore the originating MACROS panel");

        process(*processor, true);
        for (int block = 0; block < 32; ++block) process(*processor, false);
        float base = 0.0f, effective = 0.0f;
        require(processor->modulationDestinationValues("core:frequency", base, effective),
                "DISP destination telemetry unavailable");
        require(processor->modulationTelemetry().destinationCount > 0,
                "DISP workspace telemetry snapshot is empty");
        require(effective > base * 1.5f, "DISP MIDI Macro route did not reach DSP destination");
        require(DispNativeSidechainTestAccess::enableShared(*processor),
                "DISP legacy Sidechain adapter could not be enabled");
        for (int block = 0; block < 32; ++block) process(*processor, false, 0.5f);
        const auto sidechainTelemetry = processor->modulationTelemetry();
        require(sidechainTelemetry.sources[1].signalState
                    == TR::Modulation::Runtime::SourceSignalState::active
                    && sidechainTelemetry.sources[1].value > 0.45f,
                "DISP legacy controls did not drive the shared RMS profile");

        require(TR::Modulation::Tests::setNativeBaselineParameter(
                    processor->apvts, DisperserAudioProcessor::kParamSidechain, 0.0f),
                "DISP legacy Sidechain could not be disabled");
        auto matrixSidechain = TR::Modulation::makeDefaultState();
        matrixSidechain.analysisSources[1].feature = TR::Modulation::AnalysisFeature::motionRmsEnvelope;
        matrixSidechain.analysisSources[1].detector.smooth = DisperserAudioProcessor::kSidechainSmoothDefault;
        require(TR::Modulation::appendRoute(matrixSidechain, { 0, 0, true,
            TR::Modulation::SourceId::sidechainEnvelope(), TR::Modulation::Polarity::unipolar,
            1.0f, "sidechain:frequency-offset", TR::Modulation::SourceId::none(),
            TR::Modulation::Polarity::unipolar, TR::Modulation::makeLinearCurve(),
            TR::Modulation::makeLinearCurve() }) && processor->setModulationState(matrixSidechain),
            "DISP explicit MATRIX Sidechain route was rejected");
        for (int block = 0; block < 8; ++block) process(*processor, false, 0.5f);
        require(processor->modulationDestinationValues(
                    "sidechain:frequency-offset", base, effective)
                    && base == 0.0f && effective > 0.05f
                    && std::abs(DispNativeSidechainTestAccess::matrixOffset(*processor, 511)
                                - effective) < 1.0e-6f,
                "DISP MATRIX-only Sidechain did not reach the +/-5000 Hz offset law");
        require(processor->setModulationState(state),
                "DISP could not restore its main smoke state after MATRIX Sidechain proof");

        auto jitterRecipe = TR::DispModulation::makeJitterParityRecipe(
            TR::Modulation::makeDefaultState());
        require(jitterRecipe.routes.size() == 8
                    && std::none_of(jitterRecipe.routes.begin(), jitterRecipe.routes.end(),
                        [](const auto& route) { return route.destinationId == "core:feedback"; }),
                "DISP adaptive recipe topology is incomplete or targets feedback");
        require(TR::Modulation::Tests::setNativeBaselineParameter(
                    processor->apvts, "mod_macro_1", 1.0f)
                    && processor->setModulationState(jitterRecipe),
                "DISP adaptive Jitter recipe was rejected");
        float maximumFrequencyMotion = 0.0f;
        for (int block = 0; block < 32; ++block)
        {
            process(*processor, false);
            require(processor->modulationDestinationValues(
                        "motion:series-1-frequency-octave", base, effective),
                    "DISP series-motion telemetry unavailable");
            maximumFrequencyMotion = juce::jmax(maximumFrequencyMotion, std::abs(effective));
        }
        auto motionTelemetry = processor->modulationTelemetry();
        const auto firstMotion = TR::Modulation::Runtime::additionalMotionSourceFirstIndex;
        const auto firstReference = motionTelemetry.sources[firstMotion].effectiveMotionReference;
        require(maximumFrequencyMotion > 1.0e-4f
                    && motionTelemetry.sources[firstMotion].motionReferenceAvailable,
                "DISP adaptive sources did not consume the group-delay reference");
        require(TR::Modulation::Tests::setNativeBaselineParameter(
                    processor->apvts, DisperserAudioProcessor::kParamFreq, 5000.0f),
                "DISP reference sweep parameter rejected");
        for (int block = 0; block < 32; ++block) process(*processor, false);
        motionTelemetry = processor->modulationTelemetry();
        require(std::abs(motionTelemetry.sources[firstMotion].effectiveMotionReference
                         - firstReference) > 0.01f,
                "DISP frequency did not alter the group-delay reference");
        require(processor->modulationDestinationValues("core:feedback", base, effective)
                    && std::abs(effective - base) <= 1.0e-7f,
                "DISP adaptive Jitter recipe changed feedback");
        require(processor->setModulationState(state),
                "DISP could not restore its main smoke state after adaptive recipe proof");

        require(TR::Testing::writePluginCpuComparison (std::cout, "DISP", *processor),
                "DISP CPU comparison could not restore modulation state");

        juce::MemoryBlock preset;
        processor->getStateInformation(preset);
        editor.reset();
        auto restored = std::make_unique<DisperserAudioProcessor>();
        restored->setStateInformation(preset.getData(), static_cast<int>(preset.getSize()));
        require(restored->modulationState().routes.size() == 2,
                "DISP modulation routes did not survive preset round-trip");
        std::cout << "DISP modulation smoke probe passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "DISP modulation smoke probe failed: " << error.what() << '\n';
        return 1;
    }
}
