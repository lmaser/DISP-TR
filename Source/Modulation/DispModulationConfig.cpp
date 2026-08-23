#include "DispModulationConfig.h"

#include "../PluginProcessor.h"
#include "../../../TR-Shared/Modulation/Recipes/TRAdaptiveMotionRecipe.h"
#include "../../../TR-Shared/Modulation/Recipes/TRMotionRecipeUtilities.h"

namespace TR::DispModulation
{
const std::vector<Modulation::Integration::ParameterDestination>& destinations()
{
    static const std::vector<Modulation::Integration::ParameterDestination> result = []
    {
        std::vector<Modulation::Integration::ParameterDestination> items {
        { "core:frequency", "CORE", "FREQUENCY", DisperserAudioProcessor::kParamFreq,
          DisperserAudioProcessor::kFreqMin, DisperserAudioProcessor::kFreqEffectiveMax,
          true, 0.01f },
        { "core:stages", "CORE", "STAGES", DisperserAudioProcessor::kParamAmount,
          static_cast<float>(DisperserAudioProcessor::kAmountMin),
          static_cast<float>(DisperserAudioProcessor::kAmountMax), false, 0.03f },
        { "core:feedback", "CORE", "FEEDBACK", DisperserAudioProcessor::kParamFeedback,
          DisperserAudioProcessor::kFeedbackMin, DisperserAudioProcessor::kFeedbackMax,
          false, 0.01f },
        { "core:mix", "CORE", "MIX", DisperserAudioProcessor::kParamMix,
          DisperserAudioProcessor::kMixMin, DisperserAudioProcessor::kMixMax,
          false, 0.01f },
        { "phase:shape", "PHASE", "SHAPE", DisperserAudioProcessor::kParamShape,
          0.0f, 1.0f, false, 0.02f },
        { "phase:jitter", "PHASE", "JITTER", DisperserAudioProcessor::kParamJitter,
          DisperserAudioProcessor::kJitterMin, DisperserAudioProcessor::kJitterMax,
          false, 0.02f },
        { "sidechain:frequency-offset", "SIDECHAIN", "FREQUENCY OFFSET", "",
          -1.0f, 1.0f, false, 0.0f,
          Modulation::Runtime::ModulationDomain::sampleControl,
          Modulation::Runtime::SignalRepresentation::control, true, 0.0f }
        };
        for (int series = 0; series < seriesMotionLaneCount; ++series)
        {
            const auto suffix = juce::String(series + 1);
            items.push_back({ "motion:series-" + suffix + "-frequency-octave",
                "MOTION", "SERIES " + suffix + " FREQUENCY", "",
                -1.0f, 1.0f, false, 0.0f,
                Modulation::Runtime::ModulationDomain::sampleControl,
                Modulation::Runtime::SignalRepresentation::control, true, 0.0f,
                series });
            items.push_back({ "motion:series-" + suffix + "-shape-offset",
                "MOTION", "SERIES " + suffix + " SHAPE", "",
                -1.0f, 1.0f, false, 0.0f,
                Modulation::Runtime::ModulationDomain::sampleControl,
                Modulation::Runtime::SignalRepresentation::control, true, 0.0f,
                series });
        }
        return items;
    }();
    return result;
}

Modulation::State makeJitterParityRecipe(Modulation::State state, int macroOneBased)
{
    using namespace Modulation::Recipes;
    macroOneBased = juce::jlimit(1, Modulation::macroCount, macroOneBased);
    for (int series = 0; series < seriesMotionLaneCount; ++series)
        removeRoutesTo(state, {
            "motion:series-" + juce::String(series + 1) + "-frequency-octave",
            "motion:series-" + juce::String(series + 1) + "-shape-offset" });
    state.macros[static_cast<std::size_t>(macroOneBased - 1)].name = "JITTER DEPTH";
    AdaptiveResponseSourceConfig frequency;
    frequency.seed = 0x44564a4cull;
    frequency.laneSeedStride = 0x10001ull;
    frequency.laneSeedXor = 0x44565031ull;
    frequency.lanePolicy = Modulation::MotionLanePolicy::destination;
    frequency.initialPhase = 0.113f;
    frequency.referenceMaximumMs = 120000.0f;
    frequency.rateMaximumMs = 120000.0f;
    frequency.outputScale = -2.0f;
    frequency.amountLinearSmoothing = true;
    frequency.controlSmoothingSeconds = 0.0f;
    configureAdaptiveResponseSource(state, 2, macroOneBased, frequency);

    AdaptiveResponseSourceConfig shape = frequency;
    shape.laneSeedXor = 0x44565053ull;
    shape.initialPhase = 0.617f;
    shape.outputScale = 0.45f;
    shape.laneOffset = 1;
    configureAdaptiveResponseSource(state, 5, macroOneBased, shape);

    for (int series = 0; series < seriesMotionLaneCount; ++series)
    {
        appendAdaptiveResponseRoute(state, 2,
            "motion:series-" + juce::String(series + 1) + "-frequency-octave");
        appendAdaptiveResponseRoute(state, 5,
            "motion:series-" + juce::String(series + 1) + "-shape-offset");
    }
    return state;
}
}
