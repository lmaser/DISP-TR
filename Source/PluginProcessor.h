#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>

class DisperserAudioProcessor : public juce::AudioProcessor
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
	static constexpr int kAmountMax = 128;
	static constexpr int kAmountDefault = 32;

	static constexpr int kSeriesMin = 1;
	static constexpr int kSeriesMax = 4;
	static constexpr int kSeriesDefault = 1;

	static constexpr float kFreqDefault = 1000.0f;
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
	struct AllPassState
	{
		float z1 = 0.0f;
	};

	struct UiStateKeys
	{
		static constexpr const char* editorWidth = "uiEditorWidth";
		static constexpr const char* editorHeight = "uiEditorHeight";
		static constexpr const char* useCustomPalette = "uiUseCustomPalette";
		static constexpr const char* fxTailEnabled = "uiFxTailEnabled";
		static constexpr std::array<const char*, 4> customPalette {
			"uiCustomPalette0", "uiCustomPalette1", "uiCustomPalette2", "uiCustomPalette3"
		};
	};

	static float calcAllPassCoeff (float frequency, float sampleRate) noexcept;
	void resizeDspState (int stages, int series);
	void updateCoefficients (float freqHz, float shapeNorm, int stages);
	void clearStageRange (int fromStageInclusive, int toStageExclusive, int seriesCount) noexcept;
	int computeRvsIrLengthSamples (int stages, int series) const noexcept;
	void buildForwardImpulseResponse (std::vector<float>& ir,
										 int irLength,
										 int stages,
										 int series,
										 float freqHz,
										 float shapeNorm) const;
	void rebuildRvsConvolutionIfNeeded (int stages, int series, float freqHz, float shapeNorm, bool forceRebuild);

	std::array<std::vector<AllPassState>, kSeriesMax> chainL;
	std::array<std::vector<AllPassState>, kSeriesMax> chainR;
	std::vector<float> stageCoeff;
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> stagesSmoothed;
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> freqSmoothed;
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> shapeSmoothed;
	static constexpr double kStageSmoothingSeconds = 2.0;
	static constexpr double kFreqSmoothingSeconds = 1.0;
	static constexpr double kShapeSmoothingSeconds = 0.1;
	int activeStages = 0;
	int activeSeries = kSeriesDefault;
	float lastCoeffFreq = -1.0f;
	float lastCoeffShape = -1.0f;
	int lastCoeffStages = -1;

	juce::dsp::Convolution rvsConvL { juce::dsp::Convolution::Latency { 1024 } };
	juce::dsp::Convolution rvsConvR { juce::dsp::Convolution::Latency { 1024 } };
	bool rvsConvPrepared = false;
	bool rvsRebuildPending = false;
	int rvsRebuildCooldownSamples = 0;
	int pendingRvsStages = -1;
	int pendingRvsSeries = -1;
	float pendingRvsFreq = -1.0f;
	float pendingRvsShape = -1.0f;
	int lastRvsStages = -1;
	int lastRvsSeries = -1;
	float lastRvsFreq = -1.0f;
	float lastRvsShape = -1.0f;
	int lastRvsIrLength = -1;
	static constexpr int kRvsRebuildMinIntervalMs = 200;

	double currentSampleRate = 44100.0;

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

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisperserAudioProcessor)
};

