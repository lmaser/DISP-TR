#include "DispUiDefinition.h"

#include <utility>

namespace TR::DispUIV2
{
namespace V2 = SimpleUIV2;

namespace
{
std::string tooltipFor(const std::string& parameter, const std::string& label)
{
    const std::pair<const char*, const char*> descriptions[] {
        { "freq", "Dispersion centre frequency" },
        { "amount", "Number of dispersion stages" },
        { "feedback", "Recirculation amount and polarity" },
        { "mix", "Dry and processed signal balance" },
        { "series", "Number of complete dispersion banks in cascade" },
        { "shape", "Dispersion curve shape" },
        { "mod", "Dispersion-frequency multiplier" },
        { "mod_harm", "Quantize modulation to harmonic ratios" },
        { "jitter", "Series-aware stage-frequency and shape variation; feedback is unchanged" },
        { "style", "Stereo dispersion topology" },
        { "alt", "Alternate dispersion polarity between stages" },
        { "chaos", "Apply stochastic movement to the wet filter" },
        { "chaos_d", "Apply stochastic movement to dispersion timing" },
        { "midi", "Control dispersion frequency from MIDI notes" },
        { "sidechain", "Enable external sidechain control" },
        { "input", "Level entering the dispersion engine" },
        { "output", "Final plugin output level" },
        { "pan", "Wet-signal stereo position" },
        { "mix_mode", "Choose insert or send signal flow" },
        { "dry_level", "Dry level used in send mode" },
        { "wet_level", "Wet level used in send mode" },
        { "lim_mode", "Limiter position in the signal path" },
        { "lim_quality", "Limiter processing quality" },
        { "lim_threshold", "Limiter activation threshold" },
        { "filter_hp_freq", "Wet-path high-pass cutoff" },
        { "filter_lp_freq", "Wet-path low-pass cutoff" },
        { "tilt", "Wet-path spectral tilt" },
        { "filter_pos", "Filter and tilt position around dispersion" }
    };
    for (const auto& [id, text] : descriptions)
        if (parameter == id) return text;
    if (label == "FILTER OPTIONS") return "Open wet-path filter and tilt controls";
    if (label == "ROUTING") return "Open input, output and polarity routing";
    return label;
}

void addParameter(V2::SimplePluginDefinition& d, std::string id, V2::ParameterAccess access,
                  std::string target = {}, V2::StateDomain domain = V2::StateDomain::musicalParameter,
                  std::string backendJustification = {})
{
    const auto stableId = id;
    d.parameters.push_back({ std::move(id), domain, access, std::move(target),
                             std::move(backendJustification) });
    if (domain == V2::StateDomain::musicalParameter)
        d.preset.parameterWhitelist.push_back(stableId);
    else if (domain == V2::StateDomain::musicalState)
        d.preset.musicalStateWhitelist.push_back(stableId);
}

V2::SimpleControlSpec makeControl(std::string id, std::string parameter, std::string label,
                                  V2::ControlRole role = V2::ControlRole::knob)
{
    V2::SimpleControlSpec result;
    result.controlId = std::move(id);
    result.parameterId = std::move(parameter);
    result.label = std::move(label);
    result.role = role;
    result.tooltip = tooltipFor(result.parameterId, result.label);
    return result;
}

V2::SimpleGroupSpec hiddenGroup(std::string id, std::vector<V2::SimpleControlSpec> controls,
                                unsigned depth = 0)
{
    return { std::move(id), {}, std::move(controls), {}, depth, V2::GroupLabelVisibility::hidden };
}

V2::SimpleGroupSpec makeGroup(std::string id, std::string label, std::vector<V2::SimpleControlSpec> controls,
                              unsigned depth = 0)
{
    return { std::move(id), std::move(label), std::move(controls), {}, depth,
             V2::GroupLabelVisibility::automatic };
}

V2::SimpleControlSpec formatted(V2::SimpleControlSpec result, int decimals, double scale, std::string suffix,
                                double offset = 0.0)
{
    result.valueFormat = { true, decimals, scale, std::move(suffix), offset };
    return result;
}

V2::SimpleControlSpec frequency(V2::SimpleControlSpec result)
{
    result.valueFormat = { true, 1, 1.0, "", 0.0, V2::ValueStyle::frequency };
    return result;
}

V2::SimplePluginDefinition buildDefinition()
{
    V2::SimplePluginDefinition d;
    d.product = { "com.tr.audio.disp", "DISP-TR", "1.4.0", "https://github.com/lmaser/DISP-TR/issues" };
    d.capabilities = { true, true, true, true };

    for (const auto* id : { "freq", "amount", "feedback", "mix" })
        addParameter(d, id, V2::ParameterAccess::direct);
    for (int macro = 1; macro <= 8; ++macro)
    {
        const auto id = "mod_macro_" + std::to_string(macro);
        addParameter(d, id, V2::ParameterAccess::backendOnly, {},
                     V2::StateDomain::musicalParameter,
                     "Automatable Macro value exposed by the shared MACROS workspace.");
        d.preset.missingParameterDefaults.push_back({ id, 0.0 });
    }
    addParameter(d, "modulation_v1", V2::ParameterAccess::backendOnly, {},
                 V2::StateDomain::musicalState,
                 "Macro names, routes, source settings and transfer curves.");
    d.preset.missingMusicalStateDefaults.push_back({ "modulation_v1", 0.0 });
    auto mixMacro = formatted(makeControl("macro-mix", "mix", "MIX", V2::ControlRole::macro),
                              1, 100.0, "%");
    mixMacro.parameterAlternatives = { "wet_level" };
    d.macros = {
        frequency(makeControl("macro-frequency", "freq", "FREQ", V2::ControlRole::macro)),
        formatted(makeControl("macro-stages", "amount", "STAGES", V2::ControlRole::macro), 0, 1.0, ""),
        formatted(makeControl("macro-feedback", "feedback", "FEEDBACK", V2::ControlRole::macro), 1, 100.0, "%"),
        std::move(mixMacro)
    };

    for (const auto* id : { "series", "shape", "mod", "style" })
        addParameter(d, id, V2::ParameterAccess::direct);
    addParameter(d, "jitter", V2::ParameterAccess::backendOnly, {},
                 V2::StateDomain::musicalParameter,
                 "Legacy Jitter parameter retained for presets and host automation; new editing uses a MACROS motion recipe.");
    addParameter(d, "reverse", V2::ParameterAccess::backendOnly, {},
                 V2::StateDomain::musicalParameter,
                 "Legacy Reverse parameter retained for state compatibility; not exposed or activated in this release.");
    addParameter(d, "mod_harm", V2::ParameterAccess::prompt, "mod-options");

    auto series = makeControl("series-control", "series", "SERIES", V2::ControlRole::choice);
    series.choiceLabels = { "1X", "2X", "3X", "4X" };
    // DISP's host parameter is an integer in the 1..4 domain. The shared
    // choice control defaults to zero-based display values unless explicit
    // raw values are supplied; without this map raw Series=1 was painted as
    // the second label (2X) even though the DSP default was correctly 1X.
    series.choiceValues = { 1.0, 2.0, 3.0, 4.0 };
    series.choicePresentation = V2::ChoicePresentation::rail;
    auto modulation = makeControl("mod-control", "mod", "MOD");
    modulation.promptId = "mod-options";
    auto style = makeControl("style-control", "style", "STYLE", V2::ControlRole::choice);
    style.choiceLabels = { "MONO", "STEREO", "WIDE", "DUAL" };
    style.choicePresentation = V2::ChoicePresentation::rail;
    auto shapeControl = formatted(makeControl("shape-control", "shape", "SHAPE"), 1, 100.0, "%");
    addParameter(d, "alt", V2::ParameterAccess::direct);
    addParameter(d, "chaos", V2::ParameterAccess::direct);
    addParameter(d, "chaos_d", V2::ParameterAccess::direct);
    for (const auto* id : { "chaos_amt_filter", "chaos_spd_filter" })
        addParameter(d, id, V2::ParameterAccess::inspector, "chaos-filter-inspector");
    for (const auto* id : { "chaos_amt", "chaos_spd" })
        addParameter(d, id, V2::ParameterAccess::inspector, "chaos-delay-inspector");

    auto chaosFilter = makeControl("chaos-filter-control", "chaos", "CHAOS FILTER", V2::ControlRole::toggle);
    chaosFilter.inspectorId = "chaos-filter-inspector";
    auto chaosDelay = makeControl("chaos-delay-control", "chaos_d", "CHAOS DELAY", V2::ControlRole::toggle);
    chaosDelay.inspectorId = "chaos-delay-inspector";
    auto alternate = makeControl("alternate-control", "alt", "ALTERNATE", V2::ControlRole::toggle);

    addParameter(d, "midi", V2::ParameterAccess::direct);
    addParameter(d, "midiPort", V2::ParameterAccess::prompt, "midi-options", V2::StateDomain::musicalState);
    addParameter(d, "midiDelayMs", V2::ParameterAccess::prompt, "midi-options", V2::StateDomain::musicalState);
    addParameter(d, "sidechain", V2::ParameterAccess::backendOnly, {},
                 V2::StateDomain::musicalParameter,
                 "Legacy sidechain enable retained only for preset compatibility; Sidechain is activated by MACROS sources.");
    for (const auto* id : { "sidechain_gain", "sidechain_smooth", "sidechain_pol", "sidechain_hp", "sidechain_lp",
                            "sidechain_hp_on", "sidechain_lp_on", "sidechain_hp_slope", "sidechain_lp_slope" })
        addParameter(d, id, V2::ParameterAccess::backendOnly, {},
                     V2::StateDomain::musicalParameter,
                     "Legacy sidechain sub-parameter retained for preset and automation compatibility; active control migrated to MACROS workspace.");

    auto midi = makeControl("midi-control", "midi", "MIDI", V2::ControlRole::toggle);
    midi.promptId = "midi-options";
    // The shared MACROS workspace owns Sidechain. Keep this capability marker
    // available to the host without placing a direct sidechain toggle in MAIN.
    auto sidechain = makeControl("sidechain-control", {}, "SIDECHAIN", V2::ControlRole::action);
    sidechain.capability = V2::CapabilityTag::sidechain;
    V2::SimplePageSpec main { V2::TaskId::core, "MAIN", {
        hiddenGroup("main-controls", { series, shapeControl, modulation, alternate,
                                        style, chaosFilter, chaosDelay, midi })
    } };

    for (const auto* id : { "input", "output", "pan", "mix_mode", "dry_level", "wet_level",
                            "lim_mode", "lim_quality", "lim_threshold" })
        addParameter(d, id, V2::ParameterAccess::direct);
    for (const auto* id : { "filter_hp_on", "filter_hp_freq", "filter_hp_slope", "filter_lp_on",
                            "filter_lp_freq", "filter_lp_slope", "tilt", "filter_pos" })
        addParameter(d, id, V2::ParameterAccess::prompt, "filter-options");
    for (const auto* id : { "mode_in", "mode_out", "sum_bus", "inv_pol", "inv_str" })
        addParameter(d, id, V2::ParameterAccess::prompt, "routing-options");

    auto filterOptions = makeControl("filter-options-action", {}, "FILTER OPTIONS", V2::ControlRole::action);
    filterOptions.domain = V2::StateDomain::uiInstance;
    filterOptions.promptId = "filter-options";
    auto routingOptions = V2::makeCanonicalRoutingAction();
    auto input = formatted(makeControl("input-control", "input", "INPUT", V2::ControlRole::fader), 1, 1.0, " dB");
    input.meterSource = V2::MeterSource::input;
    auto output = formatted(makeControl("output-control", "output", "OUTPUT", V2::ControlRole::fader), 1, 1.0, " dB");
    output.meterSource = V2::MeterSource::output;
    V2::SimplePageSpec io { V2::TaskId::io, "I/O", V2::makeCommonIoGroups(input, output) };
    io.fixedActions = { std::move(filterOptions), std::move(routingOptions) };

    d.pages = { std::move(main), std::move(io) };
    d.auxiliaryControls = { sidechain };

    auto filterControls = V2::makeCanonicalFilterStageControls("prompt-filter", {
        "filter_hp_on", "filter_hp_freq", "filter_hp_slope", "filter_lp_on",
        "filter_lp_freq", "filter_lp_slope", "tilt" });
    auto filterPosition = makeControl("prompt-filter-position", "filter_pos", "F / T POSITION", V2::ControlRole::choice);
    filterPosition.choiceLabels = { "POST/POST", "PRE/PRE", "PRE/POST", "POST/PRE" };
    filterControls.push_back(filterPosition);

    d.prompts = {
        { "mod-options", "Modulation", { "mod_harm" }, {
            makeControl("mod-harmonic-control", "mod_harm", "HARMONIC", V2::ControlRole::toggle) } },
        V2::makeCanonicalMidiSessionPrompt(),
        { "filter-options", "Filter / Wet Path", {
            "filter_hp_on", "filter_hp_freq", "filter_hp_slope", "filter_lp_on", "filter_lp_freq",
            "filter_lp_slope", "tilt", "filter_pos" }, std::move(filterControls) },
        V2::makeCanonicalRoutingPrompt()
    };

    d.inspectors = {
        { "chaos-filter-inspector", "Chaos filter", { hiddenGroup("chaos-filter-detail", {
            formatted(makeControl("chaos-filter-amount", "chaos_amt_filter", "AMOUNT"), 0, 1.0, "%"),
            frequency(makeControl("chaos-filter-speed", "chaos_spd_filter", "SPEED")) }, 1) } },
        { "chaos-delay-inspector", "Chaos delay", { hiddenGroup("chaos-delay-detail", {
            formatted(makeControl("chaos-delay-amount", "chaos_amt", "AMOUNT"), 0, 1.0, "%"),
            frequency(makeControl("chaos-delay-speed", "chaos_spd", "SPEED")) }, 1) } }
    };

    d.signatureModel = V2::SignatureModel::phaseContour;
    d.signature = {
        { "phaseCentre", "freq" },
        { "stageDepth", "amount" },
        { "seriesDepth", "series" },
        { "phaseShape", "shape" },
        { "feedbackMagnitude", "feedback" },
        { "feedbackPolarity", "feedback" },
        { "alternatePolarity", "alt" },
        { "stereoTopology", "style" },
        { "activity", "mix" }
    };
    d.hiddenCompatibilityInputs = { "sidechain" };
    return d;
}
}

const V2::SimplePluginDefinition& definition()
{
    static const auto value = buildDefinition();
    return value;
}

const std::vector<std::string>& retiredUiParameterIds()
{
    static const std::vector<std::string> ids {
        "ui_width", "ui_height", "ui_palette", "ui_fx_tail", "ui_io_fx",
        "ui_color0", "ui_color1", "ui_color2", "ui_color3"
    };
    return ids;
}
}
