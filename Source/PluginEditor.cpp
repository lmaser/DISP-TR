// PluginEditor.cpp
#include "PluginEditor.h"
#include "InfoContent.h"
#include <functional>

using namespace TR;

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace UiStateKeys
{
    constexpr const char* editorWidth = "uiEditorWidth";
    constexpr const char* editorHeight = "uiEditorHeight";
    constexpr const char* useCustomPalette = "uiUseCustomPalette";
    constexpr const char* crtEnabled = "uiFxTailEnabled";  // string kept for preset compat
    constexpr std::array<const char*, 2> customPalette {
        "uiCustomPalette0",
        "uiCustomPalette1"
    };
}

// ── Timer constants ──
static constexpr int kCrtTimerHz  = 10;
static constexpr int kIdleTimerHz = 4;

// ── Parameter listener IDs (shared by ctor + dtor) ──
static constexpr std::array<const char*, 4> kUiMirrorParamIds {
    DisperserAudioProcessor::kParamUiPalette,
    DisperserAudioProcessor::kParamUiFxTail,
    DisperserAudioProcessor::kParamUiColor0,
    DisperserAudioProcessor::kParamUiColor1
};

static juce::String formatBarFrequencyHzText (double hz)
{
    const double safeHz = juce::jmax (0.0, hz);
    return juce::String (safeHz, 3) + " Hz";
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

static double multiplierToModSlider (double mult)
{
    mult = juce::jlimit (kModMinMult, kModMaxMult, mult);
    if (mult < 1.0)
        return (kModMaxMult - 1.0 / mult) * kModCenter / kModScale;
    return kModCenter + (mult - 1.0) * kModCenter / kModScale;
}

static juce::String formatMidiChannelTooltip (int ch)
{
    return "CHANNEL " + juce::String (ch);
}

//========================== LookAndFeel ==========================

void DisperserAudioProcessorEditor::MinimalLNF::drawLinearSlider (juce::Graphics& g,
                                                                  int x, int y, int width, int height,
                                                                  float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                                                                  const juce::Slider::SliderStyle /*style*/, juce::Slider& /*slider*/)
{
    const juce::Rectangle<float> r ((float) x, (float) y, (float) width, (float) height);

    g.setColour (scheme.outline);
    g.drawRect (r, 4.0f);

    const float pad = 7.0f;
    auto inner = r.reduced (pad);

    g.setColour (scheme.bg);
        g.fillRect (inner);

    const float fillW = juce::jlimit (0.0f, inner.getWidth(), sliderPos - inner.getX());
    auto fill = inner.withWidth (fillW);

    g.setColour (scheme.fg);
    g.fillRect (fill);
}

void DisperserAudioProcessorEditor::MinimalLNF::drawTickBox (juce::Graphics& g, juce::Component& button,
                                                            float x, float y, float w, float h,
                                                            bool ticked, bool /*isEnabled*/,
                                                            bool /*highlighted*/, bool /*down*/)
{
    juce::ignoreUnused (x, y, w, h);

    const auto local = button.getLocalBounds().toFloat().reduced (1.0f);
    const float side = juce::jlimit (14.0f,
                                     juce::jmax (14.0f, local.getHeight() - 2.0f),
                                     std::round (local.getHeight() * 0.65f));

    auto r = juce::Rectangle<float> (local.getX() + 2.0f,
                                     local.getCentreY() - (side * 0.5f),
                                     side,
                                     side).getIntersection (local);

    g.setColour (scheme.outline);
    g.drawRect (r, 4.0f);

    const float innerInset = juce::jlimit (1.0f, side * 0.45f, side * UiMetrics::tickBoxInnerInsetRatio);
    auto inner = r.reduced (innerInset);

    if (ticked)
    {
        g.setColour (scheme.fg);
        g.fillRect (inner);
    }
    else
    {
        g.setColour (scheme.bg);
        g.fillRect (inner);
    }
}

void DisperserAudioProcessorEditor::MinimalLNF::drawScrollbar (juce::Graphics& g,
                                                               juce::ScrollBar&,
                                                               int x, int y, int width, int height,
                                                               bool isScrollbarVertical,
                                                               int thumbStartPosition, int thumbSize,
                                                               bool isMouseOver, bool isMouseDown)
{
    juce::ignoreUnused (x, y, width, height);

    const auto thumbColour = scheme.text.withAlpha (isMouseDown ? 0.7f
                                                     : isMouseOver ? 0.5f
                                                                   : 0.3f);
    constexpr float barThickness = 7.0f;
    constexpr float cornerRadius = 3.5f;

    if (isScrollbarVertical)
    {
        const float bx = (float) (x + width) - barThickness - 1.0f;
        g.setColour (thumbColour);
        g.fillRoundedRectangle (bx, (float) thumbStartPosition,
                                barThickness, (float) thumbSize, cornerRadius);
    }
    else
    {
        const float by = (float) (y + height) - barThickness - 1.0f;
        g.setColour (thumbColour);
        g.fillRoundedRectangle ((float) thumbStartPosition, by,
                                (float) thumbSize, barThickness, cornerRadius);
    }
}

void DisperserAudioProcessorEditor::MinimalLNF::drawButtonBackground (juce::Graphics& g,
                                                                      juce::Button& button,
                                                                      const juce::Colour& backgroundColour,
                                                                      bool shouldDrawButtonAsHighlighted,
                                                                      bool shouldDrawButtonAsDown)
{
    auto r = button.getLocalBounds();

    auto fill = backgroundColour;
    if (shouldDrawButtonAsDown)
        fill = fill.brighter (0.12f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter (0.06f);

    g.setColour (fill);
    g.fillRect (r);

    g.setColour (scheme.outline);
    g.drawRect (r.reduced (1), 3);
}

void DisperserAudioProcessorEditor::MinimalLNF::drawAlertBox (juce::Graphics& g,
                                                              juce::AlertWindow& alert,
                                                              const juce::Rectangle<int>& textArea,
                                                              juce::TextLayout& textLayout)
{
    auto bounds = alert.getLocalBounds();

    g.setColour (scheme.bg);
    g.fillRect (bounds);

    g.setColour (scheme.outline);
    g.drawRect (bounds.reduced (1), 3);

    g.setColour (scheme.text);
    textLayout.draw (g, textArea.toFloat());
}

void DisperserAudioProcessorEditor::MinimalLNF::drawBubble (juce::Graphics& g,
                                                            juce::BubbleComponent&,
                                                            const juce::Point<float>&,
                                                            const juce::Rectangle<float>& body)
{
    drawOverlayPanel (g,
                      body.getSmallestIntegerContainer(),
                      findColour (juce::TooltipWindow::backgroundColourId),
                      findColour (juce::TooltipWindow::outlineColourId));
}

juce::Font DisperserAudioProcessorEditor::MinimalLNF::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    const float h = juce::jlimit (12.0f, 26.0f, buttonHeight * 0.48f);
    return juce::Font (juce::FontOptions (h).withStyle ("Bold"));
}

juce::Font DisperserAudioProcessorEditor::MinimalLNF::getAlertWindowMessageFont()
{
    auto f = juce::LookAndFeel_V4::getAlertWindowMessageFont();
    f.setBold (true);
    return f;
}

juce::Font DisperserAudioProcessorEditor::MinimalLNF::getLabelFont (juce::Label& label)
{
    auto f = label.getFont();
    if (f.getHeight() <= 0.0f)
    {
        const float h = juce::jlimit (12.0f, 40.0f, (float) juce::jmax (12, label.getHeight() - 6));
        f = juce::Font (juce::FontOptions (h).withStyle ("Bold"));
    }
    else
    {
        f.setBold (true);
    }

    return f;
}

juce::Font DisperserAudioProcessorEditor::MinimalLNF::getSliderPopupFont (juce::Slider&)
{
    return makeOverlayDisplayFont();
}

juce::Rectangle<int> DisperserAudioProcessorEditor::MinimalLNF::getTooltipBounds (const juce::String& tipText,
                                                                                   juce::Point<int> screenPos,
                                                                                   juce::Rectangle<int> parentArea)
{
    const auto f = makeOverlayDisplayFont();
    const int h = juce::jmax (UiMetrics::tooltipMinHeight,
                              (int) std::ceil (f.getHeight() * UiMetrics::tooltipHeightScale));

    const int anchorOffsetX = juce::jmax (8, (int) std::round ((double) h * UiMetrics::tooltipAnchorXRatio));
    const int anchorOffsetY = juce::jmax (10, (int) std::round ((double) h * UiMetrics::tooltipAnchorYRatio));
    const int parentMargin = juce::jmax (2, (int) std::round ((double) h * UiMetrics::tooltipParentMarginRatio));
    const int widthPad = juce::jmax (16, (int) std::round (f.getHeight() * UiMetrics::tooltipWidthPadFontRatio));

    const int w = juce::jmax (UiMetrics::tooltipMinWidth, stringWidth (f, tipText) + widthPad);
    auto r = juce::Rectangle<int> (screenPos.x + anchorOffsetX, screenPos.y + anchorOffsetY, w, h);
    return r.constrainedWithin (parentArea.reduced (parentMargin));
}

void DisperserAudioProcessorEditor::MinimalLNF::drawTooltip (juce::Graphics& g,
                                                              const juce::String& text,
                                                              int width,
                                                              int height)
{
    const auto f = makeOverlayDisplayFont();
    const int h = juce::jmax (UiMetrics::tooltipMinHeight,
                              (int) std::ceil (f.getHeight() * UiMetrics::tooltipHeightScale));
    const int textInsetX = juce::jmax (4, (int) std::round ((double) h * UiMetrics::tooltipTextInsetXRatio));
    const int textInsetY = juce::jmax (1, (int) std::round ((double) h * UiMetrics::tooltipTextInsetYRatio));

    drawOverlayPanel (g,
                      { 0, 0, width, height },
                      findColour (juce::TooltipWindow::backgroundColourId),
                      findColour (juce::TooltipWindow::outlineColourId));

    g.setColour (findColour (juce::TooltipWindow::textColourId));
    g.setFont (f);
    g.drawFittedText (text,
                      textInsetX,
                      textInsetY,
                      juce::jmax (1, width - (textInsetX * 2)),
                      juce::jmax (1, height - (textInsetY * 2)),
                      juce::Justification::centred,
                      1);
}

//========================== Editor ==========================

DisperserAudioProcessorEditor::DisperserAudioProcessorEditor (DisperserAudioProcessor& p)
: AudioProcessorEditor (&p), audioProcessor (p)
{
    const std::array<BarSlider*, 7> barSliders { &freqSlider, &modSlider, &feedbackSlider, &amountSlider, &seriesSlider, &shapeSlider, &mixSlider };

    useCustomPalette = audioProcessor.getUiUseCustomPalette();
    crtEnabled = audioProcessor.getUiFxTailEnabled();

    for (int i = 0; i < 2; ++i)
        customPalette[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

    setOpaque (true);
    setBufferedToImage (true);

    applyActivePalette();
    setLookAndFeel (&lnf);
    tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 250);
    tooltipWindow->setLookAndFeel (&lnf);
    tooltipWindow->setAlwaysOnTop (true);
    tooltipWindow->setInterceptsMouseClicks (false, false);

    setResizable (true, true);

    setResizeLimits (kMinW, kMinH, kMaxW, kMaxH);

    resizeConstrainer.setMinimumSize (kMinW, kMinH);
    resizeConstrainer.setMaximumSize (kMaxW, kMaxH);

    resizerCorner = std::make_unique<juce::ResizableCornerComponent> (this, &resizeConstrainer);
    addAndMakeVisible (*resizerCorner);
    resizerCorner->addMouseListener (this, true);

    addAndMakeVisible (promptOverlay);
    promptOverlay.setInterceptsMouseClicks (true, true);
    promptOverlay.setVisible (false);

    const int restoredW = juce::jlimit (kMinW, kMaxW, audioProcessor.getUiEditorWidth());
    const int restoredH = juce::jlimit (kMinH, kMaxH, audioProcessor.getUiEditorHeight());
    suppressSizePersistence = true;
    setSize (restoredW, restoredH);
    suppressSizePersistence = false;
    lastPersistedEditorW = restoredW;
    lastPersistedEditorH = restoredH;

    for (auto* slider : barSliders)
    {
        slider->setOwner (this);
        setupBar (*slider);
        addAndMakeVisible (*slider);
        slider->addListener (this);
    }

    amountSlider.setNumDecimalPlacesToDisplay (0);
    seriesSlider.setNumDecimalPlacesToDisplay (0);
    freqSlider.setNumDecimalPlacesToDisplay (3);
    shapeSlider.setNumDecimalPlacesToDisplay (2);
    feedbackSlider.setNumDecimalPlacesToDisplay (2);
    modSlider.setNumDecimalPlacesToDisplay (2);
    mixSlider.setNumDecimalPlacesToDisplay (2);

    seriesSlider.setRange ((double) DisperserAudioProcessor::kSeriesMin,
                           (double) DisperserAudioProcessor::kSeriesMax,
                           1.0);

    invButton.setButtonText ("");
    midiButton.setButtonText ("");

    addAndMakeVisible (invButton);
    addAndMakeVisible (midiButton);

    // MIDI channel tooltip overlay — invisible label positioned over the MIDI legend.
    // Provides tooltip on hover; clicks forwarded to editor via addMouseListener.
    {
        const int savedChannel = audioProcessor.getMidiChannel();
        midiChannelDisplay.setText ("", juce::dontSendNotification);
        midiChannelDisplay.setInterceptsMouseClicks (true, false);
        midiChannelDisplay.addMouseListener (this, false);
        midiChannelDisplay.setTooltip (formatMidiChannelTooltip (savedChannel));
        midiChannelDisplay.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        midiChannelDisplay.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        midiChannelDisplay.setOpaque (false);
        addAndMakeVisible (midiChannelDisplay);
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
    bindSlider (feedbackAttachment, DisperserAudioProcessor::kParamFeedback, feedbackSlider, kDefaultFeedback);
    bindSlider (modAttachment, DisperserAudioProcessor::kParamMod, modSlider, kDefaultMod);
    bindSlider (mixAttachment, DisperserAudioProcessor::kParamMix, mixSlider, kDefaultMix);

    auto bindButton = [&] (std::unique_ptr<ButtonAttachment>& attachment,
                           const char* paramId,
                           juce::Button& button)
    {
        attachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, paramId, button);
    };

    bindButton (invAttachment, DisperserAudioProcessor::kParamInv, invButton);
    bindButton (midiAttachment, DisperserAudioProcessor::kParamMidi, midiButton);

    for (auto* paramId : kUiMirrorParamIds)
        audioProcessor.apvts.addParameterListener (paramId, this);

    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]()
    {
        if (safeThis == nullptr)
            return;

        safeThis->applyPersistedUiStateFromProcessor (true, true);
    });

    juce::Timer::callAfterDelay (250, [safeThis]()
    {
        if (safeThis == nullptr)
            return;
        safeThis->applyPersistedUiStateFromProcessor (true, true);
    });

    juce::Timer::callAfterDelay (750, [safeThis]()
    {
        if (safeThis == nullptr)
            return;
        safeThis->applyPersistedUiStateFromProcessor (true, true);
    });

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

    const std::array<BarSlider*, 7> barSliders { &freqSlider, &modSlider, &feedbackSlider, &amountSlider, &seriesSlider, &shapeSlider, &mixSlider };
    for (auto* slider : barSliders)
        slider->removeListener (this);

    if (tooltipWindow != nullptr)
        tooltipWindow->setLookAndFeel (nullptr);

    setLookAndFeel (nullptr);
}

void DisperserAudioProcessorEditor::applyActivePalette()
{
    const auto& palette = useCustomPalette ? customPalette : defaultPalette;

    DISPScheme scheme;
    scheme.bg = palette[1];
    scheme.fg = palette[0];
    scheme.outline = palette[0];
    scheme.text = palette[0];

    activeScheme = scheme;
    lnf.setScheme (activeScheme);
}

void DisperserAudioProcessorEditor::applyCrtState (bool enabled)
{
    crtEnabled = enabled;
    crtEffect.setEnabled (crtEnabled);
    setComponentEffect (crtEnabled ? &crtEffect : nullptr);
    stopTimer();
    startTimerHz (crtEnabled ? kCrtTimerHz : kIdleTimerHz);
}

void DisperserAudioProcessorEditor::applyLabelTextColour (juce::Label& label, juce::Colour colour)
{
    label.setColour (juce::Label::textColourId, colour);
}

void DisperserAudioProcessorEditor::sliderValueChanged (juce::Slider* slider)
{
    auto isBarSlider = [&] (const juce::Slider* s)
    {
        return s == &amountSlider || s == &seriesSlider || s == &freqSlider || s == &shapeSlider || s == &feedbackSlider || s == &modSlider || s == &mixSlider;
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
    if (promptOverlayActive == shouldBeActive)
        return;

    promptOverlayActive = shouldBeActive;

    promptOverlay.setBounds (getLocalBounds());
    promptOverlay.setVisible (shouldBeActive);
    if (shouldBeActive)
        promptOverlay.toFront (false);

    const bool enableControls = ! shouldBeActive;
    const std::array<juce::Component*, 9> interactiveControls {
        &amountSlider, &seriesSlider, &freqSlider, &shapeSlider, &feedbackSlider, &modSlider, &mixSlider, &invButton, &midiButton
    };
    for (auto* control : interactiveControls)
        control->setEnabled (enableControls);

    if (resizerCorner != nullptr)
        resizerCorner->setEnabled (enableControls);

    repaint();

    if (promptOverlayActive)
        promptOverlay.toFront (false);

    anchorEditorOwnedPromptWindows (*this, lnf);
}

void DisperserAudioProcessorEditor::moved()
{
    if (promptOverlayActive)
        promptOverlay.toFront (false);

    anchorEditorOwnedPromptWindows (*this, lnf);
}

void DisperserAudioProcessorEditor::parentHierarchyChanged()
{
   #if JUCE_WINDOWS
    if (auto* peer = getPeer())
    {
        if (auto nativeHandle = peer->getNativeHandle())
        {
            static HBRUSH blackBrush = CreateSolidBrush (RGB (0, 0, 0));
            SetClassLongPtr (static_cast<HWND> (nativeHandle),
                             GCLP_HBRBACKGROUND,
                             reinterpret_cast<LONG_PTR> (blackBrush));
        }
    }
   #endif
}

void DisperserAudioProcessorEditor::parameterChanged (const juce::String& parameterID, float)
{
    const bool isSizeParam = parameterID == DisperserAudioProcessor::kParamUiWidth
                         || parameterID == DisperserAudioProcessor::kParamUiHeight;

    const bool isUiVisualParam = parameterID == DisperserAudioProcessor::kParamUiPalette
                             || parameterID == DisperserAudioProcessor::kParamUiFxTail
                             || parameterID == DisperserAudioProcessor::kParamUiColor0
                             || parameterID == DisperserAudioProcessor::kParamUiColor1;

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

    // ── CRT animation ──
    if (crtEnabled && w > 0 && h > 0)
    {
        crtTime += 0.1f;
        crtEffect.setTime (crtTime);

        const bool anySliderDragging = amountSlider.isMouseButtonDown()
                                    || seriesSlider.isMouseButtonDown()
                                    || freqSlider.isMouseButtonDown()
                                    || shapeSlider.isMouseButtonDown()
                                    || mixSlider.isMouseButtonDown();
        if (! anySliderDragging)
            repaint();
    }

    // ── MIDI note name polling ──
    const auto newMidiDisplay = audioProcessor.getCurrentFreqDisplay();
    if (newMidiDisplay != cachedMidiDisplay)
    {
        cachedMidiDisplay = newMidiDisplay;
        if (refreshLegendTextCache())
            updateCachedLayout();
        repaint (getRowRepaintBounds (freqSlider));
    }
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
        for (int i = 0; i < 2; ++i)
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

        const bool paletteSwitchChanged = (useCustomPalette != targetUseCustomPalette);
        const bool fxChanged = (crtEnabled != targetCrtEnabled);

        if (paletteSwitchChanged)
            useCustomPalette = targetUseCustomPalette;

        if (fxChanged)
            applyCrtState (targetCrtEnabled);

        if (paletteChanged || paletteSwitchChanged)
            applyActivePalette();

        if (paletteChanged || paletteSwitchChanged || fxChanged)
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
    const double fbV = juce::jlimit (0.0, 1.0, feedbackSlider.getValue());
    const int fbPct = (int) std::lround (fbV * 100.0);
    const float modMult = (float) modSliderToMultiplier (modSlider.getValue());
    const double mixV = juce::jlimit (0.0, 1.0, mixSlider.getValue());
    const int mixPct = (int) std::lround (mixV * 100.0);

    const auto oldAmountFullLen = cachedAmountTextFull.length();
    const auto oldAmountShortLen = cachedAmountTextShort.length();
    const auto oldSeriesFullLen = cachedSeriesTextFull.length();
    const auto oldSeriesShortLen = cachedSeriesTextShort.length();
    const auto oldFreqLen = cachedFreqTextHz.length();
    const auto oldFreqShortLen = cachedFreqTextShort.length();
    const auto oldShapeFullLen = cachedShapeTextFull.length();
    const auto oldShapeShortLen = cachedShapeTextShort.length();
    const auto oldFeedbackFullLen = cachedFeedbackTextFull.length();
    const auto oldFeedbackShortLen = cachedFeedbackTextShort.length();
    const auto oldModFullLen = cachedModTextFull.length();
    const auto oldModShortLen = cachedModTextShort.length();
    const auto oldMixFullLen = cachedMixTextFull.length();
    const auto oldMixShortLen = cachedMixTextShort.length();

    cachedAmountTextFull  = juce::String (amountV) + " STAGES";
    cachedAmountTextShort = juce::String (amountV) + " STG";

    cachedSeriesTextFull  = juce::String (seriesV) + " SERIES";
    cachedSeriesTextShort = juce::String (seriesV) + " SRS";

    cachedFreqTextHz = getFreqText();
    cachedFreqTextShort = getFreqTextShort();
    if (cachedMidiDisplay.isNotEmpty() && ! freqSlider.isMouseButtonDown())
    {
        cachedFreqIntOnly = cachedMidiDisplay;
    }
    else
    {
        cachedFreqIntOnly = formatBarFrequencyHzText (hz)
                                .upToFirstOccurrenceOf (".", false, false)
                                .upToFirstOccurrenceOf (" Hz", false, false);
    }

    cachedShapeTextFull = juce::String (shapePct).toUpperCase() + "% SHAPE";
    cachedShapeTextShort = juce::String (shapePct).toUpperCase() + "% SHP";
    cachedShapeIntOnly = juce::String (shapePct);

    cachedFeedbackTextFull = juce::String (fbPct) + "% FEEDBACK";
    cachedFeedbackTextShort = juce::String (fbPct) + "% FBK";
    cachedFeedbackIntOnly = juce::String (fbPct);

    if (std::abs (modMult - 1.0f) < kMultEpsilon)
    {
        cachedModTextFull  = "X1 MOD";
        cachedModTextShort = "X1";
    }
    else
    {
        cachedModTextFull  = "X" + juce::String (modMult, 2) + " MOD";
        cachedModTextShort = "X" + juce::String (modMult, 2);
    }
    cachedModIntOnly = "X" + juce::String (juce::roundToInt (modMult));

    cachedMixTextFull = juce::String (mixPct) + "% MIX";
    cachedMixTextShort = juce::String (mixPct) + "% MX";
    cachedMixIntOnly = juce::String (mixPct);

    const bool lengthChanged = oldAmountFullLen  != cachedAmountTextFull.length()
                            || oldAmountShortLen != cachedAmountTextShort.length()
                            || oldSeriesFullLen  != cachedSeriesTextFull.length()
                            || oldSeriesShortLen != cachedSeriesTextShort.length()
                            || oldFreqLen        != cachedFreqTextHz.length()
                            || oldFreqShortLen   != cachedFreqTextShort.length()
                            || oldShapeFullLen   != cachedShapeTextFull.length()
                            || oldShapeShortLen  != cachedShapeTextShort.length()
                            || oldFeedbackFullLen  != cachedFeedbackTextFull.length()
                            || oldFeedbackShortLen != cachedFeedbackTextShort.length()
                            || oldModFullLen    != cachedModTextFull.length()
                            || oldModShortLen   != cachedModTextShort.length()
                            || oldMixFullLen    != cachedMixTextFull.length()
                            || oldMixShortLen   != cachedMixTextShort.length();

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

    s.setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::thumbColourId, juce::Colours::transparentBlack);
}

//========================== Right-click numeric popup ==========================

namespace
{
    constexpr int kTitleAreaExtraHeightPx = 4;
    constexpr int kTitleRightGapToInfoPx = 8;
    constexpr int kVersionGapPx = 8;
    constexpr int kToggleLegendCollisionPadPx = 6;

    struct PopupSwatchButton final : public juce::TextButton
    {
        std::function<void()> onLeftClick;
        std::function<void()> onRightClick;

        void clicked() override
        {
            if (onLeftClick)
                onLeftClick();
            else
                juce::TextButton::clicked();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu())
            {
                if (onRightClick)
                    onRightClick();
                return;
            }

            juce::TextButton::mouseUp (e);
        }
    };

    struct PopupClickableLabel final : public juce::Label
    {
        using juce::Label::Label;
        std::function<void()> onClick;

        void mouseUp (const juce::MouseEvent& e) override
        {
            juce::Label::mouseUp (e);
            if (! e.mods.isPopupMenu() && onClick)
                onClick();
        }
    };

    // Label that renders using TextLayout instead of the default
    // drawFittedText / GlyphArrangement path.  This guarantees that
    // the rendered line-wrapping matches the TextLayout-based height
    // measurement in layoutInfoPopupContent, preventing clipping.
    struct TextLayoutLabel final : public juce::Label
    {
        using juce::Label::Label;

        void paint (juce::Graphics& g) override
        {
            g.fillAll (findColour (backgroundColourId));

            if (isBeingEdited())
                return;

            const auto f     = getFont();
            const auto area  = getBorderSize().subtractedFrom (getLocalBounds()).toFloat();
            const auto alpha = isEnabled() ? 1.0f : 0.5f;

            juce::AttributedString as;
            as.append (getText(), f,
                       findColour (textColourId).withMultipliedAlpha (alpha));
            as.setJustification (getJustificationType());

            juce::TextLayout layout;
            layout.createLayout (as, area.getWidth());
            layout.draw (g, area);
        }
    };
}

static void syncGraphicsPopupState (juce::AlertWindow& aw,
                                    const std::array<juce::Colour, 2>& defaultPalette,
                                    const std::array<juce::Colour, 2>& customPalette,
                                    bool useCustomPalette)
{
    if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteDefaultToggle")))
        t->setToggleState (! useCustomPalette, juce::dontSendNotification);
    if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteCustomToggle")))
        t->setToggleState (useCustomPalette, juce::dontSendNotification);

    for (int i = 0; i < 2; ++i)
    {
        if (auto* dflt = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("defaultSwatch" + juce::String (i))))
            setPaletteSwatchColour (*dflt, defaultPalette[(size_t) i]);
        if (auto* custom = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("customSwatch" + juce::String (i))))
        {
            setPaletteSwatchColour (*custom, customPalette[(size_t) i]);
            custom->setTooltip (colourToHexRgb (customPalette[(size_t) i]));
        }
    }

    auto applyLabelTextColourTo = [] (juce::Label* lbl, juce::Colour col)
    {
        if (lbl != nullptr)
            lbl->setColour (juce::Label::textColourId, col);
    };

    const juce::Colour activeText = useCustomPalette ? customPalette[0] : defaultPalette[0];
    applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteDefaultLabel")), activeText);
    applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteCustomLabel")), activeText);
    applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteTitle")), activeText);
    applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("fxLabel")), activeText);
}

static void layoutGraphicsPopupContent (juce::AlertWindow& aw)
{
    layoutAlertWindowButtons (aw);

    auto snapEven = [] (int v) { return v & ~1; };

    const int contentLeft = kPromptInnerMargin;
    const int contentRight = aw.getWidth() - kPromptInnerMargin;
    const int contentW = juce::jmax (0, contentRight - contentLeft);

    auto* dfltToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteDefaultToggle"));
    auto* dfltLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteDefaultLabel"));
    auto* customToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteCustomToggle"));
    auto* customLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteCustomLabel"));
    auto* paletteTitle = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteTitle"));
    auto* fxToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("fxToggle"));
    auto* fxLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("fxLabel"));
    auto* okBtn = aw.getNumButtons() > 0 ? aw.getButton (0) : nullptr;

    constexpr int toggleBox = GraphicsPromptLayout::toggleBox;
    constexpr int toggleGap = 4;
    constexpr int toggleVisualInsetLeft = 2;
    constexpr int swatchSize = GraphicsPromptLayout::swatchSize;
    constexpr int swatchGap = GraphicsPromptLayout::swatchGap;
    constexpr int columnGap = GraphicsPromptLayout::columnGap;
    constexpr int titleH = GraphicsPromptLayout::titleHeight;

    const int toggleVisualSide = juce::jlimit (14,
                                               juce::jmax (14, toggleBox - 2),
                                               (int) std::lround ((double) toggleBox * 0.65));

    const int swatchW = swatchSize;
    const int swatchH = (2 * swatchSize) + swatchGap;
    const int swatchGroupSize = (2 * swatchW) + swatchGap;
    const int swatchesH = swatchH;
    const int modeH = toggleBox;

    const int baseGap1 = GraphicsPromptLayout::titleToModeGap;
    const int baseGap2 = GraphicsPromptLayout::modeToSwatchesGap;

    const int titleY = snapEven (kPromptFooterBottomPad);
    const int footerY = getAlertButtonsTop (aw);

    const int bodyH = modeH + baseGap2 + swatchesH;
    const int bodyZoneTop = titleY + titleH + baseGap1;
    const int bodyZoneBottom = footerY - baseGap1;
    const int bodyZoneH = juce::jmax (0, bodyZoneBottom - bodyZoneTop);
    const int bodyY = snapEven (bodyZoneTop + juce::jmax (0, (bodyZoneH - bodyH) / 2));

    const int modeY = bodyY;
    const int blocksY = snapEven (modeY + modeH + baseGap2);

    const int dfltLabelW = (dfltLabel != nullptr) ? juce::jmax (38, stringWidth (dfltLabel->getFont(), "DFLT") + 2) : 40;
    const int customLabelW = (customLabel != nullptr) ? juce::jmax (38, stringWidth (customLabel->getFont(), "CSTM") + 2) : 40;
    const int fxLabelW = (fxLabel != nullptr)
                       ? juce::jmax (90, stringWidth (fxLabel->getFont(), fxLabel->getText().toUpperCase()) + 2)
                       : 96;

    const int toggleLabelStartOffset = toggleVisualInsetLeft + toggleVisualSide + toggleGap;
    const int dfltRowW = toggleLabelStartOffset + dfltLabelW;
    const int customRowW = toggleLabelStartOffset + customLabelW;
    const int fxRowW = toggleLabelStartOffset + fxLabelW;
    const int okBtnW = (okBtn != nullptr) ? okBtn->getWidth() : 96;

    const int leftColumnW = juce::jmax (swatchGroupSize, juce::jmax (dfltRowW, fxRowW));
    const int rightColumnW = juce::jmax (swatchGroupSize, juce::jmax (customRowW, okBtnW));
    const int columnsRowW = leftColumnW + columnGap + rightColumnW;
    const int columnsX = snapEven (contentLeft + juce::jmax (0, (contentW - columnsRowW) / 2));
    const int col0X = columnsX;
    const int col1X = columnsX + leftColumnW + columnGap;

    const int dfltX = col0X;
    const int customX = col1X;

    const int defaultSwatchStartX = col0X;
    const int customSwatchStartX = col1X;

    if (paletteTitle != nullptr)
    {
        const int paletteW = juce::jmax (100, juce::jmin (leftColumnW, contentRight - col0X));
        paletteTitle->setBounds (col0X, titleY, paletteW, titleH);
    }

    if (dfltToggle != nullptr)   dfltToggle->setBounds (dfltX, modeY, toggleBox, toggleBox);
    if (dfltLabel != nullptr)    dfltLabel->setBounds (dfltX + toggleLabelStartOffset, modeY, dfltLabelW, toggleBox);
    if (customToggle != nullptr) customToggle->setBounds (customX, modeY, toggleBox, toggleBox);
    if (customLabel != nullptr)  customLabel->setBounds (customX + toggleLabelStartOffset, modeY, customLabelW, toggleBox);

    auto placeSwatchGroup = [&] (const juce::String& prefix, int startX)
    {
        const int startY = blocksY;

        for (int i = 0; i < 2; ++i)
        {
            if (auto* b = dynamic_cast<juce::TextButton*> (aw.findChildWithID (prefix + juce::String (i))))
            {
                b->setBounds (startX + i * (swatchW + swatchGap),
                              startY,
                              swatchW,
                              swatchH);
            }
        }
    };

    placeSwatchGroup ("defaultSwatch", defaultSwatchStartX);
    placeSwatchGroup ("customSwatch", customSwatchStartX);

    if (okBtn != nullptr)
    {
        auto okR = okBtn->getBounds();
        okR.setX (col1X);
        okR.setY (footerY);
        okBtn->setBounds (okR);

        const int fxY = snapEven (footerY + juce::jmax (0, (okR.getHeight() - toggleBox) / 2));
        const int fxX = col0X;
        if (fxToggle != nullptr) fxToggle->setBounds (fxX, fxY, toggleBox, toggleBox);
        if (fxLabel != nullptr)  fxLabel->setBounds (fxX + toggleLabelStartOffset, fxY, fxLabelW, toggleBox);
    }

    auto updateVisualBounds = [] (juce::Component* c, int& minX, int& maxR)
    {
        if (c == nullptr)
            return;

        const auto r = c->getBounds();
        minX = juce::jmin (minX, r.getX());
        maxR = juce::jmax (maxR, r.getRight());
    };

    int visualMinX = aw.getWidth();
    int visualMaxR = 0;

    updateVisualBounds (paletteTitle, visualMinX, visualMaxR);
    updateVisualBounds (dfltToggle, visualMinX, visualMaxR);
    updateVisualBounds (dfltLabel, visualMinX, visualMaxR);
    updateVisualBounds (customToggle, visualMinX, visualMaxR);
    updateVisualBounds (customLabel, visualMinX, visualMaxR);
    updateVisualBounds (fxToggle, visualMinX, visualMaxR);
    updateVisualBounds (fxLabel, visualMinX, visualMaxR);
    updateVisualBounds (okBtn, visualMinX, visualMaxR);

    for (int i = 0; i < 2; ++i)
    {
        updateVisualBounds (aw.findChildWithID ("defaultSwatch" + juce::String (i)), visualMinX, visualMaxR);
        updateVisualBounds (aw.findChildWithID ("customSwatch" + juce::String (i)), visualMinX, visualMaxR);
    }

    if (visualMaxR > visualMinX)
    {
        const int leftMarginToPrompt = visualMinX;
        const int rightMarginToPrompt = aw.getWidth() - visualMaxR;

        int dx = (rightMarginToPrompt - leftMarginToPrompt) / 2;

        const int minDx = contentLeft - visualMinX;
        const int maxDx = contentRight - visualMaxR;
        dx = juce::jlimit (minDx, maxDx, dx);

        if (dx != 0)
        {
            auto shiftX = [dx] (juce::Component* c)
            {
                if (c == nullptr)
                    return;

                auto r = c->getBounds();
                r.setX (r.getX() + dx);
                c->setBounds (r);
            };

            shiftX (paletteTitle);
            shiftX (dfltToggle);
            shiftX (dfltLabel);
            shiftX (customToggle);
            shiftX (customLabel);
            shiftX (fxToggle);
            shiftX (fxLabel);
            shiftX (okBtn);

            for (int i = 0; i < 2; ++i)
            {
                shiftX (aw.findChildWithID ("defaultSwatch" + juce::String (i)));
                shiftX (aw.findChildWithID ("customSwatch" + juce::String (i)));
            }
        }
    }
}

static void layoutInfoPopupContent (juce::AlertWindow& aw)
{
    layoutAlertWindowButtons (aw);

    const int contentTop = kPromptBodyTopPad;
    const int contentBottom = getAlertButtonsTop (aw) - kPromptBodyBottomPad;
    const int contentH = juce::jmax (0, contentBottom - contentTop);
    const int bodyW = aw.getWidth() - (2 * kPromptInnerMargin);

    // Find the viewport that wraps all body content
    auto* viewport = dynamic_cast<juce::Viewport*> (aw.findChildWithID ("bodyViewport"));
    if (viewport == nullptr)
        return;

    viewport->setBounds (kPromptInnerMargin, contentTop, bodyW, contentH);

    auto* content = viewport->getViewedComponent();
    if (content == nullptr)
        return;

    // Layout children inside content component top-to-bottom
    constexpr int kItemGap = 10;
    int y = 0;
    const int innerW = bodyW - 10;  // leave room for scrollbar

    for (int i = 0; i < content->getNumChildComponents(); ++i)
    {
        auto* child = content->getChildComponent (i);
        if (child == nullptr || ! child->isVisible())
            continue;

        // Labels measure preferred height from font
        int itemH = 30;
        if (auto* label = dynamic_cast<juce::Label*> (child))
        {
            auto font = label->getFont();
            const auto text = label->getText();
            const auto border = label->getBorderSize();

            // Single-line labels (no newlines): use font height directly
            if (! text.containsChar ('\n'))
            {
                itemH = (int) std::ceil (font.getHeight()) + border.getTopAndBottom();
            }
            else
            {
                // Multi-line: measure with TextLayout
                juce::AttributedString as;
                as.append (text, font, label->findColour (juce::Label::textColourId));
                as.setJustification (label->getJustificationType());
                juce::TextLayout layout;
                const int textAreaW = innerW - border.getLeftAndRight();
                layout.createLayout (as, (float) juce::jmax (1, textAreaW));
                itemH = juce::jmax (20, (int) std::ceil (layout.getHeight()
                                                         + font.getDescent())
                                        + border.getTopAndBottom() + 4);
            }
        }
        else if (dynamic_cast<juce::HyperlinkButton*> (child) != nullptr)
        {
            itemH = 28;
        }

        child->setBounds (0, y, innerW, itemH);

        // ── Poem auto-fit: if text overflows with padding, shrink font ──
        if (auto* label = dynamic_cast<juce::Label*> (child))
        {
            const auto& props = label->getProperties();
            if (props.contains ("poemPadFraction"))
            {
                const float padFrac = (float) props["poemPadFraction"];
                const int padPx = juce::jmax (4, (int) std::round (innerW * padFrac));
                label->setBorderSize (juce::BorderSize<int> (0, padPx, 0, padPx));

                // Check if text fits; if not, shrink font until it does (min 65% scale)
                auto font = label->getFont();
                const int textAreaW = innerW - 2 * padPx;
                for (float scale = 1.0f; scale >= 0.65f; scale -= 0.025f)
                {
                    font.setHorizontalScale (scale);
                    juce::GlyphArrangement glyphs;
                    glyphs.addLineOfText (font, label->getText(), 0.0f, 0.0f);
                    if (static_cast<int> (std::ceil (glyphs.getBoundingBox (0, -1, false).getWidth())) <= textAreaW)
                        break;
                }
                label->setFont (font);
            }
        }

        y += itemH + kItemGap;
    }

    // Remove trailing gap
    if (y > kItemGap)
        y -= kItemGap;

    content->setSize (innerW, juce::jmax (contentH, y));
}

void DisperserAudioProcessorEditor::openNumericEntryPopupForSlider (juce::Slider& s)
{
    lnf.setScheme (activeScheme);

    const auto scheme = activeScheme;

    juce::String suffix;
    juce::String suffixShort;
    const bool isPercentPrompt = (&s == &shapeSlider || &s == &feedbackSlider || &s == &mixSlider);

    if (&s == &amountSlider)       { suffix = " STAGES";  suffixShort = " STG"; }
    else if (&s == &seriesSlider)  { suffix = " SERIES";  suffixShort = " SRS"; }
    else if (&s == &freqSlider)    { suffix = " Hz";      suffixShort = " Hz"; }
    else if (&s == &shapeSlider)   { suffix = " % SHAPE"; suffixShort = " %"; }
    else if (&s == &feedbackSlider){ suffix = " % FEEDBACK"; suffixShort = " % FBK"; }
    else if (&s == &modSlider)     { suffix = " X MOD";   suffixShort = " X"; }
    else if (&s == &mixSlider)     { suffix = " % MIX";   suffixShort = " %"; }

    const juce::String suffixText = suffix.trimStart();
    const juce::String suffixTextShort = suffixShort.trimStart();

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    aw->setLookAndFeel (&lnf);

    juce::String currentDisplay;
    if (&s == &amountSlider)
        currentDisplay = juce::String ((int) s.getValue());
    else if (&s == &seriesSlider)
        currentDisplay = juce::String ((int) s.getValue());
    else if (&s == &freqSlider)
        currentDisplay = juce::String (s.getValue(), 3);
    else if (&s == &shapeSlider)
        currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 4);
    else if (&s == &feedbackSlider)
        currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 4);
    else if (&s == &mixSlider)
        currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 4);
    else if (&s == &modSlider)
        currentDisplay = juce::String (modSliderToMultiplier (s.getValue()), 2);
    else
        currentDisplay = s.getTextFromValue (s.getValue());

    aw->addTextEditor ("val", currentDisplay, juce::String());

    juce::Label* suffixLabel = nullptr;
    juce::Rectangle<int> editorBaseBounds;
    std::function<void()> layoutValueAndSuffix;

    if (auto* te = aw->getTextEditor ("val"))
    {
        const auto& f = kBoldFont40();
        te->setFont (f);
        te->applyFontToAllText (f);

        auto r = te->getBounds();
        r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
        r.setY (juce::jmax (kPromptEditorMinTopPx, r.getY() - kPromptEditorRaiseYPx));
        editorBaseBounds = r;

        suffixLabel = new juce::Label ("suffix", suffixText);
        suffixLabel->setComponentID (kPromptSuffixLabelId);
        suffixLabel->setJustificationType (juce::Justification::centredLeft);
        applyLabelTextColour (*suffixLabel, scheme.text);
        suffixLabel->setBorderSize (juce::BorderSize<int> (0));
        suffixLabel->setFont (f);
        aw->addAndMakeVisible (suffixLabel);

        juce::String worstCaseText;
        if (&s == &amountSlider)       worstCaseText = "256";
        else if (&s == &seriesSlider)  worstCaseText = "4";
        else if (&s == &freqSlider)    worstCaseText = "20000.000";
        else if (&s == &shapeSlider)   worstCaseText = "100.0000";
        else if (&s == &feedbackSlider)worstCaseText = "100.0000";
        else if (&s == &mixSlider)     worstCaseText = "100.0000";
        else if (&s == &modSlider)     worstCaseText = "4.00";
        else                           worstCaseText = "999.99";

        const int maxInputTextW = juce::jmax (1, stringWidth (f, worstCaseText));

        layoutValueAndSuffix = [aw, te, suffixLabel, editorBaseBounds, isPercentPrompt, suffixText, suffixTextShort, maxInputTextW]()
        {
            const int contentPad = kPromptInlineContentPadPx;
            const int contentLeft = contentPad;
            const int contentRight = (aw != nullptr ? aw->getWidth() - contentPad : editorBaseBounds.getRight());
            const int availableW = contentRight - contentLeft;
            const int contentCenter = (contentLeft + contentRight) / 2;

            const int fullLabelW = stringWidth (suffixLabel->getFont(), suffixText) + 2;
            const bool stickPercentFull = suffixText.containsChar ('%');
            const int spaceWFull = stickPercentFull ? 0 : juce::jmax (2, stringWidth (suffixLabel->getFont(), " "));
            const int worstCaseFullW = maxInputTextW + spaceWFull + fullLabelW;

            const bool useShort = (worstCaseFullW > availableW) && suffixTextShort != suffixText;
            const juce::String& activeSuffix = useShort ? suffixTextShort : suffixText;
            suffixLabel->setText (activeSuffix, juce::dontSendNotification);

            const auto txt = te->getText();
            const int textW = juce::jmax (1, stringWidth (te->getFont(), txt));
            int labelW = stringWidth (suffixLabel->getFont(), activeSuffix) + 2;
            auto er = te->getBounds();

            const bool stickPercentToValue = activeSuffix.containsChar ('%');
            const int spaceW = stickPercentToValue ? 0 : juce::jmax (2, stringWidth (te->getFont(), " "));
            const int minGapPx = juce::jmax (1, spaceW);

            constexpr int kEditorTextPadPx = 12;
            constexpr int kMinEditorWidthPx = 24;
            const int editorW = juce::jlimit (kMinEditorWidthPx,
                                              editorBaseBounds.getWidth(),
                                              textW + (kEditorTextPadPx * 2));
            er.setWidth (editorW);

            const int combinedW = textW + minGapPx + labelW;

            int blockLeft = contentCenter - (combinedW / 2);
            const int minBlockLeft = contentLeft;
            const int maxBlockLeft = juce::jmax (minBlockLeft, contentRight - combinedW);
            blockLeft = juce::jlimit (minBlockLeft, maxBlockLeft, blockLeft);

            int teX = blockLeft - ((editorW - textW) / 2);
            const int minTeX = contentLeft;
            const int maxTeX = juce::jmax (minTeX, contentRight - editorW);
            teX = juce::jlimit (minTeX, maxTeX, teX);

            er.setX (teX);
            te->setBounds (er);

            const int textLeftActual = er.getX() + (er.getWidth() - textW) / 2;
            int labelX = textLeftActual + textW + minGapPx;
            const int minLabelX = contentLeft;
            const int maxLabelX = juce::jmax (minLabelX, contentRight - labelW);
            labelX = juce::jlimit (minLabelX, maxLabelX, labelX);

            const int labelY = er.getY();
            const int labelH = juce::jmax (1, er.getHeight());
            suffixLabel->setBounds (labelX, labelY, labelW, labelH);
        };

        te->setBounds (editorBaseBounds);
        int labelW0 = stringWidth (suffixLabel->getFont(), suffixText) + 2;
        suffixLabel->setBounds (r.getRight() + 2, r.getY() + 1, labelW0, juce::jmax (1, r.getHeight() - 2));

        if (layoutValueAndSuffix)
            layoutValueAndSuffix();

        double minVal = 0.0, maxVal = 1.0;
        int maxLen = 0, maxDecs = 4;

        if (&s == &amountSlider)
        {
            maxVal = 256.0;
            maxDecs = 0;
            maxLen = 3;
        }
        else if (&s == &seriesSlider)
        {
            maxVal = 4.0;
            maxDecs = 0;
            maxLen = 1;
        }
        else if (&s == &freqSlider)
        {
            maxVal = 20000.0;
            maxDecs = 3;
            maxLen = 9;
        }
        else if (&s == &shapeSlider)
        {
            minVal = 0.0;
            maxVal = 100.0;
            maxDecs = 4;
            maxLen = 8;
        }
        else if (&s == &feedbackSlider)
        {
            minVal = 0.0;
            maxVal = 100.0;
            maxDecs = 4;
            maxLen = 8;
        }
        else if (&s == &mixSlider)
        {
            minVal = 0.0;
            maxVal = 100.0;
            maxDecs = 4;
            maxLen = 8;
        }
        else if (&s == &modSlider)
        {
            minVal = 0.0;
            maxVal = 4.0;
            maxDecs = 2;
            maxLen = 4;
        }

        te->setInputFilter (new NumericInputFilter (minVal, maxVal, maxLen, maxDecs), true);

        const int localMaxDecs = maxDecs;
        te->onTextChange = [te, layoutValueAndSuffix, localMaxDecs]() mutable
        {
            auto txt = te->getText();
            int dot = txt.indexOfChar('.');
            if (dot >= 0)
            {
                int decimals = txt.length() - dot - 1;
                if (decimals > localMaxDecs)
                    te->setText (txt.substring (0, dot + 1 + localMaxDecs), juce::dontSendNotification);
            }
            if (layoutValueAndSuffix)
                layoutValueAndSuffix();
        };
    }

    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    applyPromptShellSize (*aw);
    layoutAlertWindowButtons (*aw);

    const juce::Font& kPromptFont = kBoldFont40();

    preparePromptTextEditor (*aw,
                             "val",
                             scheme.bg,
                             scheme.text,
                             scheme.fg,
                             kPromptFont,
                             false);

    if (suffixLabel != nullptr && ! editorBaseBounds.isEmpty())
    {
        if (auto* te = aw->getTextEditor ("val"))
            suffixLabel->setFont (te->getFont());
        if (layoutValueAndSuffix)
            layoutValueAndSuffix();
    }

    styleAlertButtons (*aw, lnf);

    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);
    juce::Slider* sliderPtr = &s;

    setPromptOverlayActive (true);

    aw->setLookAndFeel (&lnf);

    if (safeThis != nullptr)
    {
        fitAlertWindowToEditor (*aw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
        {
            if (layoutValueAndSuffix)
                layoutValueAndSuffix();
            layoutAlertWindowButtons (a);
            preparePromptTextEditor (a,
                                     "val",
                                     scheme.bg,
                                     scheme.text,
                                     scheme.fg,
                                     kPromptFont,
                                     false);
        });

        embedAlertWindowInOverlay (safeThis.getComponent(), aw);
    }
    else
    {
        aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
        bringPromptWindowToFront (*aw);
        aw->repaint();
    }

    {
        preparePromptTextEditor (*aw,
                                 "val",
                                 scheme.bg,
                                 scheme.text,
                                 scheme.fg,
                                 kPromptFont,
                                 false);
        if (auto* suffixLbl = dynamic_cast<juce::Label*> (aw->findChildWithID (kPromptSuffixLabelId)))
        {
            if (auto* te = aw->getTextEditor ("val"))
                suffixLbl->setFont (te->getFont());
        }

        if (layoutValueAndSuffix)
            layoutValueAndSuffix();

        juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
        juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThisPtr (this);
        juce::MessageManager::callAsync ([safeAw, safeThisPtr]()
        {
            if (safeAw == nullptr)
                return;
            bringPromptWindowToFront (*safeAw);
            safeAw->repaint();
        });
    }

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([safeThis, sliderPtr, aw] (int result) mutable
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);

            if (safeThis != nullptr)
                safeThis->setPromptOverlayActive (false);

            if (safeThis == nullptr || sliderPtr == nullptr)
                return;

            if (result != 1)
                return;

            const auto txt = aw->getTextEditorContents ("val").trim();
            auto normalised = txt.replaceCharacter (',', '.');

            juce::String t = normalised.trimStart();
            while (t.startsWithChar ('+'))
                t = t.substring (1).trimStart();
            const juce::String numericToken = t.initialSectionContainingOnly ("0123456789.,-");
            double v = numericToken.getDoubleValue();

            if (safeThis != nullptr && (sliderPtr == &safeThis->shapeSlider || sliderPtr == &safeThis->feedbackSlider || sliderPtr == &safeThis->mixSlider))
                v *= 0.01;

            // user typed multiplier for MOD; convert to slider's [0,1] range
            if (safeThis != nullptr && sliderPtr == &safeThis->modSlider)
                v = multiplierToModSlider (v);

            const auto range = sliderPtr->getRange();
            double clamped = juce::jlimit (range.getStart(), range.getEnd(), v);

            if (safeThis != nullptr && sliderPtr == &safeThis->freqSlider)
            {
                clamped = roundToDecimals (clamped, 4);
            }

            sliderPtr->setValue (clamped, juce::sendNotificationSync);
        }));
}

// ── MIDI Channel Prompt ───────────────────────────────────────────
void DisperserAudioProcessorEditor::openMidiChannelPrompt()
{
    lnf.setScheme (activeScheme);
    const auto scheme = activeScheme;

    const int currentChannel = audioProcessor.getMidiChannel();

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    aw->setLookAndFeel (&lnf);

    aw->addTextEditor ("val", juce::String (currentChannel), juce::String());

    juce::Label* suffixLabel = nullptr;
    juce::Rectangle<int> editorBaseBounds;
    std::function<void()> layoutValueAndSuffix;

    if (auto* te = aw->getTextEditor ("val"))
    {
        const auto& f = kBoldFont40();
        te->setFont (f);
        te->applyFontToAllText (f);

        auto r = te->getBounds();
        r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
        r.setY (juce::jmax (kPromptEditorMinTopPx, r.getY() - kPromptEditorRaiseYPx));
        editorBaseBounds = r;

        suffixLabel = new juce::Label ("suffix", "CHANNEL");
        suffixLabel->setComponentID (kPromptSuffixLabelId);
        suffixLabel->setJustificationType (juce::Justification::centredLeft);
        applyLabelTextColour (*suffixLabel, scheme.text);
        suffixLabel->setBorderSize (juce::BorderSize<int> (0));
        suffixLabel->setFont (f);
        aw->addAndMakeVisible (suffixLabel);

        const int maxInputTextW = juce::jmax (1, stringWidth (f, "16"));
        const juce::String suffixText = "CHANNEL";

        // legendFirst: label BEFORE input
        layoutValueAndSuffix = [aw, te, suffixLabel, editorBaseBounds, maxInputTextW, suffixText]()
        {
            const int contentPad = kPromptInlineContentPadPx;
            const int contentLeft = contentPad;
            const int contentRight = (aw != nullptr ? aw->getWidth() - contentPad : editorBaseBounds.getRight());
            const int contentCenter = (contentLeft + contentRight) / 2;

            const int labelW = stringWidth (suffixLabel->getFont(), suffixText) + 2;
            const int spaceW = juce::jmax (2, stringWidth (suffixLabel->getFont(), " "));
            const auto txt = te->getText();
            const int textW = juce::jmax (1, stringWidth (te->getFont(), txt));

            constexpr int kEditorTextPadPx = 12;
            constexpr int kMinEditorWidthPx = 24;
            auto er = te->getBounds();
            const int editorW = juce::jlimit (kMinEditorWidthPx,
                                              editorBaseBounds.getWidth(),
                                              textW + (kEditorTextPadPx * 2));
            er.setWidth (editorW);

            // legendFirst: [CHANNEL] [value]
            const int combinedW = labelW + spaceW + textW;
            int blockLeft = contentCenter - (combinedW / 2);
            blockLeft = juce::jlimit (contentLeft,
                                      juce::jmax (contentLeft, contentRight - combinedW),
                                      blockLeft);

            int labelX = blockLeft;
            labelX = juce::jlimit (contentLeft,
                                   juce::jmax (contentLeft, contentRight - labelW),
                                   labelX);
            suffixLabel->setBounds (labelX, er.getY(), labelW, juce::jmax (1, er.getHeight()));

            int teX = labelX + labelW + spaceW - ((editorW - textW) / 2);
            teX = juce::jlimit (contentLeft,
                                juce::jmax (contentLeft, contentRight - editorW),
                                teX);
            er.setX (teX);
            te->setBounds (er);
        };

        te->setBounds (editorBaseBounds);
        int labelW0 = stringWidth (suffixLabel->getFont(), suffixText) + 2;
        suffixLabel->setBounds (r.getX() - labelW0 - 4, r.getY() + 1, labelW0, juce::jmax (1, r.getHeight() - 2));

        if (layoutValueAndSuffix)
            layoutValueAndSuffix();

        struct MidiChannelInputFilter : public juce::TextEditor::InputFilter
        {
            juce::String filterNewText (juce::TextEditor& editor, const juce::String& newInput) override
            {
                juce::String filtered;
                for (int i = 0; i < newInput.length(); ++i)
                {
                    const auto c = newInput[i];
                    if (c >= '0' && c <= '9')
                        filtered += c;
                }
                const auto combined = editor.getText() + filtered;
                if (combined.length() > 2)
                    return {};
                const int val = combined.getIntValue();
                if (val > 16)
                    return {};
                if (combined.length() > 1 && combined[0] == '0')
                    return {};
                return filtered;
            }
        };

        te->setInputFilter (new MidiChannelInputFilter(), true);

        te->onTextChange = [te, layoutValueAndSuffix]() mutable
        {
            if (layoutValueAndSuffix)
                layoutValueAndSuffix();
        };
    }

    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    applyPromptShellSize (*aw);
    layoutAlertWindowButtons (*aw);

    const juce::Font& kPromptFont = kBoldFont40();

    preparePromptTextEditor (*aw, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);

    if (suffixLabel != nullptr && ! editorBaseBounds.isEmpty())
    {
        if (auto* te = aw->getTextEditor ("val"))
            suffixLabel->setFont (te->getFont());
        if (layoutValueAndSuffix)
            layoutValueAndSuffix();
    }

    styleAlertButtons (*aw, lnf);

    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);

    setPromptOverlayActive (true);
    aw->setLookAndFeel (&lnf);

    if (safeThis != nullptr)
    {
        fitAlertWindowToEditor (*aw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
        {
            if (layoutValueAndSuffix)
                layoutValueAndSuffix();
            layoutAlertWindowButtons (a);
            preparePromptTextEditor (a, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);
        });

        embedAlertWindowInOverlay (safeThis.getComponent(), aw);
    }
    else
    {
        aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
        bringPromptWindowToFront (*aw);
        aw->repaint();
    }

    {
        preparePromptTextEditor (*aw, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);
        if (auto* suffixLbl = dynamic_cast<juce::Label*> (aw->findChildWithID (kPromptSuffixLabelId)))
        {
            if (auto* te = aw->getTextEditor ("val"))
                suffixLbl->setFont (te->getFont());
        }

        if (layoutValueAndSuffix)
            layoutValueAndSuffix();

        juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
        juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThisPtr (this);
        juce::MessageManager::callAsync ([safeAw, safeThisPtr]()
        {
            if (safeAw == nullptr)
                return;
            bringPromptWindowToFront (*safeAw);
            safeAw->repaint();
        });
    }

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([safeThis, aw] (int result) mutable
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);

            if (safeThis != nullptr)
                safeThis->setPromptOverlayActive (false);

            if (safeThis == nullptr || result != 1)
                return;

            const auto txt = aw->getTextEditorContents ("val").trim();
            const int ch = juce::jlimit (0, 16, txt.isEmpty() ? 0 : txt.getIntValue());
            safeThis->audioProcessor.setMidiChannel (ch);
            safeThis->midiChannelDisplay.setTooltip (formatMidiChannelTooltip (ch));
        }));
}

void DisperserAudioProcessorEditor::openInfoPopup()
{
    lnf.setScheme (activeScheme);

    setPromptOverlayActive (true);

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);
    aw->setLookAndFeel (&lnf);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("GRAPHICS", 2);

    applyPromptShellSize (*aw);

    // ── Body content: parsed from InfoContent.h XML ──
    auto* bodyContent = new juce::Component();
    bodyContent->setComponentID ("bodyContent");

    auto infoFont = lnf.getAlertWindowMessageFont();
    infoFont.setHeight (infoFont.getHeight() * 1.45f);

    auto headingFont = infoFont;
    headingFont.setBold (true);
    headingFont.setHeight (infoFont.getHeight() * 1.25f);

    auto linkFont = infoFont;
    linkFont.setHeight (infoFont.getHeight() * 1.08f);

    auto poemFont = infoFont;
    poemFont.setItalic (true);

    // Parse the XML content from InfoContent.h
    auto xmlDoc = juce::XmlDocument::parse (InfoContent::xml);
    auto* contentNode = xmlDoc != nullptr ? xmlDoc->getChildByName ("content") : nullptr;

    if (contentNode != nullptr)
    {
        int elemIdx = 0;
        for (auto* node : contentNode->getChildIterator())
        {
            const auto tag  = node->getTagName();
            const auto text = node->getAllSubText().trim();
            const auto id   = tag + juce::String (elemIdx++);

            if (tag == "heading")
            {
                auto* l = new juce::Label (id, text);
                l->setComponentID (id);
                l->setJustificationType (juce::Justification::centred);
                applyLabelTextColour (*l, activeScheme.text);
                l->setFont (headingFont);
                bodyContent->addAndMakeVisible (l);
            }
            else if (tag == "text" || tag == "separator")
            {
                auto* l = new juce::Label (id, text);
                l->setComponentID (id);
                l->setJustificationType (juce::Justification::centred);
                applyLabelTextColour (*l, activeScheme.text);
                l->setFont (infoFont);
                l->setBorderSize (juce::BorderSize<int> (0));
                bodyContent->addAndMakeVisible (l);
            }
            else if (tag == "link")
            {
                const auto url = node->getStringAttribute ("url");
                auto* lnk = new juce::HyperlinkButton (text, juce::URL (url));
                lnk->setComponentID (id);
                lnk->setJustificationType (juce::Justification::centred);
                lnk->setColour (juce::HyperlinkButton::textColourId, activeScheme.text);
                lnk->setFont (linkFont, false, juce::Justification::centred);
                lnk->setTooltip ("");
                bodyContent->addAndMakeVisible (lnk);
            }
            else if (tag == "poem")
            {
                auto* l = new juce::Label (id, text);
                l->setComponentID (id);
                l->setJustificationType (juce::Justification::centred);
                applyLabelTextColour (*l, activeScheme.text);
                l->setFont (poemFont);
                l->setBorderSize (juce::BorderSize<int> (0, 0, 0, 0));
                l->getProperties().set ("poemPadFraction", 0.12f);
                bodyContent->addAndMakeVisible (l);
            }
            else if (tag == "spacer")
            {
                auto* l = new juce::Label (id, "");
                l->setComponentID (id);
                l->setFont (infoFont);
                l->setBorderSize (juce::BorderSize<int> (0));
                bodyContent->addAndMakeVisible (l);
            }
        }
    }

    auto* viewport = new juce::Viewport();
    viewport->setComponentID ("bodyViewport");
    viewport->setViewedComponent (bodyContent, true);  // viewport owns bodyContent
    viewport->setScrollBarsShown (true, false);         // vertical only
    viewport->setScrollBarThickness (8);
    viewport->setLookAndFeel (&lnf);
    aw->addAndMakeVisible (viewport);

    layoutInfoPopupContent (*aw);

    if (safeThis != nullptr)
    {
        fitAlertWindowToEditor (*aw, safeThis.getComponent(), [] (juce::AlertWindow& a)
        {
            layoutInfoPopupContent (a);
        });

        embedAlertWindowInOverlay (safeThis.getComponent(), aw);
    }
    else
    {
        aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
        bringPromptWindowToFront (*aw);
        aw->repaint();
    }

    juce::MessageManager::callAsync ([safeAw, safeThis]()
    {
        if (safeAw == nullptr || safeThis == nullptr)
            return;

        bringPromptWindowToFront (*safeAw);
        safeAw->repaint();
    });

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<DisperserAudioProcessorEditor> (this), aw] (int result) mutable
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);

            if (safeThis == nullptr)
                return;

            if (result == 2)
            {
                safeThis->openGraphicsPopup();
                return;
            }

            safeThis->setPromptOverlayActive (false);
        }));
}

void DisperserAudioProcessorEditor::openGraphicsPopup()
{
    lnf.setScheme (activeScheme);

    useCustomPalette = audioProcessor.getUiUseCustomPalette();
    crtEnabled = audioProcessor.getUiFxTailEnabled();
    crtEffect.setEnabled (crtEnabled);
    applyActivePalette();

    setPromptOverlayActive (true);

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);
    juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
    aw->setLookAndFeel (&lnf);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));

    auto labelFont = lnf.getAlertWindowMessageFont();
    labelFont.setHeight (labelFont.getHeight() * 1.20f);

    auto addPopupLabel = [this, aw] (const juce::String& id,
                                     const juce::String& text,
                                     juce::Font font,
                                     juce::Justification justification = juce::Justification::centredLeft)
    {
        auto* label = new PopupClickableLabel (id, text);
        label->setComponentID (id);
        label->setJustificationType (justification);
        applyLabelTextColour (*label, activeScheme.text);
        label->setBorderSize (juce::BorderSize<int> (0));
        label->setFont (font);
        label->setMouseCursor (juce::MouseCursor::PointingHandCursor);
        aw->addAndMakeVisible (label);
        return label;
    };

    auto* defaultToggle = new juce::ToggleButton ("");
    defaultToggle->setComponentID ("paletteDefaultToggle");
    aw->addAndMakeVisible (defaultToggle);

    auto* defaultLabel = addPopupLabel ("paletteDefaultLabel", "DFLT", labelFont);

    auto* customToggle = new juce::ToggleButton ("");
    customToggle->setComponentID ("paletteCustomToggle");
    aw->addAndMakeVisible (customToggle);

    auto* customLabel = addPopupLabel ("paletteCustomLabel", "CSTM", labelFont);

    auto paletteTitleFont = labelFont;
    paletteTitleFont.setHeight (paletteTitleFont.getHeight() * 1.30f);
    addPopupLabel ("paletteTitle", "PALETTE", paletteTitleFont, juce::Justification::centredLeft);

    for (int i = 0; i < 2; ++i)
    {
        auto* dflt = new juce::TextButton();
        dflt->setComponentID ("defaultSwatch" + juce::String (i));
        dflt->setTooltip ("Default palette colour " + juce::String (i + 1));
        aw->addAndMakeVisible (dflt);

        auto* custom = new PopupSwatchButton();
        custom->setComponentID ("customSwatch" + juce::String (i));
        custom->setTooltip (colourToHexRgb (customPalette[(size_t) i]));
        aw->addAndMakeVisible (custom);
    }

    auto* fxToggle = new juce::ToggleButton ("");
    fxToggle->setComponentID ("fxToggle");
    fxToggle->setToggleState (crtEnabled, juce::dontSendNotification);
    fxToggle->onClick = [safeThis, fxToggle]()
    {
        if (safeThis == nullptr || fxToggle == nullptr)
            return;

        safeThis->applyCrtState (fxToggle->getToggleState());
        safeThis->audioProcessor.setUiFxTailEnabled (safeThis->crtEnabled);
        safeThis->repaint();
    };
    aw->addAndMakeVisible (fxToggle);

    auto* fxLabel = addPopupLabel ("fxLabel", "GRAPHIC FX", labelFont);

    auto syncAndRepaintPopup = [safeThis, safeAw]()
    {
        if (safeThis == nullptr || safeAw == nullptr)
            return;

        syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
        layoutGraphicsPopupContent (*safeAw);
        safeAw->repaint();
    };

    auto applyPaletteAndRepaint = [safeThis]()
    {
        if (safeThis == nullptr)
            return;

        safeThis->applyActivePalette();
        safeThis->repaint();
    };

    defaultToggle->onClick = [safeThis, defaultToggle, customToggle, applyPaletteAndRepaint, syncAndRepaintPopup]() mutable
    {
        if (safeThis == nullptr || defaultToggle == nullptr || customToggle == nullptr)
            return;

        safeThis->useCustomPalette = false;
        safeThis->audioProcessor.setUiUseCustomPalette (safeThis->useCustomPalette);
        defaultToggle->setToggleState (true, juce::dontSendNotification);
        customToggle->setToggleState (false, juce::dontSendNotification);
        applyPaletteAndRepaint();
        syncAndRepaintPopup();
    };

    customToggle->onClick = [safeThis, defaultToggle, customToggle, applyPaletteAndRepaint, syncAndRepaintPopup]() mutable
    {
        if (safeThis == nullptr || defaultToggle == nullptr || customToggle == nullptr)
            return;

        safeThis->useCustomPalette = true;
        safeThis->audioProcessor.setUiUseCustomPalette (safeThis->useCustomPalette);
        defaultToggle->setToggleState (false, juce::dontSendNotification);
        customToggle->setToggleState (true, juce::dontSendNotification);
        applyPaletteAndRepaint();
        syncAndRepaintPopup();
    };

    if (defaultLabel != nullptr && defaultToggle != nullptr)
        defaultLabel->onClick = [defaultToggle]() { defaultToggle->triggerClick(); };

    if (customLabel != nullptr && customToggle != nullptr)
        customLabel->onClick = [customToggle]() { customToggle->triggerClick(); };

    if (fxLabel != nullptr && fxToggle != nullptr)
        fxLabel->onClick = [fxToggle]() { fxToggle->triggerClick(); };

    for (int i = 0; i < 2; ++i)
    {
        if (auto* customSwatch = dynamic_cast<PopupSwatchButton*> (aw->findChildWithID ("customSwatch" + juce::String (i))))
        {
            customSwatch->onLeftClick = [safeThis, safeAw, i]()
            {
                if (safeThis == nullptr)
                    return;

                auto& rng = juce::Random::getSystemRandom();
                const auto randomColour = juce::Colour::fromRGB ((juce::uint8) rng.nextInt (256),
                                                                 (juce::uint8) rng.nextInt (256),
                                                                 (juce::uint8) rng.nextInt (256));

                safeThis->customPalette[(size_t) i] = randomColour;
                safeThis->audioProcessor.setUiCustomPaletteColour (i, randomColour);
                if (safeThis->useCustomPalette)
                {
                    safeThis->applyActivePalette();
                    safeThis->repaint();
                }

                if (safeAw != nullptr)
                {
                    syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
                    layoutGraphicsPopupContent (*safeAw);
                    safeAw->repaint();
                }
            };

            customSwatch->onRightClick = [safeThis, safeAw, i]()
            {
                if (safeThis == nullptr)
                    return;

                const auto scheme = safeThis->activeScheme;

                auto* colorAw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
                colorAw->setLookAndFeel (&safeThis->lnf);
                colorAw->addTextEditor ("hex", colourToHexRgb (safeThis->customPalette[(size_t) i]), juce::String());

                if (auto* te = colorAw->getTextEditor ("hex"))
                    te->setInputFilter (new HexInputFilter(), true);

                colorAw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                colorAw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                styleAlertButtons (*colorAw, safeThis->lnf);

                applyPromptShellSize (*colorAw);
                layoutAlertWindowButtons (*colorAw);

                const juce::Font& kHexPromptFont = kBoldFont40();

                preparePromptTextEditor (*colorAw,
                                         "hex",
                                         scheme.bg,
                                         scheme.text,
                                         scheme.fg,
                                         kHexPromptFont,
                                         true,
                                         6);

                if (safeThis != nullptr)
                {
                    fitAlertWindowToEditor (*colorAw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
                    {
                        layoutAlertWindowButtons (a);
                        preparePromptTextEditor (a,
                                                 "hex",
                                                 scheme.bg,
                                                 scheme.text,
                                                 scheme.fg,
                                                 kHexPromptFont,
                                                 true,
                                                 6);
                    });

                    embedAlertWindowInOverlay (safeThis.getComponent(), colorAw, true);
                }
                else
                {
                    colorAw->centreAroundComponent (safeThis.getComponent(), colorAw->getWidth(), colorAw->getHeight());
                    bringPromptWindowToFront (*colorAw);
                    if (safeThis != nullptr && safeThis->tooltipWindow)
                        safeThis->tooltipWindow->toFront (true);
                    colorAw->repaint();
                }

                preparePromptTextEditor (*colorAw,
                                         "hex",
                                         scheme.bg,
                                         scheme.text,
                                         scheme.fg,
                                         kHexPromptFont,
                                         true,
                                         6);

                juce::Component::SafePointer<juce::AlertWindow> safeColorAw (colorAw);
                juce::MessageManager::callAsync ([safeColorAw]()
                {
                    if (safeColorAw == nullptr)
                        return;
                    bringPromptWindowToFront (*safeColorAw);
                    safeColorAw->repaint();
                });

                colorAw->enterModalState (true,
                    juce::ModalCallbackFunction::create ([safeThis, safeAw, colorAw, i] (int result) mutable
                    {
                        std::unique_ptr<juce::AlertWindow> killer (colorAw);
                        if (safeThis == nullptr)
                            return;

                        if (result != 1)
                            return;

                        juce::Colour parsed;
                        if (! tryParseHexColour (killer->getTextEditorContents ("hex"), parsed))
                            return;

                        safeThis->customPalette[(size_t) i] = parsed;
                        safeThis->audioProcessor.setUiCustomPaletteColour (i, parsed);
                        if (safeThis->useCustomPalette)
                        {
                            safeThis->applyActivePalette();
                            safeThis->repaint();
                        }

                        if (safeAw != nullptr)
                        {
                            syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
                            layoutGraphicsPopupContent (*safeAw);
                            safeAw->repaint();
                        }
                    }));
            };
        }
    }

    applyPromptShellSize (*aw);
    syncGraphicsPopupState (*aw, defaultPalette, customPalette, useCustomPalette);
    layoutGraphicsPopupContent (*aw);

    if (safeThis != nullptr)
    {
        fitAlertWindowToEditor (*aw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
        {
            syncGraphicsPopupState (a, defaultPalette, customPalette, useCustomPalette);
            layoutGraphicsPopupContent (a);
        });
    }
    if (safeThis != nullptr)
    {
        embedAlertWindowInOverlay (safeThis.getComponent(), aw);

        juce::MessageManager::callAsync ([safeAw, safeThis]()
        {
            if (safeAw == nullptr || safeThis == nullptr)
                return;

            safeAw->toFront (false);
            safeAw->repaint();
        });
    }
    else
    {
        aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
        bringPromptWindowToFront (*aw);
        aw->repaint();
    }

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([safeThis, aw] (int) mutable
        {
            std::unique_ptr<juce::AlertWindow> killer (aw);
            if (safeThis != nullptr)
                safeThis->setPromptOverlayActive (false);
        }));
}

//========================== Text helpers ==========================

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
        return cachedMidiDisplay;

    const double hz = freqSlider.getValue();

    if (hz >= kHzSwitchHz)
        return juce::String (hz / 1000.0, 2) + " kHz";

    return juce::String (hz, 2) + " Hz";
}

juce::String DisperserAudioProcessorEditor::getFreqTextShort() const
{
    if (cachedMidiDisplay.isNotEmpty() && ! freqSlider.isMouseButtonDown())
        return cachedMidiDisplay;

    const double hz = freqSlider.getValue();

    if (hz >= kHzSwitchHz)
        return juce::String (hz / 1000.0, 2) + "kHz";

    return juce::String (hz, 2) + "Hz";
}

juce::String DisperserAudioProcessorEditor::getShapeText() const
{
    const double v = juce::jlimit (0.0, 1.0, shapeSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt).toUpperCase() + "% SHAPE";
}

juce::String DisperserAudioProcessorEditor::getShapeTextShort() const
{
    const double v = juce::jlimit (0.0, 1.0, shapeSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt).toUpperCase() + "% SHP";
}

juce::String DisperserAudioProcessorEditor::getFeedbackText() const
{
    const double v = juce::jlimit (0.0, 1.0, feedbackSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt) + "% FEEDBACK";
}

juce::String DisperserAudioProcessorEditor::getFeedbackTextShort() const
{
    const double v = juce::jlimit (0.0, 1.0, feedbackSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt) + "% FBK";
}

juce::String DisperserAudioProcessorEditor::getModText() const
{
    const float mult = (float) modSliderToMultiplier (modSlider.getValue());
    if (std::abs (mult - 1.0f) < kMultEpsilon)
        return "X1 MOD";
    return "X" + juce::String (mult, 2) + " MOD";
}

juce::String DisperserAudioProcessorEditor::getModTextShort() const
{
    const float mult = (float) modSliderToMultiplier (modSlider.getValue());
    if (std::abs (mult - 1.0f) < kMultEpsilon)
        return "X1";
    return "X" + juce::String (mult, 2);
}

juce::String DisperserAudioProcessorEditor::getMixText() const
{
    const double v = juce::jlimit (0.0, 1.0, mixSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt) + "% MIX";
}

juce::String DisperserAudioProcessorEditor::getMixTextShort() const
{
    const double v = juce::jlimit (0.0, 1.0, mixSlider.getValue());
    const int pctInt = (int) std::lround (v * 100.0);
    return juce::String (pctInt) + "% MX";
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
    constexpr const char* kShapeLegendInt    = "100";

    constexpr const char* kFeedbackLegendFull  = "100% FEEDBACK";
    constexpr const char* kFeedbackLegendShort = "100% FBK";
    constexpr const char* kFeedbackLegendInt   = "100";

    constexpr const char* kModLegendFull  = "X4.00 MOD";
    constexpr const char* kModLegendShort = "X4.00";
    constexpr const char* kModLegendInt   = "X4";

    constexpr const char* kMixLegendFull  = "100% MIX";
    constexpr const char* kMixLegendShort = "100% MX";
    constexpr const char* kMixLegendInt   = "100";

    constexpr int kValueAreaHeightPx = 44;
    constexpr int kValueAreaRightMarginPx = 24;
    constexpr int kToggleLabelGapPx = 4;
    constexpr int kResizerCornerPx = 22;
    constexpr int kToggleBoxPx = 72;
    constexpr int kMinToggleBlocksGapPx = 10;
}

DisperserAudioProcessorEditor::HorizontalLayoutMetrics
DisperserAudioProcessorEditor::buildHorizontalLayout (int editorW, int valueColW)
{
    HorizontalLayoutMetrics m;
    m.barW = (int) std::round (editorW * 0.455);
    m.valuePad = (int) std::round (editorW * 0.02);
    m.valueW = valueColW;
    m.contentW = m.barW + m.valuePad + m.valueW;
    m.leftX = juce::jmax (6, (editorW - m.contentW) / 2);
    return m;
}

DisperserAudioProcessorEditor::VerticalLayoutMetrics
DisperserAudioProcessorEditor::buildVerticalLayout (int editorH, int biasY)
{
    VerticalLayoutMetrics m;
    m.rhythm = juce::jlimit (6, 16, (int) std::round (editorH * 0.018));
    const int nominalBarH = juce::jlimit (14, 120, m.rhythm * 6);
    const int nominalGapY = juce::jmax (4, m.rhythm * 4);

    m.titleH = juce::jlimit (24, 56, m.rhythm * 4);
    m.titleAreaH = m.titleH + 4;
    const int computedTitleTopPad = 6 + biasY;
    m.titleTopPad = (computedTitleTopPad > 8) ? computedTitleTopPad : 8;
    const int titleGap = m.titleTopPad;
    m.topMargin = m.titleTopPad + m.titleAreaH + titleGap;
    m.betweenSlidersAndButtons = juce::jmax (8, m.rhythm * 2);
    m.bottomMargin = m.titleTopPad;

    m.box = juce::jlimit (40, kToggleBoxPx, (int) std::round (editorH * 0.085));
    m.btnY = editorH - m.bottomMargin - m.box;
    m.availableForSliders = juce::jmax (40, m.btnY - m.betweenSlidersAndButtons - m.topMargin);

    const int nominalStack = 7 * nominalBarH + 6 * nominalGapY;
    const double stackScale = nominalStack > 0 ? juce::jmin (1.0, (double) m.availableForSliders / (double) nominalStack)
                                               : 1.0;

    m.barH = juce::jmax (14, (int) std::round (nominalBarH * stackScale));
    m.gapY = juce::jmax (4,  (int) std::round (nominalGapY * stackScale));

    auto stackHeight = [&]() { return 7 * m.barH + 6 * m.gapY; };

    while (stackHeight() > m.availableForSliders && m.gapY > 4)
        --m.gapY;

    while (stackHeight() > m.availableForSliders && m.barH > 14)
        --m.barH;

    m.topY = m.topMargin;
    return m;
}

void DisperserAudioProcessorEditor::updateCachedLayout()
{
    cachedHLayout_ = buildHorizontalLayout (getWidth(), getTargetValueColumnWidth());
    cachedVLayout_ = buildVerticalLayout (getHeight(), kLayoutVerticalBiasPx);

    const juce::Slider* sliders[7] = { &freqSlider, &modSlider, &feedbackSlider, &amountSlider, &seriesSlider, &shapeSlider, &mixSlider };

    for (int i = 0; i < 7; ++i)
    {
        const auto& bb = sliders[i]->getBounds();
        const int valueX = bb.getRight() + cachedHLayout_.valuePad;
        const int maxW = juce::jmax (0, getWidth() - valueX - kValueAreaRightMarginPx);
        const int vw   = juce::jmin (cachedHLayout_.valueW, maxW);
        const int y    = bb.getCentreY() - (kValueAreaHeightPx / 2);
        cachedValueAreas_[(size_t) i] = { valueX, y, juce::jmax (0, vw), kValueAreaHeightPx };
    }
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

    const int feedbackMaxW = juce::jmax (stringWidth (font, kFeedbackLegendFull),
                                         juce::jmax (stringWidth (font, kFeedbackLegendShort),
                                                     stringWidth (font, kFeedbackLegendInt)));

    const int modMaxW = juce::jmax (stringWidth (font, kModLegendFull),
                                    juce::jmax (stringWidth (font, kModLegendShort),
                                                stringWidth (font, kModLegendInt)));

    const int mixMaxW = juce::jmax (stringWidth (font, kMixLegendFull),
                                    juce::jmax (stringWidth (font, kMixLegendShort),
                                                stringWidth (font, kMixLegendInt)));

    const int maxW = juce::jmax (juce::jmax (amountMaxW, seriesMaxW),
                                 juce::jmax (juce::jmax (freqMaxW, shapeMaxW),
                                             juce::jmax (juce::jmax (feedbackMaxW, modMaxW), mixMaxW)));

    const int desired = maxW + 16;
    const int minW = 90;
    const int maxAllowed = juce::jmax (minW, getWidth() / 3);
    cachedValueColumnWidth = juce::jlimit (minW, maxAllowed, desired);
    cachedValueColumnWidthKey = key;
    return cachedValueColumnWidth;
}

//========================== Hit areas ==========================

juce::Rectangle<int> DisperserAudioProcessorEditor::getValueAreaFor (const juce::Rectangle<int>& barBounds) const
{
    const auto layout = buildHorizontalLayout (getWidth(), getTargetValueColumnWidth());

    const int valueX = barBounds.getRight() + layout.valuePad;
    const int maxW = juce::jmax (0, getWidth() - valueX - kValueAreaRightMarginPx);
    const int valueW = juce::jmin (layout.valueW, maxW);

    const int y = barBounds.getCentreY() - (kValueAreaHeightPx / 2);
    return { valueX, y, juce::jmax (0, valueW), kValueAreaHeightPx };
}

juce::Slider* DisperserAudioProcessorEditor::getSliderForValueAreaPoint (juce::Point<int> p)
{
    if (getValueAreaFor (amountSlider.getBounds()).contains (p))
        return &amountSlider;

    if (getValueAreaFor (seriesSlider.getBounds()).contains (p))
        return &seriesSlider;

    if (getValueAreaFor (freqSlider.getBounds()).contains (p))
        return &freqSlider;

    if (getValueAreaFor (shapeSlider.getBounds()).contains (p))
        return &shapeSlider;

    if (getValueAreaFor (feedbackSlider.getBounds()).contains (p))
        return &feedbackSlider;

    if (getValueAreaFor (modSlider.getBounds()).contains (p))
        return &modSlider;

    if (getValueAreaFor (mixSlider.getBounds()).contains (p))
        return &mixSlider;

    return nullptr;
}

namespace
{
    int getToggleVisualBoxSidePx (const juce::Component& button)
    {
        const int h = button.getHeight();
        return juce::jlimit (14, juce::jmax (14, h - 2), (int) std::lround ((double) h * 0.65));
    }

    int getToggleVisualBoxLeftPx (const juce::Component& button)
    {
        return button.getX() + 2;
    }

    juce::Rectangle<int> makeToggleLabelArea (const juce::Component& button,
                                              int collisionRight,
                                              const juce::String& fullLabel,
                                              const juce::String& shortLabel)
    {
        const auto b = button.getBounds();
        const int visualRight = getToggleVisualBoxLeftPx (button) + getToggleVisualBoxSidePx (button);
        const int x = visualRight + kToggleLabelGapPx;

        const auto& labelFont = kBoldFont40();
        const int fullW  = stringWidth (labelFont, fullLabel) + 2;
        const int shortW = stringWidth (labelFont, shortLabel) + 2;
        const int maxW   = juce::jmax (0, collisionRight - x);

        const int w = (fullW <= maxW) ? fullW : juce::jmin (shortW, maxW);
        return { x, b.getY(), w, b.getHeight() };
    }

    juce::String chooseToggleLabel (const juce::Component& button,
                                   int collisionRight,
                                   const juce::String& fullLabel,
                                   const juce::String& shortLabel)
    {
        const int visualRight = getToggleVisualBoxLeftPx (button) + getToggleVisualBoxSidePx (button);
        const int x = visualRight + kToggleLabelGapPx;
        const auto& labelFont = kBoldFont40();
        const int fullW = stringWidth (labelFont, fullLabel) + 2;
        return (fullW <= juce::jmax (0, collisionRight - x)) ? fullLabel : shortLabel;
    }
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getInvLabelArea() const
{
    return makeToggleLabelArea (invButton, midiButton.getX() - kToggleLegendCollisionPadPx, "INVERT", "INV");
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getMidiLabelArea() const
{
    return makeToggleLabelArea (midiButton, cachedValueAreas_[0].getRight(), "MIDI", "MD");
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getInfoIconArea() const
{
    const auto amountValueArea = getValueAreaFor (amountSlider.getBounds());
    const int contentRight = amountValueArea.getRight();
    const auto verticalLayout = buildVerticalLayout (getHeight(), kLayoutVerticalBiasPx);
    const int titleH = verticalLayout.titleH;
    const int titleY = verticalLayout.titleTopPad;
    const int titleAreaH = verticalLayout.titleAreaH;
    const int size = juce::jlimit (20, 36, titleH);

    const int x = contentRight - size;
    const int y = titleY + juce::jmax (0, (titleAreaH - size) / 2);
    return { x, y, size, size };
}

void DisperserAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    lastUserInteractionMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    const auto p = e.getEventRelativeTo (this).getPosition();

    if (e.mods.isPopupMenu())
    {
        if (auto* slider = getSliderForValueAreaPoint (p))
        {
            openNumericEntryPopupForSlider (*slider);
            return;
        }
    }

    if (getInfoIconArea().contains (p))
    {
        openInfoPopup();
        return;
    }

    if (getInvLabelArea().contains (p))
    {
        invButton.setToggleState (! invButton.getToggleState(), juce::sendNotificationSync);
        return;
    }

    if (getMidiLabelArea().contains (p) || midiChannelDisplay.getBounds().contains (p))
    {
        if (e.mods.isPopupMenu())
            openMidiChannelPrompt();
        else
            midiButton.setToggleState (! midiButton.getToggleState(), juce::sendNotificationSync);
        return;
    }
}

void DisperserAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    (void) e;
    lastUserInteractionMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
}

void DisperserAudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (auto* slider = getSliderForValueAreaPoint (p))
    {
        if (slider == &amountSlider)          slider->setValue (kDefaultAmount, juce::sendNotificationSync);
        else if (slider == &seriesSlider)     slider->setValue (kDefaultSeries, juce::sendNotificationSync);
        else if (slider == &freqSlider)       slider->setValue (kDefaultFreq, juce::sendNotificationSync);
        else if (slider == &shapeSlider)      slider->setValue (kDefaultShape, juce::sendNotificationSync);
        else if (slider == &feedbackSlider)   slider->setValue (kDefaultFeedback, juce::sendNotificationSync);
        else if (slider == &modSlider)        slider->setValue (kDefaultMod, juce::sendNotificationSync);
        else if (slider == &mixSlider)        slider->setValue (kDefaultMix, juce::sendNotificationSync);
        return;
    }
}

//==============================================================================

void DisperserAudioProcessorEditor::paint (juce::Graphics& g)
{
    const int W = getWidth();
    const auto& horizontalLayout = cachedHLayout_;
    const auto& verticalLayout   = cachedVLayout_;

    const auto scheme = activeScheme;

    g.fillAll (scheme.bg);
    g.setColour (scheme.text);

    constexpr float baseFontPx = 40.0f;
    constexpr float minFontPx  = 18.0f;
    constexpr float fullShrinkFloor = baseFontPx * 0.75f;
    g.setFont (kBoldFont40());

    auto tryDrawLegend = [&] (const juce::Rectangle<int>& area,
                              const juce::String& text,
                              float shrinkFloor) -> bool
    {
        auto t = text.trim();
        if (t.isEmpty() || area.getWidth() <= 2 || area.getHeight() <= 2)
            return false;

        const int split = t.lastIndexOfChar (' ');
        if (split <= 0 || split >= t.length() - 1)
        {
            g.setFont (kBoldFont40());
            return drawIfFitsWithOptionalShrink (g, area, t, baseFontPx, shrinkFloor);
        }

        const auto value  = t.substring (0, split).trimEnd();
        const auto suffix = t.substring (split + 1).trimStart();

        g.setFont (kBoldFont40());
        if (drawValueWithRightAlignedSuffix (g, area, value, suffix, false,
                                              baseFontPx, shrinkFloor))
        {
            g.setColour (scheme.text);
            return true;
        }
        return false;
    };

    auto drawLegendForMode = [&] (const juce::Rectangle<int>& area,
                                  const juce::String& fullLegend,
                                  const juce::String& shortLegend,
                                  const juce::String& intOnlyLegend)
    {
        if (tryDrawLegend (area, fullLegend, fullShrinkFloor))
            return;

        if (tryDrawLegend (area, shortLegend, minFontPx))
            return;

        g.setFont (kBoldFont40());
        drawValueNoEllipsis (g, area, intOnlyLegend, juce::String(), intOnlyLegend, baseFontPx, minFontPx);
        g.setColour (scheme.text);
    };

    {
        const int titleH = verticalLayout.titleH;

        const int barW = horizontalLayout.barW;
        const int contentW = horizontalLayout.contentW;
        const int leftX = horizontalLayout.leftX;

        const int titleX = juce::jlimit (0, juce::jmax (0, W - 1), leftX);
        const int titleW = juce::jmax (0, juce::jmin (contentW, W - titleX));
        const int titleY = verticalLayout.titleTopPad;

        auto titleFont = g.getCurrentFont();
        titleFont.setHeight ((float) titleH);
        g.setFont (titleFont);

        const auto titleArea = juce::Rectangle<int> (titleX, titleY, titleW, titleH + kTitleAreaExtraHeightPx);
        const juce::String titleText ("DISP-TR");

        g.drawText (titleText, titleArea.getX(), titleArea.getY(), titleArea.getWidth(), titleArea.getHeight(), juce::Justification::left, false);

        const auto infoIconArea = getInfoIconArea();
        const int titleRightLimit = infoIconArea.getX() - kTitleRightGapToInfoPx;
        const int titleMaxW = juce::jmax (0, titleRightLimit - titleArea.getX());
        const int titleBaseW = stringWidth (titleFont, titleText);
        const int originalTitleLimitW = juce::jmax (0, juce::jmin (titleW, barW));
        const bool originalWouldClipTitle = titleBaseW > originalTitleLimitW;

        if (titleMaxW > 0 && (originalWouldClipTitle || titleBaseW > titleMaxW))
        {
            auto fittedTitleFont = titleFont;
            fittedTitleFont.setHorizontalScale (1.0f);
            const float titleMinScale = juce::jlimit (0.4f, 1.0f, 12.0f / (float) titleH);
            for (float s = 1.0f; s >= titleMinScale; s -= 0.025f)
            {
                fittedTitleFont.setHorizontalScale (s);
                if (stringWidth (fittedTitleFont, titleText) <= titleMaxW)
                    break;
            }

            g.setColour (scheme.text);
            g.setFont (fittedTitleFont);
            g.drawText (titleText, titleArea.getX(), titleArea.getY(), titleMaxW, titleArea.getHeight(), juce::Justification::left, false);
        }

        g.setColour (scheme.text);

        auto versionFont = juce::Font (juce::FontOptions (juce::jmax (10.0f, (float) titleH * UiMetrics::versionFontRatio)).withStyle ("Bold"));
        g.setFont (versionFont);

        const int versionH = juce::jlimit (10, infoIconArea.getHeight(), (int) std::round ((double) infoIconArea.getHeight() * UiMetrics::versionHeightRatio));
        const int versionY = infoIconArea.getBottom() - versionH;

        const int desiredVersionW = juce::jlimit (28, 64, (int) std::round ((double) infoIconArea.getWidth() * UiMetrics::versionDesiredWidthRatio));
        const int versionRight = infoIconArea.getX() - kVersionGapPx;
        const int versionLeftLimit = titleArea.getX();
        const int versionX = juce::jmax (versionLeftLimit, versionRight - desiredVersionW);
        const int versionW = juce::jmax (0, versionRight - versionX);

        if (versionW > 0)
            g.drawText (juce::String ("v") + InfoContent::version, versionX, versionY, versionW, versionH,
                juce::Justification::bottomRight, false);

        g.setFont (kBoldFont40());
    }

    {
        const juce::String* fullTexts[7]  = { &cachedFreqTextHz, &cachedModTextFull,
                                               &cachedFeedbackTextFull, &cachedAmountTextFull,
                                               &cachedSeriesTextFull, &cachedShapeTextFull,
                                               &cachedMixTextFull };
        const juce::String* shortTexts[7] = { &cachedFreqTextShort, &cachedModTextShort,
                                               &cachedFeedbackTextShort, &cachedAmountTextShort,
                                               &cachedSeriesTextShort, &cachedShapeTextShort,
                                               &cachedMixTextShort };
        const juce::String intTexts[7] = {
            cachedFreqIntOnly,
            cachedModIntOnly,
            cachedFeedbackIntOnly,
            juce::String ((int) amountSlider.getValue()),
            juce::String ((int) seriesSlider.getValue()),
            cachedShapeIntOnly,
            cachedMixIntOnly
        };

        for (int i = 0; i < 7; ++i)
            drawLegendForMode (cachedValueAreas_[(size_t) i], *fullTexts[i], *shortTexts[i], intTexts[i]);
    }

    {
        const auto& labelFont = kBoldFont40();
        g.setFont (labelFont);

        const int invCR  = midiButton.getX() - kToggleLegendCollisionPadPx;
        const int midiCR = cachedValueAreas_[0].getRight();

        auto drawToggleLegend = [&] (const juce::Rectangle<int>& labelArea,
                                     const juce::String& labelText,
                                     int noCollisionRight)
        {
            const int safeW = juce::jmax (0, noCollisionRight - labelArea.getX());
            auto snapEven = [] (int v) { return v & ~1; };
            const int ax = snapEven (labelArea.getX());
            const int ay = snapEven (labelArea.getY());
            const int aw = snapEven (safeW);
            const int ah = labelArea.getHeight();
            const auto drawArea = juce::Rectangle<int> (ax, ay, aw, ah);

            g.drawText (labelText, drawArea.getX(), drawArea.getY(), drawArea.getWidth(), drawArea.getHeight(), juce::Justification::left, true);
        };

        drawToggleLegend (getInvLabelArea(), chooseToggleLabel (invButton, invCR, "INVERT", "INV"), invCR);
        drawToggleLegend (getMidiLabelArea(), chooseToggleLabel (midiButton, midiCR, "MIDI", "MD"), midiCR);
    }
    g.setColour (scheme.text);

    {
        if (cachedInfoGearPath.isEmpty())
            updateInfoIconCache();

        g.setColour (scheme.text);
        g.fillPath (cachedInfoGearPath);
        g.strokePath (cachedInfoGearPath, juce::PathStrokeType (1.0f));

        g.setColour (scheme.bg);
        g.fillEllipse (cachedInfoGearHole);
    }
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
    const auto verticalLayout = buildVerticalLayout (H, kLayoutVerticalBiasPx);

    freqSlider.setBounds      (horizontalLayout.leftX, verticalLayout.topY + 0 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);
    modSlider.setBounds       (horizontalLayout.leftX, verticalLayout.topY + 1 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);
    feedbackSlider.setBounds  (horizontalLayout.leftX, verticalLayout.topY + 2 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);
    amountSlider.setBounds    (horizontalLayout.leftX, verticalLayout.topY + 3 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);
    seriesSlider.setBounds    (horizontalLayout.leftX, verticalLayout.topY + 4 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);
    shapeSlider.setBounds     (horizontalLayout.leftX, verticalLayout.topY + 5 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);
    mixSlider.setBounds       (horizontalLayout.leftX, verticalLayout.topY + 6 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);

    const int buttonAreaX = horizontalLayout.leftX;
    const int buttonAreaW = horizontalLayout.contentW;

    const int toggleVisualSide = juce::jlimit (14,
                                               juce::jmax (14, verticalLayout.box - 2),
                                               (int) std::lround ((double) verticalLayout.box * 0.65));
    const int toggleHitW = toggleVisualSide + 6;

    const int valueStartX = horizontalLayout.leftX + horizontalLayout.barW + horizontalLayout.valuePad;
    const int invAnchorX = horizontalLayout.leftX;
    const int midiAnchorX = valueStartX;

    int invBlockX = invAnchorX;
    int midiBlockX = midiAnchorX;

    const int midiMinX = juce::jmax (midiAnchorX, invBlockX + toggleHitW + kMinToggleBlocksGapPx);
    const int midiMaxX = buttonAreaX + buttonAreaW - toggleHitW;
    if (midiMinX <= midiMaxX)
        midiBlockX = juce::jlimit (midiMinX, midiMaxX, midiBlockX);
    else
        midiBlockX = midiMaxX;

    invButton.setBounds (invBlockX, verticalLayout.btnY, toggleHitW, verticalLayout.box);
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

