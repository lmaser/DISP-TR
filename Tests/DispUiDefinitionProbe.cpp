#include "../Source/UIV2/DispUiDefinition.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace V2 = TR::SimpleUIV2;

namespace
{
void require(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message);
}

const V2::SimplePageSpec& page(const V2::SimplePluginDefinition& definition, V2::TaskId task)
{
    for (const auto& candidate : definition.pages)
        if (candidate.taskId == task) return candidate;
    throw std::runtime_error("Missing task page");
}

const V2::SimpleGroupSpec& group(const V2::SimplePageSpec& source, const std::string& id)
{
    for (const auto& candidate : source.groups)
        if (candidate.groupId == id) return candidate;
    throw std::runtime_error("Missing group: " + id);
}

void requireIds(const std::vector<V2::SimpleControlSpec>& controls,
                std::initializer_list<const char*> expected,
                const std::string& message)
{
    require(controls.size() == expected.size(), message);
    std::size_t index = 0;
    for (const auto* id : expected) require(controls[index++].controlId == id, message);
}
}

int main()
{
    try
    {
        const auto& definition = TR::DispUIV2::definition();
        const auto issues = V2::validateDefinition(definition);
        if (V2::hasValidationErrors(issues))
        {
            for (const auto& issue : issues)
                std::cerr << issue.code << " at " << issue.path << " - " << issue.message << '\n';
            throw std::runtime_error("DISP definition validation failed");
        }

        std::set<std::string> apvts, musicalState, preset, presetState, retired;
        for (const auto& item : definition.parameters)
        {
            if (item.domain == V2::StateDomain::musicalParameter) apvts.insert(item.parameterId);
            if (item.domain == V2::StateDomain::musicalState) musicalState.insert(item.parameterId);
        }
        preset.insert(definition.preset.parameterWhitelist.begin(), definition.preset.parameterWhitelist.end());
        presetState.insert(definition.preset.musicalStateWhitelist.begin(), definition.preset.musicalStateWhitelist.end());
        retired.insert(TR::DispUIV2::retiredUiParameterIds().begin(), TR::DispUIV2::retiredUiParameterIds().end());

        require(apvts.size() == 59, "Expected 59 musical APVTS parameters including legacy Reverse state");
        require(musicalState == std::set<std::string> {
                    "midiDelayMs", "midiPort", "modulation_v1" },
                "Expected MIDI and modulation musical-state values");
        require(preset == apvts, "Preset APVTS whitelist differs from musical definition");
        require(presetState == musicalState, "Preset musical-state whitelist differs from definition");
        require(retired.size() == 9, "Expected nine retired UI parameter IDs");
        for (const auto& id : retired)
            require(apvts.count(id) == 0 && preset.count(id) == 0, "Retired UI state leaked into presets");

        requireIds(definition.macros,
                   { "macro-frequency", "macro-stages", "macro-feedback", "macro-mix" },
                   "Macro order must remain FREQ, STAGES, FEEDBACK, MIX");
        require(definition.signatureModel == V2::SignatureModel::phaseContour,
                "DISP must use the phase-contour signature model");
        require(definition.signature.size() == 9, "DISP phase contour requires nine semantic roles");

        std::array<float, 9> geometryValues {};
        const auto zeroGeometry = V2::computePhaseContourGeometry(geometryValues);
        require(zeroGeometry.contourCount == 0 && zeroGeometry.amplitude == 0.0f,
                "Zero stages must flatten the master geometry");
        geometryValues[1] = 1.0f / 128.0f;
        const auto oneStageGeometry = V2::computePhaseContourGeometry(geometryValues);
        require(oneStageGeometry.contourCount == 1 && oneStageGeometry.amplitude * 72.0f >= 3.9f,
                "One stage must remain visibly deep");
        geometryValues[1] = 0.5f;
        std::array<float, 4> seriesAmplitude {};
        for (int seriesIndex = 0; seriesIndex < 4; ++seriesIndex)
        {
            geometryValues[2] = static_cast<float>(seriesIndex) / 3.0f;
            const auto geometry = V2::computePhaseContourGeometry(geometryValues);
            require(geometry.contourCount == seriesIndex + 1, "SERIES contour count must be exact");
            seriesAmplitude[static_cast<std::size_t>(seriesIndex)] = geometry.amplitude;
        }
        require(std::all_of(seriesAmplitude.begin() + 1, seriesAmplitude.end(),
                            [&](float amplitude) { return std::abs(amplitude - seriesAmplitude[0]) < 0.0001f; }),
                "SERIES must add fibres without changing master depth");
        geometryValues[0] = 0.5f;
        geometryValues[2] = 0.0f;
        geometryValues[3] = 0.0f;
        const auto shelfGeometry = V2::computePhaseContourGeometry(geometryValues);
        require(shelfGeometry.profile.front().morph > 0.75f
                && shelfGeometry.profile.back().morph < -0.75f,
                "Low SHAPE must form a broad bipolar transition");
        geometryValues[3] = 1.0f;
        const auto deltaGeometry = V2::computePhaseContourGeometry(geometryValues);
        const auto peak = std::max_element(deltaGeometry.profile.begin(), deltaGeometry.profile.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.morph < rhs.morph; });
        require(peak->morph > 0.95f
                && deltaGeometry.profile.front().morph < 0.05f
                && deltaGeometry.profile.back().morph < 0.05f,
                "High SHAPE must form one concentrated delta");

        require(definition.pages.size() == 2
                    && definition.pages[0].label == "MAIN"
                    && definition.pages[1].label == "I/O",
                "DISP must expose exactly MAIN and I/O");
        const auto& main = page(definition, V2::TaskId::core);
        const auto& mainControls = group(main, "main-controls").controls;
        requireIds(mainControls,
                   { "series-control", "shape-control", "mod-control",
                   "alternate-control", "style-control",
                     "chaos-filter-control",
                     "chaos-delay-control", "midi-control" },
                   "MAIN order changed");
        requireIds(definition.auxiliaryControls, { "sidechain-control" },
                   "SIDECHAIN must be owned by the shared MACROS workspace");
        require(definition.auxiliaryControls.front().capability == V2::CapabilityTag::sidechain,
                "SIDECHAIN auxiliary control lost its capability tag");
        require(definition.capabilities.hasSidechain, "DISP sidechain capability unexpectedly disabled");
        require(mainControls.front().choiceLabels == std::vector<std::string> { "1X", "2X", "3X", "4X" },
                "SERIES must expose the processor's four cascade counts");
        require(mainControls.front().choiceValues == std::vector<double> { 1.0, 2.0, 3.0, 4.0 },
                "SERIES UI values must match the processor's 1..4 raw domain");
        require(mainControls[3].label == "ALTERNATE",
                "Alternate polarity must use the unambiguous ALTERNATE label");
        require(main.fixedActions.empty(), "DISP MAIN must not retain a parameter footer");
        const auto& io = page(definition, V2::TaskId::io);
        requireIds(io.fixedActions,
                   { "filter-options-action", "routing-options-action" },
                   "I/O fixed utilities must remain FILTER OPTIONS then ROUTING");
        requireIds(group(io, "io-levels").controls,
                   { "input-control", "output-control" },
                   "INPUT and OUTPUT must remain one consecutive LEVELS pair");
        requireIds(group(io, "io-image").controls, { "pan-control" }, "I/O image group changed");
        requireIds(group(io, "io-mix").controls, { "mix-mode-control", "dry-level-control" },
                   "I/O mix group changed");
        requireIds(group(io, "io-limiter").controls,
                   { "lim-mode-control", "lim-quality-control", "lim-threshold-control" },
                   "I/O limiter group changed");

        std::cout << "DISP UI V2 definition passed: 59 APVTS + 3 musical state, 9 retired UI IDs excluded.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "DISP UI V2 definition failed: " << exception.what() << '\n';
        return 1;
    }
}
