#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace DISP::DSP
{
class ReverseDispersionRuntime final
{
public:
    static constexpr int maxStages = 128;
    static constexpr int maxSections = maxStages / 2;
    static constexpr int maxSeries = 4;

    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = std::max(1.0, sampleRate);
        prepared_ = true;
        reset();
    }

    void reset() noexcept
    {
        for (auto* bank : { &active_, &target_ })
            bank->clearStates();
        fadeRemaining_ = 0;
        fadeTotal_ = 0;
        configured_ = false;
        jitterSampleCounter_ = 0;
        jitterSeed_ = 0x44564A4Du;
    }

    void setParameters(int stages, int series, float frequencyHz, float shape, bool alternate,
                       bool dual, float jitter) noexcept
    {
        if (! prepared_)
            return;

        const auto safeStages = std::clamp(stages, 0, maxStages);
        const auto safeSeries = std::clamp(series, 1, maxSeries);
        if (! configured_)
        {
            active_.configure(safeStages, safeSeries, frequencyHz, shape, alternate, dual, jitter, jitterSeed_, sampleRate_);
            target_ = active_;
            configured_ = true;
            jitterAmount_ = std::clamp(jitter, 0.0f, 1.0f);
            return;
        }

        const auto changed = active_.stages != safeStages
            || active_.series != safeSeries
            || std::abs(active_.frequencyHz - frequencyHz) > 0.001f
            || std::abs(active_.shape - shape) > 0.0002f
            || active_.alternate != alternate
            || active_.dual != dual;
        if (! changed)
            return;

        target_ = active_;
        target_.configure(safeStages, safeSeries, frequencyHz, shape, alternate, dual, jitter, jitterSeed_, sampleRate_);
        fadeTotal_ = std::max(1, static_cast<int>(std::round(sampleRate_ * 0.005)));
        fadeRemaining_ = fadeTotal_;
        jitterAmount_ = std::clamp(jitter, 0.0f, 1.0f);
        jitterSampleCounter_ = 0;
    }

    void processBlock(float* left, float* right, int numSamples, bool stereo, int style, float feedback) noexcept
    {
        if (! configured_ || numSamples <= 0)
            return;

        const auto crossFeedback = style == 2;
        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (jitterAmount_ > 0.0001f)
            {
                constexpr int jitterHoldSamples = 256;
                if (jitterSampleCounter_ >= jitterHoldSamples)
                {
                    jitterSampleCounter_ = 0;
                    jitterSeed_ = nextSeed(jitterSeed_);
                    target_ = active_;
                    target_.configure(active_.stages, active_.series, active_.frequencyHz, active_.shape,
                                      active_.alternate, active_.dual, jitterAmount_, jitterSeed_, sampleRate_);
                    fadeTotal_ = std::max(1, static_cast<int>(std::round(sampleRate_ * 0.005)));
                    fadeRemaining_ = fadeTotal_;
                }
            }

            const auto activePreviousL = active_.feedbackL[0];
            const auto activePreviousR = active_.feedbackR[0];
            const auto targetPreviousL = target_.feedbackL[0];
            const auto targetPreviousR = target_.feedbackR[0];
            const auto activeL = active_.process(left[sample], true, stereo, crossFeedback, feedback,
                                                 activePreviousL, activePreviousR);
            const auto activeR = active_.processRight(right != nullptr ? right[sample] : left[sample],
                                                      stereo, crossFeedback, feedback,
                                                      activePreviousR, activePreviousL);
            if (fadeRemaining_ <= 0)
            {
                left[sample] = activeL;
                if (right != nullptr && stereo)
                    right[sample] = activeR;
                ++jitterSampleCounter_;
                continue;
            }

            const auto targetL = target_.process(left[sample], true, stereo, crossFeedback, feedback,
                                                 targetPreviousL, targetPreviousR);
            const auto targetR = target_.processRight(right != nullptr ? right[sample] : left[sample],
                                                     stereo, crossFeedback, feedback,
                                                     targetPreviousR, targetPreviousL);
            const auto alpha = 1.0f - static_cast<float>(fadeRemaining_) / static_cast<float>(fadeTotal_);
            left[sample] = activeL + alpha * (targetL - activeL);
            if (right != nullptr && stereo)
                right[sample] = activeR + alpha * (targetR - activeR);
            if (--fadeRemaining_ == 0)
                active_ = target_;

            ++jitterSampleCounter_;
        }
    }

private:
    struct Section
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 1.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

        float process(float input) noexcept
        {
            const auto output = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = input;
            y2 = y1;
            y1 = output;
            return output;
        }
    };

    struct Bank
    {
        std::array<std::array<Section, maxSections>, maxSeries> sections {};
        std::array<std::array<Section, maxSections>, maxSeries> sectionsR {};
        std::array<float, maxSeries> feedbackL {};
        std::array<float, maxSeries> feedbackR {};
        int stages = 0;
        int series = 1;
        float frequencyHz = 1000.0f;
        float shape = 0.0f;
        bool alternate = false;
        bool dual = false;

        void clearStates() noexcept
        {
            for (auto& lane : sections)
                for (auto& section : lane)
                    section.x1 = section.x2 = section.y1 = section.y2 = 0.0f;
            for (auto& lane : sectionsR)
                for (auto& section : lane)
                    section.x1 = section.x2 = section.y1 = section.y2 = 0.0f;
            feedbackL.fill(0.0f);
            feedbackR.fill(0.0f);
        }

        void configure(int newStages, int newSeries, float frequency, float newShape,
                       bool newAlternate, bool newDual, float jitter, std::uint32_t seed,
                       double sampleRate) noexcept
        {
            const auto oldSectionCount = (stages + 1) / 2;
            const auto oldSeries = series;
            stages = newStages;
            series = newSeries;
            frequencyHz = frequency;
            shape = std::clamp(newShape, 0.0f, 1.0f);
            alternate = newAlternate;
            dual = newDual;
            const auto maxFrequency = std::min(20'000.0f, 0.49f * static_cast<float>(sampleRate));
            const auto centre = std::clamp(frequencyHz, 20.0f, maxFrequency);
            const auto spread = 0.12f + (4.0f - 0.12f) * (1.0f - shape);
            const auto sectionCount = (stages + 1) / 2;

            for (int lane = 0; lane < maxSeries; ++lane)
                for (int section = 0; section < maxSections; ++section)
                    if (lane >= series || section >= sectionCount || lane >= oldSeries || section >= oldSectionCount)
                    {
                        auto& state = sections[static_cast<std::size_t>(lane)][static_cast<std::size_t>(section)];
                        state.x1 = state.x2 = state.y1 = state.y2 = 0.0f;
                        auto& stateR = sectionsR[static_cast<std::size_t>(lane)][static_cast<std::size_t>(section)];
                        stateR.x1 = stateR.x2 = stateR.y1 = stateR.y2 = 0.0f;
                    }

            for (int lane = 0; lane < series; ++lane)
                for (int section = 0; section < sectionCount; ++section)
                {
                    const auto stage = std::min(stages - 1, section * 2);
                    const auto u = stages > 1
                        ? (2.0f * static_cast<float>(stage) / static_cast<float>(stages - 1) - 1.0f)
                        : 0.0f;
                    const auto jitterFreq = (unitHash(seed, lane, section, 0) * 2.0f - 1.0f)
                        * (0.10f * std::clamp(jitter, 0.0f, 1.0f));
                    const auto jitterShape = (unitHash(seed, lane, section, 1) * 2.0f - 1.0f)
                        * (0.12f * std::clamp(jitter, 0.0f, 1.0f));
                    const auto forwardFrequency = centre * std::pow(2.0f, 0.5f * spread * u + jitterFreq);
                    const auto laneShape = std::clamp(shape + jitterShape, 0.0f, 1.0f);
                    const auto laneRadius = 0.84f + 0.10f * laneShape;
                    const auto reverseFrequency = std::clamp(maxFrequency - forwardFrequency,
                                                              1'000.0f, maxFrequency - 20.0f);
                    const auto theta = 2.0f * 3.14159265358979323846f * reverseFrequency
                        / static_cast<float>(sampleRate);
                    const auto cosine = std::cos(theta);
                    const auto alternateScale = alternate && (section & 1) ? -1.0f : 1.0f;
                    auto& coeff = sections[static_cast<std::size_t>(lane)][static_cast<std::size_t>(section)];
                    coeff.b0 = laneRadius * laneRadius;
                    coeff.b1 = -2.0f * laneRadius * cosine * alternateScale;
                    coeff.b2 = 1.0f;
                    coeff.a1 = -2.0f * laneRadius * cosine * alternateScale;
                    coeff.a2 = laneRadius * laneRadius;

                    auto& coeffR = sectionsR[static_cast<std::size_t>(lane)][static_cast<std::size_t>(section)];
                    const auto frequencyR = dual ? forwardFrequency * 0.5f : reverseFrequency;
                    const auto thetaR = 2.0f * 3.14159265358979323846f * frequencyR
                        / static_cast<float>(sampleRate);
                    const auto cosineR = std::cos(thetaR);
                    coeffR.b0 = laneRadius * laneRadius;
                    coeffR.b1 = -2.0f * laneRadius * cosineR * alternateScale;
                    coeffR.b2 = 1.0f;
                    coeffR.a1 = -2.0f * laneRadius * cosineR * alternateScale;
                    coeffR.a2 = laneRadius * laneRadius;
                }
        }

        float process(float input, bool left, bool stereo, bool crossFeedback,
                      float feedback, float previousSelf, float previousOther) noexcept
        {
            auto& feedbackState = left ? feedbackL[0] : feedbackR[0];
            auto value = input + feedback * (crossFeedback && stereo ? previousOther : previousSelf);
            for (int lane = 0; lane < series; ++lane)
                for (int section = 0; section < (stages + 1) / 2; ++section)
                    value = sections[static_cast<std::size_t>(lane)][static_cast<std::size_t>(section)].process(value);
            feedbackState = value;
            return value;
        }

        float processRight(float input, bool stereo, bool crossFeedback,
                           float feedback, float previousSelf, float previousOther) noexcept
        {
            auto value = input + feedback * (crossFeedback && stereo ? previousOther : previousSelf);
            for (int lane = 0; lane < series; ++lane)
                for (int section = 0; section < (stages + 1) / 2; ++section)
                    value = sectionsR[static_cast<std::size_t>(lane)][static_cast<std::size_t>(section)].process(value);
            feedbackR[0] = value;
            return value;
        }
    };

    static std::uint32_t nextSeed(std::uint32_t value) noexcept
    {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        return value != 0 ? value : 0xA341316Cu;
    }

    static float unitHash(std::uint32_t seed, int lane, int section, int salt) noexcept
    {
        auto value = seed ^ (0x9E3779B9u * static_cast<std::uint32_t>(lane + 1));
        value ^= 0x85EBCA6Bu * static_cast<std::uint32_t>(section + 1);
        value ^= 0xC2B2AE35u * static_cast<std::uint32_t>(salt + 1);
        value = nextSeed(value);
        return static_cast<float>(value) / static_cast<float>(0xFFFFFFFFu);
    }

    double sampleRate_ = 48'000.0;
    bool prepared_ = false;
    bool configured_ = false;
    Bank active_ {};
    Bank target_ {};
    int fadeRemaining_ = 0;
    int fadeTotal_ = 0;
    float jitterAmount_ = 0.0f;
    int jitterSampleCounter_ = 0;
    std::uint32_t jitterSeed_ = 0x44564A4Du;
};
}
