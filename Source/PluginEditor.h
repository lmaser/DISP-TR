#pragma once

#include <cstdint>
#include <atomic>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "CrtEffect.h"
#include "TRSharedUI.h"

class DisperserAudioProcessorEditor  : public juce::AudioProcessorEditor,
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

    void openNumericEntryPopupForSlider (juce::Slider& s);
    void openInfoPopup();
    void openGraphicsPopup();
    void openMidiChannelPrompt();
    void setPromptOverlayActive (bool shouldBeActive);

    DisperserAudioProcessor& audioProcessor;

    class BarSlider : public juce::Slider
    {
    public:
        using juce::Slider::Slider;

        void setOwner (DisperserAudioProcessorEditor* o) { owner = o; }
        void setAllowNumericPopup (bool allow) { allowNumericPopup = allow; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && allowNumericPopup)
            {
                if (owner != nullptr)
                    owner->openNumericEntryPopupForSlider (*this);
                return;
            }

            juce::Slider::mouseDown (e);
        }

        juce::String getTextFromValue (double v) override
        {
            if (owner != nullptr && (this == &owner->shapeSlider || this == &owner->feedbackSlider || this == &owner->mixSlider))
            {
                double percent = v * 100.0;
                juce::String t (percent, 4);
                if (t.containsChar ('.'))
                {
                    while (t.endsWithChar ('0'))
                        t = t.dropLastCharacters (1);
                    if (t.endsWithChar ('.'))
                        t = t.dropLastCharacters (1);
                }
                return t;
            }

            if (owner != nullptr && this == &owner->modSlider)
            {
                juce::String t (v, 4);
                if (t.containsChar ('.'))
                {
                    while (t.endsWithChar ('0'))
                        t = t.dropLastCharacters (1);
                    if (t.endsWithChar ('.'))
                        t = t.dropLastCharacters (1);
                }
                return t;
            }

            if (owner != nullptr && this == &owner->freqSlider)
            {
                const double rounded3 = std::round (v * 1000.0) / 1000.0;
                return juce::String (rounded3, 3);
            }

            // For input/output gain (dB)
            if (owner != nullptr && (this == &owner->inputSlider || this == &owner->outputSlider))
            {
                const double rounded1 = std::round (v * 10.0) / 10.0;
                return juce::String (rounded1, 1);
            }

            juce::String t = juce::Slider::getTextFromValue (v);
            int dot = t.indexOfChar ('.');
            if (dot >= 0)
                t = t.substring (0, dot + 1 + 4);
            return t;
        }

    private:
        DisperserAudioProcessorEditor* owner = nullptr;
        bool allowNumericPopup = true;
    };

    BarSlider amountSlider;
    BarSlider seriesSlider;
    BarSlider freqSlider;
    BarSlider shapeSlider;
    BarSlider styleSlider;
    BarSlider feedbackSlider;
    BarSlider modSlider;
    BarSlider inputSlider;
    BarSlider outputSlider;
    BarSlider mixSlider;

    using DISPScheme = TR::TRScheme;

    // ── Filter bar (dual HP/LP marker component) ──
    class FilterBarComponent : public juce::Component,
                               public juce::SettableTooltipClient
    {
    public:
        void setOwner (DisperserAudioProcessorEditor* o) { owner = o; }
        void setScheme (const DISPScheme& s) { scheme = s; repaint(); }

        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;
        void mouseMove (const juce::MouseEvent& e) override;
        void mouseDoubleClick (const juce::MouseEvent& e) override;

        void updateFromProcessor();

        float getHpFreq() const { return hpFreq_; }
        float getLpFreq() const { return lpFreq_; }
        bool  isHpOn()    const { return hpOn_; }
        bool  isLpOn()    const { return lpOn_; }

    private:
        DisperserAudioProcessorEditor* owner = nullptr;
        DISPScheme scheme {};

        float hpFreq_ = 250.0f;
        float lpFreq_ = 2000.0f;
        bool  hpOn_   = false;
        bool  lpOn_   = false;

        enum DragTarget { None, HP, LP };
        DragTarget currentDrag_ = None;

        static constexpr float kMinFreq = 20.0f;
        static constexpr float kMaxFreq = 20000.0f;
        static constexpr float kPad     = 7.0f;
        static constexpr int   kMarkerHitPx = 10;

        juce::Rectangle<float> getInnerArea() const;
        float freqToNormX (float freq) const;
        float normXToFreq (float normX) const;
        float getMarkerScreenX (float freq) const;
        DragTarget hitTestMarker (juce::Point<float> p) const;
        void  setFreqFromMouseX (float mouseX, DragTarget target);
    };

    FilterBarComponent filterBar_;

    juce::ToggleButton invButton;
    juce::ToggleButton midiButton;
    juce::Label midiChannelDisplay;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttachment> amountAttachment;
    std::unique_ptr<SliderAttachment> seriesAttachment;
    std::unique_ptr<SliderAttachment> freqAttachment;
    std::unique_ptr<SliderAttachment> shapeAttachment;
    std::unique_ptr<SliderAttachment> styleAttachment;
    std::unique_ptr<SliderAttachment> feedbackAttachment;
    std::unique_ptr<SliderAttachment> modAttachment;
    std::unique_ptr<SliderAttachment> inputAttachment;
    std::unique_ptr<SliderAttachment> outputAttachment;
    std::unique_ptr<SliderAttachment> mixAttachment;

    std::unique_ptr<ButtonAttachment> invAttachment;
    std::unique_ptr<ButtonAttachment> midiAttachment;

    juce::ComponentBoundsConstrainer resizeConstrainer;
    std::unique_ptr<juce::ResizableCornerComponent> resizerCorner;

    DISPScheme activeScheme;

    struct HorizontalLayoutMetrics
    {
        int barW = 0;
        int valuePad = 0;
        int valueW = 0;
        int contentW = 0;
        int leftX = 0;
    };

    struct VerticalLayoutMetrics
    {
        int rhythm = 0;
        int titleH = 0;
        int titleAreaH = 0;
        int titleTopPad = 0;
        int topMargin = 0;
        int betweenSlidersAndButtons = 0;
        int bottomMargin = 0;
        int box = 0;
        int btnY = 0;
        int availableForSliders = 0;
        int barH = 0;
        int gapY = 0;
        int topY = 0;
        int toggleBarH = 0;
        int toggleBarY = 0;
    };

    static HorizontalLayoutMetrics buildHorizontalLayout (int editorW, int valueColW);
    static VerticalLayoutMetrics buildVerticalLayout (int editorH, int biasY, bool ioExpanded);
    void updateCachedLayout();

    class MinimalLNF : public juce::LookAndFeel_V4
    {
    public:
        void setScheme (const DISPScheme& s)
        {
            scheme = s;
            TR::applySchemeToLookAndFeel (*this, scheme);
        }

        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                               float sliderPos, float minSliderPos, float maxSliderPos,
                               const juce::Slider::SliderStyle style, juce::Slider& slider) override;

        void drawTickBox (juce::Graphics& g, juce::Component&,
                          float x, float y, float w, float h,
                          bool ticked, bool isEnabled,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;

        void drawButtonBackground (juce::Graphics& g,
                       juce::Button& button,
                       const juce::Colour& backgroundColour,
                       bool shouldDrawButtonAsHighlighted,
                       bool shouldDrawButtonAsDown) override;

        void drawAlertBox (juce::Graphics& g,
                   juce::AlertWindow& alert,
                   const juce::Rectangle<int>& textArea,
                   juce::TextLayout& textLayout) override;

        void drawBubble (juce::Graphics&,
                 juce::BubbleComponent&,
                 const juce::Point<float>& tip,
                 const juce::Rectangle<float>& body) override;

        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
        juce::Font getAlertWindowMessageFont() override;
        juce::Font getLabelFont (juce::Label& label) override;
        juce::Font getSliderPopupFont (juce::Slider&) override;
        juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
                               juce::Point<int> screenPos,
                               juce::Rectangle<int> parentArea) override;
        void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

        void drawScrollbar (juce::Graphics&, juce::ScrollBar&,
                            int x, int y, int width, int height,
                            bool isScrollbarVertical,
                            int thumbStartPosition, int thumbSize,
                            bool isMouseOver, bool isMouseDown) override;

        int getMinimumScrollbarThumbSize (juce::ScrollBar&) override { return 16; }
        int getScrollbarButtonSize (juce::ScrollBar&) override      { return 0; }

    private:
        DISPScheme scheme {
            juce::Colours::black,
            juce::Colours::white,
            juce::Colours::white,
            juce::Colours::white
        };
    };

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

    juce::String getStyleText() const;
    juce::String getStyleTextShort() const;

    juce::String getFeedbackText() const;
    juce::String getFeedbackTextShort() const;

    juce::String getModText() const;
    juce::String getModTextShort() const;

    juce::String getMixText() const;
    juce::String getMixTextShort() const;

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
    void applyLabelTextColour (juce::Label& label, juce::Colour colour);

    template <typename T>
    friend void TR::embedAlertWindowInOverlay (T*, juce::AlertWindow*, bool);

    juce::Rectangle<int> getValueAreaFor (const juce::Rectangle<int>& barBounds) const;
    juce::Slider* getSliderForValueAreaPoint (juce::Point<int> p);
    juce::Rectangle<int> getInvLabelArea() const;
    juce::Rectangle<int> getMidiLabelArea() const;
    juce::Rectangle<int> getInfoIconArea() const;
    void updateInfoIconCache();
    bool refreshLegendTextCache();
    juce::Rectangle<int> getRowRepaintBounds (const juce::Slider& s) const;
    void applyActivePalette();
    void applyCrtState (bool enabled);

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
    juce::String cachedInputTextFull;
    juce::String cachedInputTextShort;
    juce::String cachedInputIntOnly;
    juce::String cachedOutputTextFull;
    juce::String cachedOutputTextShort;
    juce::String cachedOutputIntOnly;
    juce::String cachedFilterTextFull;
    juce::String cachedFilterTextShort;
    mutable std::uint64_t cachedValueColumnWidthKey = 0;
    mutable int cachedValueColumnWidth = 90;

    HorizontalLayoutMetrics cachedHLayout_;
    VerticalLayoutMetrics cachedVLayout_;
    std::array<juce::Rectangle<int>, 10> cachedValueAreas_;
    juce::Rectangle<int> cachedFilterValueArea_;

    static constexpr double kDefaultAmount = (double) DisperserAudioProcessor::kAmountDefault;
    static constexpr double kDefaultSeries = (double) DisperserAudioProcessor::kSeriesDefault;
    static constexpr double kDefaultFreq   = (double) DisperserAudioProcessor::kFreqDefault;
    static constexpr double kDefaultShape    = (double) DisperserAudioProcessor::kShapeDefault;
    static constexpr double kDefaultFeedback = (double) DisperserAudioProcessor::kFeedbackDefault;
    static constexpr double kDefaultMod      = (double) DisperserAudioProcessor::kModDefault;
    static constexpr double kDefaultMix      = (double) DisperserAudioProcessor::kMixDefault;
    static constexpr double kDefaultStyle    = (double) DisperserAudioProcessor::kStyleDefault;
    static constexpr double kDefaultInput    = (double) DisperserAudioProcessor::kInputDefault;
    static constexpr double kDefaultOutput   = (double) DisperserAudioProcessor::kOutputDefault;

    static constexpr int kMinW = 360;
    static constexpr int kMinH = 540;
    static constexpr int kMaxW = 800;
    static constexpr int kMaxH = 540;

    static constexpr int kLayoutVerticalBiasPx = 10;

    static constexpr double kHzSwitchHz = 999.5;

    bool promptOverlayActive = false;
    bool suppressSizePersistence = false;
    int lastPersistedEditorW = -1;
    int lastPersistedEditorH = -1;
    std::atomic<uint32_t> lastUserInteractionMs { 0 };
    static constexpr uint32_t kUserInteractionPersistWindowMs = 5000;
    bool crtEnabled = false;
    bool useCustomPalette = false;

    // CRT post-process effect
    CrtEffect crtEffect;
    float     crtTime = 0.0f;

    // IO collapsible section state
    juce::Rectangle<int> cachedToggleBarArea_;
    bool ioSectionExpanded_ = false;

    std::array<juce::Colour, 2> defaultPalette {
        juce::Colours::white,
        juce::Colours::black
    };
    std::array<juce::Colour, 2> customPalette {
        juce::Colours::white,
        juce::Colours::black
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisperserAudioProcessorEditor)
};