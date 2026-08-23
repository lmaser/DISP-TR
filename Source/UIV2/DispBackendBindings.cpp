#include "DispBackendBindings.h"
#include "DispUiDefinition.h"
#include "../Modulation/DispModulationConfig.h"
#include "../../../TR-Shared/Modulation/Integration/TRModulationPresetCodec.h"

#include <cmath>

namespace TR::DispUIV2
{
namespace
{
constexpr const char* midiPortKey = "midiPort";
constexpr const char* midiDelayKey = "midiDelayMs";
constexpr const char* selectedTaskKey = "uiV2SelectedTask";
constexpr const char* surfaceKey = "uiV2Surface";

float linearMultiplier(float value) noexcept
{
    value = juce::jlimit(0.0f, 1.0f, value);
    return value < 0.5f ? 1.0f / (4.0f - 6.0f * value)
                        : 1.0f + (value - 0.5f) * 6.0f;
}

float sliderFromLinearMultiplier(float multiplier) noexcept
{
    multiplier = juce::jlimit(0.25f, 4.0f, multiplier);
    return multiplier < 1.0f ? (4.0f - 1.0f / multiplier) / 6.0f
                             : 0.5f + (multiplier - 1.0f) / 6.0f;
}

int harmonicStep(float value) noexcept
{
    return juce::jlimit(-8, 8, juce::roundToInt(juce::jlimit(0.0f, 1.0f, value) * 16.0f - 8.0f));
}

float sliderFromHarmonicStep(int step) noexcept
{
    return static_cast<float>(juce::jlimit(-8, 8, step) + 8) / 16.0f;
}

}

DispBackendBindings::DispBackendBindings(DisperserAudioProcessor& processorToUse) noexcept
    : processor(processorToUse)
{
}

juce::AudioProcessorValueTreeState& DispBackendBindings::parameters() const noexcept
{
    return processor.apvts;
}

SimpleUIV2::ParameterSnapshot DispBackendBindings::parameterSnapshot() const
{
    SimpleUIV2::ParameterSnapshot values;
    updateParameterSnapshot(values);
    return values;
}

void DispBackendBindings::updateParameterSnapshot(SimpleUIV2::ParameterSnapshot& values) const
{
    if (values.empty()) values.reserve(definition().parameters.size());
    for (const auto& parameter : definition().parameters)
    {
        if (parameter.domain != SimpleUIV2::StateDomain::musicalParameter)
            continue;
        if (const auto* raw = processor.apvts.getRawParameterValue(juce::String(parameter.parameterId)))
            values[parameter.parameterId] = static_cast<double>(raw->load(std::memory_order_relaxed));
    }
}

std::optional<juce::String> DispBackendBindings::formatControlValue(
    std::string_view controlId, double value) const
{
    if (controlId != "mod-control") return std::nullopt;
    const auto* harmonic = processor.apvts.getRawParameterValue(DisperserAudioProcessor::kParamModHarm);
    if (harmonic != nullptr && harmonic->load(std::memory_order_relaxed) > 0.5f)
    {
        const int step = harmonicStep(static_cast<float>(value));
        return step > 0 ? "H+" + juce::String(step) : "H" + juce::String(step);
    }
    return "x" + juce::String(linearMultiplier(static_cast<float>(value)), 2);
}

std::optional<double> DispBackendBindings::parseControlValue(
    std::string_view controlId, const juce::String& text) const
{
    if (controlId != "mod-control") return std::nullopt;
    const auto numeric = text.retainCharacters("0123456789-+.,").replaceCharacter(',', '.');
    const auto* harmonic = processor.apvts.getRawParameterValue(DisperserAudioProcessor::kParamModHarm);
    if (harmonic != nullptr && harmonic->load(std::memory_order_relaxed) > 0.5f)
        return static_cast<double>(sliderFromHarmonicStep(numeric.getIntValue()));
    return static_cast<double>(sliderFromLinearMultiplier(numeric.getFloatValue()));
}

void DispBackendBindings::prepareForUiRefresh()
{
	phaseContourTelemetry = processor.getPhaseContourTelemetry();
}

std::optional<float> DispBackendBindings::signatureSemanticValue(std::string_view role) const
{
	if (role == "phaseCentre") return phaseContourTelemetry.centre;
	if (role == "stageDepth") return phaseContourTelemetry.stages;
	if (role == "seriesDepth") return phaseContourTelemetry.series;
	if (role == "phaseShape") return phaseContourTelemetry.shape;
	if (role == "feedbackMagnitude") return phaseContourTelemetry.feedbackMagnitude;
	if (role == "feedbackPolarity") return phaseContourTelemetry.feedbackPolarity;
	if (role == "alternatePolarity") return phaseContourTelemetry.alternatePolarity;
	if (role == "stereoTopology") return phaseContourTelemetry.topology;
	if (role == "activity") return phaseContourTelemetry.activity;
	return std::nullopt;
}

float DispBackendBindings::inputMeterPeak() const noexcept { return processor.getInputMeterPeak(); }
float DispBackendBindings::outputMeterPeak() const noexcept { return processor.getOutputMeterPeak(); }

SimpleUIV2::MusicalState DispBackendBindings::readMusicalState() const
{
    SimpleUIV2::MusicalState state;
    state.values.emplace(midiPortKey, static_cast<double>(processor.getMidiChannel()));
    state.values.emplace(midiDelayKey, static_cast<double>(processor.getMidiDelayMs()));
    Modulation::Integration::writePresetState(state, processor.modulationState());
    return state;
}

SimpleUIV2::MusicalState DispBackendBindings::defaultMusicalState() const
{
    SimpleUIV2::MusicalState state;
    state.values.emplace(midiPortKey, 0.0);
    state.values.emplace(midiDelayKey, 0.0);
    Modulation::Integration::writePresetState(state, Modulation::makeDefaultState());
    return state;
}

bool DispBackendBindings::validateMusicalState(const SimpleUIV2::MusicalState& state) const noexcept
{
    const auto marker = state.values.find(Modulation::Integration::presetStateId);
    const bool legacyMarker = marker != state.values.end() && marker->second == 0.0;
    if (state.values.size() != static_cast<std::size_t>(legacyMarker ? 3 : 2)
        || state.textValues.size() > 1
        || (!state.textValues.empty()
            && state.textValues.find(Modulation::Integration::presetStateId)
                 == state.textValues.end()))
        return false;
    const auto channel = state.values.find(midiPortKey);
    const auto delay = state.values.find(midiDelayKey);
    if (channel == state.values.end() || delay == state.values.end()) return false;
    Modulation::State modulation;
    return std::isfinite(channel->second) && std::isfinite(delay->second)
           && channel->second >= 0.0 && channel->second <= 16.0
           && delay->second >= 0.0 && delay->second <= 100.0
           && std::floor(channel->second) == channel->second
           && std::floor(delay->second) == delay->second
           && Modulation::Integration::readPresetState(state, modulation);
}

void DispBackendBindings::writeMusicalState(const SimpleUIV2::MusicalState& state)
{
    if (!validateMusicalState(state)) return;
    if (const auto item = state.values.find(midiPortKey); item != state.values.end())
        processor.setMidiChannel(juce::jlimit(0, 16, static_cast<int>(std::lround(item->second))));
    if (const auto item = state.values.find(midiDelayKey); item != state.values.end())
        processor.setMidiDelayMs(juce::jlimit(0, 100, static_cast<int>(std::lround(item->second))));
    Modulation::State modulation;
    if (Modulation::Integration::readPresetState(state, modulation))
        processor.setModulationState(modulation);
}

SimpleUIV2::UiInstanceState DispBackendBindings::readUiInstanceState() const
{
    SimpleUIV2::UiInstanceState state;
    state.selectedTask = static_cast<SimpleUIV2::TaskId>(juce::jlimit(
        0, 3, static_cast<int>(processor.apvts.state.getProperty(selectedTaskKey, 0))));
    state.surface = static_cast<SimpleUIV2::UiSurface>(juce::jlimit(
        0, 2, static_cast<int>(processor.apvts.state.getProperty(surfaceKey, 0))));
    return state;
}

void DispBackendBindings::writeUiInstanceState(const SimpleUIV2::UiInstanceState& state)
{
    processor.apvts.state.setProperty(selectedTaskKey, static_cast<int>(state.selectedTask), nullptr);
    processor.apvts.state.setProperty(surfaceKey, static_cast<int>(state.surface), nullptr);
}

void DispBackendBindings::setMacroName(int index, const juce::String& name)
{
    if (index < 0 || index >= Modulation::macroCount) return;
    auto mod = processor.modulationState();
    mod.macros[static_cast<std::size_t>(index)].name = name;
    processor.setModulationState(mod);
}

Modulation::State DispBackendBindings::modulationState() const { return processor.modulationState(); }
std::uint64_t DispBackendBindings::modulationStateGeneration() const noexcept { return processor.modulationStateGeneration(); }
std::array<float, Modulation::macroCount> DispBackendBindings::modulationMacroValues() const noexcept { return processor.modulationMacroValues(); }
void DispBackendBindings::setModulationMacroValue(int macro, float value) { processor.setModulationMacroValue(macro, value); }
bool DispBackendBindings::setModulationState(const Modulation::State& state) { return processor.setModulationState(state); }
Modulation::UI::SourceCapabilities DispBackendBindings::modulationSourceCapabilities() const noexcept { return { true }; }
std::vector<Modulation::UI::MotionRecipeOption> DispBackendBindings::modulationRecipeOptions() const
{
    return { { "native-jitter", "NATIVE JITTER" } };
}
bool DispBackendBindings::installModulationRecipe(const juce::String& id, int macro)
{
    if (id != "native-jitter") return false;
    auto* parameter = processor.apvts.getParameter(DisperserAudioProcessor::kParamJitter);
    if (parameter == nullptr) return false;
    const auto nativeAmount = parameter->getValue();
    const auto candidate = DispModulation::makeJitterParityRecipe(
        processor.modulationState(), macro);
    if (!processor.setModulationState(candidate)) return false;
    processor.setModulationMacroValue(macro - 1, nativeAmount);
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(parameter->convertTo0to1(0.0f));
    parameter->endChangeGesture();
    return true;
}
Modulation::Runtime::TelemetrySnapshot DispBackendBindings::modulationTelemetry() const noexcept { return processor.modulationTelemetry(); }
Modulation::UI::SidechainWorkspaceCallbacks DispBackendBindings::sidechainWorkspaceCallbacks()
{
    return Modulation::UI::singleSidechainCallbacks(parameters(), DisperserAudioProcessor::kParamSidechain,
        "sidechain-options", "Enable external sidechain modulation; open OPTIONS for DISP-TR detector settings");
}
}
