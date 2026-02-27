#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <limits>

class DisperserAudioProcessor  : public juce::AudioProcessor
{
public:
    DisperserAudioProcessor();
    ~DisperserAudioProcessor() override;

    static constexpr const char* kParamAmount    = "amount";
    static constexpr const char* kParamSeries    = "series";
    static constexpr const char* kParamFreq      = "freq";
    static constexpr const char* kParamResonance = "resonance";
    static constexpr const char* kParamReverse   = "reverse";
    static constexpr const char* kParamInv       = "inv";
    static constexpr const char* kParamS0        = "s0";
    static constexpr const char* kParamS100      = "s100";
    static constexpr const char* kParamUiWidth   = "ui_width";
    static constexpr const char* kParamUiHeight  = "ui_height";
    static constexpr const char* kParamUiPalette = "ui_palette";
    static constexpr const char* kParamUiFxTail  = "ui_fx_tail";
    static constexpr const char* kParamUiColor0  = "ui_color0";
    static constexpr const char* kParamUiColor1  = "ui_color1";
    static constexpr const char* kParamUiColor2  = "ui_color2";
    static constexpr const char* kParamUiColor3  = "ui_color3";

    static constexpr int kAmountMin = 0;
    static constexpr int kAmountMax = 256;
    static constexpr int kAmountDefault = 32;

    static constexpr int kSeriesMin = 1;
    static constexpr int kSeriesMax = 4;
    static constexpr int kSeriesDefault = 1;

    static constexpr float kFreqDefault = 1000.000f;
    static constexpr float kResonanceDefault = 0.0f;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #if ! JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    void getCurrentProgramStateInformation (juce::MemoryBlock& destData) override;
    void setCurrentProgramStateInformation (const void* data, int sizeInBytes) override;

    void setUiEditorSize (int width, int height);
    int getUiEditorWidth() const noexcept;
    int getUiEditorHeight() const noexcept;

    void setUiUseCustomPalette (bool shouldUseCustomPalette);
    bool getUiUseCustomPalette() const noexcept;

    void setUiFxTailEnabled (bool shouldEnableFxTail);
    bool getUiFxTailEnabled() const noexcept;

    void setUiCustomPaletteColour (int index, juce::Colour colour);
    juce::Colour getUiCustomPaletteColour (int index) const noexcept;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    struct Allpass1
    {
        float a  = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;

        inline float process (float x) noexcept
        {
            const float y = (-a * x) + x1 + (a * y1);
            x1 = x;
            y1 = y;
            return y;
        }

        void reset() noexcept { x1 = 0.0f; y1 = 0.0f; }
    };

    // Second-order allpass for Button2/cascade mode (shape at 100%)
    struct SecondOrderAllPassCascade
    {
        struct State { float xnz2 = 0.0f, xnz1 = 0.0f, ynz2 = 0.0f, ynz1 = 0.0f; };
        
        std::vector<State> states;
        float a0 = 0.0f;
        float a1 = 0.0f;
        int sampleRate = 48000;
        int count = 0;
        int maxCount = 64;

        void init (int sr, int maxStages = 64)
        {
            sampleRate = sr;
            maxCount = maxStages;
            states.assign ((size_t) maxStages, {});
            count = 0;
        }

        void set (float frequency, float Q, int numStages) noexcept
        {
            const float w = 2.0f * juce::MathConstants<float>::pi * frequency / (float) sampleRate;
            const float cosw = std::cos (w);
            const float alpha = std::sin (w) / (2.0f * Q);
            const float a2 = 1.0f / (1.0f + alpha);
            a0 = (1.0f - alpha) * a2;
            a1 = -2.0f * cosw * a2;

            count = juce::jlimit (0, (int) states.size(), numStages);
        }

        inline float process (float in) noexcept
        {
            float yn = in;
            int i = 0;
            
            // Manual loop unrolling by 2 for better performance
            for (; i + 1 < count; i += 2)
            {
                // Process stage i
                State& s0 = states[(size_t) i];
                const float temp0 = yn;
                yn = a0 * (yn - s0.ynz2) + a1 * (s0.xnz1 - s0.ynz1) + s0.xnz2;
                s0.xnz2 = s0.xnz1;
                s0.xnz1 = temp0;
                s0.ynz2 = s0.ynz1;
                s0.ynz1 = yn;
                
                // Process stage i+1
                State& s1 = states[(size_t) (i + 1)];
                const float temp1 = yn;
                yn = a0 * (yn - s1.ynz2) + a1 * (s1.xnz1 - s1.ynz1) + s1.xnz2;
                s1.xnz2 = s1.xnz1;
                s1.xnz1 = temp1;
                s1.ynz2 = s1.ynz1;
                s1.ynz1 = yn;
            }
            
            // Handle remaining stage if count is odd
            if (i < count)
            {
                State& s = states[(size_t) i];
                const float temp = yn;
                yn = a0 * (yn - s.ynz2) + a1 * (s.xnz1 - s.ynz1) + s.xnz2;
                s.xnz2 = s.xnz1;
                s.xnz1 = temp;
                s.ynz2 = s.ynz1;
                s.ynz1 = yn;
            }
            
            return yn;
        }

        void reset() noexcept
        {
            for (auto& s : states)
            {
                s.xnz2 = 0.0f;
                s.xnz1 = 0.0f;
                s.ynz2 = 0.0f;
                s.ynz1 = 0.0f;
            }
        }
    };

    struct StageState { Allpass1 left, right; };

    struct NetworkInstance
    {
        std::vector<StageState> stages;

        void ensureStages (int n)
        {
            if ((int) stages.size() == n) return;
            stages.assign ((size_t) n, {});
            reset();
        }

        void reset()
        {
            for (auto& s : stages) { s.left.reset(); s.right.reset(); }
        }

        void resetActive (int activeStages)
        {
            const int n = juce::jlimit (0, (int) stages.size(), activeStages);
            for (int i = 0; i < n; ++i)
            {
                stages[(size_t) i].left.reset();
                stages[(size_t) i].right.reset();
            }
        }
    };

    struct Engine
    {
        double sampleRate = 0.0;
        int maxWindowSamples = 1;

        int amount = 0;
        int activeStages = 0;
        int series = 1;
        bool reverse = false;

        std::vector<float> stageCoeffA;

        static constexpr int kMaxSeries = DisperserAudioProcessor::kSeriesMax;
        std::array<NetworkInstance, kMaxSeries> nets;

        int winN = 1;
        int hopH = 1;
        int inWritePos = 0;
        int hopCounter = 0;

        std::vector<float> inRingL, inRingR;
        std::vector<float> olaRingL, olaRingR;
        int olaReadPos = 0;
        int olaWritePos = 0;
        int framesReady = 0;

        // Profiling counters (microseconds). Updated from audio thread
        // only when DISPTR_PROFILE_RVS is enabled in the .cpp file.
        std::atomic<uint64_t> profileReverseUs { 0 };
        std::atomic<uint64_t> profileOtherUs { 0 };
        std::atomic<uint64_t> profileBlocks { 0 };
        // More granular counters to isolate hotspots in reverse processing
        std::atomic<uint64_t> profileGrabUs { 0 };
        std::atomic<uint64_t> profileFrameUs { 0 };
        std::atomic<uint64_t> profileOlaUs { 0 };
        // When > 0, Engine will use a cheaper processing mode for this many blocks
        int lowCpuModeRemaining = 0;

        std::vector<float> frameL, frameR;
        std::vector<float> winSqrt;

        // Lightweight resonator used to make `resonance` audible even
        // when the number of stages is small. Implemented as a
        // first-order allpass and mixed into the output per-sample.
        Allpass1 resoLeft, resoRight;
        float resoMix = 0.0f;
        // Target for smooth interpolation of resoMix across the audio block.
        float resoMixTarget = 0.0f;

        // Second-order allpass cascade for Button2 mode (shape=100%)
        SecondOrderAllPassCascade cascadeLeft, cascadeRight;
        float cascadeMix = 0.0f;  // 0.0 = first-order only, 1.0 = second-order cascade
        float cascadeMixTarget = 0.0f;

        float cachedFreq = -1.0f;
        float cachedShape = -1.0f;  // Renamed from cachedReso to clarify it's shape
        int cachedFreqBin = std::numeric_limits<int>::min();
        int cachedShapeBin = std::numeric_limits<int>::min();  // Renamed from cachedResoBin

        void init (double sr);
        void setTopology (int newAmount, int newSeries, bool newReverse,
                          float initFreq, float initShape);

        void processBlock (juce::AudioBuffer<float>& buffer,
                           float freqNow, float shapeNow,
                           float outputGain = 1.0f);

        void ensureAllStages (int numStages);
        void resetAllNetworks();
        void applyCoefficientsToNetworks();

        static float allpassCoeffFromFreq (float freqHz, double sampleRate) noexcept;
        static float amountNorm (int amount) noexcept;
        int windowSamplesFromAmount (int amount) const noexcept;

        void updateCoefficientsNow (int amount, float freqHz, float shape);

        // Swap contents with another Engine instance. We provide a custom
        // swap because the implicitly-deleted move/copy operations (due to
        // atomic members) make std::swap<Engine> inapplicable.
        void swap (Engine& other) noexcept;

        void makeSqrtHann (int N);
        void resetReverseOLA (int newN);
        void grabLastNToFrame();
        void processFrameReverseIR (bool processStereo);
        void olaAddFrame();
        void olaPopStereo (float& outL, float& outR) noexcept;
    };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> freqSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> resonanceSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> amountSmoothed;

    std::atomic<float>* amountParam = nullptr;
    std::atomic<float>* seriesParam = nullptr;
    std::atomic<float>* freqParam = nullptr;
    std::atomic<float>* resonanceParam = nullptr;
    std::atomic<float>* reverseParam = nullptr;
    std::atomic<float>* invParam = nullptr;
    std::atomic<float>* s0Param = nullptr;
    std::atomic<float>* s100Param = nullptr;
    std::atomic<float>* uiWidthParam = nullptr;
    std::atomic<float>* uiHeightParam = nullptr;
    std::atomic<float>* uiPaletteParam = nullptr;
    std::atomic<float>* uiFxTailParam = nullptr;
    std::array<std::atomic<float>*, 4> uiColorParams { nullptr, nullptr, nullptr, nullptr };

    double currentSampleRate = 0.0;

    Engine engA;
    Engine engB;

    bool inTransition = false;
    int transitionSamples = 0;
    int transitionPos = 0;

    bool hasPendingTopology = false;
    int pendingAmount = 0;
    int pendingSeries = 1;
    bool pendingReverse = false;

    juce::AudioBuffer<float> transitionBufferB;

    int cachedAmountKey = -1;
    int cachedSeriesKey = -1;
    bool cachedReverseKey = false;

    // Default to minimum size to avoid hosts seeing a large default before
    // any persisted ValueTree state is applied.
    std::atomic<int> uiEditorWidth { 360 };
    std::atomic<int> uiEditorHeight { 360 };
    std::atomic<int> uiUseCustomPalette { 0 };
    std::atomic<int> uiFxTailEnabled { 1 };
    std::array<std::atomic<juce::uint32>, 4> uiCustomPalette {
        std::atomic<juce::uint32> { juce::Colours::white.getARGB() },
        std::atomic<juce::uint32> { juce::Colours::black.getARGB() },
        std::atomic<juce::uint32> { juce::Colours::white.getARGB() },
        std::atomic<juce::uint32> { juce::Colours::black.getARGB() }
    };

    void startTransitionIfNeeded (int newAmount, int newSeries, bool newReverse,
                                  float freqNow, float shapeNow);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisperserAudioProcessor)
};