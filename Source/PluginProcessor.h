#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "DspDebugLog.h"
#include "Modulation/DispModulationConfig.h"
#include "../../TR-Shared/SimpleDSP/TRLimiterBank.h"
#include "../../TR-Shared/SimpleDSP/TRPhaseQuadrature.h"
#include "../../TR-Shared/SimpleDSP/TRTemporalDSP.h"
#include "../../TR-Shared/Modulation/Runtime/TRAdaptiveJitterKernel.h"
#include "ReverseDispersionRuntime.h"
#include "ReverseDispersionFirRuntime.h"

class DisperserAudioProcessor : public juce::AudioProcessor
{
public:
	DisperserAudioProcessor();
	~DisperserAudioProcessor() override;

	static constexpr const char* kParamAmount    = "amount";
	static constexpr const char* kParamSeries    = "series";
	static constexpr const char* kParamFreq      = "freq";
	static constexpr const char* kParamShape     = "shape";
	static constexpr const char* kParamJitter    = "jitter";
	static constexpr const char* kParamAlt       = "alt";
	static constexpr const char* kParamReverse   = "reverse";
	static constexpr const char* kParamFeedback  = "feedback";
	static constexpr const char* kParamMod       = "mod";
	static constexpr const char* kParamModHarm    = "mod_harm";
	static constexpr const char* kParamInput     = "input";
	static constexpr const char* kParamOutput    = "output";
	static constexpr const char* kParamMix       = "mix";
	static constexpr const char* kParamTilt      = "tilt";
	static constexpr const char* kParamPan       = "pan";
	static constexpr const char* kParamStyle     = "style";
	static constexpr const char* kParamMidi      = "midi";
	static constexpr const char* kParamSidechain = "sidechain";
	static constexpr const char* kParamSidechainGain   = "sidechain_gain";
	static constexpr const char* kParamSidechainSmooth = "sidechain_smooth";
	static constexpr const char* kParamSidechainPol    = "sidechain_pol";
	static constexpr const char* kParamSidechainHp     = "sidechain_hp";
	static constexpr const char* kParamSidechainLp     = "sidechain_lp";
	static constexpr const char* kParamSidechainHpOn   = "sidechain_hp_on";
	static constexpr const char* kParamSidechainLpOn   = "sidechain_lp_on";
	static constexpr const char* kParamSidechainHpSlope = "sidechain_hp_slope";
	static constexpr const char* kParamSidechainLpSlope = "sidechain_lp_slope";

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
	static constexpr const char* kParamLimQuality   = "lim_quality";


	static constexpr int kAmountMin = 0;
	static constexpr int kAmountMax = 128;
	static constexpr int kAmountDefault = 32;

	static constexpr int kSeriesMin = 1;
	static constexpr int kSeriesMax = 4;
	static constexpr int kSeriesDefault = 1;

	static constexpr float kFreqMin = 20.0f;
	static constexpr float kFreqBaseMax = 5000.0f;
	static constexpr float kFreqEffectiveMax = 20000.0f;
	static constexpr float kFreqDefault = 1000.0f;
	static constexpr float kShapeDefault = 0.0f;
	static constexpr float kJitterMin = 0.0f;
	static constexpr float kJitterMax = 1.0f;
	static constexpr float kJitterDefault = 0.0f;
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
	static constexpr float kSidechainSmoothMin     = 0.0f;
	static constexpr float kSidechainSmoothMax     = 1.0f;
	static constexpr float kSidechainSmoothDefault = 0.25f;
	static constexpr float kSidechainGainMin       = -144.0f;
	static constexpr float kSidechainGainMax       =   24.0f;
	static constexpr float kSidechainGainDefault   =    0.0f;
	static constexpr float kSidechainPolMin        =   -1.0f;
	static constexpr float kSidechainPolMax        =    1.0f;
	static constexpr float kSidechainPolDefault    =    1.0f;
	static constexpr float kSidechainFilterFreqMin =   20.0f;
	static constexpr float kSidechainFilterFreqMax = 20000.0f;
	static constexpr float kSidechainHpDefault     =   20.0f;
	static constexpr float kSidechainLpDefault     = 20000.0f;
	static constexpr bool  kSidechainHpOnDefault   = true;
	static constexpr bool  kSidechainLpOnDefault   = true;
	static constexpr int   kSidechainHpSlopeDefault = 1;
	static constexpr int   kSidechainLpSlopeDefault = 1;
	static constexpr float kSidechainFrequencyOffsetMaxHz = 5000.0f;

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

	float getInputMeterPeak() const noexcept { return inputMeterPeak_.load (std::memory_order_relaxed); }
	float getOutputMeterPeak() const noexcept { return outputMeterPeak_.load (std::memory_order_relaxed); }

	struct PhaseContourTelemetry
	{
		float centre = 0.0f;
		float stages = 0.0f;
		float series = 0.0f;
		float shape = 0.0f;
		float feedbackMagnitude = 0.0f;
		float feedbackPolarity = 1.0f;
		float alternatePolarity = 0.0f;
		float topology = 1.0f / 3.0f;
		float activity = 0.0f;
	};

	PhaseContourTelemetry getPhaseContourTelemetry() const noexcept;

	void setMidiChannel (int channel);
	int getMidiChannel() const noexcept;
	void setMidiDelayMs (int delayMsValue);
	int getMidiDelayMs() const noexcept;

	static juce::String getMidiNoteName (int midiNote);
	juce::String getCurrentFreqDisplay() const;

	juce::AudioProcessorValueTreeState apvts;
	static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
	TR::Modulation::State modulationState() const;
	bool setModulationState(const TR::Modulation::State&);
	std::uint64_t modulationStateGeneration() const noexcept;
	std::array<float, TR::Modulation::macroCount> modulationMacroValues() const noexcept;
	void setModulationMacroValue(int macro, float value);
	TR::Modulation::Runtime::TelemetrySnapshot modulationTelemetry() const noexcept;
	bool modulationDestinationValues(juce::StringRef id, float& base,
	                                 float& effective) const noexcept;
	friend struct DispNativeSidechainTestAccess;

private:
	TR::Modulation::Integration::ParameterModulationBridge modulation;
	TR::Modulation::Runtime::MidiEventBuffer modulationMidiEvents;
	bool useNativeSidechainForTests_ = false;
	float* jitterEvidenceNativeSeries1ForTests_ = nullptr;
	float* jitterEvidenceNativeSeries2ForTests_ = nullptr;
	float* jitterEvidenceNativePeriodMsForTests_ = nullptr;
	float* jitterEvidenceNativeRawFreq1ForTests_ = nullptr;
	float* jitterEvidenceNativeRawFreq2ForTests_ = nullptr;
	float* jitterEvidenceNativeAmountForTests_ = nullptr;
	int jitterEvidenceCapacityForTests_ = 0;

	using AllPassState = TR::DSP::FirstOrderAllPass;

	struct UiStateKeys
	{
		static constexpr const char* midiPort = "midiPort";
		static constexpr const char* midiDelayMs = "midiDelayMs";
	};

	static float calcAllPassCoeff (float frequency, float sampleRate) noexcept;
	void resizeDspState (int stages, int series);
	void updateCoefficients (float freqHz, float shapeNorm, int stages);
	void updateCoefficientsInto (float freqHz, float shapeNorm, int stages, std::vector<float>& dest);
	void clearStageRange (int fromStageInclusive, int toStageExclusive, int seriesCount) noexcept;
	void resetPhaseContourTelemetry() noexcept;
	void publishPhaseContourTelemetry (float feedback, int topology,
	                                  bool alternate, float activity) noexcept;

	std::array<std::vector<AllPassState>, kSeriesMax> chainL;
	std::array<std::vector<AllPassState>, kSeriesMax> chainR;
	std::vector<float> stageCoeff;
	std::vector<float> stageCoeffR;   // R-channel coefficients for DUAL mode
	std::array<std::vector<float>, kSeriesMax> jitterStageCoeff;
	std::array<std::vector<float>, kSeriesMax> jitterStageCoeffR;
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> stagesSmoothed;
	float smoothedFreqValue = 1000.0f;
	juce::AudioBuffer<float> jitterMotionReferencePeriods_;
	float freqEmaCoeff = 0.0f;
	float freqEmaCoeffDefault_ = 0.0f;
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> shapeSmoothed;
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> jitterSmoothed;
	static constexpr double kStageSmoothingSeconds = 0.06;
	static constexpr float kFreqTauDefault   = 0.08f;
	static constexpr float kMidiGlideTauMax  = 0.200f;
	static constexpr float kMidiGlideTauMin  = 0.0002f;
	static constexpr double kShapeSmoothingSeconds = 0.05;
	static constexpr double kJitterSmoothingSeconds = 0.0325;
	static constexpr float kJitterEpsilon = 0.000001f;
	static constexpr float kJitterMinDelaySamples = 2.0f;
	static constexpr float kJitterMinDelayMs = 0.05f;
	static constexpr float kJitterShortRefMs = 8.0f;
	static constexpr float kJitterMidRefMs = 500.0f;
	static constexpr float kJitterLongRefMs = 4000.0f;
	static constexpr float kJitterLongnessRefMs = 250.0f;
	static constexpr float kJitterHighStart = 0.55f;
	static constexpr float kJitterHighRange = 0.45f;
	static constexpr float kJitterExtremeStart = 0.82f;
	static constexpr float kJitterExtremeRange = 0.18f;
	static constexpr float kJitterDriftRateMinHz = 0.03f;
	static constexpr float kJitterDriftRateMaxHz = 2.0f;
	static constexpr float kJitterDriftRateBaseHz = 0.08f;
	static constexpr float kJitterDriftRateTopHz = 1.20f;
	static constexpr float kJitterDriftLongnessDamping = 0.65f;
	static constexpr float kJitterDriftShortnessBoost = 0.10f;
	static constexpr float kJitterFlutterRateMinHz = 2.0f;
	static constexpr float kJitterFlutterRateMaxHz = 7000.0f;
	static constexpr float kJitterFlutterRateBaseHz = 4.0f;
	static constexpr float kJitterFlutterRateTopHz = 130.0f;
	static constexpr float kJitterFlutterRefMs = 250.0f;
	static constexpr float kJitterFlutterDelayPower = 0.90f;
	static constexpr float kJitterToneStart = 0.18f;
	static constexpr float kJitterToneRange = 0.82f;
	static constexpr float kJitterToneRateSmoothTauSeconds = 0.006f;
	static constexpr float kJitterToneCeilHz = 12000.0f;
	static constexpr float kJitterToneCeilSampleRateRatio = 0.22f;
	static constexpr float kJitterToneLiftBase = 4.0f;
	static constexpr float kJitterToneLiftAmount = 190.0f;
	static constexpr float kJitterToneLiftHigh = 60.0f;
	static constexpr float kJitterToneLiftExtreme = 90.0f;
	static constexpr float kJitterToneShortnessPower = 0.70f;
	static constexpr float kJitterToneRightHarmonic = 1.618f;
	static constexpr float kJitterToneWeightShortnessPower = 0.55f;
	static constexpr float kJitterToneWeightBase = 0.35f;
	static constexpr float kJitterToneWeightAmount = 0.55f;
	static constexpr float kJitterToneWeightMax = 0.78f;
	static constexpr float kJitterToneFundamentalWeight = 0.72f;
	static constexpr float kJitterToneSecondWeight = 0.20f;
	static constexpr float kJitterToneThirdWeight = 0.08f;
	static constexpr float kJitterToneSecondPhaseL = 0.73f;
	static constexpr float kJitterToneSecondPhaseR = 1.37f;
	static constexpr float kJitterToneThirdPhaseL = 1.91f;
	static constexpr float kJitterToneThirdPhaseR = 2.47f;
	static constexpr float kJitterDriftWeightMin = 0.18f;
	static constexpr float kJitterDriftWeightMax = 0.72f;
	static constexpr float kJitterDriftWeightBase = 0.42f;
	static constexpr float kJitterDriftWeightLongness = 0.30f;
	static constexpr float kJitterDriftWeightShortness = 0.14f;
	static constexpr float kJitterFlutterWeightMin = 0.35f;
	static constexpr float kJitterFlutterWeightMax = 0.95f;
	static constexpr float kJitterFlutterWeightBase = 0.45f;
	static constexpr float kJitterFlutterWeightShortness = 0.38f;
	static constexpr float kJitterFlutterWeightHigh = 0.12f;
	static constexpr float kJitterOutputLimit = 1.25f;
	static constexpr float kJitterDepthRatio = 0.055f;
	static constexpr float kJitterDepthPower = 1.05f;
	static constexpr float kJitterMaxDepthRatio = 0.12f;
	static constexpr float kJitterMinDepthSeconds = 1.0e-7f;
	static constexpr float kJitterMinEngineRateHz = 0.01f;
	static constexpr float kJitterFeedbackDepthBase = 0.010f;
	static constexpr float kJitterFeedbackDepthRange = 0.055f;
	static constexpr float kJitterFeedbackShortBoost = 0.20f;
	static constexpr float kJitterFeedbackDepthScale = 0.60f;
	static constexpr float kJitterFeedbackSlowRateScale = 0.80f;
	static constexpr float kJitterFeedbackFastRateScale = 0.55f;
	static constexpr float kJitterFeedbackSlowWeight = 0.62f;
	static constexpr float kJitterFeedbackFastWeightBase = 0.24f;
	static constexpr float kJitterFeedbackFastShortnessWeight = 0.28f;
	static constexpr float kJitterFeedbackOutputLimit = 1.0f;
	static constexpr bool kJitterModulatesFeedback = false;
	static constexpr float kJitterFrequencyDepthScale = 2.0f;
	static constexpr float kJitterShapeDepthScale = 0.45f;
	static constexpr float kJitterCoeffSmoothMinSeconds = 0.0f;
	static constexpr float kJitterCoeffSmoothMaxSeconds = 0.05f;
	static constexpr int kCoeffUpdateInterval = 32;
	static constexpr double kSeriesCrossfadeMs = 20.0;
	int activeStages = 0;
	int activeSeries = kSeriesDefault;
	float lastCoeffFreq = -1.0f;
	float lastCoeffShape = -1.0f;
	int lastCoeffStages = -1;
	float lastCoeffFreqR  = -1.0f;
	std::array<float, kSeriesMax> jitterSeriesFreq {};
	std::array<float, kSeriesMax> jitterSeriesShape {};
	std::array<float, kSeriesMax> smoothedJitterSeriesFreq {};
	std::array<float, kSeriesMax> smoothedJitterSeriesShape {};
	std::array<float, kSeriesMax> lastJitterCoeffFreq {};
	std::array<float, kSeriesMax> lastJitterCoeffShape {};
	std::array<float, kSeriesMax> lastJitterCoeffFreqR {};
	int lastJitterCoeffStages = -1;
	int coeffUpdateCountdown = 0;
	bool jitterCoeffSmoothingReady_ = false;

	std::atomic<float> telemetryPhaseCentre_ { 0.0f };
	std::atomic<float> telemetryStageDepth_ { 0.0f };
	std::atomic<float> telemetrySeriesDepth_ { 0.0f };
	std::atomic<float> telemetryPhaseShape_ { 0.0f };
	std::atomic<float> telemetryFeedbackMagnitude_ { 0.0f };
	std::atomic<float> telemetryFeedbackPolarity_ { 1.0f };
	std::atomic<float> telemetryAlternatePolarity_ { 0.0f };
	std::atomic<float> telemetryStereoTopology_ { 1.0f / 3.0f };
	std::atomic<float> telemetryActivity_ { 0.0f };

	// Feedback
	juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmoothed;
	static constexpr double kFeedbackSmoothingSeconds = 0.05;
	static constexpr float kFbkDcBlockHz = 5.0f;
	float feedbackLastL = 0.0f;
	float feedbackLastR = 0.0f;
	float fbkDcStateInL = 0.0f;
	float fbkDcStateInR = 0.0f;
	float fbkDcStateOutL = 0.0f;
	float fbkDcStateOutR = 0.0f;
	float fbkDcCoeff = 0.999f;

	std::array<std::vector<AllPassState>, kSeriesMax> xfadeChainL;
	std::array<std::vector<AllPassState>, kSeriesMax> xfadeChainR;
	int seriesXfadeSamplesRemaining = 0;
	int seriesXfadeTotalSamples = 0;
	int previousSeries = kSeriesDefault;

	// MIDI note tracking
	enum class PendingMidiEventType
	{
		noteOn,
		noteOff,
		allNotesOff
	};

	struct PendingMidiEvent
	{
		PendingMidiEventType type = PendingMidiEventType::allNotesOff;
		int note = -1;
		int velocity = 0;
		int samplesRemaining = 0;
	};

	void clearMidiTrackingState() noexcept;
	void clearPendingMidiEvents() noexcept;
	void enqueuePendingMidiEvent (const PendingMidiEvent& event) noexcept;
	void applyPendingMidiEvent (const PendingMidiEvent& event) noexcept;
	void resetSidechainRuntime() noexcept;

	static constexpr int kPendingMidiEventCapacity = 256;
	std::atomic<float> currentMidiFrequency { 0.0f };
	std::atomic<int>   lastMidiNote { -1 };
	std::atomic<int>   lastMidiVelocity { 127 };
	std::atomic<int>   midiChannel { 0 };
	std::atomic<int>   midiDelayMs { 0 };
	std::array<PendingMidiEvent, kPendingMidiEventCapacity> pendingMidiEvents_ {};
	int pendingMidiEventCount_ = 0;


public:
	// Shared biquad type used by wet filters and sidechain detector filters.
	struct WetFilterBiquadCoeffs { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };
	struct WetFilterBiquadState  { float z1 = 0.0f, z2 = 0.0f; };

private:
	std::vector<float> sidechainFrequencyOffsetNorm_;
	float sidechainDcPrevInL_ = 0.0f;
	float sidechainDcPrevInR_ = 0.0f;
	float sidechainDcPrevOutL_ = 0.0f;
	float sidechainDcPrevOutR_ = 0.0f;
	WetFilterBiquadState sidechainHpFilterL_[2];
	WetFilterBiquadState sidechainHpFilterR_[2];
	WetFilterBiquadState sidechainLpFilterL_[2];
	WetFilterBiquadState sidechainLpFilterR_[2];
	float sidechainCarrierSmoothL_ = 0.0f;
	float sidechainCarrierSmoothR_ = 0.0f;
	float sidechainRmsEnv_ = 0.0f;
	float sidechainGateSmoothed_ = 0.0f;
	float sidechainDepthSmoothed_ = 0.0f;

	double currentSampleRate = 0.0;

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

	// Wet filter (HP + LP)
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
	std::atomic<float>* jitterParam = nullptr;
	std::atomic<float>* altParam = nullptr;
	std::atomic<float>* reverseParam = nullptr;
	std::atomic<float>* feedbackParam = nullptr;
	std::atomic<float>* modParam = nullptr;
	std::atomic<float>* modHarmParam = nullptr;
	std::atomic<float>* mixParam = nullptr;
	std::atomic<float>* tiltParam = nullptr;
	std::atomic<float>* styleParam = nullptr;
	std::atomic<float>* midiParam = nullptr;
	std::atomic<float>* sidechainParam = nullptr;
	std::atomic<float>* sidechainGainParam = nullptr;
	std::atomic<float>* sidechainSmoothParam = nullptr;
	std::atomic<float>* sidechainPolParam = nullptr;
	std::atomic<float>* sidechainHpParam = nullptr;
	std::atomic<float>* sidechainLpParam = nullptr;
	std::atomic<float>* sidechainHpOnParam = nullptr;
	std::atomic<float>* sidechainLpOnParam = nullptr;
	std::atomic<float>* sidechainHpSlopeParam = nullptr;
	std::atomic<float>* sidechainLpSlopeParam = nullptr;
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
	std::atomic<float>* limQualityParam   = nullptr;

	std::atomic<float>* mixModeParam   = nullptr;
	std::atomic<float>* dryLevelParam  = nullptr;
	std::atomic<float>* wetLevelParam  = nullptr;
	std::atomic<float>* filterPosParam = nullptr;

	std::atomic<float>* panParam       = nullptr;

	std::atomic<float> inputMeterPeak_ { 0.0f };
	std::atomic<float> outputMeterPeak_ { 0.0f };
	DISP::DSP::ReverseDispersionFirRuntime reverseDispersionRuntime_;
	bool reverseEnabledLast_ = false;

	// Chaos state (smooth S&H + Drift, per-channel D/G, quadrature F)
	bool  chaosFilterEnabled_ = false;
	bool  chaosDelayEnabled_  = false;
	bool  chaosStereo_        = false;   // true when style >= 1 (per-channel G, quadrature F)

	// CHS D parameters (common micro-delay/gain)
	float chaosAmtD_                    = 0.0f;
	float chaosAmtNormD_                = 0.0f;   // cached amtD * 0.01
	float chaosShPeriodD_               = 0.0f;
	float smoothedChaosShPeriodD_       = 0.0f;
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
	float chaosShPeriodF_             = 0.0f;
	float smoothedChaosShPeriodF_     = 0.0f;
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

	struct JitterModulator
	{
		float slowPrev = 0.0f;
		float slowCurr = 0.0f;
		float slowNext = 0.0f;
		float slowPhase = 0.0f;
		float slowDriftPhase = 0.0f;
		float slowDriftFreqHz = 0.0f;
		juce::Random slowRng;

		float fastPrev = 0.0f;
		float fastCurr = 0.0f;
		float fastNext = 0.0f;
		float fastPhase = 0.0f;
		float fastDriftPhase = 0.0f;
		float fastDriftFreqHz = 0.0f;
		juce::Random fastRng;

		float tonePhase = 0.0f;
		float toneRateHz = 0.0f;

		void reset (juce::int64 seed, float initialTonePhase) noexcept
		{
			slowPrev = slowCurr = 0.0f;
			slowPhase = slowDriftPhase = slowDriftFreqHz = 0.0f;
			slowRng.setSeed (seed ^ 0x5a17d15cll);
			slowNext = slowRng.nextFloat() * 2.0f - 1.0f;

			fastPrev = fastCurr = 0.0f;
			fastPhase = fastDriftPhase = fastDriftFreqHz = 0.0f;
			fastRng.setSeed (seed ^ 0x2f05a11ll);
			fastNext = fastRng.nextFloat() * 2.0f - 1.0f;

			tonePhase = initialTonePhase;
			toneRateHz = 0.0f;
		}
	};

	struct JitterLane
	{
		JitterModulator freq;
		JitterModulator shape;

		void reset (juce::int64 seedBase) noexcept
		{
			freq.reset (seedBase ^ 0x44565031ll, 0.113f);
			shape.reset (seedBase ^ 0x44565053ll, 0.617f);
		}
	};

	std::array<JitterLane, kSeriesMax> jitterLanes_;
	JitterModulator jitterFeedbackMod_;

	static float smoothStep01 (float x) noexcept
	{
		const float t = juce::jlimit (0.0f, 1.0f, x);
		return t * t * (3.0f - 2.0f * t);
	}

	static float applyJitterToFeedback (float feedback, float modOut, float depth) noexcept
	{
		const float fb = juce::jlimit (-1.0f, 1.0f, feedback);
		if (! kJitterModulatesFeedback)
			return fb;

		const float mag = std::abs (fb);
		if (mag <= kJitterEpsilon)
			return fb;

		const float sign = fb < 0.0f ? -1.0f : 1.0f;
		const float newMag = juce::jlimit (0.0f, 1.0f, mag * (1.0f + modOut * depth));
		return sign * newMag;
	}

	using JitterMetrics = TR::Modulation::AdaptiveJitter::Response;

	inline JitterMetrics makeJitterMetrics (float baseDelaySamples, float amount, float sr, int laneIndex) const noexcept
	{
		const float delayMs = juce::jmax (kJitterMinDelayMs,
			juce::jmax (kJitterMinDelaySamples, baseDelaySamples) * 1000.0f / sr);
		TR::Modulation::AdaptiveJitter::ResponseConfig config;
		config.toneCeilSampleRateRatio = kJitterToneCeilSampleRateRatio;
		config.toneWeightMaximum = kJitterToneWeightMax;
		return TR::Modulation::AdaptiveJitter::evaluateResponse (
			delayMs, delayMs * 0.001f, amount, sr, laneIndex, config);
	}

	inline float calcJitterEquivalentDelaySamples (float freqHz, float stages, int seriesCount) const noexcept
	{
		const float sr = juce::jmax (1.0f, (float) currentSampleRate);
		const float freq = juce::jlimit (20.0f, 0.49f * sr, freqHz);
		const float a = calcAllPassCoeff (freq, sr);
		const float omega = kTwoPi * freq / sr;
		const float denom = 1.0f + a * a - 2.0f * a * std::cos (omega);
		const float singleStageDelay = (denom > 1.0e-9f) ? (1.0f - a * a) / denom : kJitterMinDelaySamples;

		if (! std::isfinite (singleStageDelay) || singleStageDelay <= 0.0f)
			return kJitterMinDelaySamples;

		const float stageCount = juce::jmax (1.0f, stages);
		const float series = (float) juce::jmax (1, seriesCount);
		return juce::jmax (kJitterMinDelaySamples, singleStageDelay * stageCount * series);
	}

	inline float advanceJitterModulator (JitterModulator& mod, const JitterMetrics& metrics,
	                                     float sr, int laneIndex,
	                                     float* slowForTests = nullptr,
	                                     float* fastForTests = nullptr,
	                                     float* tonalForTests = nullptr) noexcept
	{
		float slowOut = 0.0f;
		float fastOut = 0.0f;
		const float slowPeriod = sr / juce::jmax (kJitterMinEngineRateHz, metrics.driftRateHz);
		const float fastPeriod = sr / juce::jmax (kJitterMinEngineRateHz, metrics.flutterRateHz);

		advanceChaosEngine (mod.slowPrev, mod.slowCurr, mod.slowNext,
		                    mod.slowPhase, mod.slowDriftPhase, mod.slowDriftFreqHz,
		                    slowOut, mod.slowRng, slowPeriod, metrics.amountMapped, sr);
		advanceChaosEngine (mod.fastPrev, mod.fastCurr, mod.fastNext,
		                    mod.fastPhase, mod.fastDriftPhase, mod.fastDriftFreqHz,
		                    fastOut, mod.fastRng, fastPeriod, metrics.amountMapped, sr);

		float toneOut = 0.0f;
		if (metrics.toneWeight > kJitterEpsilon && metrics.toneRateHz > 0.0f)
		{
			const float toneRateSmooth = std::exp (-1.0f / (sr * kJitterToneRateSmoothTauSeconds));
			if (mod.toneRateHz <= 0.0f)
				mod.toneRateHz = metrics.toneRateHz;
			else
				mod.toneRateHz = mod.toneRateHz * toneRateSmooth
				               + metrics.toneRateHz * (1.0f - toneRateSmooth);

			mod.tonePhase += mod.toneRateHz / sr;
			mod.tonePhase -= std::floor (mod.tonePhase);

			const bool oddLane = (laneIndex & 1) != 0;
			const float phase = mod.tonePhase * kTwoPi;
			toneOut = std::sin (phase) * kJitterToneFundamentalWeight
			        + std::sin (phase * 2.0f + (oddLane ? kJitterToneSecondPhaseR : kJitterToneSecondPhaseL))
			          * kJitterToneSecondWeight
			        + std::sin (phase * 3.0f + (oddLane ? kJitterToneThirdPhaseR : kJitterToneThirdPhaseL))
			          * kJitterToneThirdWeight;
		}
		else
		{
			mod.toneRateHz = 0.0f;
		}

		const float combined = slowOut * metrics.driftWeight
		                     + fastOut * metrics.flutterWeight
		                     + toneOut * metrics.toneWeight;
		if (slowForTests != nullptr) *slowForTests = slowOut;
		if (fastForTests != nullptr) *fastForTests = fastOut;
		if (tonalForTests != nullptr) *tonalForTests = toneOut;
		return juce::jlimit (-kJitterOutputLimit, kJitterOutputLimit, combined);
	}

	inline void advanceJitterLane (JitterLane& lane, float amount, float equivalentDelaySamples,
	                               int laneIndex, float& freqOctOffset, float& shapeOffset,
	                               float* flutterHzForTests = nullptr,
	                               float* depthOctForTests = nullptr,
	                               float* slowForTests = nullptr,
	                               float* fastForTests = nullptr,
	                               float* tonalForTests = nullptr) noexcept
	{
		const float amt = juce::jlimit (0.0f, 1.0f, amount);
		if (amt <= kJitterEpsilon)
		{
			freqOctOffset = 0.0f;
			shapeOffset = 0.0f;
			return;
		}

		const float sr = juce::jmax (1.0f, (float) currentSampleRate);
		const JitterMetrics freqMetrics = makeJitterMetrics (equivalentDelaySamples, amt, sr, laneIndex);
		if (flutterHzForTests != nullptr)
			*flutterHzForTests = freqMetrics.flutterRateHz;
		if (depthOctForTests != nullptr)
			*depthOctForTests = freqMetrics.delayDepthOct;
		const JitterMetrics shapeMetrics = makeJitterMetrics (equivalentDelaySamples, amt, sr, laneIndex + 1);
		const float freqOut = advanceJitterModulator (lane.freq, freqMetrics, sr, laneIndex,
			slowForTests, fastForTests, tonalForTests);
		const float shapeOut = advanceJitterModulator (lane.shape, shapeMetrics, sr, laneIndex + 1);

		freqOctOffset = -freqOut * freqMetrics.delayDepthOct * kJitterFrequencyDepthScale;
		shapeOffset = shapeOut * shapeMetrics.delayDepthOct * kJitterShapeDepthScale;
	}

	inline void advanceJitterFeedback (float amount, float equivalentDelaySamples,
	                                  float& feedbackOut, float& feedbackDepth) noexcept
	{
		const float amt = juce::jlimit (0.0f, 1.0f, amount);
		if (amt <= kJitterEpsilon)
		{
			feedbackOut = 0.0f;
			feedbackDepth = 0.0f;
			return;
		}

		const float sr = juce::jmax (1.0f, (float) currentSampleRate);
		const JitterMetrics metrics = makeJitterMetrics (equivalentDelaySamples, amt, sr, 0);
		float feedbackSlow = 0.0f;
		float feedbackFast = 0.0f;

		advanceChaosEngine (jitterFeedbackMod_.slowPrev, jitterFeedbackMod_.slowCurr, jitterFeedbackMod_.slowNext,
		                    jitterFeedbackMod_.slowPhase, jitterFeedbackMod_.slowDriftPhase, jitterFeedbackMod_.slowDriftFreqHz,
		                    feedbackSlow, jitterFeedbackMod_.slowRng,
		                    sr / juce::jmax (kJitterMinEngineRateHz, metrics.driftRateHz * kJitterFeedbackSlowRateScale),
		                    metrics.amountMapped, sr);
		advanceChaosEngine (jitterFeedbackMod_.fastPrev, jitterFeedbackMod_.fastCurr, jitterFeedbackMod_.fastNext,
		                    jitterFeedbackMod_.fastPhase, jitterFeedbackMod_.fastDriftPhase, jitterFeedbackMod_.fastDriftFreqHz,
		                    feedbackFast, jitterFeedbackMod_.fastRng,
		                    sr / juce::jmax (kJitterMinEngineRateHz, metrics.flutterRateHz * kJitterFeedbackFastRateScale),
		                    metrics.amountMapped, sr);

		feedbackOut = juce::jlimit (-kJitterFeedbackOutputLimit, kJitterFeedbackOutputLimit,
			feedbackSlow * kJitterFeedbackSlowWeight
			+ feedbackFast * (kJitterFeedbackFastWeightBase + metrics.shortness * kJitterFeedbackFastShortnessWeight));
		feedbackDepth = metrics.feedbackDepth * kJitterFeedbackDepthScale;
	}

	// Generic smooth S&H + Drift chaos engine (per-sample advance)
	inline void advanceChaosEngine (
		float& prev, float& curr, float& next, float& phase,
		float& driftPhase, float& driftFreqHz, float& output,
		juce::Random& rng, float period, float amtNorm, float sr) noexcept
	{
		TR::Modulation::AdaptiveJitter::advanceChaosEngine (
			prev, curr, next, phase, driftPhase, driftFreqHz, output, rng,
			period, amtNorm, sr, kChaosDriftAmp, kTwoPi);
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

		// Delay modulation is mono-linked to avoid mono-sum phaser artifacts.
		// Gain modulation may stay stereo for width when the style supports it.
		chaosDOut_[1] = chaosDOut_[0];
		if (! chaosStereo_)
			chaosGOut_[1] = chaosGOut_[0];
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
	static constexpr int   kLimQualityDefault   = 0;   // 0=FAST  1=CLEAN  2=TRUE PEAK (GLOBAL only)

	TR::DSP::LimiterBank limiterBank_;

	inline void applyLimiter (float& sampleL, float& sampleR, float threshLin) noexcept
	{
		limiterBank_.fastProcessor().processStereo (sampleL, sampleR, threshLin);
	}

	inline void applyLimiterMono (float& sample, float threshLin) noexcept
	{
		limiterBank_.fastProcessor().processMonoAndMirror (sample, threshLin);
	}

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DisperserAudioProcessor)
};
