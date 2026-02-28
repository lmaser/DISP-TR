// PluginEditor.cpp
#include "PluginEditor.h"
#include <functional>
#include <unordered_map>
#include <fstream>

//========================== Overflow helpers ==========================
// Helpers para medir texto y aplicar truncados según prioridad de formatos.

static std::unordered_map<std::string, int>& getStringWidthCache()
{
    static thread_local std::unordered_map<std::string, int> widthCache;
    return widthCache;
}

static int stringWidth (const juce::Font& font, const juce::String& s)
{
    auto& widthCache = getStringWidthCache();

    if (s.isEmpty())
        return 0;

    if (widthCache.size() > 2048)
        widthCache.clear();

    const int h100 = (int) std::round (font.getHeight() * 100.0f);
    std::string key;
    key.reserve (32 + (size_t) s.length());
    key += std::to_string (h100);
    key += "|";
    key += font.getTypefaceName().toStdString();
    key += font.isBold() ? "|b1" : "|b0";
    key += font.isItalic() ? "|i1" : "|i0";
    key += "|";
    key += s.toStdString();

    if (const auto it = widthCache.find (key); it != widthCache.end())
        return it->second;

    juce::GlyphArrangement ga;
    ga.addLineOfText (font, s, 0.0f, 0.0f);
    const int width = (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
    widthCache.emplace (std::move (key), width);
    return width;
}

struct GraphicsPromptLayout
{
    static constexpr int toggleBox = 34;
    static constexpr int toggleGap = 10;
    static constexpr int swatchSize = 40;
    static constexpr int swatchGap = 8;
    static constexpr int columnGap = 28;
    static constexpr int titleHeight = 24;
    static constexpr int titleToModeGap = 14;
    static constexpr int modeToSwatchesGap = 14;
};

namespace UiMetrics
{
    constexpr float tickBoxOuterScale = 2.0f;
    constexpr float tickBoxHorizontalBiasRatio = 0.1171875f;
    constexpr float tickBoxInnerInsetRatio = 0.25f;

    constexpr int tooltipMinWidth = 120;
    constexpr int tooltipMinHeight = 38;
    constexpr float tooltipHeightScale = 1.5f;
    constexpr float tooltipAnchorXRatio = 0.42f;
    constexpr float tooltipAnchorYRatio = 0.58f;
    constexpr float tooltipParentMarginRatio = 0.11f;
    constexpr float tooltipWidthPadFontRatio = 0.8f;
    constexpr float tooltipTextInsetXRatio = 0.21f;
    constexpr float tooltipTextInsetYRatio = 0.05f;

    constexpr float versionFontRatio = 0.42f;
    constexpr float versionHeightRatio = 0.62f;
    constexpr float versionDesiredWidthRatio = 1.9f;
}

namespace UiStateKeys
{
    constexpr const char* editorWidth = "uiEditorWidth";
    constexpr const char* editorHeight = "uiEditorHeight";
    constexpr const char* useCustomPalette = "uiUseCustomPalette";
    constexpr const char* fxTailEnabled = "uiFxTailEnabled";
    constexpr std::array<const char*, 4> customPalette {
        "uiCustomPalette0",
        "uiCustomPalette1",
        "uiCustomPalette2",
        "uiCustomPalette3"
    };
}

static void dismissEditorOwnedModalPrompts (juce::LookAndFeel& editorLookAndFeel);

static void dismissEditorOwnedModalPrompts (juce::LookAndFeel& editorLookAndFeel)
{
    for (int i = juce::Component::getNumCurrentlyModalComponents() - 1; i >= 0; --i)
    {
        auto* modal = juce::Component::getCurrentlyModalComponent (i);
        auto* alertWindow = dynamic_cast<juce::AlertWindow*> (modal);

        if (alertWindow == nullptr)
            continue;

        if (&alertWindow->getLookAndFeel() != &editorLookAndFeel)
            continue;

        alertWindow->exitModalState (0);
    }
}

static void bringPromptWindowToFront (juce::AlertWindow& aw)
{
    aw.setAlwaysOnTop (true);
    aw.toFront (true);
}

// Helper: embed an AlertWindow in the editor overlay and center it.
// Preserves previous behaviour; refactor to avoid duplication.
void embedAlertWindowInOverlay (DisperserAudioProcessorEditor* editor,
                                juce::AlertWindow* aw,
                                bool bringTooltip = false)
{
    if (editor == nullptr || aw == nullptr)
        return;

    editor->setPromptOverlayActive (true);
    editor->promptOverlay.addAndMakeVisible (*aw);
    const int bx = juce::jmax (0, (editor->getWidth() - aw->getWidth()) / 2);
    const int by = juce::jmax (0, (editor->getHeight() - aw->getHeight()) / 2);
    aw->setBounds (bx, by, aw->getWidth(), aw->getHeight());
    aw->toFront (false);
    if (bringTooltip && editor->tooltipWindow)
        editor->tooltipWindow->toFront (true);
    aw->repaint();
}

// Ensure an AlertWindow fits the editor width when embedded and optionally
// run a layout callback to reposition inner controls after a resize.
static void fitAlertWindowToEditor (juce::AlertWindow& aw,
                                    DisperserAudioProcessorEditor* editor,
                                    std::function<void(juce::AlertWindow&)> layoutCb = {})
{
    if (editor == nullptr)
        return;

    const int overlayPad = 12;
    const int availW = juce::jmax (120, editor->getWidth() - (overlayPad * 2));
    if (aw.getWidth() > availW)
    {
        aw.setSize (availW, juce::jmin (aw.getHeight(), editor->getHeight() - (overlayPad * 2)));
        if (layoutCb)
            layoutCb (aw);
    }
}

static void anchorEditorOwnedPromptWindows (DisperserAudioProcessorEditor& editor,
                                            juce::LookAndFeel& editorLookAndFeel)
{
    for (int i = juce::Component::getNumCurrentlyModalComponents() - 1; i >= 0; --i)
    {
        auto* modal = juce::Component::getCurrentlyModalComponent (i);
        auto* alertWindow = dynamic_cast<juce::AlertWindow*> (modal);

        if (alertWindow == nullptr)
            continue;

        if (&alertWindow->getLookAndFeel() != &editorLookAndFeel)
            continue;

        alertWindow->centreAroundComponent (&editor, alertWindow->getWidth(), alertWindow->getHeight());
        bringPromptWindowToFront (*alertWindow);
    }
}

static juce::Font makeOverlayDisplayFont()
{
    return juce::Font (juce::FontOptions (28.0f).withStyle ("Bold"));
}

static void drawOverlayPanel (juce::Graphics& g,
                              juce::Rectangle<int> bounds,
                              juce::Colour background,
                              juce::Colour outline)
{
    g.setColour (background);
    g.fillRect (bounds);

    g.setColour (outline);
    g.drawRect (bounds, 1);
}

static juce::Colour lerpColourStops (const std::array<juce::Colour, 2>& gradient, float t)
{
    return gradient[0].interpolatedWith (gradient[1], juce::jlimit (0.0f, 1.0f, t));
}

static bool isAbsoluteGradientEndpoint (const juce::Colour& c,
                                        const std::array<juce::Colour, 2>& gradient)
{
    const auto argb = c.getARGB();
    return argb == gradient[0].getARGB() || argb == gradient[1].getARGB();
}

static void parseTailTuning (const juce::String& tuning,
                             int& trimTailCount,
                             float& repeatScale)
{
    trimTailCount = 0;
    repeatScale = -1.0f;

    const auto t = tuning.trim();
    if (t.isEmpty())
        return;

    if (t.endsWithChar ('%'))
    {
        const auto number = t.dropLastCharacters (1).trim();
        const double pct = number.getDoubleValue();
        if (pct >= 0.0 && pct <= 100.0)
            repeatScale = (float) (pct / 100.0);
        return;
    }

    const int v = t.getIntValue();
    if (v < 0)
        trimTailCount = -v;
}

static float parseOptionalPercent01 (const juce::String& percentageText)
{
    const auto t = percentageText.trim();
    if (t.isEmpty())
        return -1.0f;

    juce::String number = t;
    if (number.endsWithChar ('%'))
        number = number.dropLastCharacters (1).trim();

    const double v = number.getDoubleValue();
    if (v < 0.0 || v > 100.0)
        return -1.0f;

    return (float) (v / 100.0);
}

static juce::String formatBarFrequencyHzText (double hz)
{
    const double safeHz = juce::jmax (0.0, hz);
    return juce::String (safeHz, 3).toUpperCase() + " HZ";
}

static void drawTextWithRepeatedLastCharGradient (juce::Graphics& g,
                                                  const juce::Rectangle<int>& area,
                                                  const juce::String& sourceText,
                                                  int horizontalSpacePx,
                                                  const std::array<juce::Colour, 2>& gradient,
                                                  int noCollisionRightX = -1,
                                                  const juce::String& tailTuning = juce::String(),
                                                  const juce::String& shrinkPerCharPercent = juce::String(),
                                                  const juce::String& tailVerticalMode = juce::String(),
                                                  const juce::String& referenceCharIndex = juce::String(),
                                                  const juce::String& overlapPercent = juce::String())
{
    constexpr int kMaxTailCharsDrawn = 20;
    constexpr float kMinTailCharPx = 3.0f;

    if (area.getWidth() <= 0 || area.getHeight() <= 0)
        return;

    juce::String text = sourceText.toUpperCase().trim();

    int trimTailCount = 0;
    float repeatScale = -1.0f;
    parseTailTuning (tailTuning, trimTailCount, repeatScale);

    if (text.isEmpty())
        return;

    const auto font = g.getCurrentFont();
    int maxWidth = juce::jmin (area.getWidth(), juce::jmax (0, horizontalSpacePx));
    if (noCollisionRightX >= 0)
        maxWidth = juce::jmin (maxWidth, juce::jmax (0, noCollisionRightX - area.getX()));

    if (maxWidth <= 0)
        return;

    const int baseW = stringWidth (font, text);

    g.setColour (gradient[0]);
    g.drawText (text, area.getX(), area.getY(), juce::jmin (baseW, maxWidth), area.getHeight(), juce::Justification::left, false);

    if (baseW >= maxWidth)
        return;

    const juce::juce_wchar lastChar = text[text.length() - 1];
    juce::juce_wchar selectedChar = lastChar;
    const auto refIdxText = referenceCharIndex.trim();
    if (refIdxText.isNotEmpty())
    {
        const int idx = refIdxText.getIntValue();
        if (idx >= 0 && idx < text.length())
            selectedChar = text[idx];
    }

    juce::String tailChar;
    tailChar += selectedChar;
    const float shrinkStep01 = parseOptionalPercent01 (shrinkPerCharPercent);
    const bool useShrink = (shrinkStep01 >= 0.0f);
    const auto verticalMode = tailVerticalMode.trim().toLowerCase();
    const float overlap01 = parseOptionalPercent01 (overlapPercent);
    const float overlap = juce::jlimit (0.0f, 1.0f, overlap01 < 0.0f ? 0.0f : overlap01);
    const float advanceFactor = 1.0f - overlap;

    const float baseFontH = font.getHeight();
    auto scaleForIndex = [&] (int index1Based)
    {
        if (! useShrink)
            return 1.0f;

        const float s = 1.0f - (shrinkStep01 * (float) index1Based);
        return juce::jmax (0.1f, s);
    };

    const int availableTailW = juce::jmax (0, maxWidth - baseW);
    juce::Array<float> xPositions;
    juce::Array<int> widths;

    float cursorX = 0.0f;
    float maxRight = 0.0f;

    for (int i = 1; i <= kMaxTailCharsDrawn; ++i)
    {
        auto fi = font;
        fi.setHeight (baseFontH * scaleForIndex (i));
        const int wi = stringWidth (fi, tailChar);
        if (fi.getHeight() < kMinTailCharPx || wi < (int) std::ceil (kMinTailCharPx)
            || wi <= 0)
            break;

        const float x = cursorX;
        const float right = x + (float) wi;
        if (right > (float) availableTailW + 1.0f)
            break;

        xPositions.add (x);
        widths.add (wi);
        maxRight = juce::jmax (maxRight, right);
        cursorX += (float) wi * advanceFactor;
    }

    int repeatCount = xPositions.size();
    repeatCount = juce::jmin (repeatCount, kMaxTailCharsDrawn);

    if (repeatScale >= 0.0f)
        repeatCount = (int) std::floor ((double) repeatCount * (double) repeatScale);

    if (trimTailCount > 0)
        repeatCount = juce::jmax (0, repeatCount - trimTailCount);

    if (repeatCount <= 1)
        return;

    const int baseBaselineY = area.getY()
                            + (int) std::round ((area.getHeight() - font.getHeight()) * 0.5f)
                            + (int) std::round (font.getAscent());

    // Draw from the end to the beginning so early indices stay visually on top.
    int drawableCount = 0;
    for (int i = repeatCount - 1; i >= 0; --i)
    {
        auto fi = font;
        fi.setHeight (baseFontH * scaleForIndex (i + 1));
        const int wi = juce::jmax (1, widths.getUnchecked (i));
        if (fi.getHeight() < kMinTailCharPx || wi < (int) std::ceil (kMinTailCharPx))
            continue;

        const float t = (float) (i + 1) / (float) juce::jmax (1, repeatCount);
        const auto c = lerpColourStops (gradient, t);
        if (isAbsoluteGradientEndpoint (c, gradient))
            continue;

        ++drawableCount;
    }

    if (drawableCount <= 1)
        return;

    for (int i = repeatCount - 1; i >= 0; --i)
    {
        auto fi = font;
        fi.setHeight (baseFontH * scaleForIndex (i + 1));
        const int wi = juce::jmax (1, widths.getUnchecked (i));
        if (fi.getHeight() < kMinTailCharPx || wi < (int) std::ceil (kMinTailCharPx))
            continue;

        const int x = area.getX() + baseW + juce::roundToInt (xPositions.getUnchecked (i));

        const float t = (float) (i + 1) / (float) juce::jmax (1, repeatCount);
        const auto c = lerpColourStops (gradient, t);
        if (isAbsoluteGradientEndpoint (c, gradient))
            continue;

        g.setColour (c);

        g.setFont (fi);

        int baselineY = baseBaselineY;
        if (verticalMode == "pyramid")
        {
            baselineY = area.getY()
                      + (int) std::round ((area.getHeight() - fi.getHeight()) * 0.5f)
                      + (int) std::round (fi.getAscent());
        }
        else if (verticalMode == "baseline")
        {
            baselineY = baseBaselineY;
        }

        g.drawSingleLineText (tailChar, x, baselineY, juce::Justification::left);
    }

    g.setFont (font);
}

static bool fits (juce::Graphics& g, const juce::String& s, int w)
{
    if (w <= 0) return false;
    return stringWidth (g.getCurrentFont(), s) <= w;
}

// Variante “solo medir” (sin Graphics): para decidir enable/disable en resized()
static bool fitsWithOptionalShrink_NoG (juce::Font font,
                                       const juce::String& text,
                                       int width,
                                       float baseFontPx,
                                       float shrinkFloorPx)
{
    if (width <= 0) return false;

    font.setHeight (baseFontPx);
    if (stringWidth (font, text) <= width)
        return true;

    for (float h = baseFontPx - 1.0f; h >= shrinkFloorPx; h -= 1.0f)
    {
        font.setHeight (h);
        if (stringWidth (font, text) <= width)
            return true;
    }
    return false;
}

static bool drawIfFitsWithOptionalShrink (juce::Graphics& g,
                                         const juce::Rectangle<int>& area,
                                         const juce::String& text,
                                         float baseFontPx,
                                         float shrinkFloorPx)
{
    auto font = g.getCurrentFont();
    font.setHeight (baseFontPx);
    g.setFont (font);

    if (fits (g, text, area.getWidth()))
    {
        g.drawText (text, area, juce::Justification::left, false);
        return true;
    }

    // pequeño shrink “suave” para intentar salvar unidades antes de abreviar
    for (float h = baseFontPx - 1.0f; h >= shrinkFloorPx; h -= 1.0f)
    {
        font.setHeight (h);
        g.setFont (font);
        if (fits (g, text, area.getWidth()))
        {
            g.drawText (text, area, juce::Justification::left, false);
            return true;
        }
    }

    return false;
}

static void drawValueNoEllipsis (juce::Graphics& g,
                                 const juce::Rectangle<int>& area,
                                 const juce::String& fullText,
                                 const juce::String& noUnitText,
                                 const juce::String& intOnlyText,
                                 float baseFontPx,
                                 float minFontPx)
{
    if (area.getWidth() <= 2 || area.getHeight() <= 2)
        return;

    const auto full = fullText.toUpperCase();
    const auto noU  = noUnitText.toUpperCase();
    const auto intl = intOnlyText.toUpperCase();

    const float softShrinkFloor = minFontPx;

    // FULL (con shrink suave)
    if (drawIfFitsWithOptionalShrink (g, area, full, baseFontPx, softShrinkFloor))
        return;

    // NO-UNIT (con shrink suave)
    if (noU.isNotEmpty() && drawIfFitsWithOptionalShrink (g, area, noU, baseFontPx, softShrinkFloor))
        return;

    // INT (normal)
    {
        auto font = g.getCurrentFont();
        font.setHeight (baseFontPx);
        g.setFont (font);

        if (intl.isNotEmpty() && fits (g, intl, area.getWidth()))
        {
            g.drawText (intl, area, juce::Justification::left, false);
            return;
        }

        // shrink solo para el entero
        for (float h = baseFontPx; h >= minFontPx; h -= 1.0f)
        {
            font.setHeight (h);
            g.setFont (font);
            if (intl.isNotEmpty() && fits (g, intl, area.getWidth()))
            {
                g.drawText (intl, area, juce::Justification::left, false);
                return;
            }
        }
    }
}

static bool drawValueWithRightAlignedSuffix (juce::Graphics& g,
                                             const juce::Rectangle<int>& area,
                                             const juce::String& valueText,
                                             const juce::String& suffixText,
                                             bool enableAutoMargin,
                                             float baseFontPx,
                                             float minFontPx,
                                             const std::array<juce::Colour, 2>* tailGradient = nullptr,
                                             bool tailFromSuffixToLeft = false,
                                             bool lowercaseTailChars = false,
                                             const juce::String& tailTuning = juce::String())
{
    constexpr int kMaxTailCharsDrawn = 20;
    constexpr float kMinTailCharPx = 3.0f;
    constexpr int kAutoMarginThresholdPx = 24;
    constexpr int kSingleDigitTailBudgetChars = 8;
    constexpr float kDefaultReverseShrinkStep01 = 0.20f;
    constexpr float kSingleDigitReverseShrinkStep01 = 0.10f;
    constexpr float kMinTailScale = 0.1f;
    constexpr float kTailOverlap01 = 0.0f;
    constexpr int kTailTokenChars = 1;

    if (area.getWidth() <= 2 || area.getHeight() <= 2)
        return false;

    const auto value = valueText.toUpperCase();
    const auto suffix = suffixText.toUpperCase();

    auto font = g.getCurrentFont();

    for (float h = baseFontPx; h >= minFontPx; h -= 1.0f)
    {
        font.setHeight (h);
        g.setFont (font);

        const int suffixW = stringWidth (font, suffix);
        const int valueW = stringWidth (font, value);
        const int gapW = juce::jmax (2, stringWidth (font, " "));

        const int totalW = valueW + (suffix.isNotEmpty() ? gapW : 0) + suffixW;
        if (totalW > area.getWidth())
            continue;

        const int suffixX = area.getRight() - suffixW;
        const int valueRight = suffixX - (suffix.isNotEmpty() ? gapW : 0);
        const int fullValueAreaW = juce::jmax (1, valueRight - area.getX());
        const int freeSpace = juce::jmax (0, fullValueAreaW - valueW);

        int valueX = area.getX();
        if (enableAutoMargin && freeSpace > kAutoMarginThresholdPx)
            valueX += freeSpace / 2;

        const int valueAreaW = juce::jmax (1, valueRight - valueX);

        auto computeSingleDigitReverseLaneWidth = [&]() -> int
        {
            const juce::String tailToken = suffix.substring (0, 1);
            const int tailTokenW = juce::jmax (1, stringWidth (font, tailToken));
            const int tailBudgetW = juce::jmax (tailTokenW * kSingleDigitTailBudgetChars,
                                                stringWidth (font, "SSSS"));
            const int desiredLaneW = valueW + juce::jmax (gapW, tailBudgetW);
            const int minLaneW = valueW + gapW;

            if (valueAreaW <= minLaneW)
                return valueAreaW;

            return juce::jlimit (minLaneW, valueAreaW, desiredLaneW);
        };

        int valueDrawW = valueAreaW;
        if (tailGradient != nullptr && tailFromSuffixToLeft && suffix.isNotEmpty() && value.length() <= 1)
            valueDrawW = computeSingleDigitReverseLaneWidth();

        if (tailGradient != nullptr && ! tailFromSuffixToLeft)
        {
            const auto valueArea = juce::Rectangle<int> (valueX, area.getY(), valueDrawW, area.getHeight());
            drawTextWithRepeatedLastCharGradient (g, valueArea, value, valueDrawW, *tailGradient, valueX + valueDrawW,
                                                  tailTuning, "20%", "pyramid");
            g.setColour ((*tailGradient)[0]);
        }
        else
        {
            g.drawText (value, valueX, area.getY(), valueDrawW, area.getHeight(), juce::Justification::left, false);
        }

        g.drawText (suffix, suffixX, area.getY(), suffixW, area.getHeight(), juce::Justification::left, false);

        if (tailGradient != nullptr && tailFromSuffixToLeft && suffix.isNotEmpty())
        {
            int trimTailCount = 0;
            float repeatScale = -1.0f;
            parseTailTuning (tailTuning, trimTailCount, repeatScale);

            const float shrinkStep01 = (value.length() <= 1) ? kSingleDigitReverseShrinkStep01
                                                              : kDefaultReverseShrinkStep01;
            const bool useShrink = (shrinkStep01 >= 0.0f);
            const float advanceFactor = 1.0f - kTailOverlap01;

            juce::String tailChar = suffix.substring (0, juce::jmin (kTailTokenChars, suffix.length()));
            if (lowercaseTailChars)
                tailChar = tailChar.toLowerCase();

            const int tailCharW = stringWidth (font, tailChar);
            if (tailCharW > 0)
            {
                const int leftLimit = valueX + valueW;
                const int rightLimit = suffixX;
                const int fittingSlackPx = juce::jmax (2, tailCharW / 2);
                const int leftLimitForFit = leftLimit - fittingSlackPx;

                auto scaleForIndex = [&] (int index1Based)
                {
                    if (! useShrink)
                        return 1.0f;

                    const float s = 1.0f - (shrinkStep01 * (float) index1Based);
                    return juce::jmax (kMinTailScale, s);
                };

                int repeatCount = 0;
                float usedTailW = 0.0f;
                for (int i = 1; i <= kMaxTailCharsDrawn; ++i)
                {
                    auto fi = font;
                    fi.setHeight (font.getHeight() * scaleForIndex (i));
                    const int wi = stringWidth (fi, tailChar);
                    const int xCandidate = rightLimit - (int) std::floor (usedTailW + 1.0e-6f) - wi;
                    if (fi.getHeight() < kMinTailCharPx || wi < (int) std::ceil (kMinTailCharPx)
                        || wi <= 0 || xCandidate < leftLimitForFit)
                        break;

                    usedTailW += (float) wi * advanceFactor;
                    ++repeatCount;
                }

                if (repeatScale >= 0.0f)
                    repeatCount = (int) std::floor ((double) repeatCount * (double) repeatScale);

                if (trimTailCount > 0)
                    repeatCount = juce::jmax (0, repeatCount - trimTailCount);

                repeatCount = juce::jmin (repeatCount, kMaxTailCharsDrawn);

                if (repeatCount > 1)
                {
                    std::array<int, (size_t) kMaxTailCharsDrawn> drawXs {};
                    std::array<int, (size_t) kMaxTailCharsDrawn> drawBaselines {};
                    int draw_count = 0;

                    float consumedW = 0.0f;
                    for (int i = 0; i < repeatCount; ++i)
                    {
                        auto fi = font;
                        fi.setHeight (font.getHeight() * scaleForIndex (i + 1));
                        const int wi = juce::jmax (1, stringWidth (fi, tailChar));
                        if (fi.getHeight() < kMinTailCharPx || wi < (int) std::ceil (kMinTailCharPx))
                            break;
                        const int x = rightLimit - (int) std::floor (consumedW + 1.0e-6f) - wi;

                        const int baselineY = area.getY()
                                            + (int) std::round ((area.getHeight() - fi.getHeight()) * 0.5f)
                                            + (int) std::round (fi.getAscent());

                        if (draw_count >= kMaxTailCharsDrawn)
                            break;

                        drawXs[(size_t) draw_count] = x;
                        drawBaselines[(size_t) draw_count] = baselineY;
                        ++draw_count;
                        consumedW += (float) wi * advanceFactor;
                    }

                    if (draw_count <= 1)
                    {
                        g.setFont (font);
                        g.setColour ((*tailGradient)[0]);
                        return true;
                    }

                    int drawable_count = 0;
                    for (int i = draw_count - 1; i >= 0; --i)
                    {
                        const float t = (float) (i + 1) / (float) juce::jmax (1, draw_count);
                        const auto c = lerpColourStops (*tailGradient, t);
                        if (isAbsoluteGradientEndpoint (c, *tailGradient))
                            continue;
                        ++drawable_count;
                    }

                    if (drawable_count <= 1)
                    {
                        g.setFont (font);
                        g.setColour ((*tailGradient)[0]);
                        return true;
                    }

                    // Reversed stacking priority: draw darker/later first,
                    // then lighter/earlier on top.
                    for (int i = draw_count - 1; i >= 0; --i)
                    {
                        auto fi = font;
                        fi.setHeight (font.getHeight() * scaleForIndex (i + 1));

                        const float t = (float) (i + 1) / (float) juce::jmax (1, draw_count);
                        const auto c = lerpColourStops (*tailGradient, t);
                        if (isAbsoluteGradientEndpoint (c, *tailGradient))
                            continue;

                        g.setColour (c);
                        g.setFont (fi);
                        g.drawSingleLineText (tailChar, drawXs[(size_t) i], drawBaselines[(size_t) i], juce::Justification::left);
                    }

                    g.setFont (font);
                    g.setColour ((*tailGradient)[0]);
                }
            }
        }

        return true;
    }

    return false;
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
                                     std::round (local.getHeight() * 0.50f));

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
    const std::array<BarSlider*, 4> barSliders { &amountSlider, &seriesSlider, &freqSlider, &shapeSlider };

    useCustomPalette = audioProcessor.getUiUseCustomPalette();
    fxTailEnabled = audioProcessor.getUiFxTailEnabled();

    for (int i = 0; i < 4; ++i)
        customPalette[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

    setOpaque (true);

    applyActivePalette();
    setLookAndFeel (&lnf);
    tooltipWindow = std::make_unique<juce::TooltipWindow> (nullptr, 250);
    tooltipWindow->setLookAndFeel (&lnf);
    tooltipWindow->setAlwaysOnTop (true);

    setResizable (true, true);

    // Para que el host/JUCE clipee de verdad
    setResizeLimits (kMinW, kMinH, kMaxW, kMaxH);

    resizeConstrainer.setMinimumSize (kMinW, kMinH);
    resizeConstrainer.setMaximumSize (kMaxW, kMaxH);

    resizerCorner = std::make_unique<juce::ResizableCornerComponent> (this, &resizeConstrainer);
    addAndMakeVisible (*resizerCorner);
    if (resizerCorner != nullptr)
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

    // ctor
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

    seriesSlider.setRange ((double) DisperserAudioProcessor::kSeriesMin,
                           (double) DisperserAudioProcessor::kSeriesMax,
                           1.0);

    rvsButton.setButtonText ("");
    invButton.setButtonText ("");

    addAndMakeVisible (rvsButton);
    addAndMakeVisible (invButton);

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

    auto bindButton = [&] (std::unique_ptr<ButtonAttachment>& attachment,
                           const char* paramId,
                           juce::Button& button)
    {
        attachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, paramId, button);
    };

    bindButton (rvsAttachment, DisperserAudioProcessor::kParamReverse, rvsButton);
    bindButton (invAttachment, DisperserAudioProcessor::kParamInv, invButton);

    const std::array<const char*, 6> uiMirrorParamIds {
        DisperserAudioProcessor::kParamUiPalette,
        DisperserAudioProcessor::kParamUiFxTail,
        DisperserAudioProcessor::kParamUiColor0,
        DisperserAudioProcessor::kParamUiColor1,
        DisperserAudioProcessor::kParamUiColor2,
        DisperserAudioProcessor::kParamUiColor3
    };
    for (auto* paramId : uiMirrorParamIds)
        audioProcessor.apvts.addParameterListener (paramId, this);

    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]()
    {
        if (safeThis == nullptr)
            return;

        safeThis->applyPersistedUiStateFromProcessor (true, true);
    });

    // Re-apply persisted UI size after short delays to override late host resizes.
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

    startTimerHz (10);

    refreshLegendTextCache();
}

DisperserAudioProcessorEditor::~DisperserAudioProcessorEditor()
{
    stopTimer();

    const std::array<const char*, 6> uiMirrorParamIds {
        DisperserAudioProcessor::kParamUiPalette,
        DisperserAudioProcessor::kParamUiFxTail,
        DisperserAudioProcessor::kParamUiColor0,
        DisperserAudioProcessor::kParamUiColor1,
        DisperserAudioProcessor::kParamUiColor2,
        DisperserAudioProcessor::kParamUiColor3
    };
    for (auto* paramId : uiMirrorParamIds)
        audioProcessor.apvts.removeParameterListener (paramId, this);

    audioProcessor.setUiUseCustomPalette (useCustomPalette);
    audioProcessor.setUiFxTailEnabled (fxTailEnabled);

    dismissEditorOwnedModalPrompts (lnf);
    setPromptOverlayActive (false);

    const std::array<BarSlider*, 4> barSliders { &amountSlider, &seriesSlider, &freqSlider, &shapeSlider };
    for (auto* slider : barSliders)
        slider->removeListener (this);

    if (tooltipWindow != nullptr)
        tooltipWindow->setLookAndFeel (nullptr);

    setLookAndFeel (nullptr);
}

void DisperserAudioProcessorEditor::applyActivePalette()
{
    const auto& palette = useCustomPalette ? customPalette : defaultPalette;

    DISPXScheme scheme;
    scheme.bg = palette[1];
    scheme.fg = palette[0];
    scheme.outline = palette[0];
    scheme.text = palette[0];
    scheme.fxGradientStart = palette[2];
    scheme.fxGradientEnd = palette[3];

    for (auto& s : schemes)
        s = scheme;

    lnf.setScheme (schemes[(size_t) currentSchemeIndex]);
}

void DisperserAudioProcessorEditor::applyLabelTextColour (juce::Label& label, juce::Colour colour)
{
    label.setColour (juce::Label::textColourId, colour);
}

void DisperserAudioProcessorEditor::sliderValueChanged (juce::Slider* slider)
{
    auto isBarSlider = [&] (const juce::Slider* s)
    {
        return s == &amountSlider || s == &seriesSlider || s == &freqSlider || s == &shapeSlider;
    };

    const int previousMode = labelVisibilityMode;
    const int previousValueColumnWidth = getTargetValueColumnWidth();
    const bool legendTextLengthChanged = refreshLegendTextCache();
    if (legendTextLengthChanged)
        updateLegendVisibility();
    const int currentValueColumnWidth = getTargetValueColumnWidth();

    if (labelVisibilityMode != previousMode || currentValueColumnWidth != previousValueColumnWidth || slider == nullptr)
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
    const std::array<juce::Component*, 6> interactiveControls {
        &amountSlider, &seriesSlider, &freqSlider, &shapeSlider, &rvsButton, &invButton
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

void DisperserAudioProcessorEditor::parameterChanged (const juce::String& parameterID, float)
{
    // Width/height should trigger applying size; other UI params should update palette/fx/colors.
    const bool isSizeParam = parameterID == DisperserAudioProcessor::kParamUiWidth
                         || parameterID == DisperserAudioProcessor::kParamUiHeight;

    const bool isUiVisualParam = parameterID == DisperserAudioProcessor::kParamUiPalette
                             || parameterID == DisperserAudioProcessor::kParamUiFxTail
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
        for (int i = 0; i < 4; ++i)
        {
            const auto c = audioProcessor.getUiCustomPaletteColour (i);
            if (customPalette[(size_t) i].getARGB() != c.getARGB())
            {
                customPalette[(size_t) i] = c;
                paletteChanged = true;
            }
        }

        const bool targetUseCustomPalette = audioProcessor.getUiUseCustomPalette();
        const bool targetFxTailEnabled = audioProcessor.getUiFxTailEnabled();

        const bool paletteSwitchChanged = (useCustomPalette != targetUseCustomPalette);
        const bool fxChanged = (fxTailEnabled != targetFxTailEnabled);

        if (paletteSwitchChanged)
            useCustomPalette = targetUseCustomPalette;

        if (fxChanged)
            fxTailEnabled = targetFxTailEnabled;

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

    const auto oldAmountFullLen = cachedAmountTextFull.length();
    const auto oldAmountShortLen = cachedAmountTextShort.length();
    const auto oldSeriesFullLen = cachedSeriesTextFull.length();
    const auto oldSeriesShortLen = cachedSeriesTextShort.length();
    const auto oldFreqLen = cachedFreqTextHz.length();
    const auto oldShapeFullLen = cachedShapeTextFull.length();
    const auto oldShapeShortLen = cachedShapeTextShort.length();

    cachedAmountTextFull  = juce::String (amountV) + " STAGES";
    cachedAmountTextShort = juce::String (amountV) + " STG";

    cachedSeriesTextFull  = juce::String (seriesV) + " SERIES";
    cachedSeriesTextShort = juce::String (seriesV) + " SRS";

    cachedFreqTextHz = formatBarFrequencyHzText (hz);
    cachedFreqIntOnly = cachedFreqTextHz.upToFirstOccurrenceOf (".", false, false)
                                     .upToFirstOccurrenceOf (" HZ", false, false);

    cachedShapeTextFull = juce::String (shapePct).toUpperCase() + "% SHAPE";
    cachedShapeTextShort = juce::String (shapePct).toUpperCase() + "% SHP";
    cachedShapeIntOnly = juce::String (shapePct);

    const bool lengthChanged = oldAmountFullLen  != cachedAmountTextFull.length()
                            || oldAmountShortLen != cachedAmountTextShort.length()
                            || oldSeriesFullLen  != cachedSeriesTextFull.length()
                            || oldSeriesShortLen != cachedSeriesTextShort.length()
                            || oldFreqLen        != cachedFreqTextHz.length()
                            || oldShapeFullLen   != cachedShapeTextFull.length()
                            || oldShapeShortLen  != cachedShapeTextShort.length();

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

    // Disable tooltip/popup above the bar (we use our own numeric popup)
    s.setPopupDisplayEnabled (false, false, this);
    s.setTooltip (juce::String());

    // IMPORTANT: disable popup menu so right-click can be used for our numeric popup
    s.setPopupMenuEnabled (false);

    s.setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::thumbColourId, juce::Colours::transparentBlack);
}

//========================== Right-click numeric popup ==========================

namespace
{
    static double roundToDecimals (double value, int decimals)
    {
        const int safeDecimals = juce::jlimit (0, 9, decimals);
        const double scale = std::pow (10.0, (double) safeDecimals);
        return std::round (value * scale) / scale;
    }

    constexpr int kPromptWidth = 460;
    constexpr int kPromptHeight = 336;
    constexpr int kPromptInnerMargin = 24;
    constexpr int kPromptFooterBottomPad = 24;
    constexpr int kPromptFooterGap = 12;
    constexpr int kPromptBodyTopPad = 24;
    constexpr int kPromptBodyBottomPad = 18;
    constexpr const char* kPromptSuffixLabelId = "promptSuffixLabel";

    constexpr float kPromptEditorFontScale = 1.5f;
    constexpr float kPromptEditorHeightScale = 1.4f;
    constexpr int kPromptEditorHeightPadPx = 6;
    constexpr int kPromptEditorRaiseYPx = 8;
    constexpr int kPromptEditorMinTopPx = 6;
    constexpr int kPromptEditorMinWidthPx = 180;
    constexpr int kPromptEditorMaxWidthPx = 240;
    constexpr int kPromptEditorHostPadPx = 80;

    constexpr int kPromptInlineContentPadPx = 8;
    constexpr int kPromptSuffixVInsetPx = 1;
    constexpr int kPromptSuffixBaselineDefaultPx = 3;
    constexpr int kPromptSuffixBaselineShapePx = 4;

    constexpr int kTitleAreaExtraHeightPx = 4;
    constexpr int kTitleRightGapToInfoPx = 8;
    constexpr int kVersionGapPx = 8;
    constexpr int kToggleLegendCollisionPadPx = 6;

    void applyPromptShellSize (juce::AlertWindow& aw)
    {
        aw.setSize (kPromptWidth, kPromptHeight);
    }

    int getAlertButtonsTop (const juce::AlertWindow& aw)
    {
        int buttonsTop = aw.getHeight() - (kPromptFooterBottomPad + 36);
        for (int i = 0; i < aw.getNumButtons(); ++i)
            if (auto* btn = aw.getButton (i))
                buttonsTop = juce::jmin (buttonsTop, btn->getY());
        return buttonsTop;
    }

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

}

static void layoutAlertWindowButtons (juce::AlertWindow& aw)
{
    const int btnCount = aw.getNumButtons();
    if (btnCount <= 0)
        return;

    const int footerY = aw.getHeight() - kPromptFooterBottomPad;
    const int sideMargin = kPromptInnerMargin;
    const int buttonGap = kPromptFooterGap;

    if (btnCount == 1)
    {
        if (auto* btn = aw.getButton (0))
        {
            auto r = btn->getBounds();
            r.setWidth (juce::jmax (80, r.getWidth()));
            r.setX ((aw.getWidth() - r.getWidth()) / 2);
            r.setY (footerY - r.getHeight());
            btn->setBounds (r);
        }
        return;
    }

    const int totalW = aw.getWidth();
    const int totalGap = (btnCount - 1) * buttonGap;
    const int btnWidth = juce::jmax (20, (totalW - (2 * sideMargin) - totalGap) / btnCount);

    int x = sideMargin;
    for (int i = 0; i < btnCount; ++i)
    {
        if (auto* btn = aw.getButton (i))
        {
            auto r = btn->getBounds();
            r.setWidth (btnWidth);
            r.setY (footerY - r.getHeight());
            r.setX (x);
            btn->setBounds (r);
        }
        x += btnWidth + buttonGap;
    }
}

static void layoutInfoPopupContent (juce::AlertWindow& aw)
{
    layoutAlertWindowButtons (aw);

    const int contentTop = kPromptBodyTopPad;
    const int contentBottom = getAlertButtonsTop (aw) - kPromptBodyBottomPad;
    const int contentH = juce::jmax (0, contentBottom - contentTop);

    auto* infoLabel = dynamic_cast<juce::Label*> (aw.findChildWithID ("infoText"));
    auto* infoLink = dynamic_cast<juce::HyperlinkButton*> (aw.findChildWithID ("infoLink"));

    if (infoLabel != nullptr && infoLink != nullptr)
    {
        const int labelH = juce::jlimit (26, juce::jmax (26, contentH), (int) std::lround (contentH * 0.34));
        const int linkH = juce::jlimit (20, 34, (int) std::lround (contentH * 0.18));

        const int freeH = juce::jmax (0, contentH - labelH - linkH);
        const int gap = freeH / 3;
        const int labelY = contentTop + gap;
        const int linkY = labelY + labelH + gap;

        infoLabel->setBounds (kPromptInnerMargin,
                              labelY,
                              aw.getWidth() - (2 * kPromptInnerMargin),
                              labelH);

        infoLink->setBounds (kPromptInnerMargin,
                             linkY,
                             aw.getWidth() - (2 * kPromptInnerMargin),
                             linkH);
        return;
    }

    if (infoLabel != nullptr)
    {
        infoLabel->setBounds (kPromptInnerMargin,
                              contentTop,
                              aw.getWidth() - (2 * kPromptInnerMargin),
                              juce::jmax (20, contentH));
    }
}

static juce::String colourToHexRgb (juce::Colour c)
{
    auto h2 = [] (juce::uint8 v)
    {
        return juce::String::toHexString ((int) v).paddedLeft ('0', 2).toUpperCase();
    };

    return "#" + h2 (c.getRed()) + h2 (c.getGreen()) + h2 (c.getBlue());
}

static bool tryParseHexColour (juce::String text, juce::Colour& out)
{
    auto isHexDigitAscii = [] (juce::juce_wchar ch)
    {
        return (ch >= '0' && ch <= '9')
            || (ch >= 'A' && ch <= 'F')
            || (ch >= 'a' && ch <= 'f');
    };

    text = text.trim();
    if (text.startsWithChar ('#'))
        text = text.substring (1);

    if (text.length() != 6 && text.length() != 8)
        return false;

    for (int i = 0; i < text.length(); ++i)
        if (! isHexDigitAscii (text[i]))
            return false;

    if (text.length() == 6)
    {
        const auto r = (juce::uint8) text.substring (0, 2).getHexValue32();
        const auto g = (juce::uint8) text.substring (2, 4).getHexValue32();
        const auto b = (juce::uint8) text.substring (4, 6).getHexValue32();
        out = juce::Colour (r, g, b);
        return true;
    }

    const auto a = (juce::uint8) text.substring (0, 2).getHexValue32();
    const auto r = (juce::uint8) text.substring (2, 4).getHexValue32();
    const auto g = (juce::uint8) text.substring (4, 6).getHexValue32();
    const auto b = (juce::uint8) text.substring (6, 8).getHexValue32();
    out = juce::Colour (r, g, b).withAlpha ((float) a / 255.0f);
    return true;
}

static void setPaletteSwatchColour (juce::TextButton& b, juce::Colour colour)
{
    b.setButtonText ("");
    b.setColour (juce::TextButton::buttonColourId, colour);
    b.setColour (juce::TextButton::buttonOnColourId, colour);
}

static void stylePromptTextEditor (juce::TextEditor& te,
                                   juce::Colour bg,
                                   juce::Colour text,
                                   juce::Colour accent,
                                   juce::Font baseFont,
                                   int hostWidth,
                                   bool widenAndCenter)
{
    auto popupFont = baseFont;
    popupFont.setHeight (popupFont.getHeight() * kPromptEditorFontScale);
    te.setFont (popupFont);
    te.applyFontToAllText (popupFont);
    te.setJustification (juce::Justification::centred);
    te.setIndents (0, 0);

    te.setColour (juce::TextEditor::backgroundColourId,      bg);
    te.setColour (juce::TextEditor::textColourId,            text);
    te.setColour (juce::TextEditor::outlineColourId,         bg);
    te.setColour (juce::TextEditor::focusedOutlineColourId,  bg);
    te.setColour (juce::TextEditor::highlightColourId,       accent.withAlpha (0.35f));
    te.setColour (juce::TextEditor::highlightedTextColourId, text);

    auto r = te.getBounds();
    r.setHeight ((int) (popupFont.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
    r.setY (juce::jmax (kPromptEditorMinTopPx, r.getY() - kPromptEditorRaiseYPx));

    if (widenAndCenter)
    {
        const int editorW = juce::jlimit (kPromptEditorMinWidthPx,
                                          kPromptEditorMaxWidthPx,
                                          hostWidth - kPromptEditorHostPadPx);
        r.setWidth (editorW);
        r.setX ((hostWidth - r.getWidth()) / 2);
    }

    te.setBounds (r);
    te.selectAll();
}

static void centrePromptTextEditorVertically (juce::AlertWindow& aw,
                                              juce::TextEditor& te,
                                              int minTop = kPromptEditorMinTopPx)
{
    int buttonsTop = aw.getHeight();
    for (int i = 0; i < aw.getNumButtons(); ++i)
        if (auto* btn = aw.getButton (i))
            buttonsTop = juce::jmin (buttonsTop, btn->getY());

    auto r = te.getBounds();
    const int centeredY = (buttonsTop - r.getHeight()) / 2;
    r.setY (juce::jmax (minTop, centeredY));
    te.setBounds (r);
}

static void focusAndSelectPromptTextEditor (juce::AlertWindow& aw, const juce::String& editorId)
{
    juce::Component::SafePointer<juce::AlertWindow> safeAw (&aw);
    juce::MessageManager::callAsync ([safeAw, editorId]()
    {
        if (safeAw == nullptr)
            return;

        auto* te = safeAw->getTextEditor (editorId);
        if (te == nullptr)
            return;

        if (te->isShowing() && te->isEnabled() && te->getPeer() != nullptr)
            te->grabKeyboardFocus();

        te->selectAll();
    });
}

static void preparePromptTextEditor (juce::AlertWindow& aw,
                                     const juce::String& editorId,
                                     juce::Colour bg,
                                     juce::Colour text,
                                     juce::Colour accent,
                                     juce::Font baseFont,
                                     bool widenAndCenter,
                                     int minTop = 6)
{
    if (auto* te = aw.getTextEditor (editorId))
    {
        stylePromptTextEditor (*te,
                               bg,
                               text,
                               accent,
                               baseFont,
                               aw.getWidth(),
                               widenAndCenter);
        centrePromptTextEditorVertically (aw, *te, minTop);
        focusAndSelectPromptTextEditor (aw, editorId);
    }
}

static void syncGraphicsPopupState (juce::AlertWindow& aw,
                                    const std::array<juce::Colour, 4>& defaultPalette,
                                    const std::array<juce::Colour, 4>& customPalette,
                                    bool useCustomPalette)
{
    if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteDefaultToggle")))
        t->setToggleState (! useCustomPalette, juce::dontSendNotification);
    if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteCustomToggle")))
        t->setToggleState (useCustomPalette, juce::dontSendNotification);

    for (int i = 0; i < 4; ++i)
    {
        if (auto* dflt = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("defaultSwatch" + juce::String (i))))
            setPaletteSwatchColour (*dflt, defaultPalette[(size_t) i]);
        if (auto* custom = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("customSwatch" + juce::String (i))))
        {
            setPaletteSwatchColour (*custom, customPalette[(size_t) i]);
            custom->setTooltip (colourToHexRgb (customPalette[(size_t) i]));
        }
    }

    // Helper for static context: safely apply text colour to a label pointer
    auto applyLabelTextColourTo = [] (juce::Label* lbl, juce::Colour col)
    {
        if (lbl != nullptr)
            lbl->setColour (juce::Label::textColourId, col);
    };

    // Ensure popup labels reflect the active palette text colour
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

    const int buttonsTop = getAlertButtonsTop (aw);

    const int contentLeft = kPromptInnerMargin;
    const int contentTop = kPromptBodyTopPad;
    const int contentRight = aw.getWidth() - kPromptInnerMargin;
    const int contentBottom = buttonsTop - kPromptBodyBottomPad;
    const int contentW = juce::jmax (0, contentRight - contentLeft);
    const int contentH = juce::jmax (0, contentBottom - contentTop);

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
                                               (int) std::lround ((double) toggleBox * 0.50));

    const int swatchGroupSize = (2 * swatchSize) + swatchGap;
    const int swatchesH = swatchGroupSize;
    const int modeH = toggleBox;

    const int baseGap1 = GraphicsPromptLayout::titleToModeGap;
    const int baseGap2 = GraphicsPromptLayout::modeToSwatchesGap;
    const int stackHNoTopBottom = titleH + baseGap1 + modeH + baseGap2 + swatchesH;
    const int centeredYStart = snapEven (contentTop + juce::jmax (0, (contentH - stackHNoTopBottom) / 2));
    const int symmetricTopMargin = kPromptFooterBottomPad;
    const bool hasBodyTitle = (paletteTitle != nullptr);
    const int yStart = hasBodyTitle ? snapEven (symmetricTopMargin) : centeredYStart;

    const int titleY = yStart;
    const int modeY = snapEven (titleY + titleH + baseGap1);
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
        paletteTitle->setBounds (col0X,
                                 titleY,
                                 paletteW,
                                 titleH);
    }

    if (dfltToggle != nullptr)   dfltToggle->setBounds (dfltX, modeY, toggleBox, toggleBox);
    if (dfltLabel != nullptr)    dfltLabel->setBounds (dfltX + toggleLabelStartOffset, modeY, dfltLabelW, toggleBox);
    if (customToggle != nullptr) customToggle->setBounds (customX, modeY, toggleBox, toggleBox);
    if (customLabel != nullptr)  customLabel->setBounds (customX + toggleLabelStartOffset, modeY, customLabelW, toggleBox);

    auto placeSwatchGroup = [&] (const juce::String& prefix, int startX)
    {
        const int startY = blocksY;

        for (int i = 0; i < 4; ++i)
        {
            if (auto* b = dynamic_cast<juce::TextButton*> (aw.findChildWithID (prefix + juce::String (i))))
            {
                const int col = i % 2;
                const int row = i / 2;
                b->setBounds (startX + col * (swatchSize + swatchGap),
                              startY + row * (swatchSize + swatchGap),
                              swatchSize,
                              swatchSize);
            }
        }
    };

    placeSwatchGroup ("defaultSwatch", defaultSwatchStartX);
    placeSwatchGroup ("customSwatch", customSwatchStartX);

    if (auto* okButton = aw.getNumButtons() > 0 ? aw.getButton (0) : nullptr)
    {
        if (okButton != nullptr)
        {
            auto okR = okButton->getBounds();
            okR.setX (col1X);
            okButton->setBounds (okR);

                const int fxY = snapEven (okR.getY() + juce::jmax (0, (okR.getHeight() - toggleBox) / 2));
                const int fxX = col0X;
                if (fxToggle != nullptr) fxToggle->setBounds (fxX, fxY, toggleBox, toggleBox);
                if (fxLabel != nullptr)  fxLabel->setBounds (fxX + toggleLabelStartOffset, fxY, fxLabelW, toggleBox);
            }
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

    for (int i = 0; i < 4; ++i)
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

            for (int i = 0; i < 4; ++i)
            {
                shiftX (aw.findChildWithID ("defaultSwatch" + juce::String (i)));
                shiftX (aw.findChildWithID ("customSwatch" + juce::String (i)));
            }
        }
    }

}

void DisperserAudioProcessorEditor::openNumericEntryPopupForSlider (juce::Slider& s)
{
    // make sure the LNF is using the current scheme in case it changed
    lnf.setScheme (schemes[(size_t) currentSchemeIndex]);

    // grab a local copy, we will use its raw colours below to bypass
    // any host/LNF oddities that might creep in
    const auto scheme = schemes[(size_t) currentSchemeIndex];

    // decide what suffix label should appear; we want *separate* text that
    // is not part of the editable field. use the full nomenclature from the
    // helpers (long form) rather than the previous abbreviations.
    juce::String suffix;
    if (&s == &amountSlider)         suffix = " STAGES";
    else if (&s == &seriesSlider)    suffix = " SERIES";
    else if (&s == &freqSlider)      suffix = " HZ";
    else if (&s == &shapeSlider) suffix = " % SHAPE";
    const juce::String suffixText = suffix.trimStart();
    const bool isShapePrompt = (&s == &shapeSlider);

    // Sin texto de prompt: solo input + OK/Cancel
    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);

    // enforce our custom look&feel; hosts often reset dialogs to their own LNF
    aw->setLookAndFeel (&lnf);

    const auto current = s.getTextFromValue (s.getValue());
    aw->addTextEditor ("val", current, juce::String()); // sin label

    // we will create a label just to the right of the editor showing the suffix
    juce::Label* suffixLabel = nullptr;

    // increase the font size for legibility rather than resizing the field
    // we already have a helper up near the top of this file that uses
    // GlyphArrangement to measure text. reuse that instead of TextLayout.
    // (stringWidth(font, text))

    // adaptable filter for numeric input: clamps length, number of decimals and
    // optionally a value range. the returned string is the permitted version of
    // whatever the user typed.
    struct NumericInputFilter  : juce::TextEditor::InputFilter
    {
        double minVal, maxVal;
        int maxLen, maxDecimals;
        bool isShape = false;

                NumericInputFilter (double minV, double maxV,
                                                        int maxLength, int maxDecs, bool isShapeValue = false)
            : minVal (minV), maxVal (maxV),
                            maxLen (maxLength), maxDecimals (maxDecs), isShape (isShapeValue) {}

        juce::String filterNewText (juce::TextEditor& editor,
                                    const juce::String& newText) override
        {
            bool seenDot = false;
            int decimals = 0;
            juce::String result;

            for (auto c : newText)
            {
                if (c == '.')
                {
                    if (seenDot || maxDecimals == 0)
                        continue;
                    seenDot = true;
                    result += c;
                }
                else if (juce::CharacterFunctions::isDigit (c))
                {
                    if (seenDot) ++decimals;
                    if (decimals > maxDecimals)
                        break;
                    result += c;
                }
                else if ((c == '+' || c == '-') && result.isEmpty())
                {
                    result += c;
                }

                if (maxLen > 0 && result.length() >= maxLen)
                    break;
            }

            // now check the numeric value if the new text were inserted
            juce::String proposed = editor.getText();
            int insertPos = editor.getCaretPosition();
            proposed = proposed.substring (0, insertPos) + result
                     + proposed.substring (insertPos + editor.getHighlightedText().length());

            double val = proposed.replaceCharacter(',', '.').getDoubleValue();
            if (val > maxVal)
                return juce::String(); // reject insertion that exceeds limit

            return result;
        }
    };

    juce::Rectangle<int> editorBaseBounds;
    std::function<void()> layoutValueAndSuffix;

    if (auto* te = aw->getTextEditor ("val"))
    {
        auto f = lnf.getAlertWindowMessageFont();
        f.setHeight (f.getHeight() * 1.5f);
        te->setFont (f);
        te->applyFontToAllText (f);

        // ensure the editor is tall enough to contain the larger text
        auto r = te->getBounds();
        r.setHeight ((int) (f.getHeight() * 1.4f) + 6);
        r.setY (juce::jmax (6, r.getY() - 8));
        editorBaseBounds = r;

        // create & position the suffix label; it's non-editable and won't
        // be selected when the user highlights the value.
        suffixLabel = new juce::Label ("suffix", suffixText);
        suffixLabel->setComponentID (kPromptSuffixLabelId);
        suffixLabel->setJustificationType (juce::Justification::centredLeft);
        applyLabelTextColour (*suffixLabel, scheme.text);
        suffixLabel->setBorderSize (juce::BorderSize<int> (0));
        suffixLabel->setFont (f);
        aw->addAndMakeVisible (suffixLabel);

        layoutValueAndSuffix = [aw, te, suffixLabel, editorBaseBounds, isShapePrompt]()
        {
            int labelW = stringWidth (suffixLabel->getFont(), suffixLabel->getText()) + 2;
            auto er = te->getBounds();

            const auto txt = te->getText();
            const int textW = juce::jmax (1, stringWidth (te->getFont(), txt));
            const bool stickPercentToValue = suffixLabel->getText().startsWithChar ('%');
            const int spaceW = stickPercentToValue ? 0 : juce::jmax (2, stringWidth (te->getFont(), " "));
            const int minGapPx = juce::jmax (1, spaceW);

            constexpr int kEditorTextPadPx = 12;
            constexpr int kMinEditorWidthPx = 24;
            const int editorW = juce::jlimit (kMinEditorWidthPx,
                                              editorBaseBounds.getWidth(),
                                              textW + (kEditorTextPadPx * 2));
            er.setWidth (editorW);

            const int combinedW = textW + minGapPx + labelW;

            const int contentPad = kPromptInlineContentPadPx;
            const int contentLeft = contentPad;
            const int contentRight = (aw != nullptr ? aw->getWidth() - contentPad : editorBaseBounds.getRight());
            const int contentCenter = (contentLeft + contentRight) / 2;

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

            const int vInset = kPromptSuffixVInsetPx;
            const int baselineOffset = isShapePrompt ? kPromptSuffixBaselineShapePx : kPromptSuffixBaselineDefaultPx;
            const int labelY = er.getY() + vInset + baselineOffset;
            const int labelH = juce::jmax (1, er.getHeight() - (vInset * 2) - baselineOffset);
            suffixLabel->setBounds (labelX, labelY, labelW, labelH);
        };

        // our reposition function will place the label; keep the editor at its
        // original width (no further shrink needed here). we still set the
        // bounds now so that later reposition() can rely on r's coordinates.
        te->setBounds (editorBaseBounds);
        int labelW0 = stringWidth (suffixLabel->getFont(), suffixText) + 2;
        suffixLabel->setBounds (r.getRight() + 2, r.getY() + 1, labelW0, juce::jmax (1, r.getHeight() - 2));

        // initial placement (label anchored to right)
        if (layoutValueAndSuffix)
            layoutValueAndSuffix();

        // choose limits depending on the slider being edited
        double minVal = 0.0, maxVal = 1.0;
        int maxLen = 0, maxDecs = 4;

        if (&s == &amountSlider)
        {
            maxVal = 256.0;
            maxDecs = 0;
            maxLen = 3; // up to "256"
        }
        else if (&s == &seriesSlider)
        {
            maxVal = 4.0;
            maxDecs = 0;
            maxLen = 1; // "4"
        }
        else if (&s == &freqSlider)
        {
            maxVal = 20000.0;
            maxDecs = 3;
            maxLen = 9; // "20000.000" (5 digits + dot + 3)
        }
        else if (&s == &shapeSlider)
        {
            minVal = 0.0;
            maxVal = 100.0;    // user types percent
            maxDecs = 4;
            maxLen = 8; // "100.0000" (3 digits + dot + 4)
        }

        bool isShape = (&s == &shapeSlider);
        const bool isFreq = (&s == &freqSlider);
        te->setInputFilter (new NumericInputFilter (minVal, maxVal, maxLen, maxDecs, isShape), true);

        // limit text to four decimals and move the suffix when text changes
        te->onTextChange = [te, layoutValueAndSuffix, isFreq]() mutable
        {
            auto txt = te->getText();
            int dot = txt.indexOfChar('.');
            if (dot >= 0)
            {
                int decimals = txt.length() - dot - 1;
                const int maxDecimals = isFreq ? 3 : 4;
                if (decimals > maxDecimals)
                    te->setText (txt.substring (0, dot + 1 + maxDecimals), juce::dontSendNotification);
            }
            if (layoutValueAndSuffix)
                layoutValueAndSuffix();
        };
    }

    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    applyPromptShellSize (*aw);
    layoutAlertWindowButtons (*aw);

    // We can't call lookAndFeelChanged() on AlertWindow (it's protected),
    // so just rely on calling setLookAndFeel() twice instead.

    preparePromptTextEditor (*aw,
                             "val",
                             scheme.bg,
                             scheme.text,
                             scheme.fg,
                             lnf.getAlertWindowMessageFont(),
                             false,
                             6);

    // Force initial suffix placement with final editor metrics so the first
    // frame does not show a vertical offset.
    if (suffixLabel != nullptr && ! editorBaseBounds.isEmpty())
    {
        if (auto* te = aw->getTextEditor ("val"))
            suffixLabel->setFont (te->getFont());
        if (layoutValueAndSuffix)
            layoutValueAndSuffix();
    }

    // style buttons as well – some hosts stomp them when the window is added
    for (int i = 0; i < aw->getNumButtons(); ++i)
    {
        if (auto* btn = dynamic_cast<juce::TextButton*>(aw->getButton (i)))
        {
            btn->setColour (juce::TextButton::buttonColourId,   lnf.findColour (juce::TextButton::buttonColourId));
            btn->setColour (juce::TextButton::buttonOnColourId, lnf.findColour (juce::TextButton::buttonOnColourId));
            btn->setColour (juce::TextButton::textColourOffId,  lnf.findColour (juce::TextButton::textColourOffId));
            btn->setColour (juce::TextButton::textColourOnId,   lnf.findColour (juce::TextButton::textColourOnId));
            // font is provided by the look-and-feel already; avoid calling setFont
        }
    }

    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);
    juce::Slider* sliderPtr = &s;

    setPromptOverlayActive (true);

    // re-assert our look&feel in case the host modified it when
    // adding the window to the desktop.
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
                                     lnf.getAlertWindowMessageFont(),
                                     false,
                                     6);
        });

        embedAlertWindowInOverlay (safeThis.getComponent(), aw);
    }
    else
    {
        aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
        bringPromptWindowToFront (*aw);
        aw->repaint();
    }

    // Apply larger font and final layout synchronously so the prompt is
    // fully laid out before being shown (avoids a small delayed re-layout).
    {
        auto bigFont = lnf.getAlertWindowMessageFont();
        bigFont.setHeight (bigFont.getHeight() * 1.5f);
        preparePromptTextEditor (*aw,
                                 "val",
                                 scheme.bg,
                                 scheme.text,
                                 scheme.fg,
                                 bigFont,
                                 false,
                                 6);
        if (auto* suffixLbl = dynamic_cast<juce::Label*> (aw->findChildWithID (kPromptSuffixLabelId)))
            suffixLbl->setFont (bigFont);

        if (layoutValueAndSuffix)
            layoutValueAndSuffix();

        // Keep a lightweight async fallback that only ensures the window is
        // on top and repainted — avoid re-running layout to prevent visible jumps.
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

            // user typed percent for shape; convert to slider's [0,1] range
            if (safeThis != nullptr && sliderPtr == &safeThis->shapeSlider)
                v *= 0.01;

            const auto range = sliderPtr->getRange();
            double clamped = juce::jlimit (range.getStart(), range.getEnd(), v);

            if (safeThis != nullptr && sliderPtr == &safeThis->freqSlider)
            {
                clamped = roundToDecimals (clamped, 4);
            }

            sliderPtr->setValue (clamped, juce::sendNotificationSync);
        }));
}

void DisperserAudioProcessorEditor::openInfoPopup()
{
    lnf.setScheme (schemes[(size_t) currentSchemeIndex]);

    setPromptOverlayActive (true);

    auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
    juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
    juce::Component::SafePointer<DisperserAudioProcessorEditor> safeThis (this);
    aw->setLookAndFeel (&lnf);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("GRAPHICS", 2);

    applyPromptShellSize (*aw);

    auto* infoLabel = new juce::Label ("infoText", "NMSTR -> INFO SOON");
    infoLabel->setComponentID ("infoText");
    infoLabel->setJustificationType (juce::Justification::centred);
    applyLabelTextColour (*infoLabel, schemes[(size_t) currentSchemeIndex].text);
    auto infoFont = lnf.getAlertWindowMessageFont();
    infoFont.setHeight (infoFont.getHeight() * 1.45f);
    infoLabel->setFont (infoFont);
    aw->addAndMakeVisible (infoLabel);

    auto* infoLink = new juce::HyperlinkButton ("GitHub Repository",
                                                juce::URL ("https://github.com/lmaser/DISP-TR"));
    infoLink->setComponentID ("infoLink");
    infoLink->setJustificationType (juce::Justification::centred);
    infoLink->setColour (juce::HyperlinkButton::textColourId,
                         schemes[(size_t) currentSchemeIndex].text);
    auto linkFont = infoFont;
    linkFont.setHeight (infoFont.getHeight() * 0.72f);
    infoLink->setFont (linkFont, false, juce::Justification::centred);
    aw->addAndMakeVisible (infoLink);

    layoutInfoPopupContent (*aw);

    if (safeThis != nullptr)
    {
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

        safeAw->centreAroundComponent (safeThis.getComponent(), safeAw->getWidth(), safeAw->getHeight());
        bringPromptWindowToFront (*safeAw);

        layoutInfoPopupContent (*safeAw);

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
    lnf.setScheme (schemes[(size_t) currentSchemeIndex]);

    useCustomPalette = audioProcessor.getUiUseCustomPalette();
    fxTailEnabled = audioProcessor.getUiFxTailEnabled();
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
        applyLabelTextColour (*label, schemes[(size_t) currentSchemeIndex].text);
        label->setBorderSize (juce::BorderSize<int> (0));
        label->setFont (font);
        label->setMouseCursor (juce::MouseCursor::PointingHandCursor);
        aw->addAndMakeVisible (label);
        return label;
    };

    auto stylePromptButtons = [this] (juce::AlertWindow& alert)
    {
        for (int bi = 0; bi < alert.getNumButtons(); ++bi)
        {
            if (auto* btn = dynamic_cast<juce::TextButton*> (alert.getButton (bi)))
            {
                btn->setColour (juce::TextButton::buttonColourId,   lnf.findColour (juce::TextButton::buttonColourId));
                btn->setColour (juce::TextButton::buttonOnColourId, lnf.findColour (juce::TextButton::buttonOnColourId));
                btn->setColour (juce::TextButton::textColourOffId,  lnf.findColour (juce::TextButton::textColourOffId));
                btn->setColour (juce::TextButton::textColourOnId,   lnf.findColour (juce::TextButton::textColourOnId));
            }
        }
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

    for (int i = 0; i < 4; ++i)
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
    fxToggle->setToggleState (fxTailEnabled, juce::dontSendNotification);
    fxToggle->onClick = [safeThis, fxToggle]()
    {
        if (safeThis == nullptr || fxToggle == nullptr)
            return;

        safeThis->fxTailEnabled = fxToggle->getToggleState();
        safeThis->audioProcessor.setUiFxTailEnabled (safeThis->fxTailEnabled);
        safeThis->repaint();
    };
    aw->addAndMakeVisible (fxToggle);

    auto* fxLabel = addPopupLabel ("fxLabel", "TEXT FX", labelFont);

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

    for (int i = 0; i < 4; ++i)
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

            customSwatch->onRightClick = [safeThis, safeAw, i, stylePromptButtons]()
            {
                if (safeThis == nullptr)
                    return;

                const auto scheme = safeThis->schemes[(size_t) safeThis->currentSchemeIndex];

                auto* colorAw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
                colorAw->setLookAndFeel (&safeThis->lnf);
                colorAw->addTextEditor ("hex", colourToHexRgb (safeThis->customPalette[(size_t) i]), juce::String());
                colorAw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                colorAw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                stylePromptButtons (*colorAw);

                applyPromptShellSize (*colorAw);
                layoutAlertWindowButtons (*colorAw);

                preparePromptTextEditor (*colorAw,
                                         "hex",
                                         scheme.bg,
                                         scheme.text,
                                         scheme.fg,
                                         safeThis->lnf.getAlertWindowMessageFont(),
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
                                                 safeThis->lnf.getAlertWindowMessageFont(),
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

                // Synchronously ensure prompt text editor styling is applied so
                // the prompt appears correctly before being shown.
                preparePromptTextEditor (*colorAw,
                                         "hex",
                                         scheme.bg,
                                         scheme.text,
                                         scheme.fg,
                                         safeThis->lnf.getAlertWindowMessageFont(),
                                         true,
                                         6);

                // Lightweight async: ensure window is on top and repainted,
                // but avoid re-applying layout synchronously to prevent jumps.
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

    // If we're embedding the prompt inside the editor and the editor is
    // narrower than the default prompt width, shrink the prompt to fit and
    // re-run the layout so nothing is positioned outside the visible area.
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

            syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
            layoutGraphicsPopupContent (*safeAw);
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
    const double hz = freqSlider.getValue();

    if (hz >= kHzSwitchHz)
        return juce::String (hz / 1000.0, 2).toUpperCase() + " KHZ";

    return juce::String (hz, 2).toUpperCase() + " HZ";
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

    constexpr int kValueAreaHeightPx = 44;
    constexpr int kValueAreaRightMarginPx = 24;
    constexpr int kToggleLabelGapPx = 4;
    constexpr int kToggleLabelRightPadPx = 10;
    constexpr int kResizerCornerPx = 22;
    constexpr int kToggleBoxPx = 72;
    constexpr int kMinToggleBlocksGapPx = 10;

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

    HorizontalLayoutMetrics makeHorizontalLayoutMetrics (int editorW, int valueW)
    {
        HorizontalLayoutMetrics m;
        m.barW = (int) std::round (editorW * 0.455);
        m.valuePad = (int) std::round (editorW * 0.02);
        m.valueW = valueW;
        m.contentW = m.barW + m.valuePad + m.valueW;
        m.leftX = juce::jmax (6, (editorW - m.contentW) / 2);
        return m;
    }

    VerticalLayoutMetrics makeVerticalLayoutMetrics (int editorH, int layoutVerticalBiasPx)
    {
        VerticalLayoutMetrics m;
        m.rhythm = juce::jlimit (6, 16, (int) std::round (editorH * 0.018));
        const int nominalBarH = juce::jlimit (14, 120, m.rhythm * 6);
        const int nominalGapY = juce::jmax (4, m.rhythm * 4);

        m.titleH = juce::jlimit (24, 56, m.rhythm * 4);
        m.titleAreaH = m.titleH + 4;
        const int computedTitleTopPad = 6 + layoutVerticalBiasPx;
        m.titleTopPad = (computedTitleTopPad > 8) ? computedTitleTopPad : 8;
        const int titleGap = m.titleTopPad;
        m.topMargin = m.titleTopPad + m.titleAreaH + titleGap;
        m.betweenSlidersAndButtons = juce::jmax (8, m.rhythm * 2);
        m.bottomMargin = m.titleTopPad;

        m.box = kToggleBoxPx;
        m.btnY = editorH - m.bottomMargin - m.box;
        m.availableForSliders = juce::jmax (40, m.btnY - m.betweenSlidersAndButtons - m.topMargin);

        const int nominalStack = 4 * nominalBarH + 3 * nominalGapY;
        const double stackScale = nominalStack > 0 ? juce::jmin (1.0, (double) m.availableForSliders / (double) nominalStack)
                                                   : 1.0;

        m.barH = juce::jmax (14, (int) std::round (nominalBarH * stackScale));
        m.gapY = juce::jmax (4,  (int) std::round (nominalGapY * stackScale));

        auto stackHeight = [&]() { return 4 * m.barH + 3 * m.gapY; };

        while (stackHeight() > m.availableForSliders && m.gapY > 4)
            --m.gapY;

        while (stackHeight() > m.availableForSliders && m.barH > 14)
            --m.barH;

        m.topY = m.topMargin;
        return m;
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

    const int maxW = juce::jmax (juce::jmax (amountMaxW, seriesMaxW), juce::jmax (freqMaxW, shapeMaxW));

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
    const auto layout = makeHorizontalLayoutMetrics (getWidth(), getTargetValueColumnWidth());

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

    return nullptr;
}

namespace
{
    int getToggleVisualBoxSidePx (const juce::Component& button)
    {
        const int h = button.getHeight();
        return juce::jlimit (14, juce::jmax (14, h - 2), (int) std::lround ((double) h * 0.50));
    }

    int getToggleVisualBoxLeftPx (const juce::Component& button)
    {
        return button.getX() + 2;
    }

    juce::Rectangle<int> makeToggleLabelArea (const juce::Component& button,
                                              int editorWidth,
                                              const juce::String& labelText)
    {
        const auto b = button.getBounds();
        const int visualRight = getToggleVisualBoxLeftPx (button) + getToggleVisualBoxSidePx (button);
        const int x = visualRight + kToggleLabelGapPx;

        juce::Font labelFont (juce::FontOptions (40.0f).withStyle ("Bold"));
        const int desiredW = stringWidth (labelFont, labelText) + 2;
        const int maxW = juce::jmax (0, editorWidth - x - kToggleLabelRightPadPx);
        const int w = juce::jmin (desiredW, maxW);

        return { x, b.getY(), w, b.getHeight() };
    }
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getRvsLabelArea() const
{
    return makeToggleLabelArea (rvsButton, getWidth(), "RVS");
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getInvLabelArea() const
{
    return makeToggleLabelArea (invButton, getWidth(), "INV");
}

juce::Rectangle<int> DisperserAudioProcessorEditor::getInfoIconArea() const
{
    const auto amountValueArea = getValueAreaFor (amountSlider.getBounds());
    const int contentRight = amountValueArea.getRight();
    const auto verticalLayout = makeVerticalLayoutMetrics (getHeight(), kLayoutVerticalBiasPx);
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
    const auto p = e.getPosition();

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

    if (getRvsLabelArea().contains (p))
    {
        rvsButton.setToggleState (! rvsButton.getToggleState(), juce::sendNotificationSync);
        return;
    }

    if (getInvLabelArea().contains (p))
    {
        invButton.setToggleState (! invButton.getToggleState(), juce::sendNotificationSync);
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
        return;
    }
}

//==============================================================================

void DisperserAudioProcessorEditor::paint (juce::Graphics& g)
{
    const int W = getWidth();
    const auto horizontalLayout = makeHorizontalLayoutMetrics (W, getTargetValueColumnWidth());
    const auto verticalLayout = makeVerticalLayoutMetrics (getHeight(), kLayoutVerticalBiasPx);
    const auto amountValueArea = getValueAreaFor (amountSlider.getBounds());
    const auto seriesValueArea = getValueAreaFor (seriesSlider.getBounds());
    const auto freqValueArea = getValueAreaFor (freqSlider.getBounds());
    const auto shapeValueArea = getValueAreaFor (shapeSlider.getBounds());

    const auto scheme = schemes[(size_t) currentSchemeIndex];
    const bool useShortLabels = (labelVisibilityMode == 1);
    const bool shouldHideUnitLabels = (labelVisibilityMode == 2);

    g.fillAll (scheme.bg);
    g.setColour (scheme.text);

    constexpr float baseFontPx = 40.0f;
    constexpr float minFontPx  = 18.0f;
    const juce::String barTailTuning; // "" = sin modificación; ejemplos: "80%", "-1"

    g.setFont (juce::Font (juce::FontOptions (baseFontPx).withStyle ("Bold")));

    auto drawAlignedLegend = [&] (const juce::Rectangle<int>& area,
                                  const juce::String& text,
                                  bool useAutoMargin,
                                  bool useTailEffect,
                                  bool tailFromSuffixToLeft,
                                  bool lowercaseTailChars,
                                  const juce::String& tailTuning)
    {
        auto t = text.toUpperCase().trim();
        const int split = t.lastIndexOfChar (' ');
        if (split <= 0 || split >= t.length() - 1)
            return drawValueNoEllipsis (g, area, t, juce::String(), t, baseFontPx, minFontPx), void();

        const auto value = t.substring (0, split).trimEnd();
        const auto suffix = t.substring (split + 1).trimStart();
        const auto* tailGradient = (useTailEffect && fxTailEnabled) ? &lnf.getTrailingTextGradient() : nullptr;

        if (! drawValueWithRightAlignedSuffix (g, area, value, suffix, useAutoMargin, baseFontPx, minFontPx,
                                               tailGradient, tailFromSuffixToLeft, lowercaseTailChars, tailTuning))
            drawValueNoEllipsis (g, area, t, juce::String(), value, baseFontPx, minFontPx);

        g.setColour (scheme.text);
    };

    auto drawLegendForMode = [&] (const juce::Rectangle<int>& area,
                                  const juce::String& fullLegend,
                                  const juce::String& shortLegend,
                                  const juce::String& intOnlyLegend,
                                  const juce::String& tailTuning)
    {
        if (shouldHideUnitLabels)
        {
            drawValueNoEllipsis (g, area,
                                 intOnlyLegend,
                                 juce::String(),
                                 intOnlyLegend,
                                 baseFontPx, minFontPx);
            return;
        }

        drawAlignedLegend (area,
                           useShortLabels ? shortLegend : fullLegend,
                           false,
                           true,
                           true,
                           true,
                           tailTuning);
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

        // Keep tail behaviour exactly as original (unchanged params / unchanged font).
        if (fxTailEnabled)
        {
            drawTextWithRepeatedLastCharGradient (g, titleArea, titleText, barW, lnf.getTrailingTextGradient(), titleX + barW,
                          juce::String(), "20%", "pyramid");
        }
        else
        {
            g.drawText (titleText, titleArea.getX(), titleArea.getY(), titleArea.getWidth(), titleArea.getHeight(), juce::Justification::left, false);
        }

        // If horizontal space is too tight, fix only the base title text by overdrawing a fitted version.
        const auto infoIconArea = getInfoIconArea();
        const int titleRightLimit = infoIconArea.getX() - kTitleRightGapToInfoPx;
        const int titleMaxW = juce::jmax (0, titleRightLimit - titleArea.getX());
        const int titleBaseW = stringWidth (titleFont, titleText);
        const int originalTitleLimitW = juce::jmax (0, juce::jmin (titleW, barW));
        const bool originalWouldClipTitle = titleBaseW > originalTitleLimitW;

        if (titleMaxW > 0 && (originalWouldClipTitle || titleBaseW > titleMaxW))
        {
            auto fittedTitleFont = titleFont;
            for (float h = (float) titleH; h >= 12.0f; h -= 1.0f)
            {
                fittedTitleFont.setHeight (h);
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
            g.drawText ("v1.0", versionX, versionY, versionW, versionH,
                juce::Justification::bottomRight, false);

        g.setFont (juce::Font (juce::FontOptions (baseFontPx).withStyle ("Bold")));
    }

    {
        const int v = (int) std::llround (amountSlider.getValue());

        drawLegendForMode (amountValueArea,
                           cachedAmountTextFull,
                           cachedAmountTextShort,
                           juce::String (v),
                           barTailTuning);
    }

    {
        const int v = (int) std::llround (seriesSlider.getValue());

        drawLegendForMode (seriesValueArea,
                           cachedSeriesTextFull,
                           cachedSeriesTextShort,
                           juce::String (v),
                           barTailTuning);
    }

    {
        const juce::String hzLegend = cachedFreqTextHz;
        const juce::String intOnly = cachedFreqIntOnly;
        const juce::String freqTailTuning = "-2";

        drawLegendForMode (freqValueArea,
                           hzLegend,
                           hzLegend,
                           intOnly,
                           freqTailTuning);
    }

    {
        const juce::String intOnly = cachedShapeIntOnly;

        drawLegendForMode (shapeValueArea,
                   cachedShapeTextFull,
                   cachedShapeTextShort,
                           intOnly,
                           barTailTuning);
    }

    {
        auto drawToggleLegend = [&] (const juce::Rectangle<int>& labelArea,
                                     const juce::String& labelText,
                                     int noCollisionRight,
                                     const juce::String& tailTuning)
        {
            const int safeW = juce::jmax (0, noCollisionRight - labelArea.getX());
            // snap to integer/even coordinates to avoid sub-pixel artefacts on resize
            auto snapEven = [] (int v) { return v & ~1; };
            const int ax = snapEven (labelArea.getX());
            const int ay = snapEven (labelArea.getY());
            const int aw = snapEven (safeW);
            const int ah = labelArea.getHeight();
            const auto drawArea = juce::Rectangle<int> (ax, ay, aw, ah);

            if (fxTailEnabled)
                drawTextWithRepeatedLastCharGradient (g, drawArea, labelText, getWidth(), lnf.getTrailingTextGradient(), noCollisionRight,
                                      tailTuning, "20%", "pyramid");
            else
                g.drawText (labelText, drawArea.getX(), drawArea.getY(), drawArea.getWidth(), drawArea.getHeight(), juce::Justification::left, true);
        };

        drawToggleLegend (getRvsLabelArea(), "RVS", invButton.getX() - kToggleLegendCollisionPadPx, "-3");
        drawToggleLegend (getInvLabelArea(), "INV", amountValueArea.getRight(), "-2");
    }
    g.setColour (scheme.text);

    {
        if (cachedInfoGearPath.isEmpty())
            updateInfoIconCache();

        // filled white gear + center cutout
        g.setColour (scheme.text);
        g.fillPath (cachedInfoGearPath);
        g.strokePath (cachedInfoGearPath, juce::PathStrokeType (1.0f));

        g.setColour (scheme.bg);
        g.fillEllipse (cachedInfoGearHole);
    }

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

void DisperserAudioProcessorEditor::updateLegendVisibility()
{
    constexpr float baseFontPx = 40.0f;
    constexpr float minFontPx  = 18.0f;
    const float softShrinkFloorFull  = juce::jmax (minFontPx, baseFontPx * 0.88f);
    const float softShrinkFloorShort = minFontPx;

    juce::Font measureFont (juce::FontOptions (baseFontPx).withStyle ("Bold"));

    auto areaAmount = getValueAreaFor (amountSlider.getBounds());
    auto areaSeries = getValueAreaFor (seriesSlider.getBounds());
    auto areaFreq   = getValueAreaFor (freqSlider.getBounds());
    auto areaShape = getValueAreaFor (shapeSlider.getBounds());

    // Check FULL versions using fixed worst-case templates (stable, value-independent)
    const juce::String amountFull  = kAmountLegendFull;
    const juce::String freqFull    = kFreqLegendDisplay;
    const juce::String seriesFull  = kSeriesLegendFull;
    const juce::String shapeFull   = kShapeLegendFull;

    const bool amountFullFits = fitsWithOptionalShrink_NoG (measureFont, amountFull, areaAmount.getWidth(), baseFontPx, softShrinkFloorFull);
    const bool freqFullFits = fitsWithOptionalShrink_NoG (measureFont, freqFull, areaFreq.getWidth(), baseFontPx, softShrinkFloorFull);
    const bool seriesFullFits = fitsWithOptionalShrink_NoG (measureFont, seriesFull, areaSeries.getWidth(), baseFontPx, softShrinkFloorFull);
    const bool shapeFullFits = fitsWithOptionalShrink_NoG (measureFont, shapeFull, areaShape.getWidth(), baseFontPx, softShrinkFloorFull);

    // Check SHORT versions using fixed worst-case templates (stable, value-independent)
    const juce::String amountShort = kAmountLegendShort;
    const juce::String freqShort   = kFreqLegendDisplay;
    const juce::String seriesShort = kSeriesLegendShort;
    const juce::String shapeShort  = kShapeLegendShort;

    const bool amountShortFits = fitsWithOptionalShrink_NoG (measureFont, amountShort, areaAmount.getWidth(), baseFontPx, softShrinkFloorShort);
    const bool freqShortFits = fitsWithOptionalShrink_NoG (measureFont, freqShort, areaFreq.getWidth(), baseFontPx, softShrinkFloorShort);
    const bool seriesShortFits = fitsWithOptionalShrink_NoG (measureFont, seriesShort, areaSeries.getWidth(), baseFontPx, softShrinkFloorShort);
    const bool shapeShortFits = fitsWithOptionalShrink_NoG (measureFont, shapeShort, areaShape.getWidth(), baseFontPx, softShrinkFloorShort);

    // Determine global mode: 0=Full, 1=Short, 2=None
    const bool anyFullFailed = (!amountFullFits || !freqFullFits || !seriesFullFits || !shapeFullFits);
    const bool anyShortFailed = (!amountShortFits || !freqShortFits || !seriesShortFits || !shapeShortFits);

    if (anyShortFailed)
        labelVisibilityMode = 2;  // None
    else if (anyFullFailed)
        labelVisibilityMode = 1;  // Short
    else
        labelVisibilityMode = 0;  // Full
}

void DisperserAudioProcessorEditor::resized()
{
    refreshLegendTextCache();

    // If the user is actively dragging/resizing (mouse down), treat this
    // as a recent user interaction so size persistence will occur immediately.
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

    const auto horizontalLayout = makeHorizontalLayoutMetrics (W, getTargetValueColumnWidth());
    const auto verticalLayout = makeVerticalLayoutMetrics (H, kLayoutVerticalBiasPx);

    amountSlider.setBounds    (horizontalLayout.leftX, verticalLayout.topY + 0 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);
    seriesSlider.setBounds    (horizontalLayout.leftX, verticalLayout.topY + 1 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);
    freqSlider.setBounds      (horizontalLayout.leftX, verticalLayout.topY + 2 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);
    shapeSlider.setBounds     (horizontalLayout.leftX, verticalLayout.topY + 3 * (verticalLayout.barH + verticalLayout.gapY), horizontalLayout.barW, verticalLayout.barH);

    const int buttonAreaX = horizontalLayout.leftX;
    const int buttonAreaW = horizontalLayout.contentW;

    juce::Font labelFont (juce::FontOptions (40.0f).withStyle ("Bold"));
    const int rvsLabelW = stringWidth (labelFont, "RVS") + 2;
    const int invLabelW = stringWidth (labelFont, "INV") + 2;
    const int labelGap = kToggleLabelGapPx;

    const int toggleVisualSide = juce::jlimit (14,
                                               juce::jmax (14, verticalLayout.box - 2),
                                               (int) std::lround ((double) verticalLayout.box * 0.50));
    const int toggleHitW = toggleVisualSide + 6;

    const int rvsBlockW = juce::jmax (toggleHitW, toggleHitW + labelGap + rvsLabelW);
    const int invBlockW = juce::jmax (toggleHitW, toggleHitW + labelGap + invLabelW);

    const int valueStartX = horizontalLayout.leftX + horizontalLayout.barW + horizontalLayout.valuePad;
    const int rvsAnchorX = horizontalLayout.leftX;
    const int invAnchorX = valueStartX;

    int rvsBlockX = rvsAnchorX;
    int invBlockX = invAnchorX;

    const int invMinX = juce::jmax (invAnchorX, rvsBlockX + rvsBlockW + kMinToggleBlocksGapPx);
    const int invMaxX = buttonAreaX + buttonAreaW - invBlockW;
    if (invMinX <= invMaxX)
        invBlockX = juce::jlimit (invMinX, invMaxX, invBlockX);
    else
        invBlockX = invMaxX;

    rvsButton.setBounds (rvsBlockX, verticalLayout.btnY, toggleHitW, verticalLayout.box);
    invButton.setBounds (invBlockX, verticalLayout.btnY, toggleHitW, verticalLayout.box);

    if (resizerCorner != nullptr)
        resizerCorner->setBounds (W - kResizerCornerPx, H - kResizerCornerPx, kResizerCornerPx, kResizerCornerPx);

    promptOverlay.setBounds (getLocalBounds());
    if (promptOverlayActive)
        promptOverlay.toFront (false);

    updateInfoIconCache();

    // Update legend visibility globally: if ANY slider cannot fit its labels, ALL are disabled
    updateLegendVisibility();

    // Don't modify the constrainer here to avoid reentrancy issues.
}

