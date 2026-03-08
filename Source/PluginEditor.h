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
    void setPromptOverlayActive (bool shouldBeActive);

    DisperserAudioProcessor& audioProcessor;

    class BarSlider : public juce::Slider
    {
    public:
        using juce::Slider::Slider;

        void setOwner (DisperserAudioProcessorEditor* o) { owner = o; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu())
            {
                if (owner != nullptr)
                    owner->openNumericEntryPopupForSlider (*this);
                return;
            }

            juce::Slider::mouseDown (e);
        }

        juce::String getTextFromValue (double v) override
        {
            if (owner != nullptr && this == &owner->shapeSlider)
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

            if (owner != nullptr && this == &owner->freqSlider)
            {
                const double rounded3 = std::round (v * 1000.0) / 1000.0;
                return juce::String (rounded3, 3);
            }

            juce::String t = juce::Slider::getTextFromValue (v);
            int dot = t.indexOfChar ('.');
            if (dot >= 0)
                t = t.substring (0, dot + 1 + 4);
            return t;
        }

    private:
        DisperserAudioProcessorEditor* owner = nullptr;
    };

    BarSlider amountSlider;
    BarSlider seriesSlider;
    BarSlider freqSlider;
    BarSlider shapeSlider;

    juce::ToggleButton rvsButton;
    juce::ToggleButton invButton;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttachment> amountAttachment;
    std::unique_ptr<SliderAttachment> seriesAttachment;
    std::unique_ptr<SliderAttachment> freqAttachment;
    std::unique_ptr<SliderAttachment> shapeAttachment;

    std::unique_ptr<ButtonAttachment> rvsAttachment;
    std::unique_ptr<ButtonAttachment> invAttachment;

    juce::ComponentBoundsConstrainer resizeConstrainer;
    std::unique_ptr<juce::ResizableCornerComponent> resizerCorner;

    using DISPScheme = TR::TRScheme;

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
    };

    static HorizontalLayoutMetrics buildHorizontalLayout (int editorW, int valueColW);
    static VerticalLayoutMetrics buildVerticalLayout (int editorH, int biasY);
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

    juce::String getShapeText() const;
    juce::String getShapeTextShort() const;
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
    juce::Rectangle<int> getRvsLabelArea() const;
    juce::Rectangle<int> getInvLabelArea() const;
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
    juce::String cachedSeriesTextFull;
    juce::String cachedSeriesTextShort;
    juce::String cachedFreqTextHz;
    juce::String cachedFreqIntOnly;
    juce::String cachedShapeTextFull;
    juce::String cachedShapeTextShort;
    juce::String cachedShapeIntOnly;
    mutable std::uint64_t cachedValueColumnWidthKey = 0;
    mutable int cachedValueColumnWidth = 90;

    HorizontalLayoutMetrics cachedHLayout_;
    VerticalLayoutMetrics cachedVLayout_;
    std::array<juce::Rectangle<int>, 4> cachedValueAreas_;

    static constexpr double kDefaultAmount = (double) DisperserAudioProcessor::kAmountDefault;
    static constexpr double kDefaultSeries = (double) DisperserAudioProcessor::kSeriesDefault;
    static constexpr double kDefaultFreq   = (double) DisperserAudioProcessor::kFreqDefault;
    static constexpr double kDefaultShape  = (double) DisperserAudioProcessor::kShapeDefault;

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