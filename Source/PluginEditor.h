#pragma once

#include <cstdint>
#include <atomic>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "CrtEffect.h"
#include "../../TR-Shared/SimpleUI/TRSharedUI.h"

class DisperserAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                       public juce::SettableTooltipClient,
                                       private juce::Slider::Listener,
                                       private juce::AudioProcessorValueTreeState::Listener,
                                       private juce::Timer
{
public:
    explicit DisperserAudioProcessorEditor (DisperserAudioProcessor&);
    ~DisperserAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void moved() override;
    void parentHierarchyChanged() override;

private:
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    void openNumericEntryPopupForSlider (juce::Slider& s);
    void openInfoPopup();
    void openGraphicsPopup();
    void openMidiChannelPrompt();
    void openChaosConfigPrompt (const char* amtParamId, const char* spdParamId, const juce::String& title);
    void openChaosFilterPrompt();
    void openChaosDelayPrompt();
    void openSidechainPrompt();
    void openMixSendPrompt();
    void setPromptOverlayActive (bool shouldBeActive);

    DisperserAudioProcessor& audioProcessor;

    using BarSlider = TR::SimpleBarSliderDecl;
    using MainGuiPromptToggleButton = TR::MainGuiPromptToggleButton;

    BarSlider amountSlider;
    BarSlider seriesSlider;
    BarSlider freqSlider;
    BarSlider shapeSlider;
    BarSlider jitterSlider;
    BarSlider styleSlider;
    BarSlider feedbackSlider;
    BarSlider modSlider;
    BarSlider inputSlider;
    BarSlider outputSlider;
    BarSlider tiltSlider;
    BarSlider panSlider;
    BarSlider mixSlider;
    BarSlider limThresholdSlider;

    using DISPScheme = TR::TRScheme;
    using FilterBarComponent = TR::SimpleFilterBarComponent<DisperserAudioProcessorEditor, DisperserAudioProcessor, DISPScheme>;
    FilterBarComponent filterBar_;
    using DualMixBarComponent = TR::SimpleDualMixBarComponent<DisperserAudioProcessorEditor, DisperserAudioProcessor, DISPScheme>;
    DualMixBarComponent dualMixBar_;

    MainGuiPromptToggleButton altButton;
    MainGuiPromptToggleButton midiButton;
    juce::Label midiChannelDisplay;
    MainGuiPromptToggleButton sidechainButton;
    juce::Label sidechainDisplay;

    // Chaos buttons
    MainGuiPromptToggleButton chaosFilterButton;
    MainGuiPromptToggleButton chaosDelayButton;
    juce::Label chaosFilterDisplay;
    juce::Label chaosDelayDisplay;

    // Mode In / Mode Out / Sum Bus
    juce::ComboBox modeInCombo;
    juce::ComboBox modeOutCombo;
    juce::ComboBox sumBusCombo;
    juce::ComboBox limModeCombo;
    juce::ComboBox invPolCombo;
    juce::ComboBox invStrCombo;
    juce::ComboBox mixModeCombo;
    juce::ComboBox filterPosCombo;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<SliderAttachment> amountAttachment;
    std::unique_ptr<SliderAttachment> seriesAttachment;
    std::unique_ptr<SliderAttachment> freqAttachment;
    std::unique_ptr<SliderAttachment> shapeAttachment;
    std::unique_ptr<SliderAttachment> jitterAttachment;
    std::unique_ptr<SliderAttachment> styleAttachment;
    std::unique_ptr<SliderAttachment> feedbackAttachment;
    std::unique_ptr<SliderAttachment> modAttachment;
    std::unique_ptr<SliderAttachment> inputAttachment;
    std::unique_ptr<SliderAttachment> outputAttachment;
    std::unique_ptr<SliderAttachment> tiltAttachment;
    std::unique_ptr<SliderAttachment> panAttachment;
    std::unique_ptr<SliderAttachment> mixAttachment;
    std::unique_ptr<SliderAttachment> limThresholdAttachment;

    std::unique_ptr<ButtonAttachment> altAttachment;
    std::unique_ptr<ButtonAttachment> midiAttachment;
    std::unique_ptr<ButtonAttachment> sidechainAttachment;
    std::unique_ptr<ButtonAttachment> chaosFilterAttachment;
    std::unique_ptr<ButtonAttachment> chaosDelayAttachment;

    std::unique_ptr<ComboBoxAttachment> modeInAttachment;
    std::unique_ptr<ComboBoxAttachment> modeOutAttachment;
    std::unique_ptr<ComboBoxAttachment> sumBusAttachment;
    std::unique_ptr<ComboBoxAttachment> limModeAttachment;
    std::unique_ptr<ComboBoxAttachment> invPolAttachment;
    std::unique_ptr<ComboBoxAttachment> invStrAttachment;
    std::unique_ptr<ComboBoxAttachment> mixModeAttachment;
    std::unique_ptr<ComboBoxAttachment> filterPosAttachment;

    juce::ComponentBoundsConstrainer resizeConstrainer;
    std::unique_ptr<juce::ResizableCornerComponent> resizerCorner;

    DISPScheme activeScheme;

    using HorizontalLayoutMetrics = TR::SimpleHorizontalLayoutMetrics;

    using VerticalLayoutMetrics = TR::SimpleVerticalLayoutMetrics;

    static HorizontalLayoutMetrics buildHorizontalLayout (int editorW, int valueColW);
    static VerticalLayoutMetrics buildVerticalLayout (int editorH, int biasY, bool ioExpanded);
    void updateCachedLayout();

    using MinimalLNF = TR::SimpleLookAndFeel;

    using PromptOverlay = TR::PromptOverlay;

    MinimalLNF lnf;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    PromptOverlay promptOverlay;

    void setupBar (juce::Slider& s);

    juce::String getAmountText() const;
    juce::String getAmountTextShort() const;

    juce::String getSeriesText() const;
    juce::String getSeriesTextShort() const;

    juce::String getFreqText() const;
	juce::String getFreqTextShort() const;
    juce::String getShapeText() const;
    juce::String getShapeTextShort() const;

    juce::String getJitterText() const;
    juce::String getJitterTextShort() const;

    juce::String getStyleText() const;
    juce::String getStyleTextShort() const;

    juce::String getFeedbackText() const;
    juce::String getFeedbackTextShort() const;

    juce::String getModText() const;
    juce::String getModTextShort() const;

    juce::String getMixText() const;
    juce::String getMixTextShort() const;

    juce::String getTiltText() const;
    juce::String getTiltTextShort() const;

    juce::String getPanText() const;
    juce::String getPanTextShort() const;

    juce::String getInputText() const;
    juce::String getInputTextShort() const;

    juce::String getOutputText() const;
    juce::String getOutputTextShort() const;

    juce::String getFilterText() const;
    juce::String getFilterTextShort() const;

    void openFilterPrompt();

    int getTargetValueColumnWidth() const;

    void sliderValueChanged (juce::Slider* slider) override;
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void timerCallback() override;

    void applyPersistedUiStateFromProcessor (bool applySize, bool applyPaletteAndFx);

public:
    void triggerUiRestore() { applyPersistedUiStateFromProcessor (true, true); }

private:
    void applyLabelTextColour (juce::Label& label, juce::Colour colour);

    friend class TR::SimpleFilterBarComponent<DisperserAudioProcessorEditor, DisperserAudioProcessor, DISPScheme>;
    friend class TR::SimpleDualMixBarComponent<DisperserAudioProcessorEditor, DisperserAudioProcessor, DISPScheme>;
    friend struct TR::PromptHostBridge;

    juce::Rectangle<int> getValueAreaFor (const juce::Rectangle<int>& barBounds) const;
    juce::Slider* getSliderForValueAreaPoint (juce::Point<int> p);
    juce::Rectangle<int> getAltLabelArea() const;
    juce::Rectangle<int> getMidiLabelArea() const;
    juce::Rectangle<int> getSidechainLabelArea() const;
    juce::Rectangle<int> getChaosFilterLabelArea() const;
    juce::Rectangle<int> getChaosDelayLabelArea() const;
    juce::Rectangle<int> getInfoIconArea() const;
    void updateInfoIconCache();
    bool refreshLegendTextCache();
    TR::SimpleMainPanelSpec buildMainPanelSpec();
    juce::Rectangle<int> getRowRepaintBounds (const juce::Slider& s) const;
    void applyActivePalette();
    void applyCrtState (bool enabled);
    void applyIoFxState (bool enabled);
    void updateIoFxMeterSliders();

    juce::Path cachedInfoGearPath;
    juce::Rectangle<float> cachedInfoGearHole;
    juce::String cachedAmountTextFull;
    juce::String cachedAmountTextShort;
    juce::String cachedAmountIntOnly;
    juce::String cachedSeriesTextFull;
    juce::String cachedSeriesTextShort;
    juce::String cachedSeriesIntOnly;
    juce::String cachedFreqTextHz;
    juce::String cachedFreqTextShort;
    juce::String cachedFreqIntOnly;
    juce::String cachedMidiDisplay;
    juce::String cachedShapeTextFull;
    juce::String cachedShapeTextShort;
    juce::String cachedShapeIntOnly;
    juce::String cachedJitterTextFull;
    juce::String cachedJitterTextShort;
    juce::String cachedJitterIntOnly;
    juce::String cachedStyleTextFull;
    juce::String cachedStyleTextShort;
    juce::String cachedFeedbackTextFull;
    juce::String cachedFeedbackTextShort;
    juce::String cachedFeedbackIntOnly;
    juce::String cachedModTextFull;
    juce::String cachedModTextShort;
    juce::String cachedModIntOnly;
    juce::String cachedMixTextFull;
    juce::String cachedMixTextShort;
    juce::String cachedMixIntOnly;
    juce::String cachedLimThresholdTextFull;
    juce::String cachedLimThresholdTextShort;
    juce::String cachedLimThresholdIntOnly;
    juce::String cachedTiltTextFull;
    juce::String cachedTiltTextShort;
    juce::String cachedTiltIntOnly;
    juce::String cachedInputTextFull;
    juce::String cachedInputTextShort;
    juce::String cachedInputIntOnly;
    juce::String cachedOutputTextFull;
    juce::String cachedOutputTextShort;
    juce::String cachedOutputIntOnly;
    juce::String cachedFilterTextFull;
    juce::String cachedFilterTextShort;
    juce::String cachedPanTextFull;
    juce::String cachedPanTextShort;
    mutable std::uint64_t cachedValueColumnWidthKey = 0;
    mutable int cachedValueColumnWidth = 90;

    HorizontalLayoutMetrics cachedHLayout_;
    VerticalLayoutMetrics cachedVLayout_;
    std::array<juce::Rectangle<int>, 12> cachedValueAreas_;
    juce::Rectangle<int> cachedFilterValueArea_;
    juce::Rectangle<int> cachedPanValueArea_;
    juce::Rectangle<int> cachedLimThresholdValueArea_;

    static constexpr double kDefaultLimThreshold = 0.0;
    static constexpr double kDefaultAmount = (double) DisperserAudioProcessor::kAmountDefault;
    static constexpr double kDefaultSeries = (double) DisperserAudioProcessor::kSeriesDefault;
    static constexpr double kDefaultFreq   = (double) DisperserAudioProcessor::kFreqDefault;
    static constexpr double kDefaultShape    = (double) DisperserAudioProcessor::kShapeDefault;
    static constexpr double kDefaultJitter = (double) DisperserAudioProcessor::kJitterDefault;
    static constexpr double kDefaultFeedback = (double) DisperserAudioProcessor::kFeedbackDefault;
    static constexpr double kDefaultMod      = (double) DisperserAudioProcessor::kModDefault;
    static constexpr double kDefaultMix      = (double) DisperserAudioProcessor::kMixDefault;
    static constexpr double kDefaultTilt     = (double) DisperserAudioProcessor::kTiltDefault;
    static constexpr double kDefaultStyle    = (double) DisperserAudioProcessor::kStyleDefault;
    static constexpr double kDefaultInput    = (double) DisperserAudioProcessor::kInputDefault;
    static constexpr double kDefaultOutput   = (double) DisperserAudioProcessor::kOutputDefault;

    static constexpr int kMinW = 360;
    static constexpr int kMinH = 752;
    static constexpr int kMaxW = kMinW + (kMinW / 2);
    static constexpr int kMaxH = kMinH;

    static constexpr int kLayoutVerticalBiasPx = 10;

    bool promptOverlayActive = false;
    bool suppressSizePersistence = false;
    int lastPersistedEditorW = -1;
    int lastPersistedEditorH = -1;
    std::atomic<uint32_t> lastUserInteractionMs { 0 };
    static constexpr uint32_t kUserInteractionPersistWindowMs = 5000;
    bool crtEnabled = false;
    bool ioFxEnabled = true;
    double lastInputSignalMs = -10000.0;
    double lastOutputSignalMs = -10000.0;
    bool useCustomPalette = false;

    // CRT post-process effect
    CrtEffect crtEffect;
    float     crtTime = 0.0f;

    // IO collapsible section state
    juce::Rectangle<int> cachedToggleBarArea_;
    bool ioSectionExpanded_ = false;

    static constexpr int kPaletteColourCount = 4;
    std::array<juce::Colour, kPaletteColourCount> defaultPalette = TR::defaultSimplePalette();
    std::array<juce::Colour, kPaletteColourCount> customPalette = TR::defaultSimpleCustomPalette();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisperserAudioProcessorEditor)
};
