#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace DISP::Tests
{
class ReverseFirRuntime final
{
public:
    static constexpr int maxTaps = 512;

    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = std::max(1.0, sampleRate);
        active_.clear();
        target_.clear();
        fadeRemaining_ = 0;
        fadeTotal_ = 0;
    }

    void setParameters(int stages, int series, float frequencyHz, float shape,
                       int fadeSamples = 0) noexcept
    {
        build(target_, stages, series, frequencyHz, shape);
        if (! configured_ || fadeSamples <= 0)
        {
            active_ = target_;
            configured_ = true;
            fadeRemaining_ = 0;
            fadeTotal_ = 0;
            return;
        }
        fadeTotal_ = std::max(1, fadeSamples);
        fadeRemaining_ = fadeTotal_;
    }

    void reset() noexcept
    {
        active_.clearState();
        target_.clearState();
        fadeRemaining_ = 0;
        fadeTotal_ = 0;
    }

    void processBlock(const float* input, float* output, int numSamples) noexcept
    {
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto active = active_.process(input[sample]);
            if (fadeRemaining_ <= 0)
            {
                output[sample] = active;
                continue;
            }
            const auto target = target_.process(input[sample]);
            const auto progress = 1.0f - static_cast<float>(fadeRemaining_)
                / static_cast<float>(fadeTotal_);
            output[sample] = active + progress * (target - active);
            if (--fadeRemaining_ == 0)
                active_ = target_;
        }
    }

    int tapCount() const noexcept { return active_.tapCount; }

private:
    struct Bank
    {
        std::array<float, maxTaps> taps {};
        std::array<float, maxTaps> ring {};
        int tapCount = 1;
        int writeIndex = 0;

        void clear() noexcept
        {
            taps.fill(0.0f);
            clearState();
            tapCount = 1;
        }

        void clearState() noexcept
        {
            ring.fill(0.0f);
            writeIndex = 0;
        }

        float process(float input) noexcept
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
    };

    static float coefficient(float frequency, float sampleRate) noexcept
    {
        const auto tangent = std::tan (3.14159265358979323846f * frequency / sampleRate);
        return std::isfinite(tangent) ? (1.0f - tangent) / (1.0f + tangent) : 0.0f;
    }

    void build(Bank& bank, int stages, int series, float frequencyHz, float shape) const noexcept
    {
        bank.clear();
        constexpr float minimum = 20.0f;
        const auto sampleRate = static_cast<float>(sampleRate_);
        const auto maximum = std::min(20'000.0f, 0.49f * sampleRate);
        const auto centre = std::clamp(frequencyHz, minimum, maximum);
        const auto safeShape = std::clamp(shape, 0.0f, 1.0f);
        const auto logPos = std::log2(centre / minimum) / std::log2(maximum / minimum);
        const auto lowComp = std::pow(std::clamp(1.0f - logPos, 0.0f, 1.0f), 1.15f);
        const auto shapeStrength = 1.0f + 0.95f * lowComp;
        const auto shapeComp = std::clamp(0.5f + (safeShape - 0.5f) * shapeStrength, 0.0f, 1.0f);
        const auto spreadMax = 4.0f + 1.1f * lowComp;
        const auto spread = spreadMax + (0.12f - spreadMax) * shapeComp;
        const auto warpGamma = 0.45f + (3.0f + 0.8f * lowComp - 0.45f) * shapeComp;
        const auto safeStages = std::clamp(stages, 1, 128);
        const auto safeSeries = std::clamp(series, 1, 4);

        std::array<float, maxTaps> impulse {};
        impulse[0] = 1.0f;
        for (int lane = 0; lane < safeSeries; ++lane)
            for (int stage = 0; stage < safeStages; ++stage)
            {
                const auto u = safeStages > 1
                    ? 2.0f * static_cast<float>(stage) / static_cast<float>(safeStages - 1) - 1.0f
                    : 0.0f;
                const auto warped = std::copysign(std::pow(std::abs(u), warpGamma), u);
                const auto frequency = std::clamp(centre * std::pow(2.0f, 0.5f * spread * warped),
                                                   minimum, maximum);
                const auto a = coefficient(frequency, sampleRate);
                float state = 0.0f;
                for (auto& value : impulse)
                {
                    const auto output = -a * value + state;
                    state = value + a * output;
                    value = output;
                }
            }

        int length = maxTaps;
        const auto peak = std::abs(impulse[0]);
        for (int index = maxTaps - 1; index > 0; --index)
            if (std::abs(impulse[static_cast<std::size_t>(index)]) > peak * 1.0e-5f)
            {
                length = index + 1;
                break;
            }
        bank.tapCount = std::max(1, length);
        for (int tap = 0; tap < bank.tapCount; ++tap)
            bank.taps[static_cast<std::size_t>(tap)] = impulse[static_cast<std::size_t>(bank.tapCount - 1 - tap)];
    }

    double sampleRate_ = 48'000.0;
    bool configured_ = false;
    Bank active_ {}, target_ {};
    int fadeRemaining_ = 0;
    int fadeTotal_ = 0;
};
}
