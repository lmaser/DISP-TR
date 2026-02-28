#pragma once

#include <cstdint>
#include <atomic>
#include <JuceHeader.h>
#include "PluginProcessor.h"

class DisperserAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                       private juce::Slider::Listener,
                                       private juce::AudioProcessorValueTreeState::Listener,
                                       private juce::Timer
{
public:
    explicit DisperserAudioProcessorEditor (DisperserAudioProcessor&);
    ~DisperserAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void moved() override;
    void updateLegendVisibility();

public:
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

    struct DISPXScheme
    {
        juce::Colour bg;
        juce::Colour fg;
        juce::Colour outline;
        juce::Colour text;
        juce::Colour fxGradientStart;
        juce::Colour fxGradientEnd;
    };

    std::array<DISPXScheme, 4> schemes;
    int currentSchemeIndex = 0;

    class MinimalLNF : public juce::LookAndFeel_V4
    {
    public:
           void setScheme (const DISPXScheme& s)
    {
        scheme = s;

        setColour (juce::TooltipWindow::backgroundColourId, scheme.bg);
        setColour (juce::TooltipWindow::textColourId,       scheme.text);
        setColour (juce::TooltipWindow::outlineColourId,    scheme.outline);

        setColour (juce::BubbleComponent::backgroundColourId, scheme.bg);
        setColour (juce::BubbleComponent::outlineColourId,    scheme.outline);

        setColour (juce::AlertWindow::backgroundColourId, scheme.bg);
        setColour (juce::AlertWindow::textColourId,       scheme.text);
        setColour (juce::AlertWindow::outlineColourId,    scheme.outline);

        setColour (juce::TextButton::buttonColourId,   scheme.bg);
        setColour (juce::TextButton::buttonOnColourId, scheme.fg);
        setColour (juce::TextButton::textColourOffId,  scheme.text);
        setColour (juce::TextButton::textColourOnId,   scheme.bg);

        trailingTextGradient = { scheme.fxGradientStart, scheme.fxGradientEnd };
    }

        const std::array<juce::Colour, 2>& getTrailingTextGradient() const noexcept
        {
            return trailingTextGradient;
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

    private:
        DISPXScheme scheme {
            juce::Colours::black,
            juce::Colours::white,
            juce::Colours::white,
            juce::Colours::white,
            juce::Colours::white,
            juce::Colours::black
        };
        std::array<juce::Colour, 2> trailingTextGradient { juce::Colours::white, juce::Colours::black };
    };

    class PromptOverlay : public juce::Component
    {
    public:
        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black.withAlpha (0.5f));
        }
    };

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

    // Friend: allow helper in PluginEditor.cpp to embed prompts in the overlay.
    friend void embedAlertWindowInOverlay (DisperserAudioProcessorEditor* editor,
                                           juce::AlertWindow* aw,
                                           bool bringTooltip);

    juce::Rectangle<int> getValueAreaFor (const juce::Rectangle<int>& barBounds) const;
    juce::Slider* getSliderForValueAreaPoint (juce::Point<int> p);
    juce::Rectangle<int> getRvsLabelArea() const;
    juce::Rectangle<int> getInvLabelArea() const;
    juce::Rectangle<int> getInfoIconArea() const;
    void updateInfoIconCache();
    bool refreshLegendTextCache();
    juce::Rectangle<int> getRowRepaintBounds (const juce::Slider& s) const;
    void applyActivePalette();

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

    static constexpr double kDefaultAmount = (double) DisperserAudioProcessor::kAmountDefault;
    static constexpr double kDefaultSeries = (double) DisperserAudioProcessor::kSeriesDefault;
    static constexpr double kDefaultFreq   = (double) DisperserAudioProcessor::kFreqDefault;
    static constexpr double kDefaultShape  = (double) DisperserAudioProcessor::kShapeDefault;

    static constexpr int kMinW = 360;
    static constexpr int kMinH = 360;
    static constexpr int kMaxW = 800;
    static constexpr int kMaxH = 600;

    static constexpr int kLayoutVerticalBiasPx = 10;

    static constexpr double kHzSwitchHz = 999.5;

    int labelVisibilityMode = 0;
    bool promptOverlayActive = false;
    bool suppressSizePersistence = false;
    int lastPersistedEditorW = -1;
    int lastPersistedEditorH = -1;
    std::atomic<uint32_t> lastUserInteractionMs { 0 };
    static constexpr uint32_t kUserInteractionPersistWindowMs = 5000;
    bool fxTailEnabled = true;
    bool useCustomPalette = false;
    std::array<juce::Colour, 4> defaultPalette {
        juce::Colours::white,
        juce::Colours::black,
        juce::Colours::white,
        juce::Colours::black
    };
    std::array<juce::Colour, 4> customPalette {
        juce::Colours::white,
        juce::Colours::black,
        juce::Colours::white,
        juce::Colours::black
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisperserAudioProcessorEditor)
};