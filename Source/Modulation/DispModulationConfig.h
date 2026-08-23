#pragma once

#include "../../../TR-Shared/Modulation/Integration/TRParameterModulationBridge.h"

#include <vector>

namespace TR::DispModulation
{
inline constexpr int seriesMotionLaneCount = 4;

enum Destination : int
{
    frequency = 0,
    stages,
    feedback,
    mix,
    shape,
    jitter,
    sidechainFrequencyOffset,
    seriesMotionFirst,
    destinationCount = seriesMotionFirst + 2 * seriesMotionLaneCount
};

inline constexpr int seriesFrequencyOffset(int series) noexcept
{
    return seriesMotionFirst + 2 * series;
}
inline constexpr int seriesShapeOffset(int series) noexcept
{
    return seriesMotionFirst + 2 * series + 1;
}

const std::vector<Modulation::Integration::ParameterDestination>& destinations();
Modulation::State makeJitterParityRecipe(Modulation::State, int macroOneBased = 1);
}
