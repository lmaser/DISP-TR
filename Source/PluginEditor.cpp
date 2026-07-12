// PluginEditor.cpp
#include "PluginEditor.h"
#include "InfoContent.h"
#include <functional>

using namespace TR;

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace UiStateKeys = TR::SimpleUiStateKeys;

// â”€â”€ Timer constants â”€â”€
static constexpr int kCrtTimerHz  = 10;
static constexpr int kIdleTimerHz = 4;

// â”€â”€ Parameter listener IDs (shared by ctor + dtor) â”€â”€
static constexpr std::array<const char*, 7> kUiMirrorParamIds {
    DisperserAudioProcessor::kParamUiPalette,
    DisperserAudioProcessor::kParamUiFxTail,
    DisperserAudioProcessor::kParamUiIoFx,
    DisperserAudioProcessor::kParamUiColor0,
    DisperserAudioProcessor::kParamUiColor1,
    DisperserAudioProcessor::kParamUiColor2,
    DisperserAudioProcessor::kParamUiColor3
};

static juce::String formatInlineFrequency (double hz)
{
    const double safeHz = juce::jmax (0.0, hz);
    if (safeHz < 0.05)
        return "0Hz";

    const double displayHz = std::round (safeHz * 100.0) / 100.0;
    if (displayHz >= 1000.0)
        return juce::String (displayHz / 1000.0, 2) + "kHz";
    return juce::String (displayHz, 2) + "Hz";
}

static bool isGainFaderFloor (float dB) noexcept
{
    return dB <= DisperserAudioProcessor::kGainFloorDb + 0.001f;
}

static juce::String formatGainFaderDb (float dB)
{
    if (isGainFaderFloor (dB))
        return "-INF dB";
    if (std::abs (dB) < 0.05f)
        return "0.0 dB";
    return juce::String (dB, 1) + " dB";
}

static juce::String formatGainFaderDbCompact (float dB)
{
    if (isGainFaderFloor (dB))
        return "-INFdB";
    if (std::abs (dB) < 0.05f)
        return "0.0dB";
    return juce::String (dB, 1) + "dB";
}

static constexpr double kModCenter  = 0.5;
static constexpr double kModScale   = 3.0;
static constexpr double kModMaxMult = 4.0;
static constexpr double kModMinMult = 0.25;
static constexpr float  kMultEpsilon = 0.005f;

static double modSliderToMultiplier (double v)
{
    if (v < kModCenter)
        return 1.0 / (kModMaxMult - kModScale * (v / kModCenter));
    return 1.0 + kModScale * ((v - kModCenter) / kModCenter);
}


static int modSliderToHarmonicStep (double v) noexcept
{
    const double pos = juce::jlimit (0.0, 1.0, v) * 16.0 - 8.0;
    return juce::jlimit (-8, 8, (int) std::floor (pos + 0.5));
}

static juce::String formatModHarmText (double v, bool withSuffix)
{
    const int step = modSliderToHarmonicStep (v);
    auto text = juce::String ("H") + (step > 0 ? "+" : "") + juce::String (step);
    if (withSuffix)
        text += " MOD";
    return text;
}

template <typename Processor>
static bool isModHarmEnabled (Processor& processor) noexcept
{
    if (auto* value = processor.apvts.getRawParameterValue (Processor::kParamModHarm))
        return value->load (std::memory_order_relaxed) > 0.5f;
    return false;
}

template <typename Processor>
static void setModHarmEnabled (Processor& processor, bool shouldBeEnabled)
{
    if (auto* param = processor.apvts.getParameter (Processor::kParamModHarm))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (shouldBeEnabled ? 1.0f : 0.0f));
        param->endChangeGesture();
    }
}

static juce::String formatModHarmTooltip (bool enabled)
{
    return enabled ? "HARM: ON" : "HARM: OFF";
}
static double multiplierToModSlider (double mult)
{
    mult = juce::jlimit (kModMinMult, kModMaxMult, mult);
    if (mult < 1.0)
        return (kModMaxMult - 1.0 / mult) * kModCenter / kModScale;
    return kModCenter + (mult - 1.0) * kModCenter / kModScale;
}

static juce::String formatMidiChannelTooltip (int ch, int delayMs = 0)
{
    return juce::String (ch <= 0 ? "OMNI" : "CHANNEL " + juce::String (ch))
         + " | DLY " + juce::String (juce::jlimit (0, 100, delayMs)) + "ms";
}

static juce::String formatChaosTooltip (float amountPercent, float speedHz)
{
    return "AMT " + juce::String (juce::roundToInt (juce::jlimit (0.0f, 100.0f, amountPercent))) + "%"
         + " | SPD " + juce::String (juce::jlimit (DisperserAudioProcessor::kChaosSpdMin,
                                                   DisperserAudioProcessor::kChaosSpdMax,
                                                   speedHz), 1)
         + " Hz";
}

static juce::String formatSidechainFilterTooltipText (bool on, float hz, int slope)
{
    auto freqText = [] (float hz)
    {
        const float clamped = juce::jlimit (DisperserAudioProcessor::kSidechainFilterFreqMin, DisperserAudioProcessor::kSidechainFilterFreqMax, hz);
        if (clamped >= 1000.0f) return juce::String (clamped / 1000.0f, 1) + "kHz";
        return juce::String (juce::roundToInt (clamped)) + "Hz";
    };
    const int dbOct = slope <= 0 ? 6 : (slope == 1 ? 12 : 24);
    return juce::String (on ? "ON " : "OFF ") + freqText (hz) + " " + juce::String (dbOct) + "dB";
}

static juce::String formatSidechainTooltip (float gainDb, float smooth, float pol,
                                             bool hpOn, float hp, int hpSlope,
                                             bool lpOn, float lp, int lpSlope)
{
    return "GAIN " + formatGainFaderDbCompact (gainDb)
         + " | SMOOTH " + juce::String (juce::roundToInt (juce::jlimit (DisperserAudioProcessor::kSidechainSmoothMin, DisperserAudioProcessor::kSidechainSmoothMax, smooth) * 100.0f)) + "%"
         + " | POL " + juce::String (juce::jlimit (DisperserAudioProcessor::kSidechainPolMin, DisperserAudioProcessor::kSidechainPolMax, pol), 2)
         + " | HP " + formatSidechainFilterTooltipText (hpOn, hp, hpSlope)
         + " | LP " + formatSidechainFilterTooltipText (lpOn, lp, lpSlope);
}
//========================== Editor ==========================

DisperserAudioProcessorEditor::DisperserAudioProcessorEditor (DisperserAudioProcessor& p)
: AudioProcessorEditor (&p), audioProcessor (p)
{
    const std::array<BarSlider*, 14> barSliders { &freqSlider, &modSlider, &feedbackSlider, &amountSlider, &seriesSlider, &shapeSlider, &jitterSlider, &styleSlider, &inputSlider, &outputSlider, &tiltSlider, &panSlider, &mixSlider, &limThresholdSlider };

    useCustomPalette = audioProcessor.getUiUseCustomPalette();
    crtEnabled = audioProcessor.getUiFxTailEnabled();
    ioFxEnabled = audioProcessor.getUiIoFxEnabled();
    ioSectionExpanded_ = audioProcessor.getUiIoExpanded();

    for (int i = 0; i < kPaletteColourCount; ++i)
        customPalette[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

    TR::SimpleEditorLifecycle::initCommon (*this, audioProcessor, lnf, tooltipWindow,
        promptOverlay, resizeConstrainer, resizerCorner, kMinW, kMinH, kMaxW, kMaxH);
    applyActivePalette();

    suppressSizePersistence = true;
    lastPersistedEditorW = getWidth();
    lastPersistedEditorH = getHeight();
    suppressSizePersistence = false;

    // Wire slider formats
    {
        auto wireNumeric = [this](juce::Slider& s) { openNumericEntryPopupForSlider(s); };

        auto configure = [&](BarSlider& s, SliderValueFormat fmt, int dec = 0, bool numeric = true) {
            s.setFormat(fmt, dec);
            if (numeric) s.onPopup = wireNumeric;
        };

        configure(amountSlider,       SliderValueFormat::percent,      2);
        configure(seriesSlider,       SliderValueFormat::plain,        0, false);
        configure(freqSlider,         SliderValueFormat::frequency,    2);
        configure(shapeSlider,        SliderValueFormat::percent,      2);
        configure(jitterSlider,       SliderValueFormat::percent,      2);
        configure(styleSlider,        SliderValueFormat::plain,        0, false);
        configure(feedbackSlider,     SliderValueFormat::percent,      2);
        configure(modSlider,          SliderValueFormat::plain,        2);
        configure(inputSlider,        SliderValueFormat::gainDb,       1);
        configure(outputSlider,       SliderValueFormat::gainDb,       1);
        configure(tiltSlider,         SliderValueFormat::plain,        1);
        configure(panSlider,          SliderValueFormat::pan,          0);
        configure(mixSlider,          SliderValueFormat::percent,      2);
        configure(limThresholdSlider, SliderValueFormat::plain,        1);
    }

    for (auto* slider : barSliders) {
        setupBar (*slider);
        addAndMakeVisible (*slider);
        slider->addListener (this);
    }

    amountSlider.setNumDecimalPlacesToDisplay (0);
    seriesSlider.setNumDecimalPlacesToDisplay (0);
    freqSlider.setNumDecimalPlacesToDisplay (3);
    shapeSlider.setNumDecimalPlacesToDisplay (1);
    jitterSlider.setNumDecimalPlacesToDisplay (1);
    feedbackSlider.setNumDecimalPlacesToDisplay (1);
    modSlider.setNumDecimalPlacesToDisplay (2);
    styleSlider.setNumDecimalPlacesToDisplay (0);
    inputSlider.setNumDecimalPlacesToDisplay (1);
    outputSlider.setNumDecimalPlacesToDisplay (1);
    inputSlider.setSkewFactor (DisperserAudioProcessor::kGainSkew);
    outputSlider.setSkewFactor (DisperserAudioProcessor::kGainSkew);
    tiltSlider.setNumDecimalPlacesToDisplay (1);
    panSlider.setNumDecimalPlacesToDisplay (1);
    mixSlider.setNumDecimalPlacesToDisplay (1);
    limThresholdSlider.setNumDecimalPlacesToDisplay (1);

    // IO sliders start hidden (collapsible section, collapsed by default)
    TR::setSimpleComponentVisible (inputSlider, false);
    TR::setSimpleComponentVisible (outputSlider, false);
    TR::setSimpleComponentVisible (tiltSlider, false);
    TR::setSimpleComponentVisible (panSlider, false);
    TR::setSimpleComponentVisible (mixSlider, false);
    TR::setSimpleComponentVisible (limThresholdSlider, false);

    // Filter bar â€” hidden along with IO sliders in collapsed state
    filterBar_.setOwner (this);
    filterBar_.setScheme (activeScheme);
    addAndMakeVisible (filterBar_);
    TR::setSimpleComponentVisible (filterBar_, false);
    filterBar_.updateFromProcessor();

    // Chaos buttons (visible only when IO expanded)
    chaosFilterButton.setButtonText ("");
    addAndMakeVisible (chaosFilterButton);
    TR::setSimpleComponentVisible (chaosFilterButton, false);
    {
        const float savedAmt = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamChaosAmtFilter)->load();
        const float savedSpd = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamChaosSpdFilter)->load();
        chaosFilterDisplay.setText ("", juce::dontSendNotification);
        chaosFilterDisplay.setInterceptsMouseClicks (true, false);
        chaosFilterDisplay.addMouseListener (this, false);
        chaosFilterDisplay.setTooltip (formatChaosTooltip (savedAmt, savedSpd));
        TR::configureSimpleTransparentLabel (chaosFilterDisplay, activeScheme);
        addAndMakeVisible (chaosFilterDisplay);
        TR::setSimpleComponentVisible (chaosFilterDisplay, false);
    }
    chaosDelayButton.setButtonText ("");
    addAndMakeVisible (chaosDelayButton);
    TR::setSimpleComponentVisible (chaosDelayButton, false);
    {
        const float savedAmt = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamChaosAmt)->load();
        const float savedSpd = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamChaosSpd)->load();
        chaosDelayDisplay.setText ("", juce::dontSendNotification);
        chaosDelayDisplay.setInterceptsMouseClicks (true, false);
        chaosDelayDisplay.addMouseListener (this, false);
        chaosDelayDisplay.setTooltip (formatChaosTooltip (savedAmt, savedSpd));
        TR::configureSimpleTransparentLabel (chaosDelayDisplay, activeScheme);
        addAndMakeVisible (chaosDelayDisplay);
        TR::setSimpleComponentVisible (chaosDelayDisplay, false);
    }

    // Mode In / Mode Out / Sum Bus combos
    {
        auto setupModeCombo = [this] (juce::ComboBox& combo)
        {
            addAndMakeVisible (combo);
            combo.addItem ("L+R",  1);
            combo.addItem ("M/S",  2);
            combo.addItem ("MID",  3);
            combo.addItem ("SIDE", 4);
            TR::centreSimpleCombo (combo);
            combo.setLookAndFeel (&lnf);
            TR::setSimpleComponentVisible (combo, false);
        };
        setupModeCombo (modeInCombo);
        setupModeCombo (modeOutCombo);

        addAndMakeVisible (sumBusCombo);
        sumBusCombo.addItem ("ST",              1);
        sumBusCombo.addItem (juce::String::fromUTF8 (u8"\u2192M"), 2);
        sumBusCombo.addItem (juce::String::fromUTF8 (u8"\u2192S"), 3);
        TR::centreSimpleCombo (sumBusCombo);
        sumBusCombo.setLookAndFeel (&lnf);
        TR::setSimpleComponentVisible (sumBusCombo, false);
    }

    // Limiter Mode combo
    {
        addAndMakeVisible (limModeCombo);
        limModeCombo.addItem ("NONE",   1);
        limModeCombo.addItem ("WET",    2);
        limModeCombo.addItem ("GLOBAL", 3);
        TR::centreSimpleCombo (limModeCombo);
        limModeCombo.setLookAndFeel (&lnf);
        TR::setSimpleComponentVisible (limModeCombo, false);
    }

    // Invert Polarity / Invert Stereo combos
    {
        auto setupInvCombo = [this] (juce::ComboBox& combo)
        {
            addAndMakeVisible (combo);
            combo.addItem ("NONE",   1);
            combo.addItem ("WET",    2);
            combo.addItem ("GLOBAL", 3);
            TR::centreSimpleCombo (combo);
            combo.setLookAndFeel (&lnf);
            TR::setSimpleComponentVisible (combo, false);
        };
        setupInvCombo (invPolCombo);
        setupInvCombo (invStrCombo);
    }

    // Mix Mode combo (INSERT / SEND)
    {
        addAndMakeVisible (mixModeCombo);
        mixModeCombo.addItem ("INSERT", 1);
        mixModeCombo.addItem ("SEND",   2);
        TR::centreSimpleCombo (mixModeCombo);
        mixModeCombo.setLookAndFeel (&lnf);
        TR::setSimpleComponentVisible (mixModeCombo, false);
    }

    // Filter Position combo (POST / PRE)
    {
        addAndMakeVisible (filterPosCombo);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25bc T\u25bc"), 1);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25b2 T\u25b2"), 2);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25b2 T\u25bc"), 3);
        filterPosCombo.addItem (juce::String::fromUTF8 (u8"F\u25bc T\u25b2"), 4);
        TR::centreSimpleCombo (filterPosCombo);
        filterPosCombo.setLookAndFeel (&lnf);
        TR::setSimpleComponentVisible (filterPosCombo, false);
    }

    // Dual Mix Bar (SEND mode)
    addAndMakeVisible (dualMixBar_);
    dualMixBar_.setOwner (this);
    TR::setSimpleComponentVisible (dualMixBar_, false);

    seriesSlider.setRange ((double) DisperserAudioProcessor::kSeriesMin,
                           (double) DisperserAudioProcessor::kSeriesMax,
                           1.0);
    styleSlider.setRange (0, 3, 1);

    altButton.setButtonText ("");
    midiButton.setButtonText ("");
    sidechainButton.setButtonText ("");

    addAndMakeVisible (altButton);
    addAndMakeVisible (midiButton);
    addAndMakeVisible (sidechainButton);
    sidechainButton.setInterceptsMouseClicks (false, false);
    sidechainButton.addMouseListener (this, false);

    // MIDI channel tooltip overlay â€” invisible label positioned over the MIDI legend.
    // Provides tooltip on hover; clicks forwarded to editor via addMouseListener.
    {
        const int savedChannel = audioProcessor.getMidiChannel();
        midiChannelDisplay.setText ("", juce::dontSendNotification);
        midiChannelDisplay.setInterceptsMouseClicks (true, false);
        midiChannelDisplay.addMouseListener (this, false);
        midiChannelDisplay.setTooltip (formatMidiChannelTooltip (savedChannel, audioProcessor.getMidiDelayMs()));
        TR::configureSimpleTransparentLabel (midiChannelDisplay, activeScheme);
        addAndMakeVisible (midiChannelDisplay);
    }

    {
        const float savedScGain = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamSidechainGain)->load();
        const float savedSmooth = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamSidechainSmooth)->load();
        const float savedScPol = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamSidechainPol)->load();
        const float savedScHp = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamSidechainHp)->load();
        const float savedScLp = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamSidechainLp)->load();
        const bool savedScHpOn = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamSidechainHpOn)->load() > 0.5f;
        const bool savedScLpOn = audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamSidechainLpOn)->load() > 0.5f;
        sidechainDisplay.setText ("", juce::dontSendNotification);
        sidechainDisplay.setInterceptsMouseClicks (true, false);
        sidechainDisplay.addMouseListener (this, false);
        const int savedScHpSlope = (int) std::lround (audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamSidechainHpSlope)->load());
        const int savedScLpSlope = (int) std::lround (audioProcessor.apvts.getRawParameterValue (DisperserAudioProcessor::kParamSidechainLpSlope)->load());
        sidechainDisplay.setTooltip (formatSidechainTooltip (savedScGain, savedSmooth, savedScPol, savedScHpOn, savedScHp, savedScHpSlope, savedScLpOn, savedScLp, savedScLpSlope));
        TR::configureSimpleTransparentLabel (sidechainDisplay, activeScheme);
        addAndMakeVisible (sidechainDisplay);
    }

    auto bindSlider = [&] (std::unique_ptr<SliderAttachment>& attachment,
                           const char* paramId,
                           BarSlider& slider,
                           double defaultValue)
    {
        attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, paramId, slider);
        slider.setDoubleClickReturnValue (true, defaultValue);
    };

    bindSlider (amountAttachment, DisperserAudioProcessor::kParamAmount, amountSlider, kDefaultAmount);
    bindSlider (seriesAttachment, DisperserAudioProcessor::kParamSeries, seriesSlider, kDefaultSeries);
    bindSlider (freqAttachment, DisperserAudioProcessor::kParamFreq, freqSlider, kDefaultFreq);
    bindSlider (shapeAttachment, DisperserAudioProcessor::kParamShape, shapeSlider, kDefaultShape);
    bindSlider (jitterAttachment, DisperserAudioProcessor::kParamJitter, jitterSlider, kDefaultJitter);
    bindSlider (styleAttachment, DisperserAudioProcessor::kParamStyle, styleSlider, kDefaultStyle);
    bindSlider (feedbackAttachment, DisperserAudioProcessor::kParamFeedback, feedbackSlider, kDefaultFeedback);
    bindSlider (modAttachment, DisperserAudioProcessor::kParamMod, modSlider, kDefaultMod);
    bindSlider (inputAttachment, DisperserAudioProcessor::kParamInput, inputSlider, kDefaultInput);
    bindSlider (outputAttachment, DisperserAudioProcessor::kParamOutput, outputSlider, kDefaultOutput);
    bindSlider (tiltAttachment, DisperserAudioProcessor::kParamTilt, tiltSlider, kDefaultTilt);
    bindSlider (panAttachment,  DisperserAudioProcessor::kParamPan,  panSlider,  0.5);
    bindSlider (mixAttachment, DisperserAudioProcessor::kParamMix, mixSlider, kDefaultMix);
    bindSlider (limThresholdAttachment, DisperserAudioProcessor::kParamLimThreshold, limThresholdSlider, kDefaultLimThreshold);

    auto bindButton = [&] (std::unique_ptr<ButtonAttachment>& attachment,
                           const char* paramId,
                           juce::Button& button)
    {
        attachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, paramId, button);
    };

    bindButton (altAttachment, DisperserAudioProcessor::kParamAlt, altButton);
    bindButton (midiAttachment, DisperserAudioProcessor::kParamMidi, midiButton);
    bindButton (sidechainAttachment, DisperserAudioProcessor::kParamSidechain, sidechainButton);
    bindButton (chaosFilterAttachment, DisperserAudioProcessor::kParamChaos, chaosFilterButton);
    bindButton (chaosDelayAttachment,  DisperserAudioProcessor::kParamChaosD, chaosDelayButton);

    modeInAttachment  = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, DisperserAudioProcessor::kParamModeIn,  modeInCombo);
    modeOutAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, DisperserAudioProcessor::kParamModeOut, modeOutCombo);
    sumBusAttachment  = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, DisperserAudioProcessor::kParamSumBus,  sumBusCombo);
    limModeAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, DisperserAudioProcessor::kParamLimMode, limModeCombo);
    invPolAttachment  = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, DisperserAudioProcessor::kParamInvPol,  invPolCombo);
    invStrAttachment  = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, DisperserAudioProcessor::kParamInvStr,  invStrCombo);
    mixModeAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, DisperserAudioProcessor::kParamMixMode, mixModeCombo);
    filterPosAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, DisperserAudioProcessor::kParamFilterPos, filterPosCombo);

    for (auto* paramId : kUiMirrorParamIds)
        audioProcessor.apvts.addParameterListener (paramId, this);

    TR::SimpleEditorLifecycle::scheduleUiRestore (*this);

    applyCrtState (crtEnabled);

    refreshLegendTextCache();
    resized();
}

DisperserAudioProcessorEditor::~DisperserAudioProcessorEditor()
{
    setComponentEffect (nullptr);
    stopTimer();

    for (auto* paramId : kUiMirrorParamIds)
        audioProcessor.apvts.removeParameterListener (paramId, this);

    audioProcessor.setUiUseCustomPalette (useCustomPalette);
    audioProcessor.setUiFxTailEnabled (crtEnabled);

    dismissEditorOwnedModalPrompts (lnf);
    setPromptOverlayActive (false);

    const std::array<BarSlider*, 14> barSliders { &freqSlider, &modSlider, &feedbackSlider, &amountSlider, &seriesSlider, &shapeSlider, &jitterSlider, &styleSlider, &inputSlider, &outputSlider, &tiltSlider, &panSlider, &mixSlider, &limThresholdSlider };
    for (auto* slider : barSliders)
        slider->removeListener (this);

    if (tooltipWindow != nullptr)
        tooltipWindow->setLookAndFeel (nullptr);

    modeInCombo.setLookAndFeel (nullptr);
    modeOutCombo.setLookAndFeel (nullptr);
    sumBusCombo.setLookAndFeel (nullptr);
    limModeCombo.setLookAndFeel (nullptr);
    invPolCombo.setLookAndFeel (nullptr);
    invStrCombo.setLookAndFeel (nullptr);
    mixModeCombo.setLookAndFeel (nullptr);
    filterPosCombo.setLookAndFeel (nullptr);

    setLookAndFeel (nullptr);
}

void DisperserAudioProcessorEditor::applyActivePalette()
{
    const auto& palette = useCustomPalette ? customPalette : defaultPalette;
    activeScheme = TR::applySimplePalette(palette, lnf,
        { &chaosFilterDisplay, &chaosDelayDisplay, &midiChannelDisplay, &sidechainDisplay },
        { &amountSlider, &seriesSlider, &freqSlider, &shapeSlider, &jitterSlider, &styleSlider, &feedbackSlider, &modSlider, &inputSlider, &outputSlider, &tiltSlider, &panSlider, &mixSlider, &limThresholdSlider },
        { &modeInCombo, &modeOutCombo, &sumBusCombo, &limModeCombo, &invPolCombo, &invStrCombo, &mixModeCombo, &filterPosCombo });
    filterBar_.setScheme(activeScheme);
    dualMixBar_.setScheme(activeScheme);
    updateIoFxMeterSliders();
}

void DisperserAudioProcessorEditor::applyCrtState (bool enabled)
{
    crtEnabled = enabled;
    TR::SimpleUIController::applyCrt (crtEnabled, *this, *this, crtEffect, crtTime, kCrtTimerHz, kIdleTimerHz);
}

void DisperserAudioProcessorEditor::applyIoFxState (bool enabled)
{
    ioFxEnabled = enabled;
    updateIoFxMeterSliders();
}

void DisperserAudioProcessorEditor::updateIoFxMeterSliders()
{
    TR::SimpleUIController::updateIoMeters (defaultPalette, customPalette, useCustomPalette,
        inputSlider, outputSlider, ioFxEnabled,
        lastInputSignalMs, lastOutputSignalMs,
        audioProcessor.getInputMeterPeak(), audioProcessor.getOutputMeterPeak());
}

void DisperserAudioProcessorEditor::applyLabelTextColour (juce::Label& label, juce::Colour colour)
{
    TR::applySimpleLabelTextColour (label, colour);
}

void DisperserAudioProcessorEditor::sliderValueChanged (juce::Slider* slider)
{
    auto isBarSlider = [&] (const juce::Slider* s)
    {
        return s == &amountSlider || s == &seriesSlider || s == &freqSlider || s == &shapeSlider || s == &jitterSlider || s == &styleSlider
            || s == &feedbackSlider || s == &modSlider || s == &inputSlider || s == &outputSlider || s == &tiltSlider || s == &panSlider
            || s == &mixSlider || s == &limThresholdSlider;
    };

    refreshLegendTextCache();

    if (slider == nullptr)
    {
        repaint();
        return;
    }

    if (isBarSlider (slider))
    {
        repaint (getRowRepaintBounds (*slider));
        return;
    }

    repaint();
}

void DisperserAudioProcessorEditor::setPromptOverlayActive (bool shouldBeActive)
{
    TR::SimpleUIController::setOverlayActive (*this, promptOverlay, promptOverlayActive, shouldBeActive, lnf);
}

void DisperserAudioProcessorEditor::moved()
{
    TR::SimpleUIController::anchorPromptsOnMove (*this, promptOverlayActive, promptOverlay, lnf);
}

void DisperserAudioProcessorEditor::parentHierarchyChanged()
{
    TR::SimpleUIController::darkenWindowBackground_Hwnd (*this);
}

void DisperserAudioProcessorEditor::parameterChanged (const juce::String& parameterID, float)
{
    const bool isSizeParam = parameterID == DisperserAudioProcessor::kParamUiWidth
                         || parameterID == DisperserAudioProcessor::kParamUiHeight;

    const bool isUiVisualParam = parameterID == DisperserAudioProcessor::kParamUiPalette
                             || parameterID == DisperserAudioProcessor::kParamUiFxTail
                             || parameterID == DisperserAudioProcessor::kParamUiIoFx
                             || parameterID == DisperserAudioProcessor::kParamUiColor0
                             || parameterID == DisperserAudioProcessor::kParamUiColor1
                             || parameterID == DisperserAudioProcessor::kParamUiColor2
                             || parameterID == DisperserAudioProcessor::kParamUiColor3;

    if (! isSizeParam && ! isUiVisualParam)
        return;

    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);
    juce::MessageManager::callAsync ([safeThis, isSizeParam]()
    {
        if (safeThis == nullptr)
            return;

        if (isSizeParam)
            safeThis->applyPersistedUiStateFromProcessor (true, false);
        else
            safeThis->applyPersistedUiStateFromProcessor (false, true);
    });
}

void DisperserAudioProcessorEditor::timerCallback()
{
    if (suppressSizePersistence)
        return;

    // â”€â”€ MIDI note name polling â”€â”€
    const auto newMidiDisplay = audioProcessor.getCurrentFreqDisplay();
    if (newMidiDisplay != cachedMidiDisplay)
    {
        cachedMidiDisplay = newMidiDisplay;
        if (refreshLegendTextCache())
            updateCachedLayout();
        repaint (getRowRepaintBounds (freqSlider));
    }

    const int w = getWidth();
    const int h = getHeight();

    const uint32_t last = lastUserInteractionMs.load (std::memory_order_relaxed);
    const uint32_t now = juce::Time::getMillisecondCounter();
    const bool userRecent = (now - last) <= (uint32_t) kUserInteractionPersistWindowMs;

    if ((w != lastPersistedEditorW || h != lastPersistedEditorH) && userRecent)
    {
        audioProcessor.setUiEditorSize (w, h);
        lastPersistedEditorW = w;
        lastPersistedEditorH = h;
    }

    // â”€â”€ CRT animation â”€â”€
    if (crtEnabled && w > 0 && h > 0)
    {
        crtTime += 0.1f;
        crtEffect.setTime (crtTime);

        const bool anySliderDragging = amountSlider.isMouseButtonDown()
                                    || seriesSlider.isMouseButtonDown()
                                    || freqSlider.isMouseButtonDown()
                                    || shapeSlider.isMouseButtonDown()
                                    || jitterSlider.isMouseButtonDown()
                                    || styleSlider.isMouseButtonDown()
                                    || feedbackSlider.isMouseButtonDown()
                                    || modSlider.isMouseButtonDown()
                                    || inputSlider.isMouseButtonDown()
                                    || outputSlider.isMouseButtonDown()
                                    || mixSlider.isMouseButtonDown();
        if (! anySliderDragging)
            repaint();
    }

    // Keep filter bar markers up to date
    if (filterBar_.isVisible())
        filterBar_.updateFromProcessor();

    // Keep dual mix bar markers up to date + visibility swap
    if (ioSectionExpanded_)
    {
        const float prevDry = dualMixBar_.getDryLevel();
        const float prevWet = dualMixBar_.getWetLevel();
        dualMixBar_.updateFromProcessor();
        const bool isSendMode = mixModeCombo.getSelectedId() == 2;

        // Refresh legend when levels change in SEND mode
        if (isSendMode && (dualMixBar_.getDryLevel() != prevDry || dualMixBar_.getWetLevel() != prevWet))
        {
            if (refreshLegendTextCache())
                updateCachedLayout();
            repaint();
        }

        if (mixSlider.isVisible() == isSendMode)
        {
            TR::setSimpleComponentVisible (mixSlider, ! isSendMode);
            TR::setSimpleComponentVisible (dualMixBar_, isSendMode);
            if (refreshLegendTextCache())
                updateCachedLayout();
            repaint();
        }
    }
    else
    {
        if (dualMixBar_.isVisible())
            dualMixBar_.updateFromProcessor();

        const bool isSend = (mixModeCombo.getSelectedItemIndex() == 1);
        if (isSend && mixSlider.isVisible())
        {
            TR::setSimpleComponentVisible (mixSlider, false);
            TR::setSimpleComponentVisible (dualMixBar_, true);
        }
        else if (! isSend && dualMixBar_.isVisible())
        {
            TR::setSimpleComponentVisible (dualMixBar_, false);
            TR::setSimpleComponentVisible (mixSlider, true);
        }
    }

    updateIoFxMeterSliders();
}

void DisperserAudioProcessorEditor::applyPersistedUiStateFromProcessor (bool applySize, bool applyPaletteAndFx)
{
    if (applySize)
    {
        const int targetW = juce::jlimit (kMinW, kMaxW, audioProcessor.getUiEditorWidth());
        const int targetH = juce::jlimit (kMinH, kMaxH, audioProcessor.getUiEditorHeight());

        if (getWidth() != targetW || getHeight() != targetH)
        {
            suppressSizePersistence = true;
            setSize (targetW, targetH);
            suppressSizePersistence = false;
        }
    }

    if (applyPaletteAndFx)
    {
        bool paletteChanged = false;
        for (int i = 0; i < kPaletteColourCount; ++i)
        {
            const auto c = audioProcessor.getUiCustomPaletteColour (i);
            if (customPalette[(size_t) i].getARGB() != c.getARGB())
            {
                customPalette[(size_t) i] = c;
                paletteChanged = true;
            }
        }

        const bool targetUseCustomPalette = audioProcessor.getUiUseCustomPalette();
        const bool targetCrtEnabled = audioProcessor.getUiFxTailEnabled();
        const bool targetIoFxEnabled = audioProcessor.getUiIoFxEnabled();

        const bool paletteSwitchChanged = (useCustomPalette != targetUseCustomPalette);
        const bool fxChanged = (crtEnabled != targetCrtEnabled);
        const bool ioFxChanged = (ioFxEnabled != targetIoFxEnabled);

        const bool targetIoExpanded = audioProcessor.getUiIoExpanded();
        const bool ioChanged = (ioSectionExpanded_ != targetIoExpanded);
        if (ioChanged)
        {
            ioSectionExpanded_ = targetIoExpanded;
            resized();
        }

        if (paletteSwitchChanged)
            useCustomPalette = targetUseCustomPalette;

        if (fxChanged)
            applyCrtState (targetCrtEnabled);
        if (ioFxChanged)
            applyIoFxState (targetIoFxEnabled);

        if (paletteChanged || paletteSwitchChanged)
            applyActivePalette();

        if (paletteChanged || paletteSwitchChanged || fxChanged || ioFxChanged || ioChanged)
            repaint();
    }
}

bool DisperserAudioProcessorEditor::refreshLegendTextCache()
{
    const int amountV = (int) std::llround (amountSlider.getValue());
    const int seriesV = (int) std::llround (seriesSlider.getValue());
    const double hz = freqSlider.getValue();
    const double shapeV = juce::jlimit (0.0, 1.0, shapeSlider.getValue());
    const int shapePct = (int) std::lround (shapeV * 100.0);
    const double jitterV = juce::jlimit (0.0, 1.0, jitterSlider.getValue());
    const int jitterPct = (int) std::lround (jitterV * 100.0);
    const double fbV = juce::jlimit (-1.0, 1.0, feedbackSlider.getValue());
    const int fbPct = (int) std::lround (fbV * 100.0);
    const float modMult = (float) modSliderToMultiplier (modSlider.getValue());
    const double mixV = juce::jlimit (0.0, 1.0, mixSlider.getValue());
    const int mixPct = (int) std::lround (mixV * 100.0);
    const float limDb = (float) limThresholdSlider.getValue();

    const auto oldAmountFullLen = cachedAmountTextFull.length();
    const auto oldAmountShortLen = cachedAmountTextShort.length();
    const auto oldSeriesFullLen = cachedSeriesTextFull.length();
    const auto oldSeriesShortLen = cachedSeriesTextShort.length();
    const auto oldFreqLen = cachedFreqTextHz.length();
    const auto oldFreqShortLen = cachedFreqTextShort.length();
    const auto oldShapeFullLen = cachedShapeTextFull.length();
    const auto oldShapeShortLen = cachedShapeTextShort.length();
    const auto oldJitterFullLen = cachedJitterTextFull.length();
    const auto oldJitterShortLen = cachedJitterTextShort.length();
    const auto oldStyleFullLen = cachedStyleTextFull.length();
    const auto oldStyleShortLen = cachedStyleTextShort.length();
    const auto oldFeedbackFullLen = cachedFeedbackTextFull.length();
    const auto oldFeedbackShortLen = cachedFeedbackTextShort.length();
    const auto oldModFullLen = cachedModTextFull.length();
    const auto oldModShortLen = cachedModTextShort.length();
    const auto oldMixFullLen = cachedMixTextFull.length();
    const auto oldMixShortLen = cachedMixTextShort.length();
    const auto oldInputFullLen = cachedInputTextFull.length();
    const auto oldInputShortLen = cachedInputTextShort.length();
    const auto oldOutputFullLen = cachedOutputTextFull.length();
    const auto oldOutputShortLen = cachedOutputTextShort.length();
    const auto oldPanFullLen = cachedPanTextFull.length();
    const auto oldPanShortLen = cachedPanTextShort.length();
    const auto oldLimFullLen = cachedLimThresholdTextFull.length();
    const auto oldLimShortLen = cachedLimThresholdTextShort.length();

    cachedAmountTextFull  = juce::String (amountV) + " STAGES";
    cachedAmountTextShort = juce::String (amountV) + " STG";
    cachedAmountIntOnly   = juce::String (amountV);

    cachedSeriesTextFull  = juce::String (seriesV) + " SERIES";
    cachedSeriesTextShort = juce::String (seriesV) + " SRS";
    cachedSeriesIntOnly   = juce::String (seriesV);

    cachedFreqTextHz = getFreqText();
    cachedFreqTextShort = getFreqTextShort();
    if (cachedMidiDisplay.isNotEmpty() && ! freqSlider.isMouseButtonDown())
    {
        cachedFreqIntOnly = cachedMidiDisplay;
    }
    else
    {
        cachedFreqIntOnly = formatInlineFrequency (hz);
    }

    cachedShapeTextFull = juce::String (shapePct) + "% SHAPE";
    cachedShapeTextShort = juce::String (shapePct) + "% SHP";
    cachedShapeIntOnly = juce::String (shapePct) + "%";

    cachedJitterTextFull = juce::String (jitterPct).toUpperCase() + "% JITTER";
    cachedJitterTextShort = juce::String (jitterPct).toUpperCase() + "% JIT";
    cachedJitterIntOnly = juce::String (jitterPct);

    cachedStyleTextFull  = getStyleText();
    cachedStyleTextShort = getStyleTextShort();

    cachedFeedbackTextFull = juce::String (fbPct) + "% FBK";
    cachedFeedbackTextShort = juce::String (fbPct) + "% FBK";
    cachedFeedbackIntOnly = juce::String (fbPct) + "%";

    cachedModTextFull  = getModText();
    cachedModTextShort = getModTextShort();
    cachedModIntOnly   = isModHarmEnabled (audioProcessor) ? formatModHarmText (modSlider.getValue(), false)
                                                           : ("X" + juce::String (juce::roundToInt (modMult)));

    cachedMixTextFull = getMixText();
    cachedMixTextShort = getMixTextShort();

    if (mixModeCombo.getSelectedId() == 2)
    {
        const bool isDry = (dualMixBar_.getLastTouched() != DualMixBarComponent::WET);
        const float level = isDry ? dualMixBar_.getDryLevel() : dualMixBar_.getWetLevel();
        const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
        const juce::String suffix = isDry ? " DRY" : " WET";
        if (dB <= -100.0f) cachedMixIntOnly = "-INF" + suffix;
        else if (std::abs (dB) < 0.05f) cachedMixIntOnly = "0.0dB" + suffix;
        else cachedMixIntOnly = juce::String ((int) dB) + "dB" + suffix;
    }
    else
    {
        cachedMixIntOnly = juce::String (mixPct) + "%";
    }

    cachedTiltTextFull = getTiltText();
    cachedTiltTextShort = getTiltTextShort();
    {
        const float tiltVal = (float) tiltSlider.getValue();
        cachedTiltIntOnly = (std::abs (tiltVal) < 0.05f) ? "0.0dB" : (juce::String (tiltVal, 1) + "dB");
    }

    cachedInputTextFull = getInputText();
    cachedInputTextShort = getInputTextShort();
    {
        const float inDb = (float) inputSlider.getValue();
        if (isGainFaderFloor (inDb))
            cachedInputIntOnly = "-INF";
        else
            cachedInputIntOnly = formatGainFaderDbCompact (inDb);
    }

    cachedOutputTextFull = getOutputText();
    cachedOutputTextShort = getOutputTextShort();
    {
        const float outDb = (float) outputSlider.getValue();
        if (isGainFaderFloor (outDb))
            cachedOutputIntOnly = "-INF";
        else
            cachedOutputIntOnly = formatGainFaderDbCompact (outDb);
    }

    cachedFilterTextFull  = getFilterText();
    cachedFilterTextShort = getFilterTextShort();

    cachedPanTextFull  = getPanText();
    cachedPanTextShort = getPanTextShort();

    {
        const auto limText = juce::String (std::abs (limDb) < 0.05f ? 0.0f : limDb, 1);
        if (limDb >= -0.05f)
        {
            cachedLimThresholdTextFull  = "0.0 dB LIM";
            cachedLimThresholdTextShort = "0.0 dB LIM";
            cachedLimThresholdIntOnly   = "0.0dB";
        }
        else
        {
            cachedLimThresholdTextFull  = limText + " dB LIM";
            cachedLimThresholdTextShort = limText + " dB LIM";
            cachedLimThresholdIntOnly   = limText + "dB";
        }
    }

    const bool lengthChanged = oldAmountFullLen  != cachedAmountTextFull.length()
                            || oldAmountShortLen != cachedAmountTextShort.length()
                            || oldSeriesFullLen  != cachedSeriesTextFull.length()
                            || oldSeriesShortLen != cachedSeriesTextShort.length()
                            || oldFreqLen        != cachedFreqTextHz.length()
                            || oldFreqShortLen   != cachedFreqTextShort.length()
                            || oldShapeFullLen   != cachedShapeTextFull.length()
                            || oldShapeShortLen  != cachedShapeTextShort.length()
                            || oldJitterFullLen  != cachedJitterTextFull.length()
                            || oldJitterShortLen != cachedJitterTextShort.length()
                            || oldStyleFullLen   != cachedStyleTextFull.length()
                            || oldStyleShortLen  != cachedStyleTextShort.length()
                            || oldFeedbackFullLen  != cachedFeedbackTextFull.length()
                            || oldFeedbackShortLen != cachedFeedbackTextShort.length()
                            || oldModFullLen    != cachedModTextFull.length()
                            || oldModShortLen   != cachedModTextShort.length()
                            || oldMixFullLen    != cachedMixTextFull.length()
                            || oldMixShortLen   != cachedMixTextShort.length()
                            || oldInputFullLen   != cachedInputTextFull.length()
                            || oldInputShortLen  != cachedInputTextShort.length()
                            || oldOutputFullLen   != cachedOutputTextFull.length()
                            || oldOutputShortLen  != cachedOutputTextShort.length()
                            || oldPanFullLen      != cachedPanTextFull.length()
                            || oldPanShortLen     != cachedPanTextShort.length()
                            || oldLimFullLen      != cachedLimThresholdTextFull.length()
                            || oldLimShortLen     != cachedLimThresholdTextShort.length();

    return lengthChanged;
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getRowRepaintBounds (const juce::Slider& s) const
{
    auto bounds = s.getBounds().getUnion (getValueAreaFor (s.getBounds()));
    return bounds.expanded (8, 8).getIntersection (getLocalBounds());
}

void DisperserAudioProcessorEditor::setupBar (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::LinearBar);
    s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);

    s.setPopupDisplayEnabled (false, false, this);
    s.setTooltip (juce::String());

    s.setPopupMenuEnabled (false);

    TR::applySimpleTransparentSliderColours (s, activeScheme);
}

//========================== Right-click numeric popup ==========================

namespace
{

    using PopupSwatchButton = TR::PopupSwatchButton;

    using PopupClickableLabel = TR::PopupClickableLabel;
    using TextLayoutLabel = TR::TextLayoutLabel;
}

void DisperserAudioProcessorEditor::openNumericEntryPopupForSlider (juce::Slider& s)
{
    if (&s == &seriesSlider || &s == &styleSlider)
        return;

    lnf.setScheme (activeScheme);

    const bool isModHarmPrompt = (&s == &modSlider && isModHarmEnabled (audioProcessor));

    TR::NumericEntryPromptSpec spec;

    if (&s == &amountSlider)       { spec.suffix = " STAGES";    spec.suffixShort = " STG"; }
    else if (&s == &freqSlider)    { spec.suffix = " Hz";        spec.suffixShort = " Hz"; }
    else if (&s == &shapeSlider)   { spec.suffix = " % SHP";     spec.suffixShort = " % SHP"; }
    else if (&s == &jitterSlider)  { spec.suffix = " % JITTER";  spec.suffixShort = " % JIT"; }
    else if (&s == &feedbackSlider){ spec.suffix = " % FBK";     spec.suffixShort = " % FBK"; }
    else if (&s == &modSlider)     { if (! isModHarmPrompt) spec.prefix = "X"; spec.suffix = " MOD"; spec.suffixShort = " MOD"; }
    else if (&s == &mixSlider)     { spec.suffix = " % MIX";     spec.suffixShort = " % MIX"; }
    else if (&s == &panSlider)     { spec.suffix = " % PAN";     spec.suffixShort = " % PAN"; }
    else if (&s == &inputSlider)   { spec.suffix = " dB INPUT";  spec.suffixShort = " dB IN"; }
    else if (&s == &outputSlider)  { spec.suffix = " dB OUTPUT"; spec.suffixShort = " dB OUT"; }
    else if (&s == &tiltSlider)    { spec.suffix = " dB TILT";   spec.suffixShort = " dB TILT"; }
    else if (&s == &limThresholdSlider) { spec.suffix = " dB LIM"; spec.suffixShort = " dB LIM"; }

    if (&s == &amountSlider)
        spec.currentDisplay = juce::String ((int) s.getValue());
    else if (&s == &freqSlider)
        spec.currentDisplay = juce::String (s.getValue(), 3);
    else if (&s == &shapeSlider || &s == &jitterSlider || &s == &mixSlider || &s == &panSlider)
        spec.currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), &s == &panSlider ? 0 : 2);
    else if (&s == &feedbackSlider)
        spec.currentDisplay = juce::String (juce::jlimit (-100.0, 100.0, s.getValue() * 100.0), 2);
    else if (&s == &modSlider)
        spec.currentDisplay = isModHarmPrompt ? formatModHarmText (s.getValue(), false)
                                              : juce::String (modSliderToMultiplier (s.getValue()), 2);
    else
        spec.currentDisplay = s.getTextFromValue (s.getValue());

    if (&s == &amountSlider)       { spec.minValue = (double) DisperserAudioProcessor::kAmountMin; spec.maxValue = (double) DisperserAudioProcessor::kAmountMax; spec.maxDecimals = 0; spec.maxLength = 3; spec.worstCaseText = "128"; }
    else if (&s == &freqSlider)    { spec.minValue = DisperserAudioProcessor::kFreqMin; spec.maxValue = DisperserAudioProcessor::kFreqEffectiveMax; spec.maxDecimals = 3; spec.maxLength = 8; spec.worstCaseText = "20000.000"; }
    else if (&s == &shapeSlider || &s == &jitterSlider || &s == &mixSlider) { spec.minValue = 0.0; spec.maxValue = 100.0; spec.maxDecimals = 2; spec.maxLength = 6; spec.worstCaseText = "100.00"; }
    else if (&s == &feedbackSlider){ spec.minValue = -100.0; spec.maxValue = 100.0; spec.maxDecimals = 2; spec.maxLength = 7; spec.worstCaseText = "-100.00"; }
    else if (&s == &modSlider)
    {
        if (isModHarmPrompt) { spec.minValue = -8.0; spec.maxValue = 8.0; spec.maxDecimals = 0; spec.maxLength = 4; spec.worstCaseText = "H+8"; spec.inputKind = TR::NumericEntryPromptInputKind::HarmonicStep; }
        else { spec.minValue = 0.0; spec.maxValue = 4.0; spec.maxDecimals = 2; spec.maxLength = 4; spec.worstCaseText = "4.00"; }
    }
    else if (&s == &inputSlider || &s == &outputSlider) { spec.minValue = DisperserAudioProcessor::kGainFloorDb; spec.maxValue = DisperserAudioProcessor::kGainMaxDb; spec.maxDecimals = 1; spec.maxLength = 6; spec.worstCaseText = "-144.0"; }
    else if (&s == &tiltSlider)    { spec.minValue = DisperserAudioProcessor::kTiltMin; spec.maxValue = DisperserAudioProcessor::kTiltMax; spec.maxDecimals = 1; spec.maxLength = 4; spec.worstCaseText = "-6.0"; }
    else if (&s == &limThresholdSlider) { const auto r = limThresholdSlider.getRange(); spec.minValue = r.getStart(); spec.maxValue = r.getEnd(); spec.maxDecimals = 1; spec.maxLength = 5; spec.worstCaseText = "-36.0"; }
    else if (&s == &panSlider)     { spec.minValue = 0.0; spec.maxValue = 100.0; spec.maxDecimals = 0; spec.maxLength = 3; spec.worstCaseText = "100"; }

    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);
    juce::Slider* sliderPtr = &s;
    spec.onAccept = [safeThis, sliderPtr] (const juce::String& txt)
    {
        if (safeThis == nullptr || sliderPtr == nullptr)
            return;

        auto normalised = txt.replaceCharacter (',', '.');
        juce::String t = normalised.trimStart();
        while (t.startsWithChar ('+'))
            t = t.substring (1).trimStart();

        double v = t.initialSectionContainingOnly ("0123456789.,-").getDoubleValue();

        if (sliderPtr == &safeThis->shapeSlider || sliderPtr == &safeThis->jitterSlider
            || sliderPtr == &safeThis->mixSlider || sliderPtr == &safeThis->panSlider
            || sliderPtr == &safeThis->feedbackSlider)
            v *= 0.01;

        if (sliderPtr == &safeThis->modSlider)
        {
            if (isModHarmEnabled (safeThis->audioProcessor))
            {
                juce::String h = normalised.trim().toUpperCase();
                if (h.startsWithChar ('H')) h = h.substring (1).trimStart();
                while (h.startsWithChar ('+')) h = h.substring (1).trimStart();
                const int step = juce::jlimit (-8, 8, h.getIntValue());
                v = ((double) step + 8.0) / 16.0;
            }
            else
            {
                v = multiplierToModSlider (v);
            }
        }

        const auto range = sliderPtr->getRange();
        double clamped = juce::jlimit (range.getStart(), range.getEnd(), v);
        if (sliderPtr == &safeThis->freqSlider)
            clamped = roundToDecimals (clamped, 3);
        sliderPtr->setValue (clamped, juce::sendNotificationSync);
    };

    TR::openNumericEntryPopupShared (this, lnf, activeScheme, spec);
}

// â”€â”€ Filter Prompt (HP/LP frequency + slope) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void DisperserAudioProcessorEditor::openFilterPrompt()
{
    lnf.setScheme (activeScheme);
    auto& vts = audioProcessor.apvts;

    FilterPromptSpec spec;
    spec.hpParam = DisperserAudioProcessor::kParamFilterHpFreq;
    spec.lpParam = DisperserAudioProcessor::kParamFilterLpFreq;
    spec.hpOnParam = DisperserAudioProcessor::kParamFilterHpOn;
    spec.lpOnParam = DisperserAudioProcessor::kParamFilterLpOn;
    spec.hpSlopeParam = DisperserAudioProcessor::kParamFilterHpSlope;
    spec.lpSlopeParam = DisperserAudioProcessor::kParamFilterLpSlope;
    spec.freqMin = 20.0f;
    spec.freqMax = 20000.0f;
    spec.hpDefault = DisperserAudioProcessor::kFilterHpFreqDefault;
    spec.lpDefault = DisperserAudioProcessor::kFilterLpFreqDefault;
    spec.slopeMin = DisperserAudioProcessor::kFilterSlopeMin;
    spec.slopeMax = DisperserAudioProcessor::kFilterSlopeMax;
    spec.refreshFilterDisplay = [this] { filterBar_.updateFromProcessor(); };

    openFilterPromptShared (this, lnf, activeScheme, vts, spec);
}


// â”€â”€ MIDI Channel Prompt â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void DisperserAudioProcessorEditor::openMidiChannelPrompt()
{
    TR::openMidiChannelDelayPromptShared<DisperserAudioProcessorEditor> (this,
                                                   lnf,
                                                   activeScheme,
                                                   [this]() { return audioProcessor.getMidiChannel(); },
                                                   [this] (int ch) { audioProcessor.setMidiChannel (ch); },
                                                   [this]() { return audioProcessor.getMidiDelayMs(); },
                                                   [this] (int delayMs) { audioProcessor.setMidiDelayMs (delayMs); },
                                                   [this] (int ch, int delayMs)
                                                   {
                                                       midiChannelDisplay.setTooltip (formatMidiChannelTooltip (ch, delayMs));
                                                   });
}

void DisperserAudioProcessorEditor::openChaosConfigPrompt (const char* amtParamId,
                                                 const char* spdParamId,
                                                 const juce::String& title)
{
    auto& vts = audioProcessor.apvts;
    const bool isFilterChaos = title == "CHSF";
    const TR::SimpleChaosPromptBinding binding {
        amtParamId,
        spdParamId,
        isFilterChaos ? vts.getRawParameterValue (DisperserAudioProcessor::kParamChaosAmtFilter)->load()
                      : vts.getRawParameterValue (DisperserAudioProcessor::kParamChaosAmt)->load(),
        isFilterChaos ? vts.getRawParameterValue (DisperserAudioProcessor::kParamChaosSpdFilter)->load()
                      : vts.getRawParameterValue (DisperserAudioProcessor::kParamChaosSpd)->load()
    };

    TR::openSimpleChaosPromptAction<DisperserAudioProcessorEditor> (this,
                                            lnf,
                                            activeScheme,
                                            vts,
                                            binding,
                                            [this, isFilterChaos, amtParamId, spdParamId]
                                            {
                                                const auto amt = audioProcessor.apvts.getRawParameterValue (amtParamId)->load();
                                                const auto spd = audioProcessor.apvts.getRawParameterValue (spdParamId)->load();
                                                const auto tip = formatChaosTooltip (amt, spd);
                                                if (isFilterChaos)
                                                    chaosFilterDisplay.setTooltip (tip);
                                                else
                                                    chaosDelayDisplay.setTooltip (tip);
                                                repaint();
                                            });
}

void DisperserAudioProcessorEditor::openChaosFilterPrompt()
{
    TR::openSimpleChaosSelectorPromptAction (
        [this] (const char* amountParamId, const char* speedParamId, const juce::String& title)
        {
            openChaosConfigPrompt (amountParamId, speedParamId, title);
        },
        DisperserAudioProcessor::kParamChaosAmtFilter,
        DisperserAudioProcessor::kParamChaosSpdFilter,
        true);
}

void DisperserAudioProcessorEditor::openChaosDelayPrompt()
{
    TR::openSimpleChaosSelectorPromptAction (
        [this] (const char* amountParamId, const char* speedParamId, const juce::String& title)
        {
            openChaosConfigPrompt (amountParamId, speedParamId, title);
        },
        DisperserAudioProcessor::kParamChaosAmt,
        DisperserAudioProcessor::kParamChaosSpd,
        false);
}

void DisperserAudioProcessorEditor::openSidechainPrompt()
{
    lnf.setScheme (activeScheme);
    auto& vts = audioProcessor.apvts;

    SidechainPromptSpec spec;
    spec.gainParam = DisperserAudioProcessor::kParamSidechainGain;
    spec.smoothParam = DisperserAudioProcessor::kParamSidechainSmooth;
    spec.polParam = DisperserAudioProcessor::kParamSidechainPol;
    spec.hpParam = DisperserAudioProcessor::kParamSidechainHp;
    spec.lpParam = DisperserAudioProcessor::kParamSidechainLp;
    spec.hpOnParam = DisperserAudioProcessor::kParamSidechainHpOn;
    spec.lpOnParam = DisperserAudioProcessor::kParamSidechainLpOn;
    spec.hpSlopeParam = DisperserAudioProcessor::kParamSidechainHpSlope;
    spec.lpSlopeParam = DisperserAudioProcessor::kParamSidechainLpSlope;

    spec.gainMin = DisperserAudioProcessor::kSidechainGainMin;
    spec.gainMax = DisperserAudioProcessor::kSidechainGainMax;
    spec.gainDefault = DisperserAudioProcessor::kSidechainGainDefault;
    spec.gainSkew = DisperserAudioProcessor::kGainSkew;
    spec.smoothMin = DisperserAudioProcessor::kSidechainSmoothMin;
    spec.smoothMax = DisperserAudioProcessor::kSidechainSmoothMax;
    spec.smoothDefault = DisperserAudioProcessor::kSidechainSmoothDefault;
    spec.polMin = DisperserAudioProcessor::kSidechainPolMin;
    spec.polMax = DisperserAudioProcessor::kSidechainPolMax;
    spec.polDefault = DisperserAudioProcessor::kSidechainPolDefault;
    spec.freqMin = DisperserAudioProcessor::kSidechainFilterFreqMin;
    spec.freqMax = DisperserAudioProcessor::kSidechainFilterFreqMax;
    spec.hpDefault = DisperserAudioProcessor::kSidechainHpDefault;
    spec.lpDefault = DisperserAudioProcessor::kSidechainLpDefault;
    spec.slopeMin = DisperserAudioProcessor::kFilterSlopeMin;
    spec.slopeMax = DisperserAudioProcessor::kFilterSlopeMax;

    spec.refreshTooltip = [this]
    {
        auto& state = audioProcessor.apvts;
        sidechainDisplay.setTooltip (formatSidechainTooltip (
            state.getRawParameterValue (DisperserAudioProcessor::kParamSidechainGain)->load(),
            state.getRawParameterValue (DisperserAudioProcessor::kParamSidechainSmooth)->load(),
            state.getRawParameterValue (DisperserAudioProcessor::kParamSidechainPol)->load(),
            state.getRawParameterValue (DisperserAudioProcessor::kParamSidechainHpOn)->load() > 0.5f,
            state.getRawParameterValue (DisperserAudioProcessor::kParamSidechainHp)->load(),
            (int) std::lround (state.getRawParameterValue (DisperserAudioProcessor::kParamSidechainHpSlope)->load()),
            state.getRawParameterValue (DisperserAudioProcessor::kParamSidechainLpOn)->load() > 0.5f,
            state.getRawParameterValue (DisperserAudioProcessor::kParamSidechainLp)->load(),
            (int) std::lround (state.getRawParameterValue (DisperserAudioProcessor::kParamSidechainLpSlope)->load())));
    };

    openSidechainPromptShared (this, lnf, activeScheme, vts, spec);
}


juce::String DisperserAudioProcessorEditor::getAmountText() const
{
    const int v = (int) std::llround (amountSlider.getValue());
    return juce::String (v) + " STAGES";
}

juce::String DisperserAudioProcessorEditor::getAmountTextShort() const
{
    const int v = (int) std::llround (amountSlider.getValue());
    return juce::String (v) + " STG";
}

juce::String DisperserAudioProcessorEditor::getSeriesText() const
{
    const int v = (int) std::llround (seriesSlider.getValue());
    return juce::String (v) + " SERIES";
}

juce::String DisperserAudioProcessorEditor::getSeriesTextShort() const
{
    const int v = (int) std::llround (seriesSlider.getValue());
    return juce::String (v) + " SRS";
}

juce::String DisperserAudioProcessorEditor::getFreqText() const
{
    if (cachedMidiDisplay.isNotEmpty() && ! freqSlider.isMouseButtonDown())
        return cachedMidiDisplay + " FREQ";

    const double hz = freqSlider.getValue();

    return formatInlineFrequency (hz) + " FREQ";
}

juce::String DisperserAudioProcessorEditor::getFreqTextShort() const
{
    if (cachedMidiDisplay.isNotEmpty() && ! freqSlider.isMouseButtonDown())
        return cachedMidiDisplay + " FREQ";

    const double hz = freqSlider.getValue();

    return formatInlineFrequency (hz) + " FREQ";
}

juce::String DisperserAudioProcessorEditor::getShapeText() const
{
    const double v = juce::jlimit (0.0, 1.0, shapeSlider.getValue());
    return juce::String ((int) std::lround (v * 100.0)) + "% SHAPE";
}

juce::String DisperserAudioProcessorEditor::getShapeTextShort() const
{
    const double v = juce::jlimit (0.0, 1.0, shapeSlider.getValue());
    return juce::String ((int) std::lround (v * 100.0)) + "% SHP";
}

juce::String DisperserAudioProcessorEditor::getJitterText() const
{
    const double v = juce::jlimit (0.0, 1.0, jitterSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt).toUpperCase() + "% JITTER";
}

juce::String DisperserAudioProcessorEditor::getJitterTextShort() const
{
    const double v = juce::jlimit (0.0, 1.0, jitterSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt).toUpperCase() + "% JIT";
}

juce::String DisperserAudioProcessorEditor::getStyleText() const
{
    const int style = (int) styleSlider.getValue();
    if (style == 0) return "MONO STYLE";
    if (style == 2) return "WIDE STYLE";
    if (style == 3) return "DUAL STYLE";
    return "STEREO STYLE";
}

juce::String DisperserAudioProcessorEditor::getStyleTextShort() const
{
    const int style = (int) styleSlider.getValue();
    if (style == 0) return "MONO";
    if (style == 2) return "WIDE";
    if (style == 3) return "DUAL";
    return "STEREO";
}

juce::String DisperserAudioProcessorEditor::getFeedbackText() const
{
    const double v = juce::jlimit (-1.0, 1.0, feedbackSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt) + "% FBK";
}

juce::String DisperserAudioProcessorEditor::getFeedbackTextShort() const
{
    const double v = juce::jlimit (-1.0, 1.0, feedbackSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt) + "% FBK";
}

juce::String DisperserAudioProcessorEditor::getModText() const
{
    if (isModHarmEnabled (audioProcessor))
        return formatModHarmText (modSlider.getValue(), true);

    const float mult = (float) modSliderToMultiplier (modSlider.getValue());
    if (std::abs (mult - 1.0f) < kMultEpsilon)
        return "X1 MOD";
    return "X" + juce::String (mult, 2) + " MOD";
}

juce::String DisperserAudioProcessorEditor::getModTextShort() const
{
    if (isModHarmEnabled (audioProcessor))
        return formatModHarmText (modSlider.getValue(), false);

    const float mult = (float) modSliderToMultiplier (modSlider.getValue());
    if (std::abs (mult - 1.0f) < kMultEpsilon)
        return "X1 MOD";
    return "X" + juce::String (mult, 2) + " MOD";
}

juce::String DisperserAudioProcessorEditor::getMixText() const
{
    if (mixModeCombo.getSelectedId() == 2)
    {
        const bool isDry = (dualMixBar_.getLastTouched() != DualMixBarComponent::WET);
        const float level = isDry ? dualMixBar_.getDryLevel() : dualMixBar_.getWetLevel();
        const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
        const juce::String suffix = isDry ? " DRY" : " WET";
        if (dB <= -100.0f) return "-INF dB" + suffix;
        if (std::abs (dB) < 0.05f) return "0.0 dB" + suffix;
        return juce::String (dB, 1) + " dB" + suffix;
    }
    const double v = juce::jlimit (0.0, 1.0, mixSlider.getValue());
    return juce::String ((int) std::lround (v * 100.0)) + "% MIX";
}

juce::String DisperserAudioProcessorEditor::getMixTextShort() const
{
    if (mixModeCombo.getSelectedId() == 2)
    {
        const bool isDry = (dualMixBar_.getLastTouched() != DualMixBarComponent::WET);
        const float level = isDry ? dualMixBar_.getDryLevel() : dualMixBar_.getWetLevel();
        const float dB = (level <= 0.0001f) ? -100.0f : 20.0f * std::log10 (level);
        const juce::String suffix = isDry ? " DRY" : " WET";
        if (dB <= -100.0f) return "-INF" + suffix;
        if (std::abs (dB) < 0.05f) return "0.0dB" + suffix;
        return juce::String (dB, 1) + "dB" + suffix;
    }
    const double v = juce::jlimit (0.0, 1.0, mixSlider.getValue());
    return juce::String ((int) std::lround (v * 100.0)) + "% MIX";
}

juce::String DisperserAudioProcessorEditor::getInputText() const
{
    const float db = (float) inputSlider.getValue();
    return formatGainFaderDb (db) + " INPUT";
}

juce::String DisperserAudioProcessorEditor::getInputTextShort() const
{
    const float db = (float) inputSlider.getValue();
    return formatGainFaderDb (db) + " IN";
}

juce::String DisperserAudioProcessorEditor::getOutputText() const
{
    const float db = (float) outputSlider.getValue();
    return formatGainFaderDb (db) + " OUTPUT";
}

juce::String DisperserAudioProcessorEditor::getOutputTextShort() const
{
    const float db = (float) outputSlider.getValue();
    return formatGainFaderDb (db) + " OUT";
}

juce::String DisperserAudioProcessorEditor::getTiltText() const
{
    const float db = (float) tiltSlider.getValue();
    if (std::abs (db) < 0.05f)
        return "0.0 dB TILT";
    return juce::String (db, 1) + " dB TILT";
}

juce::String DisperserAudioProcessorEditor::getTiltTextShort() const
{
    const float db = (float) tiltSlider.getValue();
    if (std::abs (db) < 0.05f)
        return "0.0 dB TLT";
    return juce::String (db, 1) + " dB TLT";
}

juce::String DisperserAudioProcessorEditor::getPanText() const
{
    const float v = (float) panSlider.getValue();
    const int pct = juce::roundToInt ((v - 0.5f) * 200.0f);
    if (pct == 0) return "C PAN";
    if (pct < 0)  return "L" + juce::String (-pct) + " PAN";
    return "R" + juce::String (pct) + " PAN";
}

juce::String DisperserAudioProcessorEditor::getPanTextShort() const
{
    const float v = (float) panSlider.getValue();
    const int pct = juce::roundToInt ((v - 0.5f) * 200.0f);
    if (pct == 0) return "C";
    if (pct < 0)  return "L" + juce::String (-pct);
    return "R" + juce::String (pct);
}

juce::String DisperserAudioProcessorEditor::getFilterText() const
{
    return "FILTER";
}

juce::String DisperserAudioProcessorEditor::getFilterTextShort() const
{
    return "FLTR";
}

namespace
{
    constexpr const char* kAmountLegendFull  = "256 STAGES";
    constexpr const char* kAmountLegendShort = "256 STG";
    constexpr const char* kAmountLegendInt   = "256";

    constexpr const char* kSeriesLegendFull  = "999 SERIES";
    constexpr const char* kSeriesLegendShort = "999 SRS";
    constexpr const char* kSeriesLegendInt   = "999";

    constexpr const char* kFreqLegendDisplay = "20000.00 HZ";
    constexpr const char* kFreqLegendAlt     = "20.00 KHZ";
    constexpr const char* kFreqLegendInt     = "20000";

    constexpr const char* kShapeLegendFull   = "100% SHAPE";
    constexpr const char* kShapeLegendShort  = "100% SHP";
    constexpr const char* kShapeLegendInt    = "100%";

    constexpr const char* kJitterLegendFull  = "100% JITTER";
    constexpr const char* kJitterLegendShort = "100% JIT";
    constexpr const char* kJitterLegendInt   = "100";

    constexpr const char* kStyleLegendFull   = "STEREO STYLE";
    constexpr const char* kStyleLegendShort  = "STEREO";
    constexpr const char* kStyleLegendInt    = "STEREO";

    constexpr const char* kFeedbackLegendFull  = "100% FBK";
    constexpr const char* kFeedbackLegendShort = "100% FBK";
    constexpr const char* kFeedbackLegendInt   = "100%";

    constexpr const char* kModLegendFull  = "X4.00 MOD";
    constexpr const char* kModLegendShort = "X4.00";
    constexpr const char* kModLegendInt   = "X4";

    constexpr const char* kInputLegendFull  = "-100.0 dB INPUT";
    constexpr const char* kInputLegendShort = "-100.0 dB IN";
    constexpr const char* kInputLegendInt   = "-100.0dB";

    constexpr const char* kOutputLegendFull  = "-100.0 dB OUTPUT";
    constexpr const char* kOutputLegendShort = "-100.0 dB OUT";
    constexpr const char* kOutputLegendInt   = "-100.0dB";

    constexpr const char* kTiltLegendFull  = "-6.0 dB TILT";
    constexpr const char* kTiltLegendShort = "-6.0 dB TLT";
    constexpr const char* kTiltLegendInt   = "-6.0dB";

    constexpr const char* kMixLegendFull  = "100% MIX";
    constexpr const char* kMixLegendShort = "100% MIX";
    constexpr const char* kMixLegendInt   = "100%";

    constexpr const char* kLimLegendFull  = "-36.0 dB LIM";
    constexpr const char* kLimLegendShort = "-36.0 dB LIM";
    constexpr const char* kLimLegendInt   = "-36.0dB";
    constexpr int kResizerCornerPx = 22;
    constexpr int kToggleBoxPx = 72;
    constexpr int kMinToggleBlocksGapPx = 10;
}

DisperserAudioProcessorEditor::HorizontalLayoutMetrics
DisperserAudioProcessorEditor::buildHorizontalLayout (int editorW, int valueColW)
{
    const auto shared = TR::buildSimpleHorizontalLayout (editorW, valueColW);
    HorizontalLayoutMetrics m;
    m.barW = shared.barW;
    m.valuePad = shared.valuePad;
    m.valueW = shared.valueW;
    m.contentW = shared.contentW;
    m.leftX = shared.leftX;
    return m;
}

DisperserAudioProcessorEditor::VerticalLayoutMetrics
DisperserAudioProcessorEditor::buildVerticalLayout (int editorH, int biasY, bool ioExpanded)
{
    TR::SimpleVerticalLayoutConfig config;
    config.mainRows = 8;
    config.collapsedButtonRows = 1;
    config.collapsedSliderBottomRow = 0;
    config.expandedHasSidechainRow = true;

    const auto shared = TR::buildSimpleVerticalLayout (editorH, biasY, ioExpanded, config);
    VerticalLayoutMetrics m;
    m.rhythm = shared.rhythm;
    m.titleH = shared.titleH;
    m.titleAreaH = shared.titleAreaH;
    m.titleTopPad = shared.titleTopPad;
    m.topMargin = shared.topMargin;
    m.betweenSlidersAndButtons = shared.betweenSlidersAndButtons;
    m.bottomMargin = shared.bottomMargin;
    m.box = shared.box;
    m.btnRowGap = shared.btnRowGap;
    m.chaosRowY = shared.chaosRowY;
    m.sidechainRowY = shared.sidechainRowY;
    m.btnY = shared.btnY;
    m.availableForSliders = shared.availableForSliders;
    m.barH = shared.barH;
    m.gapY = shared.gapY;
    m.firstGapY = shared.firstGapY;
    m.topY = shared.topY;
    m.toggleBarH = shared.toggleBarH;
    m.toggleBarY = shared.toggleBarY;
    return m;
}

void DisperserAudioProcessorEditor::updateCachedLayout()
{
    cachedHLayout_ = buildHorizontalLayout (getWidth(), getTargetValueColumnWidth());
    cachedVLayout_ = buildVerticalLayout (getHeight(), kLayoutVerticalBiasPx, ioSectionExpanded_);

    const juce::Slider* sliders[12] = { &freqSlider, &modSlider, &feedbackSlider, &amountSlider,
                                         &seriesSlider, &shapeSlider, &jitterSlider, &styleSlider,
                                         &inputSlider, &outputSlider, &tiltSlider, &mixSlider };

    for (int i = 0; i < 12; ++i)
    {
        if (! sliders[i]->isVisible())
        {
            // MIX row (index 11): use dualMixBar_ bounds when SEND mode is active
            if (i == 11 && dualMixBar_.isVisible())
            {
                const auto& bb = dualMixBar_.getBounds();
                cachedValueAreas_[11] = TR::makeSimpleValueArea (bb, cachedHLayout_, getWidth());
                continue;
            }
            cachedValueAreas_[(size_t) i] = {};
            continue;
        }

        const auto& bb = sliders[i]->getBounds();
                cachedValueAreas_[(size_t) i] = TR::makeSimpleValueArea (bb, cachedHLayout_, getWidth());
    }

    // Filter bar value area
    if (filterBar_.isVisible())
    {
        const auto& fb = filterBar_.getBounds();
                cachedFilterValueArea_ = TR::makeSimpleValueArea (fb, cachedHLayout_, getWidth());
    }
    else
    {
        cachedFilterValueArea_ = {};
    }

    // Pan slider value area
    if (panSlider.isVisible())
    {
        const auto& pb = panSlider.getBounds();
                cachedPanValueArea_ = TR::makeSimpleValueArea (pb, cachedHLayout_, getWidth());
    }
    else
    {
        cachedPanValueArea_ = {};
    }

    // Lim threshold slider value area
    if (limThresholdSlider.isVisible())
    {
        const auto& lb = limThresholdSlider.getBounds();
                cachedLimThresholdValueArea_ = TR::makeSimpleValueArea (lb, cachedHLayout_, getWidth());
    }
    else
    {
        cachedLimThresholdValueArea_ = {};
    }

    // Cache toggle bar area
    cachedToggleBarArea_ = TR::makeSimpleToggleBarArea (cachedHLayout_, cachedVLayout_);
}

int DisperserAudioProcessorEditor::getTargetValueColumnWidth() const
{
    std::uint64_t key = 1469598103934665603ull;
    auto mix = [&] (std::uint64_t v)
    {
        key ^= v;
        key *= 1099511628211ull;
    };

    mix ((std::uint64_t) getWidth());

    if (key == cachedValueColumnWidthKey)
        return cachedValueColumnWidth;

    constexpr float baseFontPx = 40.0f;
    juce::Font font (juce::FontOptions (baseFontPx).withStyle ("Bold"));

    const int amountMaxW = juce::jmax (stringWidth (font, kAmountLegendFull),
                                       juce::jmax (stringWidth (font, kAmountLegendShort),
                                                   stringWidth (font, kAmountLegendInt)));

    const int seriesMaxW = juce::jmax (stringWidth (font, kSeriesLegendFull),
                                       juce::jmax (stringWidth (font, kSeriesLegendShort),
                                                   stringWidth (font, kSeriesLegendInt)));

    const int freqMaxW = juce::jmax (stringWidth (font, kFreqLegendDisplay),
                                     juce::jmax (stringWidth (font, kFreqLegendAlt),
                                                 stringWidth (font, kFreqLegendInt)));

    const int shapeMaxW = juce::jmax (stringWidth (font, kShapeLegendFull),
                                      juce::jmax (stringWidth (font, kShapeLegendShort),
                                                  stringWidth (font, kShapeLegendInt)));

    const int jitterMaxW = juce::jmax (stringWidth (font, kJitterLegendFull),
                                       juce::jmax (stringWidth (font, kJitterLegendShort),
                                                   stringWidth (font, kJitterLegendInt)));

    const int styleMaxW = juce::jmax (stringWidth (font, kStyleLegendFull),
                                      juce::jmax (stringWidth (font, kStyleLegendShort),
                                                  stringWidth (font, kStyleLegendInt)));

    const int feedbackMaxW = juce::jmax (stringWidth (font, kFeedbackLegendFull),
                                         juce::jmax (stringWidth (font, kFeedbackLegendShort),
                                                     stringWidth (font, kFeedbackLegendInt)));

    const int modMaxW = juce::jmax (stringWidth (font, kModLegendFull),
                                    juce::jmax (stringWidth (font, kModLegendShort),
                                                stringWidth (font, kModLegendInt)));

    const int mixMaxW = juce::jmax (stringWidth (font, kMixLegendFull),
                                    juce::jmax (stringWidth (font, kMixLegendShort),
                                                stringWidth (font, kMixLegendInt)));

    const int inputMaxW = juce::jmax (stringWidth (font, kInputLegendFull),
                                      juce::jmax (stringWidth (font, kInputLegendShort),
                                                  stringWidth (font, kInputLegendInt)));

    const int outputMaxW = juce::jmax (stringWidth (font, kOutputLegendFull),
                                       juce::jmax (stringWidth (font, kOutputLegendShort),
                                                   stringWidth (font, kOutputLegendInt)));

    const int tiltMaxW = juce::jmax (stringWidth (font, kTiltLegendFull),
                                     juce::jmax (stringWidth (font, kTiltLegendShort),
                                                 stringWidth (font, kTiltLegendInt)));

    const int limMaxW = juce::jmax (stringWidth (font, kLimLegendFull),
                                    juce::jmax (stringWidth (font, kLimLegendShort),
                                                stringWidth (font, kLimLegendInt)));

    const int maxW = juce::jmax (juce::jmax (amountMaxW, seriesMaxW),
                                 juce::jmax (juce::jmax (freqMaxW, shapeMaxW),
                                             juce::jmax (juce::jmax (jitterMaxW, juce::jmax (feedbackMaxW, modMaxW)),
                                                         juce::jmax (juce::jmax (styleMaxW, mixMaxW),
                                                                     juce::jmax (juce::jmax (inputMaxW, outputMaxW),
                                                                                 juce::jmax (tiltMaxW, limMaxW))))));

    const int desired = maxW + 16;
    const int minW = 90;
    // Allow up to 40% of editor width for longer legends (INPUT/OUTPUT with dB units)
    const int maxAllowed = juce::jmax (minW, (int) std::round (getWidth() * 0.40));
    cachedValueColumnWidth = juce::jlimit (minW, maxAllowed, desired);
    cachedValueColumnWidthKey = key;
    return cachedValueColumnWidth;
}

//========================== Hit areas ==========================

juce::Rectangle<int> DisperserAudioProcessorEditor::getValueAreaFor (const juce::Rectangle<int>& barBounds) const
{
    return TR::makeSimpleValueArea (barBounds, cachedHLayout_, getWidth());
}

juce::Slider* DisperserAudioProcessorEditor::getSliderForValueAreaPoint (juce::Point<int> p)
{
    if (auto* slider = TR::findSimpleSliderForValueAreaPoint (p, cachedValueAreas_, {
            { 0, &amountSlider },
            { 1, &seriesSlider },
            { 2, &freqSlider },
            { 3, &shapeSlider },
            { 4, &jitterSlider },
            { 5, &styleSlider },
            { 6, &feedbackSlider },
            { 7, &modSlider },
            { 8, &inputSlider },
            { 9, &outputSlider },
            { 10, &tiltSlider },
            { 11, &mixSlider } }))
        return slider;

    if (cachedPanValueArea_.contains (p))
        return &panSlider;

    if (cachedLimThresholdValueArea_.contains (p))
        return &limThresholdSlider;

    return nullptr;
}

namespace
{
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getAltLabelArea() const
{
    return TR::makeSimpleToggleLabelArea (altButton, midiButton.getX() - TR::kSimpleToggleLegendCollisionPadPx, "ALT", "ALT");
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getMidiLabelArea() const
{
    return TR::makeSimpleToggleLabelArea (midiButton, getWidth() - TR::kSimpleToggleLegendCollisionPadPx, "MIDI", "MIDI");
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getSidechainLabelArea() const
{
    return TR::makeSimpleToggleLabelArea (sidechainButton, getWidth() - TR::kSimpleToggleLegendCollisionPadPx, "SIDECHAIN", "SC");
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getChaosFilterLabelArea() const
{
    if (chaosFilterButton.getWidth() <= 0 || chaosFilterButton.getHeight() <= 0)
        return {};

    return TR::makeSimpleToggleLabelArea (chaosFilterButton,
                                          chaosDelayButton.getX() - TR::kSimpleToggleLegendCollisionPadPx,
                                          "CHSF", "CHSF");
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getChaosDelayLabelArea() const
{
    if (chaosDelayButton.getWidth() <= 0 || chaosDelayButton.getHeight() <= 0)
        return {};

    return TR::makeSimpleToggleLabelArea (chaosDelayButton,
                                          getWidth() - TR::kSimpleToggleLegendCollisionPadPx,
                                          "CHSD", "CHSD");
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getInfoIconArea() const
{
    // Use a visible slider for content-right calculation in both modes
    const auto& refSlider = amountSlider.isVisible() ? amountSlider
                          : mixSlider.isVisible()    ? mixSlider
                          :                            amountSlider;
    const auto refValueArea = getValueAreaFor (refSlider.getBounds());
    const int contentRight = refValueArea.getRight();
    const auto verticalLayout = buildVerticalLayout (getHeight(), kLayoutVerticalBiasPx, ioSectionExpanded_);
    const int titleH = verticalLayout.titleH;
    const int titleY = verticalLayout.titleTopPad;
    const int titleAreaH = verticalLayout.titleAreaH;
    const int size = juce::jlimit (20, 36, titleH);

    const int x = contentRight - size;
    const int y = titleY + juce::jmax (0, (titleAreaH - size) / 2);
    return { x, y, size, size };
}

void DisperserAudioProcessorEditor::openMixSendPrompt()
{
    TR::openMixSendPromptShared<DisperserAudioProcessorEditor> (this,
                                          lnf,
                                          activeScheme,
                                          audioProcessor.apvts,
                                          DisperserAudioProcessor::kParamDryLevel,
                                          DisperserAudioProcessor::kParamWetLevel,
                                          DisperserAudioProcessor::kDryLevelDefault,
                                          DisperserAudioProcessor::kWetLevelDefault,
                                          [this]() { dualMixBar_.updateFromProcessor(); });
}

void DisperserAudioProcessorEditor::openInfoPopup()
{
    lnf.setScheme (activeScheme);
    TR::openInfoPopupFromXmlShared<DisperserAudioProcessorEditor> (this,
                                           lnf,
                                           activeScheme,
                                           InfoContent::xml,
                                           [this]() { openGraphicsPopup(); });
}


void DisperserAudioProcessorEditor::openGraphicsPopup()
{
    lnf.setScheme (activeScheme);
    useCustomPalette = audioProcessor.getUiUseCustomPalette();
    crtEnabled = false;
    ioFxEnabled = audioProcessor.getUiIoFxEnabled();
    crtEffect.setEnabled (false);
    applyActivePalette();

    TR::openGraphicsPopupShared<DisperserAudioProcessorEditor> (this,
                                        lnf,
                                        activeScheme,
                                        defaultPalette,
                                        customPalette,
                                        useCustomPalette,
                                        ioFxEnabled,
                                        [this] (bool enabled)
                                        {
                                            useCustomPalette = enabled;
                                            audioProcessor.setUiUseCustomPalette (enabled);
                                        },
                                        [this] (int index, juce::Colour colour)
                                        {
                                            customPalette[(size_t) index] = colour;
                                            audioProcessor.setUiCustomPaletteColour (index, colour);
                                        },
                                        [this] (bool enabled)
                                        {
                                            applyIoFxState (enabled);
                                            audioProcessor.setUiIoFxEnabled (ioFxEnabled);
                                        },
                                        [this]()
                                        {
                                            applyActivePalette();
                                            updateIoFxMeterSliders();
                                            repaint();
                                        });
}


//========================== Mouse interactions ==========================

void DisperserAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    lastUserInteractionMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    const auto p = e.getEventRelativeTo (this).getPosition();

    if (TR::SimpleMouseRouter::routeMouseDown (*this, e, p,
            cachedToggleBarArea_, ioSectionExpanded_,
            modSlider, getValueAreaFor (modSlider.getBounds()),
            [this] { return isModHarmEnabled (audioProcessor); },
            [this] (bool v) { setModHarmEnabled (audioProcessor, v); },
            [this] (bool v) { return formatModHarmTooltip (v); },
            [this] {
                if (refreshLegendTextCache()) updateCachedLayout();
            },
            filterBar_, cachedFilterValueArea_,
            [this] { openFilterPrompt(); },
            [this] (juce::Point<int> pt) { return getSliderForValueAreaPoint (pt); },
            [this] (juce::Slider& s) { openNumericEntryPopupForSlider (s); },
            getInfoIconArea(), crtEnabled,
            [this] { openInfoPopup(); },
            {
                TR::SimpleMouseRouter::ToggleBinding { &altButton,         getAltLabelArea(),         nullptr,             {},                 true },
                TR::SimpleMouseRouter::ToggleBinding { &midiButton,        getMidiLabelArea(),        &midiChannelDisplay, [this] { openMidiChannelPrompt(); } },
                TR::SimpleMouseRouter::ToggleBinding { &sidechainButton,   getSidechainLabelArea(),   &sidechainDisplay,   [this] { openSidechainPrompt(); } },
                TR::SimpleMouseRouter::ToggleBinding { &chaosFilterButton, getChaosFilterLabelArea(), &chaosFilterDisplay, [this] { openChaosFilterPrompt(); } },
                TR::SimpleMouseRouter::ToggleBinding { &chaosDelayButton,  getChaosDelayLabelArea(),  &chaosDelayDisplay,  [this] { openChaosDelayPrompt(); } },
            }))
        return;
}



void DisperserAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    (void) e;
    lastUserInteractionMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
}

void DisperserAudioProcessorEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getEventRelativeTo (this).getPosition();
    TR::routeSimpleHoverTooltip (*this, tooltipWindow.get(), p,
    {
        { modSlider.isVisible() ? getValueAreaFor (modSlider.getBounds()) : juce::Rectangle<int>(),
          formatModHarmTooltip (isModHarmEnabled (audioProcessor)) }
    });
}

void DisperserAudioProcessorEditor::mouseExit (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);
    TR::clearSimpleHoverTooltip (*this, tooltipWindow.get());
}

void DisperserAudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    TR::SimpleMouseRouter::routeMouseDoubleClick (*this, e.getPosition(),
        [this] (juce::Point<int> pt) { return getSliderForValueAreaPoint (pt); },
        {
            { &amountSlider,       kDefaultAmount },
            { &seriesSlider,       kDefaultSeries },
            { &freqSlider,         kDefaultFreq },
            { &shapeSlider,        kDefaultShape },
            { &jitterSlider,       kDefaultJitter },
            { &styleSlider,        kDefaultStyle },
            { &feedbackSlider,     kDefaultFeedback },
            { &modSlider,          kDefaultMod },
            { &inputSlider,        kDefaultInput },
            { &outputSlider,       kDefaultOutput },
            { &tiltSlider,         kDefaultTilt },
            { &panSlider,          DisperserAudioProcessor::kPanDefault },
            { &mixSlider,          kDefaultMix },
            { &limThresholdSlider, kDefaultLimThreshold },
        });
}

//==============================================================================

TR::SimpleMainPanelSpec DisperserAudioProcessorEditor::buildMainPanelSpec()
{
    TR::SimpleMainPanelSpec spec;
    spec.title   = "DISP-TR";
    spec.version = juce::String ("v") + InfoContent::version;
    spec.ioExpanded   = ioSectionExpanded_;
    spec.toggleBarArea = cachedToggleBarArea_;

    // Main rows (8 sliders visible in collapsed)
    {
        const juce::String* full[8]  = { &cachedFreqTextHz, &cachedModTextFull, &cachedFeedbackTextFull,
                                          &cachedAmountTextFull, &cachedSeriesTextFull, &cachedShapeTextFull,
                                          &cachedJitterTextFull, &cachedStyleTextFull };
        const juce::String* shrt[8]  = { &cachedFreqTextShort, &cachedModTextShort, &cachedFeedbackTextShort,
                                          &cachedAmountTextShort, &cachedSeriesTextShort, &cachedShapeTextShort,
                                          &cachedJitterTextShort, &cachedStyleTextShort };
        const juce::String* intOnly[8] = { &cachedFreqIntOnly, &cachedModIntOnly, &cachedFeedbackIntOnly,
                                            &cachedAmountIntOnly, &cachedSeriesIntOnly, &cachedShapeIntOnly,
                                            &cachedJitterIntOnly, &cachedStyleTextShort };
        for (int i = 0; i < 8; ++i)
            TR::addSimpleMainPanelRow (spec, false, full[i], shrt[i], intOnly[i],
                                       cachedValueAreas_[(size_t) i]);
    }

    // Expanded-only rows
    {
        auto addIfVisible = [&](const juce::Slider& s, const juce::Rectangle<int>& area,
                                 const juce::String* full, const juce::String* shrt,
                                 const juce::String* intOnly = nullptr)
        {
            TR::addSimpleMainPanelRow (spec, true, full, shrt, intOnly, area, s.isVisible());
        };
        addIfVisible (inputSlider,  cachedValueAreas_[8],  &cachedInputTextFull,  &cachedInputTextShort,  &cachedInputIntOnly);
        addIfVisible (outputSlider, cachedValueAreas_[9],  &cachedOutputTextFull, &cachedOutputTextShort, &cachedOutputIntOnly);
        addIfVisible (tiltSlider,   cachedValueAreas_[10], &cachedTiltTextFull,   &cachedTiltTextShort,   &cachedTiltIntOnly);
        TR::addSimpleMainPanelRow (spec, true, &cachedFilterTextFull, &cachedFilterTextShort, nullptr,
                                   cachedFilterValueArea_, filterBar_.isVisible());
        addIfVisible (panSlider,    cachedPanValueArea_,        &cachedPanTextFull,    &cachedPanTextShort);
        addIfVisible (mixSlider,    cachedValueAreas_[11],      &cachedMixTextFull,    &cachedMixTextShort,   &cachedMixIntOnly);
        addIfVisible (limThresholdSlider, cachedLimThresholdValueArea_, &cachedLimThresholdTextFull, &cachedLimThresholdTextShort, &cachedLimThresholdIntOnly);
    }

    // Combo labels
    spec.combosVisible = modeInCombo.isVisible();
    spec.comboLabels = {
        { &modeInCombo, "MODE IN", "IN" },
        { &modeOutCombo, "MODE OUT", "OUT" },
        { &sumBusCombo, "SUM BUS", "SUM" },
        { &limModeCombo, "LIMIT", "LIM" },
        { &mixModeCombo, "MIX", "MIX" },
        { &filterPosCombo, "F / T", "F/T" },
        { &invPolCombo, "INV POL", "POL" },
        { &invStrCombo, "INV STR", "STR" }
    };

    // Toggles
    {
        const int W = getWidth();
        TR::addSimpleMainPanelToggle (spec, false, chaosFilterButton, getChaosFilterLabelArea(), "CHSF", "CHSF",
                                      TR::makeSimpleMainPanelRightBoundBefore (chaosDelayButton, W));
        TR::addSimpleMainPanelToggle (spec, false, chaosDelayButton,
                                      TR::makeSimpleToggleLabelArea (chaosDelayButton, TR::makeSimpleMainPanelRightBound (W), "CHSD", "CHSD"),
                                      "CHSD", "CHSD", TR::makeSimpleMainPanelRightBound (W));
        TR::addSimpleMainPanelToggle (spec, false, sidechainButton, getSidechainLabelArea(), "SIDECHAIN", "SC",
                                      TR::makeSimpleMainPanelRightBound (W));
    }

    // Collapsed toggles
    if (! ioSectionExpanded_)
    {
        const int W = getWidth();
        TR::addSimpleMainPanelToggle (spec, true, altButton, getAltLabelArea(), "ALT", "ALT",
                                      TR::makeSimpleMainPanelRightBoundBefore (midiButton, W));
        TR::addSimpleMainPanelToggle (spec, true, midiButton, getMidiLabelArea(), "MIDI", "MIDI",
                                      TR::makeSimpleMainPanelRightBound (W));
    }

    // Info gear
    if (cachedInfoGearPath.isEmpty())
        updateInfoIconCache();
    TR::setSimpleMainPanelInfoGear (spec, cachedInfoGearPath, cachedInfoGearHole);

    return spec;
}

void DisperserAudioProcessorEditor::paint (juce::Graphics& g)
{
    TR::SimpleMainPanelRenderer::paint (g, buildMainPanelSpec(), activeScheme, kBoldFont40(), getWidth());
}

void DisperserAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
    juce::ignoreUnused (g);
}

void DisperserAudioProcessorEditor::updateInfoIconCache()
{
    const auto iconArea = getInfoIconArea();
    const auto iconF = iconArea.toFloat();
    const auto center = iconF.getCentre();
    const float toothTipR = (float) iconArea.getWidth() * 0.47f;
    const float toothRootR = toothTipR * 0.78f;
    const float holeR = toothTipR * 0.40f;
    constexpr int teeth = 8;

    cachedInfoGearPath.clear();
    for (int i = 0; i < teeth * 2; ++i)
    {
        const float a = -juce::MathConstants<float>::halfPi
                      + (juce::MathConstants<float>::pi * (float) i / (float) teeth);
        const float r = (i % 2 == 0) ? toothTipR : toothRootR;
        const float x = center.x + std::cos (a) * r;
        const float y = center.y + std::sin (a) * r;

        if (i == 0)
            cachedInfoGearPath.startNewSubPath (x, y);
        else
            cachedInfoGearPath.lineTo (x, y);
    }
    cachedInfoGearPath.closeSubPath();
    cachedInfoGearHole = { center.x - holeR, center.y - holeR, holeR * 2.0f, holeR * 2.0f };
}

void DisperserAudioProcessorEditor::resized()
{
    refreshLegendTextCache();

    if (! suppressSizePersistence)
    {
        if (juce::ModifierKeys::getCurrentModifiers().isAnyMouseButtonDown()
            || juce::Desktop::getInstance().getMainMouseSource().isDragging())
        {
            lastUserInteractionMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
        }
    }

    const int W = getWidth();
    const int H = getHeight();

    if (! suppressSizePersistence)
    {
        const uint32_t last = lastUserInteractionMs.load (std::memory_order_relaxed);
        const uint32_t now = juce::Time::getMillisecondCounter();
        const bool userRecent = (now - last) <= (uint32_t) kUserInteractionPersistWindowMs;
        if ((W != lastPersistedEditorW || H != lastPersistedEditorH) && userRecent)
        {
            audioProcessor.setUiEditorSize (W, H);
            lastPersistedEditorW = W;
            lastPersistedEditorH = H;
        }
    }

    const auto horizontalLayout = buildHorizontalLayout (W, getTargetValueColumnWidth());
    const auto verticalLayout = buildVerticalLayout (H, kLayoutVerticalBiasPx, ioSectionExpanded_);

    // Position sliders â€” toggle bar always at top, swaps between main and IO bars

    if (ioSectionExpanded_)
    {
        // Expanded: [toggle bar] â†’ INPUT, OUTPUT, TILT, FILTER, PAN, MIX, LIM, MODE combos, CHSF | CHSD; main params hidden
        TR::placeSimpleRowComponent (inputSlider, horizontalLayout, verticalLayout, 0);
        TR::placeSimpleRowComponent (outputSlider, horizontalLayout, verticalLayout, 1);
        TR::placeSimpleRowComponent (tiltSlider, horizontalLayout, verticalLayout, 2);
        TR::placeSimpleRowComponent (filterBar_, horizontalLayout, verticalLayout, 3);
        TR::placeSimpleRowComponent (panSlider, horizontalLayout, verticalLayout, 4);
        TR::placeSimpleRowComponent (mixSlider, horizontalLayout, verticalLayout, 5);
        TR::placeSimpleRowComponent (dualMixBar_, horizontalLayout, verticalLayout, 5);
        TR::placeSimpleRowComponent (limThresholdSlider, horizontalLayout, verticalLayout, 6);

        TR::setSimpleComponentVisible (inputSlider, true);
        TR::setSimpleComponentVisible (outputSlider, true);
        TR::setSimpleComponentVisible (tiltSlider, true);
        TR::setSimpleComponentVisible (filterBar_, true);
        TR::setSimpleComponentVisible (panSlider, true);
        TR::setSimpleComponentVisible (limThresholdSlider, true);

        {
            const int blockTopLimit = limThresholdSlider.getBottom() + verticalLayout.gapY;
            const int blockBottomLimit = verticalLayout.chaosRowY - verticalLayout.gapY;
            TR::placeSimpleIoComboGrid (horizontalLayout, verticalLayout, blockTopLimit, blockBottomLimit,
                                        modeInCombo, modeOutCombo, sumBusCombo, limModeCombo,
                                        mixModeCombo, filterPosCombo, invPolCombo, invStrCombo);
        }

        // Chaos buttons at chaosRowY
        const int chaosY = verticalLayout.chaosRowY;
        TR::placeSimpleWideTogglePair (chaosFilterButton, chaosDelayButton, horizontalLayout, verticalLayout, chaosY);
        TR::placeSimpleDisplayLabel (chaosFilterDisplay, getChaosFilterLabelArea());
        TR::placeSimpleDisplayLabel (chaosDelayDisplay, getChaosDelayLabelArea());
        TR::placeSimpleToggleAt (sidechainButton, horizontalLayout, verticalLayout, false, verticalLayout.sidechainRowY);
        TR::placeSimpleDisplayLabel (sidechainDisplay, getSidechainLabelArea());

        TR::setSimpleComponentVisible (modeInCombo, true);
        TR::setSimpleComponentVisible (modeOutCombo, true);
        TR::setSimpleComponentVisible (sumBusCombo, true);
        TR::setSimpleComponentVisible (limModeCombo, true);
        TR::setSimpleComponentVisible (invPolCombo, true);
        TR::setSimpleComponentVisible (invStrCombo, true);
        TR::setSimpleComponentVisible (mixModeCombo, true);
        TR::setSimpleComponentVisible (filterPosCombo, true);
        {
            const bool isSendMode = mixModeCombo.getSelectedId() == 2;
            TR::setSimpleComponentVisible (mixSlider, ! isSendMode);
            TR::setSimpleComponentVisible (dualMixBar_, isSendMode);
        }
        TR::setSimpleComponentVisible (chaosFilterButton, true);
        TR::setSimpleComponentVisible (chaosFilterDisplay, true);
        TR::setSimpleComponentVisible (chaosDelayButton, true);
        TR::setSimpleComponentVisible (chaosDelayDisplay, true);
        TR::setSimpleComponentVisible (sidechainButton, true);
        TR::setSimpleComponentVisible (sidechainDisplay, true);

        TR::setSimpleComponentVisible (altButton, false);
        TR::setSimpleComponentVisible (midiButton, false);
        TR::setSimpleComponentVisible (midiChannelDisplay, false);

        TR::hideSimpleComponent (freqSlider);
        TR::hideSimpleComponent (modSlider);
        TR::hideSimpleComponent (feedbackSlider);
        TR::hideSimpleComponent (amountSlider);
        TR::hideSimpleComponent (seriesSlider);
        TR::hideSimpleComponent (shapeSlider);
        TR::hideSimpleComponent (jitterSlider);
        TR::hideSimpleComponent (styleSlider);

        TR::setSimpleComponentVisible (freqSlider, false);
        TR::setSimpleComponentVisible (modSlider, false);
        TR::setSimpleComponentVisible (feedbackSlider, false);
        TR::setSimpleComponentVisible (amountSlider, false);
        TR::setSimpleComponentVisible (seriesSlider, false);
        TR::setSimpleComponentVisible (shapeSlider, false);
        TR::setSimpleComponentVisible (jitterSlider, false);
        TR::setSimpleComponentVisible (styleSlider, false);
    }
    else
    {
        // Collapsed: [toggle bar] â†’ main params; IO + filter + chaos hidden
        TR::placeSimpleRowComponent (freqSlider, horizontalLayout, verticalLayout, 0);
        TR::placeSimpleRowComponent (modSlider, horizontalLayout, verticalLayout, 1);
        TR::placeSimpleRowComponent (feedbackSlider, horizontalLayout, verticalLayout, 2);
        TR::placeSimpleRowComponent (amountSlider, horizontalLayout, verticalLayout, 3);
        TR::placeSimpleRowComponent (seriesSlider, horizontalLayout, verticalLayout, 4);
        TR::placeSimpleRowComponent (shapeSlider, horizontalLayout, verticalLayout, 5);
        TR::placeSimpleRowComponent (jitterSlider, horizontalLayout, verticalLayout, 6);
        TR::placeSimpleRowComponent (styleSlider, horizontalLayout, verticalLayout, 7);

        TR::setSimpleComponentVisible (freqSlider, true);
        TR::setSimpleComponentVisible (modSlider, true);
        TR::setSimpleComponentVisible (feedbackSlider, true);
        TR::setSimpleComponentVisible (amountSlider, true);
        TR::setSimpleComponentVisible (seriesSlider, true);
        TR::setSimpleComponentVisible (shapeSlider, true);
        TR::setSimpleComponentVisible (jitterSlider, true);
        TR::setSimpleComponentVisible (styleSlider, true);

        TR::hideSimpleComponent (inputSlider);
        TR::hideSimpleComponent (outputSlider);
        TR::hideSimpleComponent (tiltSlider);
        TR::hideSimpleComponent (filterBar_);
        TR::hideSimpleComponent (panSlider);
        TR::hideSimpleComponent (mixSlider);
        TR::hideSimpleComponent (dualMixBar_);
        TR::hideSimpleComponent (limThresholdSlider);

        TR::setSimpleComponentVisible (inputSlider, false);
        TR::setSimpleComponentVisible (outputSlider, false);
        TR::setSimpleComponentVisible (tiltSlider, false);
        TR::setSimpleComponentVisible (filterBar_, false);
        TR::setSimpleComponentVisible (panSlider, false);
        TR::setSimpleComponentVisible (mixSlider, false);
        TR::setSimpleComponentVisible (dualMixBar_, false);
        TR::setSimpleComponentVisible (limThresholdSlider, false);

        TR::setSimpleComponentVisible (chaosFilterButton, false);
        TR::setSimpleComponentVisible (chaosFilterDisplay, false);
        TR::setSimpleComponentVisible (chaosDelayButton, false);
        TR::setSimpleComponentVisible (chaosDelayDisplay, false);
        TR::setSimpleComponentVisible (sidechainButton, false);
        TR::setSimpleComponentVisible (sidechainDisplay, false);
        TR::setSimpleComponentVisible (modeInCombo, false);
        TR::setSimpleComponentVisible (modeOutCombo, false);
        TR::setSimpleComponentVisible (sumBusCombo, false);
        TR::setSimpleComponentVisible (limModeCombo, false);
        TR::setSimpleComponentVisible (invPolCombo, false);
        TR::setSimpleComponentVisible (invStrCombo, false);
        TR::setSimpleComponentVisible (mixModeCombo, false);
        TR::setSimpleComponentVisible (filterPosCombo, false);

        TR::setSimpleComponentVisible (altButton, true);
        TR::setSimpleComponentVisible (midiButton, true);
        TR::setSimpleComponentVisible (midiChannelDisplay, true);
    }

    const int buttonAreaX = horizontalLayout.leftX;
    const int buttonAreaW = horizontalLayout.contentW;
    const int toggleHitW = TR::makeSimpleToggleHitWidth (verticalLayout);

    const int valueStartX = TR::simpleRightColumnX (horizontalLayout);
const int altAnchorX = horizontalLayout.leftX;
    const int midiAnchorX = valueStartX;

    int altBlockX = altAnchorX;
    int midiBlockX = midiAnchorX;

    const int midiMinX = juce::jmax (midiAnchorX, altBlockX + toggleHitW + kMinToggleBlocksGapPx);
    const int midiMaxX = buttonAreaX + buttonAreaW - toggleHitW;
    if (midiMinX <= midiMaxX)
        midiBlockX = juce::jlimit (midiMinX, midiMaxX, midiBlockX);
    else
        midiBlockX = midiMaxX;

    altButton.setBounds (altBlockX, verticalLayout.btnY, toggleHitW, verticalLayout.box);
    midiButton.setBounds (midiBlockX, verticalLayout.btnY, toggleHitW, verticalLayout.box);

    // Position invisible tooltip overlay on the MIDI label area
    midiChannelDisplay.setBounds (getMidiLabelArea());

    if (resizerCorner != nullptr)
        resizerCorner->setBounds (W - kResizerCornerPx, H - kResizerCornerPx, kResizerCornerPx, kResizerCornerPx);

    promptOverlay.setBounds (getLocalBounds());
    if (promptOverlayActive)
        promptOverlay.toFront (false);

    updateCachedLayout();

    updateInfoIconCache();
    crtEffect.setResolution (static_cast<float> (W), static_cast<float> (H));
}







