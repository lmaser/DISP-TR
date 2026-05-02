#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "DspDebugLog.h"

class DisperserAudioProcessor : public juce::AudioProcessor
{
public:
	DisperserAudioProcessor();
	~DisperserAudioProcessor() override;

	static constexpr const char* kParamAmount    = "amount";
	static constexpr const char* kParamSeries    = "series";
	static constexpr const char* kParamFreq      = "freq";
	static constexpr const char* kParamShape     = "shape";
	static constexpr const char* kParamVariation = "variation";
	static constexpr const char* kParamAlt       = "alt";
	static constexpr const char* kParamFeedback  = "feedback";
	static constexpr const char* kParamMod       = "mod";
	static constexpr const char* kParamInput     = "input";
	static constexpr const char* kParamOutput    = "output";
	static constexpr const char* kParamMix       = "mix";
	static constexpr const char* kParamTilt      = "tilt";
	static constexpr const char* kParamPan       = "pan";
	static constexpr const char* kParamStyle     = "style";
	static constexpr const char* kParamMidi      = "midi";
	static constexpr const char* kParamS0        = "s0";
	static constexpr const char* kParamS100      = "s100";

	static constexpr const char* kParamFilterHpFreq  = "filter_hp_freq";
	static constexpr const char* kParamFilterLpFreq  = "filter_lp_freq";
	static constexpr const char* kParamFilterHpSlope = "filter_hp_slope";
	static constexpr const char* kParamFilterLpSlope = "filter_lp_slope";
	static constexpr const char* kParamFilterHpOn    = "filter_hp_on";
	static constexpr const char* kParamFilterLpOn    = "filter_lp_on";

	// Chaos
	static constexpr const char* kParamChaos         = "chaos";
	static constexpr const char* kParamChaosD        = "chaos_d";
	static constexpr const char* kParamChaosAmt      = "chaos_amt";
	static constexpr const char* kParamChaosSpd      = "chaos_spd";
	static constexpr const char* kParamChaosAmtFilter = "chaos_amt_filter";
	static constexpr const char* kParamChaosSpdFilter = "chaos_spd_filter";

	static constexpr const char* kParamModeIn   = "mode_in";
	static constexpr const char* kParamModeOut  = "mode_out";
	static constexpr const char* kParamSumBus   = "sum_bus";

	// Invert
	static constexpr const char* kParamInvPol = "inv_pol";
	static constexpr const char* kParamInvStr = "inv_str";

	// Mix Mode + Dry/Wet levels (SEND mode)
	static constexpr const char* kParamMixMode  = "mix_mode";
	static constexpr const char* kParamDryLevel = "dry_level";
	static constexpr const char* kParamWetLevel = "wet_level";

	// Filter position
	static constexpr const char* kParamFilterPos = "filter_pos";

	// Limiter
	static constexpr const char* kParamLimThreshold = "lim_threshold";
	static constexpr const char* kParamLimMode      = "lim_mode";

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
	static constexpr float kShapeDefault = 0.0f;
	static constexpr float kVariationMin = 0.0f;
	static constexpr float kVariationMax = 1.0f;
	static constexpr float kVariationDefault = 0.0f;
	static constexpr float kFeedbackMin     = -1.0f;
	static constexpr float kFeedbackMax     = 1.0f;
	static constexpr float kFeedbackDefault = 0.0f;
	static constexpr float kModMin     = 0.0f;
	static constexpr float kModMax     = 1.0f;
	static constexpr float kModDefault = 0.5f;
	static constexpr float kGainFloorDb  = -144.0f;
	static constexpr float kGainMaxDb    =   24.0f;
	static constexpr float kGainDefaultDb =   0.0f;
	static constexpr float kGainSkew     = 4.4965561056f; // 0 dB at the fader midpoint
	static constexpr float kInputMin     = kGainFloorDb;
	static constexpr float kInputMax     = kGainMaxDb;
	static constexpr float kInputDefault = kGainDefaultDb;

	static constexpr float kOutputMin     = kGainFloorDb;
	static constexpr float kOutputMax     = kGainMaxDb;
	static constexpr float kOutputDefault = kGainDefaultDb;

	static constexpr float kMixMin     = 0.0f;
	static constexpr float kMixMax     = 1.0f;
	static constexpr float kMixDefault = 1.0f;

	static constexpr float kTiltMin     = -6.0f;
	static constexpr float kTiltMax     =  6.0f;
	static constexpr float kTiltDefault =  0.0f;

	static constexpr float kPanMin     = 0.0f;
	static constexpr float kPanMax     = 1.0f;
	static constexpr float kPanDefault = 0.5f;

	static constexpr int kStyleMin     = 0;
	static constexpr int kStyleMax     = 3;         // 0 = MONO, 1 = STEREO, 2 = WIDE, 3 = DUAL
	static constexpr float kStyleDefault = 1.0f;    // STEREO by default

	static constexpr float kFilterFreqMin     = 20.0f;
	static constexpr float kFilterFreqMax     = 20000.0f;
	static constexpr float kFilterHpFreqDefault = 250.0f;
	static constexpr float kFilterLpFreqDefault = 2000.0f;
	static constexpr int   kFilterSlopeMin     = 0;       // 6 dB/oct
	static constexpr int   kFilterSlopeMax     = 2;       // 24 dB/oct
	static constexpr int   kFilterSlopeDefault = 1;       // 12 dB/oct

	// Chaos ranges
	static constexpr float kChaosAmtMin     = 0.0f;
	static constexpr float kChaosAmtMax     = 100.0f;
	static constexpr float kChaosAmtDefault = 50.0f;
	static constexpr float kChaosSpdMin     = 0.01f;
	static constexpr float kChaosSpdMax     = 100.0f;
	static constexpr float kChaosSpdDefault = 5.0f;

	static constexpr int   kModeInOutDefault = 0;
	static constexpr int   kSumBusDefault    = 0;
	static constexpr int   kInvPolDefault    = 0;   // 0=NONE  1=WET  2=GLOBAL
	static constexpr int   kInvStrDefault    = 0;   // 0=NONE  1=WET  2=GLOBAL
	static constexpr int   kMixModeDefault   = 0;   // 0=INSERT, 1=SEND
	static constexpr float kDryLevelDefault  = 0.0f;
	static constexpr float kWetLevelDefault  = 1.0f;
	static constexpr int   kFilterPosDefault = 0;   // 0=POST, 1=PRE
	static constexpr float kSqrt2Over2       = 0.707106781f;

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

	void setMidiChannel (int channel);
	int getMidiChannel() const noexcept;

	void setUiIoExpanded (bool expanded);
	bool getUiIoExpanded() const noexcept;

	static juce::String getMidiNoteName (int midiNote);
	juce::String getCurrentFreqDisplay() const;

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
		static constexpr const char* midiPort = "midiPort";
		static constexpr const char* ioExpanded = "uiIoExpanded";
		static constexpr std::array<const char*, 4> customPalette {
			"uiCustomPalette0", "uiCustomPalette1", "uiCustomPalette2", "uiCustomPalette3"
		};
	};

	static float calcAllPassCoeff (float frequency, float sampleRate) noexcept;
	void resizeDspState (int stages, int series);
	void updateCoefficients (float freqHz, float shapeNorm, int stages);
	void updateCoefficientsInto (float freqHz, float shapeNorm, int stages, std::vector<float>& dest);
	void clearStageRange (int fromStageInclusive, int toStageExclusive, int seriesCount) noexcept;

	std::array<std::vector<AllPassState>, kSeriesMax> chainL;
	std::array<std::vector<AllPassState>, kSeriesMax> chainR;
	std::vector<float> stageCoeff;
	std::vector<float> stageCoeffR;   // R-channel coefficients for DUAL mode
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> stagesSmoothed;
	float smoothedFreqValue = 1000.0f;
	float freqEmaCoeff = 0.0f;
	float freqEmaCoeffDefault_ = 0.0f;
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> shapeSmoothed;
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> variationSmoothed;
	static constexpr double kStageSmoothingSeconds = 0.06;
	static constexpr float kFreqTauDefault   = 0.08f;
	static constexpr float kMidiGlideTauMax  = 0.200f;
	static constexpr float kMidiGlideTauMin  = 0.0002f;
	static constexpr double kShapeSmoothingSeconds = 0.05;
	static constexpr double kVariationSmoothingSeconds = 0.05;
	static constexpr float kVariationFreqDepthOct = 0.25f;
	static constexpr float kVariationShapeDepth = 0.04f;
	static constexpr int kCoeffUpdateInterval = 32;
	static constexpr double kSeriesCrossfadeMs = 20.0;
	int activeStages = 0;
	int activeSeries = kSeriesDefault;
	float lastCoeffFreq = -1.0f;
	float lastCoeffShape = -1.0f;
	int lastCoeffStages = -1;
	float lastCoeffFreqR  = -1.0f;
	int coeffUpdateCountdown = 0;

	// Feedback
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmoothed;
	static constexpr double kFeedbackSmoothingSeconds = 0.05;
	float feedbackLastL = 0.0f;
	float feedbackLastR = 0.0f;

	std::array<std::vector<AllPassState>, kSeriesMax> xfadeChainL;
	std::array<std::vector<AllPassState>, kSeriesMax> xfadeChainR;
	int seriesXfadeSamplesRemaining = 0;
	int seriesXfadeTotalSamples = 0;
	int previousSeries = kSeriesDefault;

	// MIDI note tracking
	std::atomic<float> currentMidiFrequency { 0.0f };
	std::atomic<int>   lastMidiNote { -1 };
	std::atomic<int>   lastMidiVelocity { 127 };
	std::atomic<int>   midiChannel { 0 };

	double currentSampleRate = 44100.0;

	// Input / Output / Mix gain smoothing (same as ECHO-TR)
	float smoothedInputGain = 1.0f;
	float smoothedOutputGain = 1.0f;
	float smoothedMix = 1.0f;
	float smoothedDryLevel = kDryLevelDefault;
	float smoothedWetLevel = kWetLevelDefault;
	float smoothedPan = kPanDefault;
	float smoothedLimThreshold = 1.0f;
	bool  filterPre_  = false;
	bool  tiltPre_    = false;

	// Tilt EQ (1-pole shelving, pivot 1 kHz)
	float tiltDb_        = 0.0f;
	float tiltB0_ = 1.0f, tiltB1_ = 0.0f, tiltA1_ = 0.0f;
	float tiltTargetB0_ = 1.0f, tiltTargetB1_ = 0.0f, tiltTargetA1_ = 0.0f;
	float tiltState_[2]  = { 0.0f, 0.0f };
	float lastTiltDb_    = 0.0f;
	float tiltSmoothSc_  = 0.0f;

	// Pre-allocated dry buffer for mix blend (avoids malloc in processBlock)
	juce::AudioBuffer<float> dryBuffer;

public:
	// Wet filter (HP + LP)
	struct WetFilterBiquadCoeffs { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };
	struct WetFilterBiquadState  { float z1 = 0.0f, z2 = 0.0f; };
private:
	struct WetFilterChannelState
	{
		WetFilterBiquadState hp[2];   // up to 2 cascaded sections (24 dB/oct)
		WetFilterBiquadState lp[2];
		void reset() { *this = {}; }
	};
	WetFilterChannelState wetFilterState_[2];       // L, R
	WetFilterBiquadCoeffs hpCoeffs_[2];             // section 0, 1
	WetFilterBiquadCoeffs lpCoeffs_[2];
	WetFilterBiquadCoeffs hpCoeffsR_[2];             // R channel coeffs (stereo chaos)
	WetFilterBiquadCoeffs lpCoeffsR_[2];
	float smoothedFilterHpFreq_ = kFilterHpFreqDefault;
	float smoothedFilterLpFreq_ = kFilterLpFreqDefault;
	float lastCalcHpFreq_ = -1.0f;
	float lastCalcLpFreq_ = -1.0f;
	int   lastCalcHpSlope_ = -1;
	int   lastCalcLpSlope_ = -1;
	static constexpr int kFilterCoeffUpdateInterval = 32;
	int   filterCoeffCountdown_ = 0;

	void updateFilterCoeffs (bool forceHp, bool forceLp);

	std::atomic<float>* inputParam = nullptr;
	std::atomic<float>* outputParam = nullptr;
	std::atomic<float>* amountParam = nullptr;
	std::atomic<float>* seriesParam = nullptr;
	std::atomic<float>* freqParam = nullptr;
	std::atomic<float>* shapeParam = nullptr;
	std::atomic<float>* variationParam = nullptr;
	std::atomic<float>* altParam = nullptr;
	std::atomic<float>* feedbackParam = nullptr;
	std::atomic<float>* modParam = nullptr;
	std::atomic<float>* mixParam = nullptr;
	std::atomic<float>* tiltParam = nullptr;
	std::atomic<float>* styleParam = nullptr;
	std::atomic<float>* midiParam = nullptr;
	std::atomic<float>* s0Param = nullptr;
	std::atomic<float>* s100Param = nullptr;
	std::atomic<float>* filterHpFreqParam  = nullptr;
	std::atomic<float>* filterLpFreqParam  = nullptr;
	std::atomic<float>* filterHpSlopeParam = nullptr;
	std::atomic<float>* filterLpSlopeParam = nullptr;
	std::atomic<float>* filterHpOnParam    = nullptr;
	std::atomic<float>* filterLpOnParam    = nullptr;
	std::atomic<float>* chaosParam         = nullptr;
	std::atomic<float>* chaosDelayParam    = nullptr;
	std::atomic<float>* chaosAmtParam      = nullptr;
	std::atomic<float>* chaosSpdParam      = nullptr;
	std::atomic<float>* chaosAmtFilterParam = nullptr;
	std::atomic<float>* chaosSpdFilterParam = nullptr;

	std::atomic<float>* modeInParam   = nullptr;
	std::atomic<float>* modeOutParam  = nullptr;
	std::atomic<float>* sumBusParam   = nullptr;

	std::atomic<float>* invPolParam       = nullptr;
	std::atomic<float>* invStrParam       = nullptr;

	std::atomic<float>* limThresholdParam = nullptr;
	std::atomic<float>* limModeParam      = nullptr;

	std::atomic<float>* mixModeParam   = nullptr;
	std::atomic<float>* dryLevelParam  = nullptr;
	std::atomic<float>* wetLevelParam  = nullptr;
	std::atomic<float>* filterPosParam = nullptr;

	std::atomic<float>* panParam       = nullptr;

	std::atomic<float>* uiWidthParam = nullptr;
	std::atomic<float>* uiHeightParam = nullptr;
	std::atomic<float>* uiPaletteParam = nullptr;
	std::atomic<float>* uiFxTailParam = nullptr;
	std::array<std::atomic<float>*, 4> uiColorParams { nullptr, nullptr, nullptr, nullptr };

	std::atomic<int> uiEditorWidth { 360 };
	std::atomic<int> uiEditorHeight { 360 };
	std::atomic<int> uiUseCustomPalette { 0 };
	std::atomic<int> uiFxTailEnabled { 0 };
	std::array<std::atomic<juce::uint32>, 4> uiCustomPalette {
		std::atomic<juce::uint32> { juce::Colours::white.getARGB() },
		std::atomic<juce::uint32> { juce::Colours::black.getARGB() },
		std::atomic<juce::uint32> { juce::Colours::white.getARGB() },
		std::atomic<juce::uint32> { juce::Colours::black.getARGB() }
	};

	// Chaos state (smooth S&H + Drift, per-channel D/G, quadrature F)
	bool  chaosFilterEnabled_ = false;
	bool  chaosDelayEnabled_  = false;
	bool  chaosStereo_        = false;   // true when style >= 1 (per-channel G, quadrature F)

	// CHS D parameters (common micro-delay/gain)
	float chaosAmtD_                    = 0.0f;
	float chaosAmtNormD_                = 0.0f;   // cached amtD * 0.01
	float chaosShPeriodD_               = 8820.0f;
	float smoothedChaosShPeriodD_       = 8820.0f;
	float chaosDelayMaxSamples_         = 0.0f;
	float smoothedChaosDelayMaxSamples_ = 0.0f;
	float chaosGainMaxDb_               = 0.0f;
	float smoothedChaosGainMaxDb_       = 0.0f;
	float chaosDelaySmoothedSamples_[2] = {};
	bool  chaosDelaySmoothReady_[2]     = {};
	float chaosDriveAmtSmoothed_        = 0.0f;
	float chaosDriveSpdSmoothed_        = 0.0f;
	bool  chaosDriveParamSmoothReady_   = false;

	// CHS D smooth S&H + Drift: micro-delay offset
	float chaosDPrev_[2]         = {};
	float chaosDCurr_[2]         = {};
	float chaosDNext_[2]         = {};
	float chaosDPhase_[2]        = {};
	float chaosDDriftPhase_[2]   = {};
	float chaosDDriftFreqHz_[2]  = {};
	float chaosDOut_[2]          = {};
	juce::Random chaosDRng_[2];

	// CHS D smooth S&H + Drift: gain (per-channel, decorrelated)
	float chaosGPrev_[2]         = {};
	float chaosGCurr_[2]         = {};
	float chaosGNext_[2]         = {};
	float chaosGPhase_[2]        = {};
	float chaosGDriftPhase_[2]   = {};
	float chaosGDriftFreqHz_[2]  = {};
	float chaosGOut_[2]          = {};
	juce::Random chaosGRng_[2];

	// CHS F parameters (filter cutoff modulation)
	float chaosAmtF_                  = 0.0f;
	float chaosShPeriodF_             = 8820.0f;
	float smoothedChaosShPeriodF_     = 8820.0f;
	float chaosFilterMaxOct_          = 0.0f;
	float smoothedChaosFilterMaxOct_  = 0.0f;
	float chaosFilterAmtSmoothed_     = 0.0f;
	float chaosFilterSpdSmoothed_     = 0.0f;
	bool  chaosFilterParamSmoothReady_ = false;

	// CHS F smooth S&H + Drift: filter (mono S&H + quadrature drift)
	float chaosFPrev_            = 0.0f;
	float chaosFCurr_            = 0.0f;
	float chaosFNext_            = 0.0f;
	float chaosFPhase_           = 0.0f;
	float chaosFDriftPhase_      = 0.0f;   // single phase; R = +90 deg offset
	float chaosFDriftFreqHz_     = 0.0f;
	float chaosFOut_[2]          = {};     // [0]=L, [1]=R (quadrature when stereo)
	juce::Random chaosFRng_;

	// Chaos per-sample param smoothing (precomputed in prepareToPlay)
	float chaosParamSmoothCoeff_ = 0.999f;
	float cachedChaosParamSmoothCoeff_ = 0.999f;
	float chaosDelaySmoothStep_ = 0.001f;

	static constexpr int kChaosDelayBufLen = 1024;
	float chaosDelayBuf_[2][kChaosDelayBufLen] = {};
	int   chaosDelayWritePos_ = 0;

	static constexpr float kChaosDriftAmp = 0.3f;
	static constexpr float kTwoPi = 6.283185307f;

	struct VariationDrift
	{
		float phase1 = 0.0f;
		float phase2 = 0.0f;
		float phase3 = 0.0f;

		void reset() noexcept
		{
			phase1 = 0.0f;
			phase2 = 0.0f;
			phase3 = 0.0f;
		}

		float advance (float rateHz, float sampleRate) noexcept
		{
			const float invSr = 1.0f / juce::jmax (1.0f, sampleRate);
			phase1 += rateHz * invSr;
			phase2 += rateHz * 0.7937005f * invSr;
			phase3 += rateHz * 0.6180340f * invSr;

			if (phase1 >= 1.0f) phase1 -= std::floor (phase1);
			if (phase2 >= 1.0f) phase2 -= std::floor (phase2);
			if (phase3 >= 1.0f) phase3 -= std::floor (phase3);

			return std::sin (phase1 * kTwoPi) * 0.5f
			     + std::sin (phase2 * kTwoPi) * 0.3f
			     + std::sin (phase3 * kTwoPi) * 0.2f;
		}
	};

	VariationDrift variationFreqDrift_;
	VariationDrift variationShapeDrift_;

	inline void advanceVariation (float amount, float& freqOctOffset, float& shapeOffset) noexcept
	{
		const float amt = juce::jlimit (0.0f, 1.0f, amount);
		if (amt <= 0.000001f)
		{
			freqOctOffset = 0.0f;
			shapeOffset = 0.0f;
			return;
		}

		const float sr = (float) currentSampleRate;
		const float baseRate = 0.03f + amt * 0.12f;
		freqOctOffset = variationFreqDrift_.advance (baseRate, sr) * amt * kVariationFreqDepthOct;
		shapeOffset = variationShapeDrift_.advance (baseRate * 1.37f, sr) * amt * kVariationShapeDepth;
	}

	// Generic smooth S&H + Drift chaos engine (per-sample advance)
	inline void advanceChaosEngine (
		float& prev, float& curr, float& next, float& phase,
		float& driftPhase, float& driftFreqHz, float& output,
		juce::Random& rng, float period, float amtNorm, float sr) noexcept
	{
		const float safePeriod = juce::jmax (1.0f, period);
		phase += 1.0f / safePeriod;
		if (phase >= 1.0f)
		{
			phase -= std::floor (phase);
			prev = curr;
			curr = next;
			next = rng.nextFloat() * 2.0f - 1.0f;
			const float driftBase = sr / safePeriod * 0.37f;
			driftFreqHz = driftBase * (0.88f + rng.nextFloat() * 0.24f);
		}
		const float t = juce::jlimit (0.0f, 1.0f, phase);
		const float t2 = t * t;
		const float t3 = t2 * t;
		const float u = t3 * (t * (t * 6.0f - 15.0f) + 10.0f);
		const float shValue = curr + (next - curr) * u;

		driftPhase += driftFreqHz / sr;
		if (driftPhase > 1e6f) driftPhase -= 1e6f;
		const float driftValue = std::sin (driftPhase * kTwoPi) * kChaosDriftAmp;

		const float shWeight = juce::jlimit (0.0f, 1.0f, amtNorm * 1.5f - 0.15f);
		output = driftValue + shValue * shWeight;
	}

	inline void advanceChaosD() noexcept
	{
		const float sr = (float) currentSampleRate;
		const float smoothStep = 1.0f - chaosParamSmoothCoeff_;
		const float targetAmt = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmtD_);
		const float targetSpd = juce::jlimit (kChaosSpdMin, kChaosSpdMax, sr / juce::jmax (1.0f, chaosShPeriodD_));

		if (! chaosDriveParamSmoothReady_)
		{
			chaosDriveParamSmoothReady_ = true;
			if (chaosDriveSpdSmoothed_ <= 0.0f)
				chaosDriveSpdSmoothed_ = targetSpd;
		}

		chaosDriveAmtSmoothed_ += (targetAmt - chaosDriveAmtSmoothed_) * smoothStep;
		const float spdLog = std::log (juce::jmax (kChaosSpdMin, chaosDriveSpdSmoothed_));
		const float targetSpdLog = std::log (targetSpd);
		chaosDriveSpdSmoothed_ = std::exp (spdLog + (targetSpdLog - spdLog) * smoothStep);

		chaosAmtNormD_ = chaosDriveAmtSmoothed_ * 0.01f;
		smoothedChaosDelayMaxSamples_ = chaosAmtNormD_ * 0.005f * sr;
		smoothedChaosGainMaxDb_ = chaosAmtNormD_ * 1.0f;
		smoothedChaosShPeriodD_ = sr / juce::jmax (kChaosSpdMin, chaosDriveSpdSmoothed_);

		const float period = smoothedChaosShPeriodD_;
		const int nCh = chaosStereo_ ? 2 : 1;

		for (int c = 0; c < nCh; ++c)
		{
			advanceChaosEngine (chaosDPrev_[c], chaosDCurr_[c], chaosDNext_[c], chaosDPhase_[c],
				chaosDDriftPhase_[c], chaosDDriftFreqHz_[c], chaosDOut_[c],
				chaosDRng_[c], period, chaosAmtNormD_, sr);

			advanceChaosEngine (chaosGPrev_[c], chaosGCurr_[c], chaosGNext_[c], chaosGPhase_[c],
				chaosGDriftPhase_[c], chaosGDriftFreqHz_[c], chaosGOut_[c],
				chaosGRng_[c], period, chaosAmtNormD_, sr);
		}

		// Mono chaos: copy ch0 to ch1.
		if (! chaosStereo_)
		{
			chaosDOut_[1] = chaosDOut_[0];
			chaosGOut_[1] = chaosGOut_[0];
		}
	}

	inline void applyChaosDelay (float& wetL, float& wetR) noexcept
	{
		const int wp = chaosDelayWritePos_;
		chaosDelayBuf_[0][wp] = wetL;
		chaosDelayBuf_[1][wp] = wetR;

		const float centerDelay = smoothedChaosDelayMaxSamples_;
		const int mask = kChaosDelayBufLen - 1;

		for (int ch = 0; ch < 2; ++ch)
		{
			const float targetDelaySamp = juce::jlimit (0.0f, (float) (kChaosDelayBufLen - 2),
				centerDelay + chaosDOut_[ch] * smoothedChaosDelayMaxSamples_);
			float& delaySamp = chaosDelaySmoothedSamples_[ch];
			if (! chaosDelaySmoothReady_[ch])
			{
				delaySamp = targetDelaySamp;
				chaosDelaySmoothReady_[ch] = true;
			}
			else
			{
				delaySamp += (targetDelaySamp - delaySamp) * chaosDelaySmoothStep_;
			}

			const float readPos = (float) wp - delaySamp;
			const int iPos = (int) std::floor (readPos);
			const float frac = readPos - (float) iPos;

			const float p0 = chaosDelayBuf_[ch][(iPos - 1) & mask];
			const float p1 = chaosDelayBuf_[ch][ iPos      & mask];
			const float p2 = chaosDelayBuf_[ch][(iPos + 1) & mask];
			const float p3 = chaosDelayBuf_[ch][(iPos + 2) & mask];
			const float c0 = p1;
			const float c1 = p2 - (1.0f / 3.0f) * p0 - 0.5f * p1 - (1.0f / 6.0f) * p3;
			const float c2 = 0.5f * (p0 + p2) - p1;
			const float c3 = (1.0f / 6.0f) * (p3 - p0) + 0.5f * (p1 - p2);
			float& wet = (ch == 0) ? wetL : wetR;
			wet = ((c3 * frac + c2) * frac + c1) * frac + c0;
		}

		chaosDelayWritePos_ = (wp + 1) & mask;

		for (int ch = 0; ch < 2; ++ch)
		{
			const float gainDb = chaosGOut_[ch] * smoothedChaosGainMaxDb_;
			const float ex = gainDb * 0.16609640474f;
			const float exln2 = ex * 0.6931472f;
			const float gainLin = 1.0f + exln2 * (1.0f + exln2 * 0.5f);
			float& wet = (ch == 0) ? wetL : wetR;
			wet *= gainLin;
		}
	}

	inline void advanceChaosF() noexcept
	{
		const float sr       = (float) currentSampleRate;
		const float smoothStep = 1.0f - chaosParamSmoothCoeff_;
		const float targetAmt = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmtF_);
		const float targetSpd = juce::jlimit (kChaosSpdMin, kChaosSpdMax, sr / juce::jmax (1.0f, chaosShPeriodF_));

		if (! chaosFilterParamSmoothReady_)
		{
			chaosFilterParamSmoothReady_ = true;
			if (chaosFilterSpdSmoothed_ <= 0.0f)
				chaosFilterSpdSmoothed_ = targetSpd;
		}

		chaosFilterAmtSmoothed_ += (targetAmt - chaosFilterAmtSmoothed_) * smoothStep;
		const float spdLog = std::log (juce::jmax (kChaosSpdMin, chaosFilterSpdSmoothed_));
		const float targetSpdLog = std::log (targetSpd);
		chaosFilterSpdSmoothed_ = std::exp (spdLog + (targetSpdLog - spdLog) * smoothStep);

		const float amtNormF = chaosFilterAmtSmoothed_ * 0.01f;
		smoothedChaosFilterMaxOct_ = amtNormF * 2.0f;
		smoothedChaosShPeriodF_ = sr / juce::jmax (kChaosSpdMin, chaosFilterSpdSmoothed_);
		const float period = smoothedChaosShPeriodF_;

		const float safePeriod = juce::jmax (1.0f, period);
		chaosFPhase_ += 1.0f / safePeriod;
		if (chaosFPhase_ >= 1.0f)
		{
			chaosFPhase_ -= std::floor (chaosFPhase_);
			chaosFPrev_ = chaosFCurr_;
			chaosFCurr_ = chaosFNext_;
			chaosFNext_ = chaosFRng_.nextFloat() * 2.0f - 1.0f;
			const float driftBase = sr / safePeriod * 0.37f;
			chaosFDriftFreqHz_ = driftBase * (0.88f + chaosFRng_.nextFloat() * 0.24f);
		}

		const float t = juce::jlimit (0.0f, 1.0f, chaosFPhase_);
		const float t2 = t * t;
		const float t3 = t2 * t;
		const float u = t3 * (t * (t * 6.0f - 15.0f) + 10.0f);
		const float shValue = chaosFCurr_ + (chaosFNext_ - chaosFCurr_) * u;

		chaosFDriftPhase_ += chaosFDriftFreqHz_ / sr;
		if (chaosFDriftPhase_ > 1e6f) chaosFDriftPhase_ -= 1e6f;
		const float driftL = std::sin (chaosFDriftPhase_ * kTwoPi) * kChaosDriftAmp;

		const float shWeight = juce::jlimit (0.0f, 1.0f, amtNormF * 1.5f - 0.15f);
		chaosFOut_[0] = driftL + shValue * shWeight;

		if (chaosStereo_)
		{
			const float driftR = std::sin (chaosFDriftPhase_ * kTwoPi + kTwoPi * 0.25f) * kChaosDriftAmp;
			chaosFOut_[1] = driftR + shValue * shWeight;
		}
		else
		{
			chaosFOut_[1] = chaosFOut_[0];
		}
	}

	DspDebugLog dspLog;

	// Limiter ranges and defaults
	static constexpr float kLimThresholdMin     = -36.0f;
	static constexpr float kLimThresholdMax     = 0.0f;
	static constexpr float kLimThresholdDefault = 0.0f;
	static constexpr int   kLimModeDefault      = 0;   // 0=NONE  1=WET  2=GLOBAL

	// Dual-stage transparent limiter state (stereo-linked)
	static constexpr float kLimFloor = 1.0e-12f;
	float limEnv1_[2] = { kLimFloor, kLimFloor };
	float limEnv2_[2] = { kLimFloor, kLimFloor };
	float limAtt1_ = 0.0f;
	float limRel1_ = 0.0f;
	float limRel2_ = 0.0f;

	inline void applyLimiter (float& sampleL, float& sampleR, float threshLin) noexcept
	{
		const float peakL = std::abs (sampleL);
		const float peakR = std::abs (sampleR);

		// Stage 1 - leveler (2 ms attack, 10 ms release)
		for (int ch = 0; ch < 2; ++ch)
		{
			const float p = (ch == 0) ? peakL : peakR;
			if (p > limEnv1_[ch])
				limEnv1_[ch] = limAtt1_ * limEnv1_[ch] + (1.0f - limAtt1_) * p;
			else
				limEnv1_[ch] = limRel1_ * limEnv1_[ch] + (1.0f - limRel1_) * p;
			if (limEnv1_[ch] < kLimFloor) limEnv1_[ch] = kLimFloor;
		}

		// Stage 2 - brickwall (instant attack, 100 ms release)
		for (int ch = 0; ch < 2; ++ch)
		{
			const float p = (ch == 0) ? peakL : peakR;
			if (p > limEnv2_[ch])
				limEnv2_[ch] = p;
			else
				limEnv2_[ch] = limRel2_ * limEnv2_[ch] + (1.0f - limRel2_) * p;
			if (limEnv2_[ch] < kLimFloor) limEnv2_[ch] = kLimFloor;
		}

		// Stereo-linked gain reduction
		float gr = 1.0f;
		const float maxEnv1 = juce::jmax (limEnv1_[0], limEnv1_[1]);
		const float maxEnv2 = juce::jmax (limEnv2_[0], limEnv2_[1]);
		if (maxEnv1 > threshLin)
			gr = juce::jmin (gr, threshLin / maxEnv1);
		if (maxEnv2 > threshLin)
			gr = juce::jmin (gr, threshLin / maxEnv2);

		sampleL *= gr;
		sampleR *= gr;
	}

	inline void applyLimiterMono (float& sample, float threshLin) noexcept
	{
		const float peak = std::abs (sample);
		if (peak > limEnv1_[0])
			limEnv1_[0] = limAtt1_ * limEnv1_[0] + (1.0f - limAtt1_) * peak;
		else
			limEnv1_[0] = limRel1_ * limEnv1_[0] + (1.0f - limRel1_) * peak;
		if (limEnv1_[0] < kLimFloor) limEnv1_[0] = kLimFloor;

		if (peak > limEnv2_[0])
			limEnv2_[0] = peak;
		else
			limEnv2_[0] = limRel2_ * limEnv2_[0] + (1.0f - limRel2_) * peak;
		if (limEnv2_[0] < kLimFloor) limEnv2_[0] = kLimFloor;

		float gr = 1.0f;
		if (limEnv1_[0] > threshLin)
			gr = juce::jmin (gr, threshLin / limEnv1_[0]);
		if (limEnv2_[0] > threshLin)
			gr = juce::jmin (gr, threshLin / limEnv2_[0]);

		sample *= gr;
		limEnv1_[1] = limEnv1_[0];
		limEnv2_[1] = limEnv2_[0];
	}

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisperserAudioProcessor)
};

