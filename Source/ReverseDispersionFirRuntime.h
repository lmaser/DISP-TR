#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <chrono>

namespace DISP::DSP
{
// Causal, bounded FIR approximation of the time-reversed direct dispersion
// impulse. The IR is generated from the same forward allpass coefficient law;
// processBlock itself performs only fixed-size convolution and crossfade.
class ReverseDispersionFirRuntime final
{
public:
    // 512 taps is insufficient for the bounded direct response at 96 kHz.
    // Keep headroom for the 1024-sample audit window; this is still a
    // provisional runtime until partitioning replaces the per-sample loop.
    static constexpr int maxTaps = 1024;

    void prepare(double sampleRate) noexcept
    {
        stopWorker();
        sampleRate_ = std::max(1.0, sampleRate);
        active_.clear();
        target_.clear();
        configured_ = false;
        fadeRemaining_ = 0;
        fadeTotal_ = 0;
        feedbackL_ = feedbackR_ = 0.0f;
        startWorker();
    }

    ~ReverseDispersionFirRuntime() { stopWorker(); }

    void reset() noexcept
    {
        active_.clearState();
        target_.clearState();
        fadeRemaining_ = 0;
        fadeTotal_ = 0;
        feedbackL_ = feedbackR_ = 0.0f;
    }

    void setParameters(int stages, int series, float frequencyHz, float shape,
                       bool alternate, bool dual, float jitter) noexcept
    {
        const auto safeStages = std::clamp(stages, 1, 128);
        const auto safeSeries = std::clamp(series, 1, 4);
        if (configured_ && safeStages == stages_ && safeSeries == series_
            && std::abs(frequencyHz - frequencyHz_) < 0.001f
            && std::abs(shape - shape_) < 0.0002f
            && alternate == alternate_ && dual == dual_
            && std::abs(jitter - jitter_) < 0.0002f)
            return;

        stages_ = safeStages;
        series_ = safeSeries;
        frequencyHz_ = frequencyHz;
        shape_ = shape;
        alternate_ = alternate;
        dual_ = dual;
        jitter_ = jitter;
        if (! configured_)
        {
            build(target_, safeStages, safeSeries, frequencyHz, shape, alternate, dual, jitter);
            active_ = target_;
            configured_ = true;
            fadeRemaining_ = 0;
            return;
        }
        requestStages_.store(safeStages, std::memory_order_relaxed);
        requestSeries_.store(safeSeries, std::memory_order_relaxed);
        requestFrequency_.store(frequencyHz, std::memory_order_relaxed);
        requestShape_.store(shape, std::memory_order_relaxed);
        requestAlternate_.store(alternate, std::memory_order_relaxed);
        requestDual_.store(dual, std::memory_order_relaxed);
        requestJitter_.store(jitter, std::memory_order_relaxed);
        requestGeneration_.fetch_add(1, std::memory_order_release);
    }

    void processBlock(float* left, float* right, int numSamples, bool stereo,
                      int style, float feedback) noexcept
    {
        if (! configured_ || numSamples <= 0 || left == nullptr)
            return;
        pollPublished();
        const auto crossFeedback = style == 2;
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto inputL = left[sample]
                + feedback * (crossFeedback && stereo ? feedbackR_ : feedbackL_);
            const auto inputR = (right != nullptr && stereo ? right[sample] : inputL)
                + feedback * (crossFeedback && stereo ? feedbackL_ : feedbackR_);
            const auto activeL = active_.processL(inputL);
            const auto activeR = active_.processR(inputR);
            auto outputL = activeL;
            auto outputR = activeR;
            if (fadeRemaining_ > 0)
            {
                const auto targetL = target_.processL(inputL);
                const auto targetR = target_.processR(inputR);
                const auto alpha = 1.0f - static_cast<float>(fadeRemaining_)
                    / static_cast<float>(fadeTotal_);
                outputL += alpha * (targetL - outputL);
                outputR += alpha * (targetR - outputR);
                if (--fadeRemaining_ == 0)
                    active_ = target_;
            }
            left[sample] = outputL;
            if (right != nullptr && stereo)
                right[sample] = outputR;
            feedbackL_ = outputL;
            feedbackR_ = outputR;
        }
    }

    int latencySamples() const noexcept { return 0; }
    int tapCount() const noexcept { return active_.tapCount; }

private:
    void startWorker()
    {
        workerRunning_.store(true, std::memory_order_release);
        worker_ = std::thread([this]
        {
            uint64_t builtGeneration = 0;
            while (workerRunning_.load(std::memory_order_acquire))
            {
                const auto generation = requestGeneration_.load(std::memory_order_acquire);
                if (generation != 0 && generation != builtGeneration)
                {
                    Bank bank;
                    build(bank, requestStages_.load(std::memory_order_relaxed),
                          requestSeries_.load(std::memory_order_relaxed),
                          requestFrequency_.load(std::memory_order_relaxed),
                          requestShape_.load(std::memory_order_relaxed),
                          requestAlternate_.load(std::memory_order_relaxed),
                          requestDual_.load(std::memory_order_relaxed),
                          requestJitter_.load(std::memory_order_relaxed));
                    bank.generation = generation;
                    std::atomic_store_explicit(&published_, std::make_shared<Bank>(bank),
                                               std::memory_order_release);
                    builtGeneration = generation;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    void stopWorker() noexcept
    {
        workerRunning_.store(false, std::memory_order_release);
        if (worker_.joinable())
            worker_.join();
        std::atomic_store_explicit(&published_, std::shared_ptr<Bank> {},
                                   std::memory_order_release);
    }

    void pollPublished() noexcept
    {
        const auto bank = std::atomic_load_explicit(&published_, std::memory_order_acquire);
        if (! bank || bank->generation <= appliedGeneration_)
            return;
        target_ = *bank;
        target_.clearState();
        fadeTotal_ = std::max(1, static_cast<int>(std::round(sampleRate_ * 0.005)));
        fadeRemaining_ = fadeTotal_;
        appliedGeneration_ = bank->generation;
    }

    struct Bank
    {
        std::array<float, maxTaps> tapsL {};
        std::array<float, maxTaps> tapsR {};
        std::array<float, maxTaps> ringL {};
        std::array<float, maxTaps> ringR {};
        int tapCount = 1;
        uint64_t generation = 0;
        int writeIndexL = 0;
        int writeIndexR = 0;

        void clear() noexcept { tapsL.fill(0.0f); tapsR.fill(0.0f); clearState(); tapCount = 1; }
        void clearState() noexcept
        {
            ringL.fill(0.0f);
            ringR.fill(0.0f);
            writeIndexL = writeIndexR = 0;
        }

        float process(const std::array<float, maxTaps>& taps,
                      std::array<float, maxTaps>& ring, int& writeIndex,
                      float input) noexcept
        {
            ring[static_cast<std::size_t>(writeIndex)] = input;
            float output = 0.0f;
            for (int tap = 0; tap < tapCount; ++tap)
            {
                auto index = writeIndex - tap;
                if (index < 0) index += maxTaps;
                output += taps[static_cast<std::size_t>(tap)]
                    * ring[static_cast<std::size_t>(index)];
            }
            if (++writeIndex == maxTaps) writeIndex = 0;
            return output;
        }
        float processL(float input) noexcept { return process(tapsL, ringL, writeIndexL, input); }
        float processR(float input) noexcept { return process(tapsR, ringR, writeIndexR, input); }
    };

    static float coefficient(float frequency, float sampleRate) noexcept
    {
        const auto tangent = std::tan(3.14159265358979323846f * frequency / sampleRate);
        return std::isfinite(tangent) ? (1.0f - tangent) / (1.0f + tangent) : 0.0f;
    }

    void build(Bank& bank, int stages, int series, float frequencyHz, float shape,
               bool alternate, bool dual, float jitter) const noexcept
    {
        bank.clear();
        const auto sampleRate = static_cast<float>(sampleRate_);
        const auto maximum = std::min(20'000.0f, 0.49f * sampleRate);
        const auto centre = std::clamp(frequencyHz, 20.0f, maximum);
        const auto safeShape = std::clamp(shape, 0.0f, 1.0f);
        const auto logPos = std::log2(centre / 20.0f) / std::log2(maximum / 20.0f);
        const auto lowComp = std::pow(std::clamp(1.0f - logPos, 0.0f, 1.0f), 1.15f);
        const auto shapeStrength = 1.0f + 0.95f * lowComp;
        const auto shapeComp = std::clamp(0.5f + (safeShape - 0.5f) * shapeStrength, 0.0f, 1.0f);
        const auto spreadMax = 4.0f + 1.1f * lowComp;
        const auto spread = spreadMax + (0.12f - spreadMax) * shapeComp;
        const auto warpGamma = 0.45f + (3.0f + 0.8f * lowComp - 0.45f) * shapeComp;

        std::array<float, maxTaps> impulseL {}, impulseR {};
        impulseL[0] = impulseR[0] = 1.0f;
        for (int lane = 0; lane < series; ++lane)
            for (int stage = 0; stage < stages; ++stage)
            {
                const auto u = stages > 1
                    ? 2.0f * static_cast<float>(stage) / static_cast<float>(stages - 1) - 1.0f : 0.0f;
                const auto warped = std::copysign(std::pow(std::abs(u), warpGamma), u);
                const auto frequency = std::clamp(centre * std::pow(2.0f, 0.5f * spread * warped),
                                                   20.0f, maximum);
                const auto jitterOffset = (unitHash(lane, stage) * 2.0f - 1.0f)
                    * 0.10f * std::clamp(jitter, 0.0f, 1.0f);
                const auto aL = coefficient(std::clamp(frequency * std::pow(2.0f, jitterOffset),
                                                        20.0f, maximum), sampleRate);
                const auto aR = coefficient(std::clamp((dual ? frequency * 0.5f : frequency),
                                                        20.0f, maximum), sampleRate);
                processAllpass(impulseL, aL, alternate && (stage & 1));
                processAllpass(impulseR, aR, alternate && (stage & 1));
            }

        int length = maxTaps;
        for (int index = maxTaps - 1; index > 0; --index)
            if (std::max(std::abs(impulseL[static_cast<std::size_t>(index)]),
                         std::abs(impulseR[static_cast<std::size_t>(index)])) > 1.0e-5f)
            { length = index + 1; break; }
        bank.tapCount = std::max(1, length);
        for (int tap = 0; tap < bank.tapCount; ++tap)
        {
            bank.tapsL[static_cast<std::size_t>(tap)] = impulseL[static_cast<std::size_t>(length - 1 - tap)];
            bank.tapsR[static_cast<std::size_t>(tap)] = impulseR[static_cast<std::size_t>(length - 1 - tap)];
        }
    }

    static void processAllpass(std::array<float, maxTaps>& impulse, float coefficientValue,
                               bool negate) noexcept
    {
        const auto a = negate ? -coefficientValue : coefficientValue;
        float state = 0.0f;
        for (auto& value : impulse)
        {
            const auto output = -a * value + state;
            state = value + a * output;
            value = output;
        }
    }

    static float unitHash(int lane, int stage) noexcept
    {
        auto value = static_cast<std::uint32_t>(0x44564A4Du)
            ^ (0x9E3779B9u * static_cast<std::uint32_t>(lane + 1))
            ^ (0x85EBCA6Bu * static_cast<std::uint32_t>(stage + 1));
        value ^= value << 13; value ^= value >> 17; value ^= value << 5;
        return static_cast<float>(value) / static_cast<float>(0xFFFFFFFFu);
    }

    double sampleRate_ = 48'000.0;
    bool configured_ = false;
    int stages_ = 0, series_ = 0;
    float frequencyHz_ = 0.0f, shape_ = 0.0f, jitter_ = 0.0f;
    bool alternate_ = false, dual_ = false;
    Bank active_ {}, target_ {};
    int fadeRemaining_ = 0, fadeTotal_ = 0;
    float feedbackL_ = 0.0f, feedbackR_ = 0.0f;
    std::atomic<bool> workerRunning_ { false };
    std::thread worker_;
    std::shared_ptr<Bank> published_;
    std::atomic<uint64_t> requestGeneration_ { 0 };
    uint64_t appliedGeneration_ = 0;
    std::atomic<int> requestStages_ { 1 }, requestSeries_ { 1 };
    std::atomic<float> requestFrequency_ { 1000.0f }, requestShape_ { 0.0f }, requestJitter_ { 0.0f };
    std::atomic<bool> requestAlternate_ { false }, requestDual_ { false };
};
}
