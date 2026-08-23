#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
constexpr int maxSections = 64;
constexpr float pi = 3.14159265358979323846f;

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
    std::array<Section, maxSections> sections {};
    int count = 0;

    float process(float input) noexcept
    {
        for (int index = 0; index < count; ++index)
            input = sections[static_cast<std::size_t>(index)].process(input);
        return input;
    }
};

class ReverseDispersionBank
{
public:
    void prepare(double sampleRate, int stages)
    {
        sampleRate_ = sampleRate;
        stages_ = std::clamp(stages, 1, maxSections * 2);
        active_.count = (stages_ + 1) / 2;
        target_.count = active_.count;
        reset();
    }

    void setParameters(float frequencyHz, float shape)
    {
        fillCoefficients(active_, frequencyHz, shape);
        target_ = active_;
        fadeRemaining_ = 0;
    }

    void beginParameterChange(float frequencyHz, float shape, int fadeSamples)
    {
        target_ = active_;
        fillCoefficients(target_, frequencyHz, shape);
        fadeTotal_ = std::max(1, fadeSamples);
        fadeRemaining_ = fadeTotal_;
    }

    void reset() noexcept
    {
        for (auto* bank : { &active_, &target_ })
            for (auto& section : bank->sections)
                section.x1 = section.x2 = section.y1 = section.y2 = 0.0f;
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

private:
    void fillCoefficients(Bank& bank, float frequencyHz, float shape) const noexcept
    {
        const auto sr = static_cast<float>(sampleRate_);
        const auto maxFrequency = std::min(20'000.0f, 0.49f * sr);
        const auto center = std::clamp(frequencyHz, 20.0f, maxFrequency);
        const auto safeShape = std::clamp(shape, 0.0f, 1.0f);
        const auto radius = 0.84f + 0.10f * safeShape;
        const auto spread = 0.12f + (4.0f - 0.12f) * (1.0f - safeShape);
        const auto count = bank.count;

        for (int section = 0; section < count; ++section)
        {
            const auto firstStage = std::min(stages_ - 1, section * 2);
            const auto normalized = stages_ > 1
                ? (2.0f * static_cast<float>(firstStage) / static_cast<float>(stages_ - 1) - 1.0f)
                : 0.0f;
            const auto forwardFrequency = center * std::pow(2.0f, 0.5f * spread * normalized);
            const auto reverseFrequency = std::clamp(maxFrequency - forwardFrequency,
                                                     1'000.0f, maxFrequency - 20.0f);
            const auto theta = 2.0f * pi * reverseFrequency / sr;
            const auto cosine = std::cos(theta);
            auto& state = bank.sections[static_cast<std::size_t>(section)];
            state.b0 = radius * radius;
            state.b1 = -2.0f * radius * cosine;
            state.b2 = 1.0f;
            state.a1 = -2.0f * radius * cosine;
            state.a2 = radius * radius;
        }
    }

    double sampleRate_ = 48'000.0;
    int stages_ = 16;
    Bank active_ {};
    Bank target_ {};
    int fadeRemaining_ = 0;
    int fadeTotal_ = 0;
};

void require(bool condition, const char* message)
{
    if (! condition)
        throw std::runtime_error(message);
}

std::vector<float> render(double sampleRate, int blockSize, bool automate)
{
    constexpr int length = 8192;
    ReverseDispersionBank bank;
    bank.prepare(sampleRate, 16);
    bank.setParameters(1'000.0f, 0.65f);
    std::vector<float> input(length, 0.0f), output(length, 0.0f);
    input[0] = 1.0f;
    for (int offset = 0; offset < length; offset += blockSize)
    {
        const auto count = std::min(blockSize, length - offset);
        if (automate && offset == 2048)
            bank.beginParameterChange(2'500.0f, 0.35f, 256);
        bank.processBlock(input.data() + offset, output.data() + offset, count);
    }
    return output;
}
}

int main()
{
    try
    {
        for (const auto sampleRate : { 44'100.0, 48'000.0, 96'000.0 })
        {
            const auto reference = render(sampleRate, 128, false);
            for (const auto blockSize : { 16, 32, 64, 128, 256, 512, 1024 })
            {
                const auto result = render(sampleRate, blockSize, false);
                auto maximumDelta = 0.0f;
                for (std::size_t index = 0; index < result.size(); ++index)
                {
                    require(std::isfinite(result[index]), "reverse state probe produced non-finite audio");
                    maximumDelta = std::max(maximumDelta, std::abs(result[index] - reference[index]));
                }
                require(maximumDelta < 1.0e-6f, "reverse state probe is block-size dependent");
            }

            const auto automated = render(sampleRate, 128, true);
            require(std::all_of(automated.begin(), automated.end(),
                                [](float value) { return std::isfinite(value); }),
                    "reverse parameter crossfade produced non-finite audio");

            const auto start = std::chrono::steady_clock::now();
            constexpr int benchmarkBlocks = 4'000;
            std::vector<float> input(128, 0.0f), output(128, 0.0f);
            input[0] = 1.0f;
            ReverseDispersionBank benchmark;
            benchmark.prepare(sampleRate, 16);
            benchmark.setParameters(1'000.0f, 0.65f);
            for (int block = 0; block < benchmarkBlocks; ++block)
                benchmark.processBlock(input.data(), output.data(), 128);
            const auto elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            require(std::all_of(output.begin(), output.end(), [](float value) { return std::isfinite(value); }),
                    "reverse benchmark produced non-finite audio");
            std::cout << "sample_rate=" << sampleRate << " block_invariance=pass cpu_ms=" << elapsedMs << '\n';
        }
        std::cout << "DISP reverse state probe passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "DISP reverse state probe failed: " << error.what() << '\n';
        return 1;
    }
}
