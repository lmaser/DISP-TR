#include "DispV2EditorFactory.h"
#include "DispBackendBindings.h"
#include "DispUiDefinition.h"
#include "../Modulation/DispModulationConfig.h"
#include "../../../TR-Shared/Modulation/UI/TRSimpleModulationWorkspace.h"
#include "../../../TR-Shared/SimpleUIV2/Runtime/SimpleEditorHost.h"

namespace TR::DispUIV2
{
juce::AudioProcessorEditor* createEditor(DisperserAudioProcessor& processor)
{
    std::vector<Modulation::UI::DestinationOption> destinations;
    int telemetryIndex = 0;
    for (const auto& descriptor : DispModulation::destinations())
        destinations.push_back({ descriptor.id, descriptor.group, descriptor.label,
                                 true, {}, telemetryIndex++ });
    auto backend = std::make_unique<DispBackendBindings>(processor);
    auto& modulationBackend = *backend;
    auto modulation = std::make_unique<Modulation::UI::SimpleModulationWorkspace>(
        Modulation::UI::workspaceCallbacks(modulationBackend), std::move(destinations),
        modulationBackend.sidechainWorkspaceCallbacks());
    return new SimpleUIV2::SimpleEditorHost(
        processor, definition(), std::move(backend),
        std::move(modulation));
}
}
