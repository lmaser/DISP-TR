#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <fstream>
#include <chrono>

// Set to 1 to enable debug file logging (writes to disk can cause CPU spikes).
#define DISPTR_ENABLE_DEBUG 1
// Enable lightweight profiling counters for reverse (RVS) path.
// We enable profiling by default for instrumented runs.
#define DISPTR_PROFILE_RVS 1
// If set to 1, compile-time option to disable reverse processing entirely
// (useful to compare perf with RVS off without changing the UI).
// For profiling runs we want RVS active, so set to 0.
#define DISPTR_DISABLE_RVS 0

// Low-CPU automatic mode: when avg µs/block exceeds this threshold,
// enable a reduced-quality processing mode for the following number of blocks.
#define DISPTR_LOW_CPU_THRESHOLD_US 1200
#define DISPTR_LOW_CPU_DURATION_BLOCKS 480

// Macros for tight per-stage processing to avoid lambda/function overhead
#define PROCESS_CHAIN_MONO(stptr, x) do { int s = 0; for (; s + 1 < numStages; s += 2) { x = (stptr)[s].left.process (x); x = (stptr)[s + 1].left.process (x); } if (s < numStages) x = (stptr)[s].left.process (x); } while (0)
#define PROCESS_CHAIN_STEREO(stptr, xL, xR) do { int s = 0; for (; s + 1 < numStages; s += 2) { xL = (stptr)[s].left.process (xL); xR = (stptr)[s].right.process (xR); xL = (stptr)[s + 1].left.process (xL); xR = (stptr)[s + 1].right.process (xR); } if (s < numStages) { xL = (stptr)[s].left.process (xL); xR = (stptr)[s].right.process (xR); } } while (0)

namespace
{
    constexpr bool kUseActiveStageCoeffPropagation = false;
    constexpr int kMaxSafeWindowSamples = 1 << 20;
    constexpr int kMaxSafeTransitionSamples = 1 << 20;

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

    inline double sanitizeSampleRate (double sr) noexcept
    {
        return (std::isfinite (sr) && sr > 0.0) ? sr : 0.0;
    }

    inline float loadAtomicOrDefault (const std::atomic<float>* p, float fallback) noexcept
    {
        const float v = (p != nullptr) ? p->load (std::memory_order_relaxed) : fallback;
        return std::isfinite (v) ? v : fallback;
    }

    inline int loadIntParamOrDefault (const std::atomic<float>* p, int fallback) noexcept
    {
        return (int) std::lround (loadAtomicOrDefault (p, (float) fallback));
    }

    inline bool loadBoolParamOrDefault (const std::atomic<float>* p, bool fallback = false) noexcept
    {
        return p != nullptr ? (p->load (std::memory_order_relaxed) > 0.5f) : fallback;
    }

    inline void setParameterPlainValue (juce::AudioProcessorValueTreeState& apvts,
                                        const char* paramId,
                                        float plainValue)
    {
        if (auto* p = apvts.getParameter (paramId))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
            p->endChangeGesture();
        }

        apvts.state.setProperty (paramId, plainValue, nullptr);
    }
}

DisperserAudioProcessor::DisperserAudioProcessor()
: AudioProcessor (BusesProperties()
                  .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                  .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
  apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    amountParam = apvts.getRawParameterValue (kParamAmount);
    seriesParam = apvts.getRawParameterValue (kParamSeries);
    freqParam = apvts.getRawParameterValue (kParamFreq);
    resonanceParam = apvts.getRawParameterValue (kParamResonance);
    reverseParam = apvts.getRawParameterValue (kParamReverse);
    invParam = apvts.getRawParameterValue (kParamInv);
    s0Param = apvts.getRawParameterValue (kParamS0);
    s100Param = apvts.getRawParameterValue (kParamS100);
    uiWidthParam = apvts.getRawParameterValue (kParamUiWidth);
    uiHeightParam = apvts.getRawParameterValue (kParamUiHeight);
    uiPaletteParam = apvts.getRawParameterValue (kParamUiPalette);
    uiFxTailParam = apvts.getRawParameterValue (kParamUiFxTail);
    uiColorParams[0] = apvts.getRawParameterValue (kParamUiColor0);
    uiColorParams[1] = apvts.getRawParameterValue (kParamUiColor1);
    uiColorParams[2] = apvts.getRawParameterValue (kParamUiColor2);
    uiColorParams[3] = apvts.getRawParameterValue (kParamUiColor3);

        jassert (amountParam != nullptr);
        jassert (seriesParam != nullptr);
        jassert (freqParam != nullptr);
        jassert (resonanceParam != nullptr);
        jassert (reverseParam != nullptr);
        jassert (invParam != nullptr);
}

DisperserAudioProcessor::~DisperserAudioProcessor() = default;

const juce::String DisperserAudioProcessor::getName() const { return JucePlugin_Name; }
bool DisperserAudioProcessor::acceptsMidi() const { return false; }
bool DisperserAudioProcessor::producesMidi() const { return false; }
bool DisperserAudioProcessor::isMidiEffect() const { return false; }
double DisperserAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int DisperserAudioProcessor::getNumPrograms() { return 1; }
int DisperserAudioProcessor::getCurrentProgram() { return 0; }
void DisperserAudioProcessor::setCurrentProgram (int) {}
const juce::String DisperserAudioProcessor::getProgramName (int) { return {}; }
void DisperserAudioProcessor::changeProgramName (int, const juce::String&) {}

void DisperserAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);

    const double safeSampleRate = sanitizeSampleRate (sampleRate);
    if (safeSampleRate <= 0.0)
    {
        jassertfalse;
        return;
    }

    currentSampleRate = safeSampleRate;

    const double smoothTimeSec = 0.02;
    freqSmoothed.reset (safeSampleRate, smoothTimeSec);
    resonanceSmoothed.reset (safeSampleRate, smoothTimeSec);
    amountSmoothed.reset (safeSampleRate, 0.05);

    const float freqInit = loadAtomicOrDefault (freqParam, kFreqDefault);
    const float resoInit = loadAtomicOrDefault (resonanceParam, kResonanceDefault);
    const float amountInit = loadAtomicOrDefault (amountParam, (float) kAmountDefault);

    freqSmoothed.setCurrentAndTargetValue (freqInit);
    resonanceSmoothed.setCurrentAndTargetValue (resoInit);
    amountSmoothed.setCurrentAndTargetValue (amountInit);

    engA.init (safeSampleRate);
    engB.init (safeSampleRate);

    const int amount = juce::jlimit (kAmountMin, kAmountMax, (int) std::lround (amountSmoothed.getCurrentValue()));
    const int series = juce::jlimit (kSeriesMin, kSeriesMax, loadIntParamOrDefault (seriesParam, kSeriesDefault));
    const bool reverse = loadBoolParamOrDefault (reverseParam, false);

    const float f0 = freqSmoothed.getCurrentValue();
    const float r0 = resonanceSmoothed.getCurrentValue();

    engA.setTopology (amount, series, reverse, f0, r0);

    cachedAmountKey = amount;
    cachedSeriesKey = series;
    cachedReverseKey = reverse;

    inTransition = false;
    transitionSamples = (int) std::round (0.050 * safeSampleRate);
    transitionSamples = juce::jlimit (16, kMaxSafeTransitionSamples, transitionSamples);
    transitionPos = 0;
}

void DisperserAudioProcessor::releaseResources() {}

#if ! JucePlugin_PreferredChannelConfigurations
bool DisperserAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}
#endif

void DisperserAudioProcessor::startTransitionIfNeeded (int newAmount, int newSeries, bool newReverse,
                                                      float freqNow, float shapeNow)
{
    newAmount = juce::jlimit (kAmountMin, kAmountMax, newAmount);
    newSeries = juce::jlimit (kSeriesMin, kSeriesMax, newSeries);

    auto setCachedTopologyKeys = [&]()
    {
        cachedAmountKey = newAmount;
        cachedSeriesKey = newSeries;
        cachedReverseKey = newReverse;
    };

    const bool topoChanged =
        (newAmount != cachedAmountKey) ||
        (newSeries != cachedSeriesKey) ||
        (newReverse != cachedReverseKey);

    if (! topoChanged)
        return;

    if (inTransition)
    {
        hasPendingTopology = true;
        pendingAmount = newAmount;
        pendingSeries = newSeries;
        pendingReverse = newReverse;

        setCachedTopologyKeys();
        return;
    }

    engB.setTopology (newAmount, newSeries, newReverse, freqNow, shapeNow);

    inTransition = true;
    transitionPos = 0;

    setCachedTopologyKeys();
}

void DisperserAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    const int totalNumInputChannels  = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();

    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const float freqParamNow = loadAtomicOrDefault (freqParam, kFreqDefault);
    const float shapeParamNow = loadAtomicOrDefault (resonanceParam, kResonanceDefault);  //Param is called resonance but represents shape
    const float amountParamNow = loadAtomicOrDefault (amountParam, (float) kAmountDefault);
    const int series = juce::jlimit (kSeriesMin, kSeriesMax, loadIntParamOrDefault (seriesParam, kSeriesDefault));
    const bool reverse = loadBoolParamOrDefault (reverseParam, false);
    const bool invPol = loadBoolParamOrDefault (invParam, false);
    const bool debugS0 = loadBoolParamOrDefault (s0Param, false);
    const bool debugS100 = loadBoolParamOrDefault (s100Param, false);

    freqSmoothed.setTargetValue (freqParamNow);
    resonanceSmoothed.setTargetValue (shapeParamNow);
    amountSmoothed.setTargetValue (amountParamNow);

    const float freqStart = freqSmoothed.getCurrentValue();
    const float shapeStart = resonanceSmoothed.getCurrentValue();

    freqSmoothed.skip (numSamples);
    resonanceSmoothed.skip (numSamples);

    const float freqEnd = freqSmoothed.getCurrentValue();
    const float shapeEnd = resonanceSmoothed.getCurrentValue();

    const float amountStart = amountSmoothed.getCurrentValue();
    amountSmoothed.skip (numSamples);
    const float amountEnd = amountSmoothed.getCurrentValue();

    const float freqNow = 0.5f * (freqStart + freqEnd);
    float shapeNow = 0.5f * (shapeStart + shapeEnd);
    const float amountNowContinuous = 0.5f * (amountStart + amountEnd);
    
    // Debug override: S0 = force shape to 0 (Button1), S100 = force shape to 1 (Button2)
    if (debugS0) shapeNow = 0.0f;
    if (debugS100) shapeNow = 1.0f;
    const int amountNowRounded = juce::jlimit (kAmountMin, kAmountMax, (int) std::lround (amountNowContinuous));

    int amountNow = amountNowRounded;
    if (cachedAmountKey >= 0)
    {
        constexpr float kAmountHysteresis = 0.60f;
        const float lowerToChange = (float) cachedAmountKey - kAmountHysteresis;
        const float upperToChange = (float) cachedAmountKey + kAmountHysteresis;

        if (amountNowContinuous > lowerToChange && amountNowContinuous < upperToChange)
            amountNow = cachedAmountKey;
    }

    const float outputGain = invPol ? -1.0f : 1.0f;

    startTransitionIfNeeded (amountNow, series, reverse, freqNow, shapeNow);

    // Debug file logging is disabled by default because of I/O cost.
#if DISPTR_ENABLE_DEBUG
    try
    {
        std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/param_debug.txt", std::ios::app);
        if (dbg)
        {
            dbg << "processBlock: freqParamNow=" << freqParamNow
                << " shapeParamNow=" << shapeParamNow
                << " amountParamNow=" << amountParamNow
                << " freqNow=" << freqNow
                << " shapeNow=" << shapeNow;
            // also log state property for shape/resonance
            const auto stateShape = apvts.state.getProperty (kParamResonance);
            if (! stateShape.isVoid())
                dbg << " stateShape=" << (double) (float) stateShape;
            else
                dbg << " stateShape=void";
            dbg << std::endl;
            dbg.close();
        }
    }
    catch (...) {}
#endif

    if (! inTransition)
    {
        engA.amount  = amountNow;
        engA.series  = juce::jlimit (kSeriesMin, kSeriesMax, series);
        engA.reverse = reverse;

        engA.processBlock (buffer, freqNow, shapeNow, outputGain);

        return;
    }

    const int channels = buffer.getNumChannels();
    if (transitionBufferB.getNumChannels() != channels || transitionBufferB.getNumSamples() != numSamples)
        transitionBufferB.setSize (channels, numSamples, false, false, true);
    transitionBufferB.makeCopyOf (buffer, true);

    engA.processBlock (buffer, freqNow, shapeNow, outputGain);
    engB.processBlock (transitionBufferB, freqNow, shapeNow, outputGain);

    auto* const* outPtrs = buffer.getArrayOfWritePointers();
    const float* const* bPtrs = transitionBufferB.getArrayOfReadPointers();
    const int safeTransitionSamples = juce::jmax (1, transitionSamples);
    const float invTransitionSamples = 1.0f / (float) safeTransitionSamples;

    const int remainingRampSamples = juce::jmax (0, safeTransitionSamples - transitionPos);
    const int rampSamples = juce::jmin (numSamples, remainingRampSamples);

    if (channels == 1)
    {
        float* out0 = outPtrs[0];
        const float* b0 = bPtrs[0];

        for (int n = 0; n < rampSamples; ++n)
        {
            const float t = (float) transitionPos * invTransitionSamples;
            const float a = 1.0f - t;

            out0[n] = a * out0[n] + t * b0[n];
            ++transitionPos;
        }

        for (int n = rampSamples; n < numSamples; ++n)
            out0[n] = b0[n];
    }
    else if (channels == 2)
    {
        float* out0 = outPtrs[0];
        float* out1 = outPtrs[1];
        const float* b0 = bPtrs[0];
        const float* b1 = bPtrs[1];

        for (int n = 0; n < rampSamples; ++n)
        {
            const float t = (float) transitionPos * invTransitionSamples;
            const float a = 1.0f - t;

            out0[n] = a * out0[n] + t * b0[n];
            out1[n] = a * out1[n] + t * b1[n];
            ++transitionPos;
        }

        for (int n = rampSamples; n < numSamples; ++n)
        {
            out0[n] = b0[n];
            out1[n] = b1[n];
        }
    }
    else
    {
        for (int n = 0; n < rampSamples; ++n)
        {
            const float t = (float) transitionPos * invTransitionSamples;
            const float a = 1.0f - t;

            for (int ch = 0; ch < channels; ++ch)
                outPtrs[ch][n] = a * outPtrs[ch][n] + t * bPtrs[ch][n];

            ++transitionPos;
        }

        for (int n = rampSamples; n < numSamples; ++n)
            for (int ch = 0; ch < channels; ++ch)
                outPtrs[ch][n] = bPtrs[ch][n];
    }

    if (transitionPos >= safeTransitionSamples)
    {
        engA.swap (engB);

        inTransition = false;
        transitionPos = 0;

        if (hasPendingTopology)
        {
            hasPendingTopology = false;
            engB.setTopology (pendingAmount, pendingSeries, pendingReverse, freqNow, shapeNow);
            inTransition = true;
            transitionPos = 0;
        }
    }

}

bool DisperserAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* DisperserAudioProcessor::createEditor()
{
    return new DisperserAudioProcessorEditor (*this);
}

void DisperserAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Don't sample the active editor size here; use the atomics written by the editor to avoid host overwrite.

    auto stateToSave = apvts.copyState();
    stateToSave.setProperty (UiStateKeys::editorWidth, uiEditorWidth.load (std::memory_order_relaxed), nullptr);
    stateToSave.setProperty (UiStateKeys::editorHeight, uiEditorHeight.load (std::memory_order_relaxed), nullptr);
    stateToSave.setProperty (UiStateKeys::useCustomPalette, uiUseCustomPalette.load (std::memory_order_relaxed) != 0, nullptr);
    stateToSave.setProperty (UiStateKeys::fxTailEnabled, uiFxTailEnabled.load (std::memory_order_relaxed) != 0, nullptr);
    for (int i = 0; i < 4; ++i)
    {
        const int paletteValue = (int) apvts.state.getProperty (UiStateKeys::customPalette[(size_t) i],
                                                                 (int) uiCustomPalette[(size_t) i].load (std::memory_order_relaxed));
        uiCustomPalette[(size_t) i].store ((juce::uint32) paletteValue, std::memory_order_relaxed);
        stateToSave.setProperty (UiStateKeys::customPalette[(size_t) i],
                                 paletteValue,
                                 nullptr);
    }

    // Debug: write saved UI size to a file for diagnostics
    try
    {
        std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/ui_state_saved.txt", std::ios::app);
        if (dbg)
        {
            dbg << "getStateInformation: saving uiEditorWidth=" << uiEditorWidth.load (std::memory_order_relaxed)
                << " uiEditorHeight=" << uiEditorHeight.load (std::memory_order_relaxed) << std::endl;
            dbg.close();
        }
    }
    catch (...) {}

    // Append profiling counters (written from GUI/host thread via this call,
    // safe because we only read atomic counters). This helps capture runtime
    // profiling without performing I/O on the audio thread.
    try
    {
        std::ofstream pdbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/profile_summary.txt", std::ios::app);
        if (pdbg)
        {
            const uint64_t aRev = engA.profileReverseUs.load (std::memory_order_relaxed);
            const uint64_t aOth = engA.profileOtherUs.load (std::memory_order_relaxed);
            const uint64_t aBlk = engA.profileBlocks.load (std::memory_order_relaxed);

            const uint64_t bRev = engB.profileReverseUs.load (std::memory_order_relaxed);
            const uint64_t bOth = engB.profileOtherUs.load (std::memory_order_relaxed);
            const uint64_t bBlk = engB.profileBlocks.load (std::memory_order_relaxed);

              const uint64_t aGrab = engA.profileGrabUs.load (std::memory_order_relaxed);
              const uint64_t aFrame = engA.profileFrameUs.load (std::memory_order_relaxed);
              const uint64_t aOla = engA.profileOlaUs.load (std::memory_order_relaxed);

              const uint64_t bGrab = engB.profileGrabUs.load (std::memory_order_relaxed);
              const uint64_t bFrame = engB.profileFrameUs.load (std::memory_order_relaxed);
              const uint64_t bOla = engB.profileOlaUs.load (std::memory_order_relaxed);

              pdbg << "profile_summary: engA reverseUs=" << aRev << " otherUs=" << aOth << " blocks=" << aBlk
                  << " | engB reverseUs=" << bRev << " otherUs=" << bOth << " blocks=" << bBlk << std::endl;
              pdbg << "  engA grabUs=" << aGrab << " frameUs=" << aFrame << " olaUs=" << aOla
                  << " | engB grabUs=" << bGrab << " frameUs=" << bFrame << " olaUs=" << bOla << std::endl;
            pdbg.close();
        }
    }
    catch (...) {}

    juce::MemoryOutputStream mos (destData, true);
    stateToSave.writeToStream (mos);
}

void DisperserAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const juce::ValueTree tree = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (tree.isValid())
    {
        apvts.replaceState (tree);

        uiEditorWidth.store ((int) apvts.state.getProperty (UiStateKeys::editorWidth,
                                                            uiEditorWidth.load (std::memory_order_relaxed)),
                             std::memory_order_relaxed);
        uiEditorHeight.store ((int) apvts.state.getProperty (UiStateKeys::editorHeight,
                                                             uiEditorHeight.load (std::memory_order_relaxed)),
                              std::memory_order_relaxed);
        uiUseCustomPalette.store ((bool) apvts.state.getProperty (UiStateKeys::useCustomPalette,
                                                                  uiUseCustomPalette.load (std::memory_order_relaxed) != 0)
                                     ? 1 : 0,
                                  std::memory_order_relaxed);
        uiFxTailEnabled.store ((bool) apvts.state.getProperty (UiStateKeys::fxTailEnabled,
                                                               uiFxTailEnabled.load (std::memory_order_relaxed) != 0)
                                  ? 1 : 0,
                               std::memory_order_relaxed);

        for (int i = 0; i < 4; ++i)
        {
            const auto stored = apvts.state.getProperty (UiStateKeys::customPalette[(size_t) i],
                                                         (int) uiCustomPalette[(size_t) i].load (std::memory_order_relaxed));
            uiCustomPalette[(size_t) i].store ((juce::uint32) (int) stored, std::memory_order_relaxed);
        }

        // Debug: write loaded UI size to a file for diagnostics
        try
        {
            std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/ui_state_loaded.txt", std::ios::app);
            if (dbg)
            {
                dbg << "setStateInformation: loaded uiEditorWidth=" << uiEditorWidth.load (std::memory_order_relaxed)
                    << " uiEditorHeight=" << uiEditorHeight.load (std::memory_order_relaxed) << std::endl;
                dbg.close();
            }
        }
        catch (...) {}
    }
}

void DisperserAudioProcessor::getCurrentProgramStateInformation (juce::MemoryBlock& destData)
{
    getStateInformation (destData);
}

void DisperserAudioProcessor::setCurrentProgramStateInformation (const void* data, int sizeInBytes)
{
    setStateInformation (data, sizeInBytes);
}

void DisperserAudioProcessor::setUiEditorSize (int width, int height)
{
    const int safeWidth = juce::jmax (1, width);
    const int safeHeight = juce::jmax (1, height);

    uiEditorWidth.store (safeWidth, std::memory_order_relaxed);
    uiEditorHeight.store (safeHeight, std::memory_order_relaxed);

    apvts.state.setProperty (UiStateKeys::editorWidth, safeWidth, nullptr);
    apvts.state.setProperty (UiStateKeys::editorHeight, safeHeight, nullptr);

    // Do NOT expose UI size as automatable parameters; persist via ValueTree only.
}

int DisperserAudioProcessor::getUiEditorWidth() const noexcept
{
    const auto fromState = apvts.state.getProperty (UiStateKeys::editorWidth);
    if (! fromState.isVoid())
        return juce::jmax (1, (int) fromState);
    int ret = 0;
    int paramVal = -1;
    if (uiWidthParam != nullptr)
        paramVal = (int) std::lround (uiWidthParam->load (std::memory_order_relaxed));

    const int atomicVal = uiEditorWidth.load (std::memory_order_relaxed);

    if (uiWidthParam != nullptr)
        ret = juce::jmax (1, paramVal);
    else
        ret = juce::jmax (1, atomicVal);

    // Debug: log source values
    try
    {
        std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/ui_state_get.log", std::ios::app);
        if (dbg)
        {
            dbg << "getUiEditorWidth: fromStateVoid=" << fromState.isVoid()
                << " paramVal=" << paramVal
                << " atomicVal=" << atomicVal
                << " ret=" << ret << std::endl;
            dbg.close();
        }
    }
    catch (...) {}

    return ret;
}

int DisperserAudioProcessor::getUiEditorHeight() const noexcept
{
    const auto fromState = apvts.state.getProperty (UiStateKeys::editorHeight);
    if (! fromState.isVoid())
        return juce::jmax (1, (int) fromState);
    int ret = 0;
    int paramVal = -1;
    if (uiHeightParam != nullptr)
        paramVal = (int) std::lround (uiHeightParam->load (std::memory_order_relaxed));

    const int atomicVal = uiEditorHeight.load (std::memory_order_relaxed);

    if (uiHeightParam != nullptr)
        ret = juce::jmax (1, paramVal);
    else
        ret = juce::jmax (1, atomicVal);

    // Debug: log source values
    try
    {
        std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/ui_state_get.log", std::ios::app);
        if (dbg)
        {
            dbg << "getUiEditorHeight: fromStateVoid=" << fromState.isVoid()
                << " paramVal=" << paramVal
                << " atomicVal=" << atomicVal
                << " ret=" << ret << std::endl;
            dbg.close();
        }
    }
    catch (...) {}

    return ret;
}

void DisperserAudioProcessor::setUiUseCustomPalette (bool shouldUseCustomPalette)
{
    uiUseCustomPalette.store (shouldUseCustomPalette ? 1 : 0, std::memory_order_relaxed);
    apvts.state.setProperty (UiStateKeys::useCustomPalette, shouldUseCustomPalette, nullptr);

    setParameterPlainValue (apvts, kParamUiPalette, shouldUseCustomPalette ? 1.0f : 0.0f);

    updateHostDisplay();
}

bool DisperserAudioProcessor::getUiUseCustomPalette() const noexcept
{
    const auto fromState = apvts.state.getProperty (UiStateKeys::useCustomPalette);
    if (! fromState.isVoid())
        return (bool) fromState;

    if (uiPaletteParam != nullptr)
        return uiPaletteParam->load (std::memory_order_relaxed) > 0.5f;

    return uiUseCustomPalette.load (std::memory_order_relaxed) != 0;
}

void DisperserAudioProcessor::setUiFxTailEnabled (bool shouldEnableFxTail)
{
    uiFxTailEnabled.store (shouldEnableFxTail ? 1 : 0, std::memory_order_relaxed);
    apvts.state.setProperty (UiStateKeys::fxTailEnabled, shouldEnableFxTail, nullptr);

    setParameterPlainValue (apvts, kParamUiFxTail, shouldEnableFxTail ? 1.0f : 0.0f);

    updateHostDisplay();
}

bool DisperserAudioProcessor::getUiFxTailEnabled() const noexcept
{
    const auto fromState = apvts.state.getProperty (UiStateKeys::fxTailEnabled);
    if (! fromState.isVoid())
        return (bool) fromState;

    if (uiFxTailParam != nullptr)
        return uiFxTailParam->load (std::memory_order_relaxed) > 0.5f;

    return uiFxTailEnabled.load (std::memory_order_relaxed) != 0;
}

void DisperserAudioProcessor::setUiCustomPaletteColour (int index, juce::Colour colour)
{
    const int safeIndex = juce::jlimit (0, 3, index);
    const auto argb = colour.getARGB();
    const int rgb = ((int) colour.getRed() << 16) | ((int) colour.getGreen() << 8) | (int) colour.getBlue();

    uiCustomPalette[(size_t) safeIndex].store (argb, std::memory_order_relaxed);
    apvts.state.setProperty (UiStateKeys::customPalette[(size_t) safeIndex], (int) argb, nullptr);

    const char* colourParamIds[4] { kParamUiColor0, kParamUiColor1, kParamUiColor2, kParamUiColor3 };
    setParameterPlainValue (apvts, colourParamIds[safeIndex], (float) rgb);

    updateHostDisplay();
}

juce::Colour DisperserAudioProcessor::getUiCustomPaletteColour (int index) const noexcept
{
    const int safeIndex = juce::jlimit (0, 3, index);

    const auto fromState = apvts.state.getProperty (UiStateKeys::customPalette[(size_t) safeIndex]);
    if (! fromState.isVoid())
        return juce::Colour ((juce::uint32) (int) fromState);

    if (uiColorParams[(size_t) safeIndex] != nullptr)
    {
        const int rgb = juce::jlimit (0, 0xFFFFFF,
                                      (int) std::lround (uiColorParams[(size_t) safeIndex]->load (std::memory_order_relaxed)));
        const juce::uint8 r = (juce::uint8) ((rgb >> 16) & 0xFF);
        const juce::uint8 g = (juce::uint8) ((rgb >> 8) & 0xFF);
        const juce::uint8 b = (juce::uint8) (rgb & 0xFF);
        return juce::Colour::fromRGB (r, g, b);
    }

    return juce::Colour (uiCustomPalette[(size_t) safeIndex].load (std::memory_order_relaxed));
}

juce::AudioProcessorValueTreeState::ParameterLayout DisperserAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        kParamAmount, "Stages", kAmountMin, kAmountMax, kAmountDefault));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        kParamSeries, "Series", kSeriesMin, kSeriesMax, kSeriesDefault));

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        kParamFreq, "Frequency",
        juce::NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.35f), kFreqDefault));

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        kParamResonance, "Resonance",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kResonanceDefault));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        kParamReverse, "Reverse", false));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        kParamInv, "Inv", false));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        kParamS0, "S0", false));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        kParamS100, "S100", false));

    // UI width/height are persisted via ValueTree properties but should not
    // be exposed as automatable parameters to the host (hosts can map/learn them).
    // We intentionally do NOT create parameters for kParamUiWidth / kParamUiHeight here.

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        kParamUiPalette, "UI Palette", false));

    params.push_back (std::make_unique<juce::AudioParameterBool>(
        kParamUiFxTail, "UI FX Tail", true));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        kParamUiColor0, "UI Color 0", 0, 0xFFFFFF, 0xFFFFFF));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        kParamUiColor1, "UI Color 1", 0, 0xFFFFFF, 0x000000));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        kParamUiColor2, "UI Color 2", 0, 0xFFFFFF, 0xFFFFFF));

    params.push_back (std::make_unique<juce::AudioParameterInt>(
        kParamUiColor3, "UI Color 3", 0, 0xFFFFFF, 0x000000));

    return { params.begin(), params.end() };
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DisperserAudioProcessor();
}

void DisperserAudioProcessor::Engine::init (double sr)
{
    constexpr int kMaxStages = DisperserAudioProcessor::kAmountMax;

    const double safeSampleRate = sanitizeSampleRate (sr);
    if (safeSampleRate <= 0.0)
    {
        jassertfalse;
        return;
    }

    sampleRate = safeSampleRate;
    maxWindowSamples = juce::jlimit (1,
                                     kMaxSafeWindowSamples,
                                     (int) std::ceil (0.250 * sampleRate));

    amount = 0;
    activeStages = 0;
    series = 1;
    reverse = false;

    stageCoeffA.assign ((size_t) kMaxStages, 0.0f);
    for (auto& n : nets)
        n.ensureStages (kMaxStages);

    resetReverseOLA (1);

    cascadeLeft.init ((int) sr, 64);
    cascadeRight.init ((int) sr, 64);

    cachedFreq = -1.0f;
    cachedShape = -1.0f;
    cachedFreqBin = std::numeric_limits<int>::min();
    cachedShapeBin = std::numeric_limits<int>::min();

    // Reset resonator state
    resoLeft.reset();
    resoRight.reset();
    resoMix = 0.0f;
    resoMixTarget = 0.0f;

    // Reset cascade state
    cascadeLeft.reset();
    cascadeRight.reset();
    cascadeMix = 0.0f;
    cascadeMixTarget = 0.0f;
}

void DisperserAudioProcessor::Engine::setTopology (int newAmount, int newSeries, bool newReverse,
                                                  float initFreq, float initShape)
{
    amount = juce::jlimit (DisperserAudioProcessor::kAmountMin,
                           DisperserAudioProcessor::kAmountMax,
                           newAmount);
    series = juce::jlimit (DisperserAudioProcessor::kSeriesMin,
                           DisperserAudioProcessor::kSeriesMax,
                           newSeries);
    reverse = newReverse;

    ensureAllStages (amount);
    updateCoefficientsNow (amount, initFreq, initShape);
    resetAllNetworks();

    if (reverse)
        resetReverseOLA (windowSamplesFromAmount (amount));

    cachedFreq = initFreq;
    cachedShape = initShape;
    cachedFreqBin = std::numeric_limits<int>::min();
    cachedShapeBin = std::numeric_limits<int>::min();
}

void DisperserAudioProcessor::Engine::ensureAllStages (int numStages)
{
    constexpr int kMaxStages = DisperserAudioProcessor::kAmountMax;

    activeStages = juce::jlimit (0, kMaxStages, numStages);

    if ((int) stageCoeffA.size() != activeStages)
        stageCoeffA.assign ((size_t) activeStages, 0.0f);

    for (auto& net : nets)
        net.ensureStages (activeStages);
}

void DisperserAudioProcessor::Engine::resetAllNetworks()
{
    for (auto& net : nets)
        net.resetActive (activeStages);
}

void DisperserAudioProcessor::Engine::applyCoefficientsToNetworks()
{
    const int numStages = activeStages;
    if (numStages <= 0)
        return;

    for (auto& net : nets)
    {
        const int stageCount = (int) net.stages.size();

        if (kUseActiveStageCoeffPropagation)
        {
            if (stageCount < numStages)
                continue;
        }
        else
        {
            if (stageCount != numStages)
                continue;
        }

        StageState* st = net.stages.data();
        for (int i = 0; i < numStages; ++i)
        {
            const float a = stageCoeffA[(size_t) i];
            st[i].left.a  = a;
            st[i].right.a = a;
        }
    }
}

float DisperserAudioProcessor::Engine::allpassCoeffFromFreq (float freqHz, double sr) noexcept
{
    freqHz = juce::jlimit (20.0f, 20000.0f, freqHz);

    const double w = juce::MathConstants<double>::pi * (double) freqHz / sr;
    const double t = std::tan (w);

    const double a = (1.0 - t) / (1.0 + t);
    return (float) juce::jlimit (-0.9999, 0.9999, a);
}

float DisperserAudioProcessor::Engine::amountNorm (int a) noexcept
{
    return juce::jlimit (0.0f, 1.0f, (float) a / (float) DisperserAudioProcessor::kAmountMax);
}

int DisperserAudioProcessor::Engine::windowSamplesFromAmount (int a) const noexcept
{
    const float n = amountNorm (a);
    const float winMs = 5.0f + 245.0f * std::pow (n, 0.70f);
    int ws = (int) std::round ((winMs / 1000.0f) * (float) sampleRate);
    return juce::jlimit (1, maxWindowSamples, ws);
}

void DisperserAudioProcessor::Engine::updateCoefficientsNow (int a, float freqHz, float shape)
{
    const int numStages = activeStages;
    if (numStages <= 0 || sampleRate <= 0.0)
        return;

    // Debug file logging can be enabled for development; disabled by default.
#if DISPTR_ENABLE_DEBUG
    try
    {
        std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/freq_debug.txt", std::ios::app);
        if (dbg)
        {
            dbg << "updateCoefficientsNow: amount=" << a
                << " activeStages=" << numStages
                << " sampleRate=" << sampleRate
                << " freqHz=" << freqHz
                << " shape=" << shape << std::endl;
            dbg.close();
        }
    }
    catch (...) {}
#endif

    const float n = amountNorm (a);
    const float baseSpreadOct = 0.10f + 2.90f * std::pow (n, 0.50f);
    
    // Shape: 0% = wide spread, 100% = collapsed to center (pinch)
    const float shapeNorm = juce::jlimit (0.0f, 1.0f, shape);
    
    // Collapse spread when shape increases - this creates the pinch effect
    const float spreadOct = baseSpreadOct * (1.0f - shapeNorm * 0.99f);
    
    // Gamma: shape controls distribution exponent for additional pinch
    const float gamma = 1.0f + shapeNorm * 4.0f;  // 1.0 to 5.0
    
    // Always use first-order allpass (disable cascade)
    cascadeMixTarget = 0.0f;
    
    // Temporarily set aScale to 1.0 to avoid bias introduced by scaling
    // the allpass tan-domain. If this fixes centering, we can tune
    // aScale more carefully (or make it frequency-dependent).
    const float aScale = 1.0f;

    float fMin = freqHz * std::pow (2.0f, -spreadOct);
    float fMax = freqHz * std::pow (2.0f,  spreadOct);

    const float kLowHz = 20.0f;
    const float kHighHz = 20000.0f;

    // Try to preserve the geometric mean (freqHz) when clamping bounds.
    // If fMin is below kLowHz, clamp fMin to kLowHz and set fMax = freq^2 / fMin
    // (so sqrt(fMin*fMax) == freq). If that pushes fMax above kHighHz, fall
    // back to clamping both to the legal range.
    float fMinRaw = fMin;
    float fMaxRaw = fMax;

    if (fMinRaw < kLowHz)
    {
        fMinRaw = kLowHz;
        fMaxRaw = (freqHz * freqHz) / fMinRaw;
    }

    if (fMaxRaw > kHighHz)
    {
        fMaxRaw = kHighHz;
        fMinRaw = (freqHz * freqHz) / fMaxRaw;
    }

    // Ensure final bounds are inside absolute limits
    fMin = juce::jlimit (kLowHz, kHighHz, fMinRaw);
    fMax = juce::jlimit (kLowHz, kHighHz, fMaxRaw);
    if (fMax <= fMin * 1.0001f)
        fMax = fMin * 1.0001f;

    // Debug: log final fMin/fMax to help diagnose low-frequency centering
#if DISPTR_ENABLE_DEBUG
    try
    {
        std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/freq_debug.txt", std::ios::app);
        if (dbg)
        {
            dbg << "  fMinRaw=" << fMinRaw << " fMaxRaw=" << fMaxRaw
                << " fMin=" << fMin << " fMax=" << fMax << std::endl;
            dbg.close();
        }
    }
    catch (...) {}
#endif

    const float ratio = fMax / fMin;
    const int denom = juce::jmax (1, numStages - 1);

    // Pinch intensity: pull frequencies toward center when shape > 0
    const float pinchIntensityGlobal = shapeNorm * 0.95f;

    for (int i = 0; i < numStages; ++i)
    {
        const float pos = (float) i / (float) denom;
        const float shaped = std::pow (pos, gamma);

        float stageFreq = fMin * std::pow (ratio, shaped);
        stageFreq = juce::jlimit (20.0f, 20000.0f, stageFreq);

        // Apply a 'pinch' (shape/resonance) blend towards the center frequency.
        // Use geometric interpolation (multiplicative) rather than linear mix
        // so frequency ratios are preserved and the behaviour matches filter
        // design expectations for allpass stages.
        const float s = pinchIntensityGlobal;
        if (s > 1e-6f)
        {
            const double lf = std::log ((double) stageFreq);
            const double lc = std::log (juce::jmax (1e-6, (double) freqHz));
            const double blended = std::exp ((1.0 - (double) s) * lf + (double) s * lc);
            stageFreq = (float) juce::jlimit (20.0, 20000.0, blended);
        }

        float aa = allpassCoeffFromFreq (stageFreq, sampleRate);

        // Debug: record first few stage frequencies and coefficients (before and after scaling)
        if (i < 4)
        {
#if DISPTR_ENABLE_DEBUG
            try
            {
                std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/freq_debug.txt", std::ios::app);
                if (dbg)
                {
                    dbg << "  stage " << i << " stageFreq=" << stageFreq;
                    dbg << " coeffA(before)=" << aa;
                    const double t_recon = (1.0 - (double) aa) / (1.0 + (double) aa);
                    const double w_recon = std::atan (t_recon);
                    const double freq_recon = w_recon * sampleRate / juce::MathConstants<double>::pi;
                    dbg << " freqRecon(before)=" << freq_recon;
                    dbg.close();
                }
            }
            catch (...) {}
#endif
        }

        // Apply aScale in the tan(omega/2) domain to avoid shifting the
        // effective centre frequency massively when 'a' is near 1.0.
        // Inverse mapping: t = (1 - a) / (1 + a); then scale t, then recompute a.
        float t = (1.0f - aa) / (1.0f + aa);
        const float tScaled = t * aScale;
        aa = (1.0f - tScaled) / (1.0f + tScaled);
        // Allow coefficients very close to +/-1.0 (but not exactly 1) to
        // preserve very low frequency targets. Using 0.9999 avoids numeric
        // instability while preventing the large frequency shift caused by
        // clamping to 0.99.
        aa = juce::jlimit (-0.9999f, 0.9999f, aa);

        // Log scaled coefficient and reconstructed frequency
        if (i < 4)
        {
#if DISPTR_ENABLE_DEBUG
            try
            {
                std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/freq_debug.txt", std::ios::app);
                if (dbg)
                {
                    dbg << " coeffA(after)=" << aa;
                    const double t_recon2 = (1.0 - (double) aa) / (1.0 + (double) aa);
                    const double w_recon2 = std::atan (t_recon2);
                    const double freq_recon2 = w_recon2 * sampleRate / juce::MathConstants<double>::pi;
                    dbg << " freqRecon(after)=" << freq_recon2 << "\n";
                    dbg.close();
                }
            }
            catch (...) {}
#endif
        }

        stageCoeffA[(size_t) i] = aa;
    }

    // Debug: log gamma and some representative stages (middle and last)
#if DISPTR_ENABLE_DEBUG
    try
    {
        std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/freq_debug.txt", std::ios::app);
        if (dbg)
        {
            dbg << "  gamma=" << gamma;
            const int midIdx = numStages / 2;
            if (midIdx < (int) stageCoeffA.size())
            {
                const float midA = stageCoeffA[(size_t) midIdx];
                dbg << " midIdx=" << midIdx << " midCoeff=" << midA;
                const double t_recon = (1.0 - (double) midA) / (1.0 + (double) midA);
                const double w_recon = std::atan (t_recon);
                const double freq_recon = w_recon * sampleRate / juce::MathConstants<double>::pi;
                dbg << " midFreqRecon=" << freq_recon;
            }
            const int lastIdx = numStages - 1;
            if (lastIdx >= 0 && lastIdx < (int) stageCoeffA.size())
            {
                const float lastA = stageCoeffA[(size_t) lastIdx];
                dbg << " lastIdx=" << lastIdx << " lastCoeff=" << lastA;
                const double t_recon2 = (1.0 - (double) lastA) / (1.0 + (double) lastA);
                const double w_recon2 = std::atan (t_recon2);
                const double freq_recon2 = w_recon2 * sampleRate / juce::MathConstants<double>::pi;
                dbg << " lastFreqRecon=" << freq_recon2;
            }
            dbg << std::endl;
            dbg.close();
        }
    }
    catch (...) {}
#endif

    // Update lightweight resonator (disabled)
    resoMixTarget = 0.0f;
    const float aaReso = allpassCoeffFromFreq (freqHz, sampleRate);
    resoLeft.a = aaReso;
    resoRight.a = aaReso;

    // Disable cascade - using optimized first-order allpass
    cascadeLeft.set (freqHz, 0.5f, 0);
    cascadeRight.set (freqHz, 0.5f, 0);

#if DISPTR_ENABLE_DEBUG
    try
    {
        std::ofstream dbg ("e:/Workspace/Production/JUCE_projects/DISP-TR/freq_debug.txt", std::ios::app);
        if (dbg)
        {
            dbg << "  FIRST-ORDER PINCH: shapeNorm=" << shapeNorm 
                << " spreadOct=" << spreadOct << " gamma=" << gamma 
                << " pinch=" << pinchIntensityGlobal << std::endl;
            dbg.close();
        }
    }
    catch (...) {}
#endif

    applyCoefficientsToNetworks();
}

void DisperserAudioProcessor::Engine::swap (Engine& other) noexcept
{
    using std::swap;

    swap (sampleRate, other.sampleRate);
    swap (maxWindowSamples, other.maxWindowSamples);

    swap (amount, other.amount);
    swap (activeStages, other.activeStages);
    swap (series, other.series);
    swap (reverse, other.reverse);

    swap (stageCoeffA, other.stageCoeffA);
    swap (nets, other.nets);

    swap (winN, other.winN);
    swap (hopH, other.hopH);
    swap (inWritePos, other.inWritePos);
    swap (hopCounter, other.hopCounter);

    swap (inRingL, other.inRingL);
    swap (inRingR, other.inRingR);
    swap (olaRingL, other.olaRingL);
    swap (olaRingR, other.olaRingR);

    swap (olaReadPos, other.olaReadPos);
    swap (olaWritePos, other.olaWritePos);
    swap (framesReady, other.framesReady);

    swap (frameL, other.frameL);
    swap (frameR, other.frameR);
    swap (winSqrt, other.winSqrt);

    // Allpass state (POD) can be swapped element-wise
    swap (resoLeft.a, other.resoLeft.a);
    swap (resoLeft.x1, other.resoLeft.x1);
    swap (resoLeft.y1, other.resoLeft.y1);
    swap (resoRight.a, other.resoRight.a);
    swap (resoRight.x1, other.resoRight.x1);
    swap (resoRight.y1, other.resoRight.y1);

    swap (resoMix, other.resoMix);
    swap (resoMixTarget, other.resoMixTarget);

    // Cascade state
    swap (cascadeLeft.states, other.cascadeLeft.states);
    swap (cascadeLeft.a0, other.cascadeLeft.a0);
    swap (cascadeLeft.a1, other.cascadeLeft.a1);
    swap (cascadeLeft.count, other.cascadeLeft.count);
    swap (cascadeRight.states, other.cascadeRight.states);
    swap (cascadeRight.a0, other.cascadeRight.a0);
    swap (cascadeRight.a1, other.cascadeRight.a1);
    swap (cascadeRight.count, other.cascadeRight.count);

    swap (cascadeMix, other.cascadeMix);
    swap (cascadeMixTarget, other.cascadeMixTarget);

    swap (cachedFreq, other.cachedFreq);
    swap (cachedShape, other.cachedShape);
    swap (cachedFreqBin, other.cachedFreqBin);
    swap (cachedShapeBin, other.cachedShapeBin);

    // Atomics cannot be std::swap'd; swap their integer contents via load/store
    {
        const uint64_t a = profileReverseUs.load (std::memory_order_relaxed);
        const uint64_t b = other.profileReverseUs.load (std::memory_order_relaxed);
        profileReverseUs.store (b, std::memory_order_relaxed);
        other.profileReverseUs.store (a, std::memory_order_relaxed);
    }
    {
        const uint64_t a = profileOtherUs.load (std::memory_order_relaxed);
        const uint64_t b = other.profileOtherUs.load (std::memory_order_relaxed);
        profileOtherUs.store (b, std::memory_order_relaxed);
        other.profileOtherUs.store (a, std::memory_order_relaxed);
    }
    {
        const uint64_t a = profileBlocks.load (std::memory_order_relaxed);
        const uint64_t b = other.profileBlocks.load (std::memory_order_relaxed);
        profileBlocks.store (b, std::memory_order_relaxed);
        other.profileBlocks.store (a, std::memory_order_relaxed);
    }
    {
        const uint64_t a = profileGrabUs.load (std::memory_order_relaxed);
        const uint64_t b = other.profileGrabUs.load (std::memory_order_relaxed);
        profileGrabUs.store (b, std::memory_order_relaxed);
        other.profileGrabUs.store (a, std::memory_order_relaxed);
    }
    {
        const uint64_t a = profileFrameUs.load (std::memory_order_relaxed);
        const uint64_t b = other.profileFrameUs.load (std::memory_order_relaxed);
        profileFrameUs.store (b, std::memory_order_relaxed);
        other.profileFrameUs.store (a, std::memory_order_relaxed);
    }
    {
        const uint64_t a = profileOlaUs.load (std::memory_order_relaxed);
        const uint64_t b = other.profileOlaUs.load (std::memory_order_relaxed);
        profileOlaUs.store (b, std::memory_order_relaxed);
        other.profileOlaUs.store (a, std::memory_order_relaxed);
    }
}

void DisperserAudioProcessor::Engine::makeSqrtHann (int N)
{
    winSqrt.assign ((size_t) N, 0.0f);
    if (N <= 1) { winSqrt[0] = 1.0f; return; }

    for (int n = 0; n < N; ++n)
    {
        const float w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) n / (float) (N - 1));
        winSqrt[(size_t) n] = std::sqrt (juce::jmax (0.0f, w));
    }
}

void DisperserAudioProcessor::Engine::resetReverseOLA (int newN)
{
    winN = juce::jlimit (1, maxWindowSamples, newN);
    hopH = juce::jmax (1, winN / 2);

    inRingL.assign ((size_t) winN, 0.0f);
    inRingR.assign ((size_t) winN, 0.0f);
    inWritePos = 0;
    hopCounter = 0;

    const int olaSize = juce::jmax (2, 2 * winN);
    olaRingL.assign ((size_t) olaSize, 0.0f);
    olaRingR.assign ((size_t) olaSize, 0.0f);

    olaReadPos = 0;
    olaWritePos = olaReadPos + winN;
    if (olaWritePos >= olaSize)
        olaWritePos -= olaSize;
    framesReady = 0;

    frameL.assign ((size_t) winN, 0.0f);
    frameR.assign ((size_t) winN, 0.0f);

    makeSqrtHann (winN);
}

void DisperserAudioProcessor::Engine::grabLastNToFrame()
{
#if DISPTR_PROFILE_RVS
    using namespace std::chrono;
    const auto t0 = high_resolution_clock::now();
#endif
    const int firstLen = winN - inWritePos;

    for (int i = 0; i < firstLen; ++i)
    {
        const float w = winSqrt[(size_t) i];
        frameL[(size_t) i] = inRingL[(size_t) (inWritePos + i)] * w;
        frameR[(size_t) i] = inRingR[(size_t) (inWritePos + i)] * w;
    }

    for (int i = firstLen; i < winN; ++i)
    {
        const float w = winSqrt[(size_t) i];
        frameL[(size_t) i] = inRingL[(size_t) (i - firstLen)] * w;
        frameR[(size_t) i] = inRingR[(size_t) (i - firstLen)] * w;
    }

#if DISPTR_PROFILE_RVS
    const auto t1 = high_resolution_clock::now();
    const uint64_t us = (uint64_t) duration_cast<microseconds> (t1 - t0).count();
    profileGrabUs.fetch_add (us, std::memory_order_relaxed);
#endif
}

void DisperserAudioProcessor::Engine::processFrameReverseIR (bool processStereo)
{
#if DISPTR_PROFILE_RVS
    using namespace std::chrono;
    const auto t0 = high_resolution_clock::now();
#endif
    const int numStages = activeStages;
    if (numStages <= 0) return;

    const int sCount = juce::jlimit (1, kMaxSeries, series);
    std::array<StageState*, kMaxSeries> stagePtrs {};
    for (int inst = 0; inst < sCount; ++inst)
        stagePtrs[(size_t) inst] = nets[(size_t) inst].stages.data();

        // Using macro-based chain processing to keep inner loops tight;
        // macros are defined at file scope near the top of this file.

    if (processStereo)
    {
        switch (sCount)
        {
            case 4:
                for (int n = 0; n < winN; ++n)
                {
                    const int ri = winN - 1 - n;
                    float xL = frameL[(size_t) ri];
                    float xR = frameR[(size_t) ri];

                    PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[2], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[3], xL, xR);

                    frameL[(size_t) ri] = xL;
                    frameR[(size_t) ri] = xR;
                }
                break;

            case 3:
                for (int n = 0; n < winN; ++n)
                {
                    const int ri = winN - 1 - n;
                    float xL = frameL[(size_t) ri];
                    float xR = frameR[(size_t) ri];

                    PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[2], xL, xR);

                    frameL[(size_t) ri] = xL;
                    frameR[(size_t) ri] = xR;
                }
                break;

            case 2:
                for (int n = 0; n < winN; ++n)
                {
                    const int ri = winN - 1 - n;
                    float xL = frameL[(size_t) ri];
                    float xR = frameR[(size_t) ri];

                    PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);

                    frameL[(size_t) ri] = xL;
                    frameR[(size_t) ri] = xR;
                }
                break;

            default:
                for (int n = 0; n < winN; ++n)
                {
                    const int ri = winN - 1 - n;
                    float xL = frameL[(size_t) ri];
                    float xR = frameR[(size_t) ri];

                    PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);

                    frameL[(size_t) ri] = xL;
                    frameR[(size_t) ri] = xR;
                }
                break;
        }

        return;
    }

    switch (sCount)
    {
        case 4:
            for (int n = 0; n < winN; ++n)
            {
                const int ri = winN - 1 - n;
                float xL = frameL[(size_t) ri];

                PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                PROCESS_CHAIN_MONO (stagePtrs[2], xL);
                PROCESS_CHAIN_MONO (stagePtrs[3], xL);

                frameL[(size_t) ri] = xL;
                frameR[(size_t) ri] = xL;
            }
            break;

        case 3:
            for (int n = 0; n < winN; ++n)
            {
                const int ri = winN - 1 - n;
                float xL = frameL[(size_t) ri];

                PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                PROCESS_CHAIN_MONO (stagePtrs[2], xL);

                frameL[(size_t) ri] = xL;
                frameR[(size_t) ri] = xL;
            }
            break;

        case 2:
            for (int n = 0; n < winN; ++n)
            {
                const int ri = winN - 1 - n;
                float xL = frameL[(size_t) ri];

                PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                PROCESS_CHAIN_MONO (stagePtrs[1], xL);

                frameL[(size_t) ri] = xL;
                frameR[(size_t) ri] = xL;
            }
            break;

        default:
            for (int n = 0; n < winN; ++n)
            {
                const int ri = winN - 1 - n;
                float xL = frameL[(size_t) ri];

                PROCESS_CHAIN_MONO (stagePtrs[0], xL);

                frameL[(size_t) ri] = xL;
                frameR[(size_t) ri] = xL;
            }
            break;
    }

#if DISPTR_PROFILE_RVS
    const auto t1 = high_resolution_clock::now();
    const uint64_t us = (uint64_t) duration_cast<microseconds> (t1 - t0).count();
    profileFrameUs.fetch_add (us, std::memory_order_relaxed);
#endif
}

void DisperserAudioProcessor::Engine::olaAddFrame()
{
#if DISPTR_PROFILE_RVS
    using namespace std::chrono;
    const auto t0 = high_resolution_clock::now();
#endif
    const int olaSize = (int) olaRingL.size();
    const float* winPtr = winSqrt.data();
    const float* frameLPtr = frameL.data();
    const float* frameRPtr = frameR.data();
    float* olaLPtr = olaRingL.data();
    float* olaRPtr = olaRingR.data();

    const int firstLen = juce::jmin (winN, olaSize - olaWritePos);
    // copy first contiguous region
    for (int i = 0; i < firstLen; ++i)
    {
        const float w = winPtr[i];
        olaLPtr[olaWritePos + i] += frameLPtr[i] * w;
        olaRPtr[olaWritePos + i] += frameRPtr[i] * w;
    }

    // wrap-around tail
    for (int i = firstLen; i < winN; ++i)
    {
        const float w = winPtr[i];
        const int p = i - firstLen;
        olaLPtr[p] += frameLPtr[i] * w;
        olaRPtr[p] += frameRPtr[i] * w;
    }

    olaWritePos += hopH;
    if (olaWritePos >= olaSize)
        olaWritePos -= olaSize;
    framesReady = 1;

#if DISPTR_PROFILE_RVS
    const auto t1 = high_resolution_clock::now();
    const uint64_t us = (uint64_t) duration_cast<microseconds> (t1 - t0).count();
    profileOlaUs.fetch_add (us, std::memory_order_relaxed);
#endif
}

void DisperserAudioProcessor::Engine::olaPopStereo (float& outL, float& outR) noexcept
{
    if (framesReady == 0)
    {
        outL = 0.0f;
        outR = 0.0f;
        return;
    }

    const int readPos = olaReadPos;
    const float* olaLPtr = olaRingL.data();
    const float* olaRPtr = olaRingR.data();
    outL = olaLPtr[readPos];
    outR = olaRPtr[readPos];

    olaRingL[(size_t) readPos] = 0.0f;
    olaRingR[(size_t) readPos] = 0.0f;

    ++olaReadPos;
    if (olaReadPos >= (int) olaRingL.size())
        olaReadPos = 0;
}

void DisperserAudioProcessor::Engine::processBlock (juce::AudioBuffer<float>& buffer,
                                                   float freqNow, float shapeNow,
                                                   float outputGain)
{
    const int numSamples = buffer.getNumSamples();
    const int channels = buffer.getNumChannels();
    const bool isMono = (channels == 1);

    auto* ch0 = buffer.getWritePointer (0);
    float* ch1 = isMono ? nullptr : buffer.getWritePointer (1);

#if DISPTR_PROFILE_RVS
    using namespace std::chrono;
    const auto blockStart = high_resolution_clock::now();
#endif

    // Interpolate cascade mix smoothly across this block
    float cascadeMixCur = cascadeMix;
    const float cascadeMixTargetLocal = cascadeMixTarget;
    const float cascadeMixStep = (cascadeMixTargetLocal - cascadeMixCur) / (float) juce::jmax (1, numSamples);

    // Interpolate reso mix smoothly across this block to avoid steps when
    // the GUI changes `shape`/`resonance`. We compute a per-sample step
    // and use a local running value (avoids lambda overhead).
    float resoMixCur = resoMix;
    const float resoMixTargetLocal = resoMixTarget;
    const float resoMixStep = (resoMixTargetLocal - resoMixCur) / (float) juce::jmax (1, numSamples);

    // Determine whether resonator mixing will be active this block. If not,
    // we can skip per-sample mixing entirely. If it becomes active partway
    // through the block, compute warmSamples to avoid doing a branch per
    // sample: handle initial samples without mixing, then the remaining
    // samples with a fixed mixing path (no branches inside loop).
    const float mixThreshold = 1e-6f;
    const bool resoActive = (std::fabs (resoMixCur) > mixThreshold) || (std::fabs (resoMixTargetLocal) > mixThreshold);
    int warmSamples = 0;
    if (resoActive && resoMixCur <= mixThreshold && resoMixStep > 0.0f)
    {
        warmSamples = (int) std::ceil ((mixThreshold - resoMixCur) / resoMixStep);
        if (warmSamples < 0) warmSamples = 0;
        if (warmSamples > numSamples) warmSamples = numSamples;
    }

    if (amount <= 0 || activeStages <= 0)
        return;

    const float safeFreq = juce::jlimit (20.0f, 20000.0f, freqNow);
    const float safeShape = juce::jlimit (0.0f, 1.0f, shapeNow);

    const int freqBin = (int) std::lround (1200.0 * std::log2 ((double) safeFreq / 20.0));
    const int shapeBin = (int) std::lround ((double) safeShape * 1000.0);

    if (freqBin != cachedFreqBin || shapeBin != cachedShapeBin)
    {
        cachedFreq = safeFreq;
        cachedShape = safeShape;
        cachedFreqBin = freqBin;
        cachedShapeBin = shapeBin;
        updateCoefficientsNow (amount, safeFreq, safeShape);
    }

    // Optional compile-time override to disable reverse path for perf tests
    // (forces engine to behave as if reverse==false).
#if DISPTR_DISABLE_RVS
    const bool reverseLocal = false;
#else
    const bool reverseLocal = reverse;
#endif

    if (! reverseLocal)
    {
        const int numStages = activeStages;
        const int sCount = juce::jlimit (1, kMaxSeries, series);
        std::array<StageState*, kMaxSeries> stagePtrs {};
        for (int inst = 0; inst < sCount; ++inst)
            stagePtrs[(size_t) inst] = nets[(size_t) inst].stages.data();

        // Use optimized first-order allpass with pinch
        const bool usePureCascade = false;
        const bool usePureFirstOrder = true;
        const bool useMixed = false;

        if (usePureCascade)
        {
            // Button2 mode: pure second-order cascade
            if (isMono)
            {
                for (int n = 0; n < numSamples; ++n)
                {
                    const float in = ch0[n];
                    const float proc = cascadeLeft.process (in);
                    ch0[n] = proc * outputGain;
                    cascadeMixCur += cascadeMixStep;
                }
            }
            else
            {
                for (int n = 0; n < numSamples; ++n)
                {
                    const float inL = ch0[n];
                    const float inR = ch1[n];
                    const float outL = cascadeLeft.process (inL);
                    const float outR = cascadeRight.process (inR);
                    ch0[n] = outL * outputGain;
                    ch1[n] = outR * outputGain;
                    cascadeMixCur += cascadeMixStep;
                }
            }
        }
        else if (usePureFirstOrder)
        {
            // Button1 mode: pure first-order allpass (existing code)
            // Using macro-based chain processing to keep inner loops tight;
            // macros are defined earlier in the file.

            if (isMono)
            {
            switch (sCount)
            {
                case 4:
                    // initial warm samples without mixing
                    for (int n = 0; n < warmSamples; ++n)
                    {
                        float xL = ch0[n];
                        PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                        PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                        PROCESS_CHAIN_MONO (stagePtrs[2], xL);
                        PROCESS_CHAIN_MONO (stagePtrs[3], xL);
                        ch0[n] = xL * outputGain;
                        resoMixCur += resoMixStep;
                    }

                    // remaining samples: if resonator inactive, this loop is all we need
                    if (! resoActive)
                    {
                        for (int n = warmSamples; n < numSamples; ++n)
                        {
                            float xL = ch0[n];
                            PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[2], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[3], xL);
                            ch0[n] = xL * outputGain;
                        }
                    }
                    else
                    {
                        for (int n = warmSamples; n < numSamples; ++n)
                        {
                            float xL = ch0[n];
                            PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[2], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[3], xL);
                            ch0[n] = xL * outputGain;
                            const float mix = resoMixCur;
                            if (mix > mixThreshold)
                            {
                                const float origL = ch0[n];
                                const float procL = resoLeft.process (origL);
                                ch0[n] = (1.0f - mix) * origL + mix * procL;
                            }
                            resoMixCur += resoMixStep;
                        }
                    }
                    break;

                case 3:
                    for (int n = 0; n < warmSamples; ++n)
                    {
                        float xL = ch0[n];
                        PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                        PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                        PROCESS_CHAIN_MONO (stagePtrs[2], xL);
                        ch0[n] = xL * outputGain;
                        resoMixCur += resoMixStep;
                    }
                    if (! resoActive)
                    {
                        for (int n = warmSamples; n < numSamples; ++n)
                        {
                            float xL = ch0[n];
                            PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[2], xL);
                            ch0[n] = xL * outputGain;
                        }
                    }
                    else
                    {
                        for (int n = warmSamples; n < numSamples; ++n)
                        {
                            float xL = ch0[n];
                            PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[2], xL);
                            ch0[n] = xL * outputGain;
                            const float mix = resoMixCur;
                            if (mix > mixThreshold)
                            {
                                const float origL = ch0[n];
                                const float procL = resoLeft.process (origL);
                                ch0[n] = (1.0f - mix) * origL + mix * procL;
                            }
                            resoMixCur += resoMixStep;
                        }
                    }
                    break;

                case 2:
                    for (int n = 0; n < warmSamples; ++n)
                    {
                        float xL = ch0[n];
                        PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                        PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                        ch0[n] = xL * outputGain;
                        resoMixCur += resoMixStep;
                    }
                    if (! resoActive)
                    {
                        for (int n = warmSamples; n < numSamples; ++n)
                        {
                            float xL = ch0[n];
                            PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                            ch0[n] = xL * outputGain;
                        }
                    }
                    else
                    {
                        for (int n = warmSamples; n < numSamples; ++n)
                        {
                            float xL = ch0[n];
                            PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                            PROCESS_CHAIN_MONO (stagePtrs[1], xL);
                            ch0[n] = xL * outputGain;
                            const float mix = resoMixCur;
                            if (mix > mixThreshold)
                            {
                                const float origL = ch0[n];
                                const float procL = resoLeft.process (origL);
                                ch0[n] = (1.0f - mix) * origL + mix * procL;
                            }
                            resoMixCur += resoMixStep;
                        }
                    }
                    break;

                default:
                    for (int n = 0; n < warmSamples; ++n)
                    {
                        float xL = ch0[n];
                        PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                        ch0[n] = xL * outputGain;
                        resoMixCur += resoMixStep;
                    }
                    if (! resoActive)
                    {
                        for (int n = warmSamples; n < numSamples; ++n)
                        {
                            float xL = ch0[n];
                            PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                            ch0[n] = xL * outputGain;
                        }
                    }
                    else
                    {
                        for (int n = warmSamples; n < numSamples; ++n)
                        {
                            float xL = ch0[n];
                            PROCESS_CHAIN_MONO (stagePtrs[0], xL);
                            ch0[n] = xL * outputGain;
                            const float mix = resoMixCur;
                            if (mix > mixThreshold)
                            {
                                const float origL = ch0[n];
                                const float procL = resoLeft.process (origL);
                                ch0[n] = (1.0f - mix) * origL + mix * procL;
                            }
                            resoMixCur += resoMixStep;
                        }
                    }
                    break;
            }

            return;
        }
        else // stereo
        {
        switch (sCount)
        {
            case 4:
                for (int n = 0; n < warmSamples; ++n)
                {
                    float xL = ch0[n];
                    float xR = ch1[n];
                    PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[2], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[3], xL, xR);
                    ch0[n] = xL * outputGain;
                    ch1[n] = xR * outputGain;
                    resoMixCur += resoMixStep;
                }
                if (! resoActive)
                {
                    for (int n = warmSamples; n < numSamples; ++n)
                    {
                        float xL = ch0[n];
                        float xR = ch1[n];
                        PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[2], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[3], xL, xR);
                        ch0[n] = xL * outputGain;
                        ch1[n] = xR * outputGain;
                    }
                }
                else
                {
                    for (int n = warmSamples; n < numSamples; ++n)
                    {
                        float xL = ch0[n];
                        float xR = ch1[n];
                        PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[2], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[3], xL, xR);
                        ch0[n] = xL * outputGain;
                        ch1[n] = xR * outputGain;
                        const float mix = resoMixCur;
                        if (mix > mixThreshold)
                        {
                            const float origL = ch0[n];
                            const float procL = resoLeft.process (origL);
                            ch0[n] = (1.0f - mix) * origL + mix * procL;
                            const float origR = ch1[n];
                            const float procR = resoRight.process (origR);
                            ch1[n] = (1.0f - mix) * origR + mix * procR;
                        }
                        resoMixCur += resoMixStep;
                    }
                }
                break;

            case 3:
                for (int n = 0; n < warmSamples; ++n)
                {
                    float xL = ch0[n];
                    float xR = ch1[n];
                    PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[2], xL, xR);
                    ch0[n] = xL * outputGain;
                    ch1[n] = xR * outputGain;
                    resoMixCur += resoMixStep;
                }
                if (! resoActive)
                {
                    for (int n = warmSamples; n < numSamples; ++n)
                    {
                        float xL = ch0[n];
                        float xR = ch1[n];
                        PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[2], xL, xR);
                        ch0[n] = xL * outputGain;
                        ch1[n] = xR * outputGain;
                    }
                }
                else
                {
                    for (int n = warmSamples; n < numSamples; ++n)
                    {
                        float xL = ch0[n];
                        float xR = ch1[n];
                        PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[2], xL, xR);
                        ch0[n] = xL * outputGain;
                        ch1[n] = xR * outputGain;
                        const float mix = resoMixCur;
                        if (mix > mixThreshold)
                        {
                            const float origL = ch0[n];
                            const float procL = resoLeft.process (origL);
                            ch0[n] = (1.0f - mix) * origL + mix * procL;
                            const float origR = ch1[n];
                            const float procR = resoRight.process (origR);
                            ch1[n] = (1.0f - mix) * origR + mix * procR;
                        }
                        resoMixCur += resoMixStep;
                    }
                }
                break;

            case 2:
                for (int n = 0; n < warmSamples; ++n)
                {
                    float xL = ch0[n];
                    float xR = ch1[n];
                    PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                    PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                    ch0[n] = xL * outputGain;
                    ch1[n] = xR * outputGain;
                    resoMixCur += resoMixStep;
                }
                if (! resoActive)
                {
                    for (int n = warmSamples; n < numSamples; ++n)
                    {
                        float xL = ch0[n];
                        float xR = ch1[n];
                        PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                        ch0[n] = xL * outputGain;
                        ch1[n] = xR * outputGain;
                    }
                }
                else
                {
                    for (int n = warmSamples; n < numSamples; ++n)
                    {
                        float xL = ch0[n];
                        float xR = ch1[n];
                        PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                        PROCESS_CHAIN_STEREO (stagePtrs[1], xL, xR);
                        ch0[n] = xL * outputGain;
                        ch1[n] = xR * outputGain;
                        const float mix = resoMixCur;
                        if (mix > mixThreshold)
                        {
                            const float origL = ch0[n];
                            const float procL = resoLeft.process (origL);
                            ch0[n] = (1.0f - mix) * origL + mix * procL;
                            const float origR = ch1[n];
                            const float procR = resoRight.process (origR);
                            ch1[n] = (1.0f - mix) * origR + mix * procR;
                        }
                        resoMixCur += resoMixStep;
                    }
                }
                break;

            default:
                for (int n = 0; n < warmSamples; ++n)
                {
                    float xL = ch0[n];
                    float xR = ch1[n];
                    PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                    ch0[n] = xL * outputGain;
                    ch1[n] = xR * outputGain;
                    resoMixCur += resoMixStep;
                }
                if (! resoActive)
                {
                    for (int n = warmSamples; n < numSamples; ++n)
                    {
                        float xL = ch0[n];
                        float xR = ch1[n];
                        PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                        ch0[n] = xL * outputGain;
                        ch1[n] = xR * outputGain;
                    }
                }
                else
                {
                    for (int n = warmSamples; n < numSamples; ++n)
                    {
                        float xL = ch0[n];
                        float xR = ch1[n];
                        PROCESS_CHAIN_STEREO (stagePtrs[0], xL, xR);
                        ch0[n] = xL * outputGain;
                        ch1[n] = xR * outputGain;
                        const float mix = resoMixCur;
                        if (mix > mixThreshold)
                        {
                            const float origL = ch0[n];
                            const float procL = resoLeft.process (origL);
                            ch0[n] = (1.0f - mix) * origL + mix * procL;
                            const float origR = ch1[n];
                            const float procR = resoRight.process (origR);
                            ch1[n] = (1.0f - mix) * origR + mix * procR;
                        }
                        resoMixCur += resoMixStep;
                    }
                }
                break;
        }

        // Profiling: record time spent in 'other' path when enabled.
    #if DISPTR_PROFILE_RVS
        using namespace std::chrono;
        const auto blockEnd = high_resolution_clock::now();
        const uint64_t us = (uint64_t) duration_cast<microseconds> (blockEnd - blockStart).count();
        profileOtherUs.fetch_add (us, std::memory_order_relaxed);
        profileBlocks.fetch_add (1, std::memory_order_relaxed);
    #endif
        } // end stereo else
        } // end usePureFirstOrder
        else if (useMixed)
        {
            // Mixed mode: crossfade between first-order and cascade
            // Process both algorithms and blend per-sample based on cascadeMixCur
            if (isMono)
            {
                for (int n = 0; n < numSamples; ++n)
                {
                    // Process with first-order allpass chain
                    float xFirst = ch0[n];
                    for (int st = 0; st < activeStages; ++st)
                    {
                        xFirst = stagePtrs[0][st].left.process (xFirst);
                    }

                    // Process with cascade
                    const float xCascade = cascadeLeft.process (ch0[n]);

                    // Crossfade
                    const float mix = cascadeMixCur;
                    ch0[n] = ((1.0f - mix) * xFirst + mix * xCascade) * outputGain;
                    cascadeMixCur += cascadeMixStep;
                }
            }
            else
            {
                for (int n = 0; n < numSamples; ++n)
                {
                    // Process left with first-order
                    float xFirstL = ch0[n];
                    for (int st = 0; st < activeStages; ++st)
                    {
                        xFirstL = stagePtrs[0][st].left.process (xFirstL);
                    }

                    // Process right with first-order
                    float xFirstR = ch1[n];
                    for (int st = 0; st < activeStages; ++st)
                    {
                        xFirstR = stagePtrs[0][st].right.process (xFirstR);
                    }

                    // Process with cascade
                    const float xCascadeL = cascadeLeft.process (ch0[n]);
                    const float xCascadeR = cascadeRight.process (ch1[n]);

                    // Crossfade
                    const float mix = cascadeMixCur;
                    ch0[n] = ((1.0f - mix) * xFirstL + mix * xCascadeL) * outputGain;
                    ch1[n] = ((1.0f - mix) * xFirstR + mix * xCascadeR) * outputGain;
                    cascadeMixCur += cascadeMixStep;
                }
            }
        } // end useMixed
        
        return;
    }

    // Reverse path: split into warmSamples (no resonator processing) and
    // the rest (with resonator mixing) to avoid per-sample branch.
    for (int n = 0; n < warmSamples; ++n)
    {
        inRingL[(size_t) inWritePos] = ch0[n];
        inRingR[(size_t) inWritePos] = isMono ? ch0[n] : ch1[n];
        ++inWritePos;
        if (inWritePos >= winN)
            inWritePos = 0;

        float outL = 0.0f, outR = 0.0f;
        olaPopStereo (outL, outR);

        ch0[n] = outL * outputGain;
        if (! isMono) ch1[n] = outR * outputGain;

        resoMixCur += resoMixStep;

        if (++hopCounter >= hopH)
        {
            hopCounter = 0;

            grabLastNToFrame();
            processFrameReverseIR (! isMono);
            olaAddFrame();
        }
    }

    for (int n = warmSamples; n < numSamples; ++n)
    {
        inRingL[(size_t) inWritePos] = ch0[n];
        inRingR[(size_t) inWritePos] = isMono ? ch0[n] : ch1[n];
        ++inWritePos;
        if (inWritePos >= winN)
            inWritePos = 0;

        float outL = 0.0f, outR = 0.0f;
        olaPopStereo (outL, outR);

        ch0[n] = outL * outputGain;
        if (! isMono) ch1[n] = outR * outputGain;

        const float mix = resoMixCur;
        if (mix > mixThreshold)
        {
            const float origL = ch0[n];
            const float procL = resoLeft.process (origL);
            ch0[n] = (1.0f - mix) * origL + mix * procL;
            if (! isMono)
            {
                const float origR = ch1[n];
                const float procR = resoRight.process (origR);
                ch1[n] = (1.0f - mix) * origR + mix * procR;
            }
        }
        resoMixCur += resoMixStep;

        if (++hopCounter >= hopH)
        {
            hopCounter = 0;

            grabLastNToFrame();
            processFrameReverseIR (! isMono);
            olaAddFrame();
        }
    }

    #if DISPTR_PROFILE_RVS
        // Profiling: record time spent in reverse path
        using namespace std::chrono;
        const auto blockEnd = high_resolution_clock::now();
        const uint64_t us = (uint64_t) duration_cast<microseconds> (blockEnd - blockStart).count();
        profileReverseUs.fetch_add (us, std::memory_order_relaxed);
        profileBlocks.fetch_add (1, std::memory_order_relaxed);
    #endif
}

// Undefine the processing macros to avoid leaking into other translation units
#undef PROCESS_CHAIN_MONO
#undef PROCESS_CHAIN_STEREO