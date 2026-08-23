#include "DispReverseFirRuntime.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (! condition) throw std::runtime_error(message);
}

std::vector<float> render(double sampleRate, int blockSize, bool automate)
{
    constexpr int length = 4096;
    DISP::Tests::ReverseFirRuntime runtime;
    runtime.prepare(sampleRate);
    runtime.setParameters(16, 1, 1000.0f, 0.65f);
    std::vector<float> input(length, 0.0f), output(length, 0.0f);
    input[0] = 1.0f;
    for (int offset = 0; offset < length; offset += blockSize)
    {
        const auto count = std::min(blockSize, length - offset);
        if (automate && offset == 1024)
            runtime.setParameters(16, 1, 2500.0f, 0.35f, 256);
        runtime.processBlock(input.data() + offset, output.data() + offset, count);
    }
    return output;
}

double correlation(const std::vector<float>& left, const std::vector<float>& right)
{
    double dot = 0.0, leftEnergy = 0.0, rightEnergy = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        dot += static_cast<double>(left[index]) * right[index];
        leftEnergy += static_cast<double>(left[index]) * left[index];
        rightEnergy += static_cast<double>(right[index]) * right[index];
    }
    return dot / std::sqrt(std::max(1.0e-30, leftEnergy * rightEnergy));
}
}

int main()
{
    try
    {
        for (const auto sampleRate : { 44'100.0, 48'000.0, 96'000.0 })
        {
            const auto reference = render(sampleRate, 128, false);
            for (const auto blockSize : { 32, 64, 128, 256, 512 })
            {
                const auto result = render(sampleRate, blockSize, false);
                auto maximumDelta = 0.0f;
                for (std::size_t index = 0; index < result.size(); ++index)
                {
                    require(std::isfinite(result[index]), "FIR reverse output is non-finite");
                    maximumDelta = std::max(maximumDelta, std::abs(result[index] - reference[index]));
                }
                require(maximumDelta < 1.0e-6f, "FIR reverse is block-size dependent");
            }

            const auto automated = render(sampleRate, 128, true);
            require(std::all_of(automated.begin(), automated.end(),
                                [](float value) { return std::isfinite(value); }),
                    "FIR reverse IR swap is non-finite");
            std::cout << "sample_rate=" << sampleRate
                      << " block_invariance=pass"
                      << " automation_correlation=" << correlation(reference, automated)
                      << '\n';
        }
        std::cout << "DISP reverse FIR runtime probe passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "DISP reverse FIR runtime probe failed: " << error.what() << '\n';
        return 1;
    }
}
