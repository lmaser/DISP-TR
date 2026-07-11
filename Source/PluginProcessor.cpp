#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DspDebugLog.h"
#include "../../TR-Shared/SimpleDSP/TRSimpleDSP.h"

namespace
{
	constexpr int kRtScratchMaxSamples = 65536;

	inline float loadAtomicOrDefault (std::atomic<float>* p, float def) noexcept
	{
		return p != nullptr ? p->load (std::memory_order_relaxed) : def;
	}

	inline int loadIntParamOrDefault (std::atomic<float>* p, int def) noexcept
	{
		return (int) std::lround (loadAtomicOrDefault (p, (float) def));
	}

	inline bool loadBoolParamOrDefault (std::atomic<float>* p, bool def) noexcept
	{
		return loadAtomicOrDefault (p, def ? 1.0f : 0.0f) > 0.5f;
	}

	inline float modSliderToLinearMultiplier (float v) noexcept
	{
		v = juce::jlimit (0.0f, 1.0f, v);
		if (v < 0.5f)
			return 1.0f / (4.0f - 6.0f * v);
		return 1.0f + ((v - 0.5f) * 6.0f);
	}

	inline float smoothStep01 (float x) noexcept
	{
		x = juce::jlimit (0.0f, 1.0f, x);
		return x * x * (3.0f - 2.0f * x);
	}

	inline float harmonicModStepToMultiplier (float step) noexcept
	{
		step = juce::jlimit (-8.0f, 8.0f, step);
		return step >= 0.0f ? (1.0f + step) : (1.0f / (1.0f - step));
	}

	inline float modSliderToHarmonicMultiplier (float v) noexcept
	{
		const float pos = juce::jlimit (0.0f, 1.0f, v) * 16.0f - 8.0f;
		const float transitionWidth = 0.08f;
		const float centre = juce::jlimit (-8.0f, 8.0f, std::floor (pos + 0.5f));
		const float delta = pos - centre;
		float step = centre;

		if (delta > 0.5f - transitionWidth && centre < 8.0f)
		{
			const float t = (delta - (0.5f - transitionWidth)) / transitionWidth;
			step = centre + smoothStep01 (t);
		}
		else if (delta < -0.5f + transitionWidth && centre > -8.0f)
		{
			const float t = (delta + 0.5f) / transitionWidth;
			step = (centre - 1.0f) + smoothStep01 (t);
		}

		return harmonicModStepToMultiplier (step);
	}

	inline float modSliderToEffectiveMultiplier (float v, bool harmonicMode) noexcept
	{
		return harmonicMode ? modSliderToHarmonicMultiplier (v)
		                    : modSliderToLinearMultiplier (v);
	}
	inline void setParameterPlainValue (juce::AudioProcessorValueTreeState& apvts,
										const char* paramId,
										float plainValue)
	{
		if (auto* param = apvts.getParameter (paramId))
		{
			const float norm = param->convertTo0to1 (plainValue);
			param->setValueNotifyingHost (norm);
		}
	}

	// Gain / mix EMA coefficient: one-pole ~5 ms time constant at 44.1 kHz.
	constexpr float kGainSmoothCoeff = 0.9955f;

	inline float fastDecibelsToGain (float dB) noexcept
	{
		if (dB <= -100.0f) return 0.0f;
		return std::exp2 (dB * 0.16609640474f);   // log2(10)/20
	}

	inline float gainFaderDecibelsToGain (float dB) noexcept
	{
		return TR::DSP::decibelsToGain (dB, DisperserAudioProcessor::kGainFloorDb);
	}

	inline float sanitiseFeedbackWrite (float v) noexcept
	{
		if (! std::isfinite (v))
			return 0.0f;

		constexpr float knee = 32.0f;
		constexpr float limit = 64.0f;
		const float av = std::abs (v);
		if (av <= knee)
			return v;

		const float range = limit - knee;
		const float excess = av - knee;
		const float shaped = knee + range * excess / (excess + range);
		return std::copysign (shaped, v);
	}

	inline float dcBlockTick (float in, float& inState, float& outState, float r) noexcept
	{
		outState = r * (outState + in - inState);
		inState = in;
		if (! (outState > -1.0e15f && outState < 1.0e15f))
		{
			outState = 0.0f;
			inState = 0.0f;
		}
		return outState;
	}

	inline juce::NormalisableRange<float> makeGainFaderRange() noexcept
	{
		return juce::NormalisableRange<float> (DisperserAudioProcessor::kGainFloorDb,
		                                       DisperserAudioProcessor::kGainMaxDb,
		                                       0.0f,
		                                       DisperserAudioProcessor::kGainSkew);
	}

	// Biquad coefficient calculators for wet HP/LP filters
	using BQC = DisperserAudioProcessor::WetFilterBiquadCoeffs;

	inline BQC calcOnePoleLP (float freq, float sr)
	{
		const float w = juce::MathConstants<float>::twoPi * freq / sr;
		const float alpha = w / (1.0f + w);
		return { alpha, 0.0f, 0.0f, -(1.0f - alpha), 0.0f };
	}

	inline BQC calcOnePoleHP (float freq, float sr)
	{
		const float w = juce::MathConstants<float>::twoPi * freq / sr;
		const float a = 1.0f / (1.0f + w);
		return { a, -a, 0.0f, -(1.0f - a), 0.0f };
	}

	inline BQC calcBiquadLP (float freq, float sr, float Q)
	{
		const float w0 = juce::MathConstants<float>::twoPi * freq / sr;
		const float cs = std::cos (w0);
		const float sn = std::sin (w0);
		const float alpha = sn / (2.0f * Q);
		const float a0 = 1.0f + alpha;
		return { ((1.0f - cs) * 0.5f) / a0,
				 (1.0f - cs) / a0,
				 ((1.0f - cs) * 0.5f) / a0,
				 (-2.0f * cs) / a0,
				 (1.0f - alpha) / a0 };
	}

	inline BQC calcBiquadHP (float freq, float sr, float Q)
	{
		const float w0 = juce::MathConstants<float>::twoPi * freq / sr;
		const float cs = std::cos (w0);
		const float sn = std::sin (w0);
		const float alpha = sn / (2.0f * Q);
		const float a0 = 1.0f + alpha;
		return { ((1.0f + cs) * 0.5f) / a0,
				 -(1.0f + cs) / a0,
				 ((1.0f + cs) * 0.5f) / a0,
				 (-2.0f * cs) / a0,
				 (1.0f - alpha) / a0 };
	}

	// 2nd/4th-order Butterworth Q values
	constexpr float kBW2_Q = 0.70710678f;    // 1 / sqrt(2)
	constexpr float kBW4_Q1 = 0.54119610f;   // 1 / (2 cos(3pi/8))
	constexpr float kBW4_Q2 = 1.30656296f;   // 1 / (2 cos(pi/8))

	inline float processBiquad (const BQC& c,
							   DisperserAudioProcessor::WetFilterBiquadState& s,
							   float x) noexcept
	{
		const float y = c.b0 * x + s.z1;
		s.z1 = c.b1 * x - c.a1 * y + s.z2;
		s.z2 = c.b2 * x - c.a2 * y;
		return y;
	}
}

DisperserAudioProcessor::DisperserAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
	: AudioProcessor (BusesProperties()
					 #if ! JucePlugin_IsMidiEffect
					  #if ! JucePlugin_IsSynth
					   .withInput  ("Input", juce::AudioChannelSet::stereo(), true)
					   .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)
					  #endif
					   .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
					 #endif
					   )
#endif
	, apvts (*this, nullptr, "Parameters", createParameterLayout())
{
	amountParam = apvts.getRawParameterValue (kParamAmount);
	seriesParam = apvts.getRawParameterValue (kParamSeries);
	freqParam = apvts.getRawParameterValue (kParamFreq);
	shapeParam = apvts.getRawParameterValue (kParamShape);
	jitterParam = apvts.getRawParameterValue (kParamJitter);
	altParam = apvts.getRawParameterValue (kParamAlt);
	feedbackParam = apvts.getRawParameterValue (kParamFeedback);
	modParam = apvts.getRawParameterValue (kParamMod);
	modHarmParam = apvts.getRawParameterValue (kParamModHarm);
	mixParam = apvts.getRawParameterValue (kParamMix);
	tiltParam = apvts.getRawParameterValue (kParamTilt);
	panParam  = apvts.getRawParameterValue (kParamPan);
	styleParam = apvts.getRawParameterValue (kParamStyle);
	midiParam = apvts.getRawParameterValue (kParamMidi);
	sidechainParam = apvts.getRawParameterValue (kParamSidechain);
	sidechainGainParam = apvts.getRawParameterValue (kParamSidechainGain);
	sidechainSmoothParam = apvts.getRawParameterValue (kParamSidechainSmooth);
	sidechainPolParam = apvts.getRawParameterValue (kParamSidechainPol);
	sidechainHpParam = apvts.getRawParameterValue (kParamSidechainHp);
	sidechainLpParam = apvts.getRawParameterValue (kParamSidechainLp);
	sidechainHpOnParam = apvts.getRawParameterValue (kParamSidechainHpOn);
	sidechainLpOnParam = apvts.getRawParameterValue (kParamSidechainLpOn);
	sidechainHpSlopeParam = apvts.getRawParameterValue (kParamSidechainHpSlope);
	sidechainLpSlopeParam = apvts.getRawParameterValue (kParamSidechainLpSlope);
	s0Param = apvts.getRawParameterValue (kParamS0);
	s100Param = apvts.getRawParameterValue (kParamS100);
	uiWidthParam = apvts.getRawParameterValue (kParamUiWidth);
	uiHeightParam = apvts.getRawParameterValue (kParamUiHeight);
	uiPaletteParam = apvts.getRawParameterValue (kParamUiPalette);
	uiFxTailParam = apvts.getRawParameterValue (kParamUiFxTail);
	uiIoFxParam = apvts.getRawParameterValue (kParamUiIoFx);
	uiColorParams[0] = apvts.getRawParameterValue (kParamUiColor0);
	uiColorParams[1] = apvts.getRawParameterValue (kParamUiColor1);
	uiColorParams[2] = apvts.getRawParameterValue (kParamUiColor2);
	uiColorParams[3] = apvts.getRawParameterValue (kParamUiColor3);
	inputParam = apvts.getRawParameterValue (kParamInput);
	outputParam = apvts.getRawParameterValue (kParamOutput);
	filterHpFreqParam  = apvts.getRawParameterValue (kParamFilterHpFreq);
	filterLpFreqParam  = apvts.getRawParameterValue (kParamFilterLpFreq);
	filterHpSlopeParam = apvts.getRawParameterValue (kParamFilterHpSlope);
	filterLpSlopeParam = apvts.getRawParameterValue (kParamFilterLpSlope);
	filterHpOnParam    = apvts.getRawParameterValue (kParamFilterHpOn);
	filterLpOnParam    = apvts.getRawParameterValue (kParamFilterLpOn);
	chaosParam         = apvts.getRawParameterValue (kParamChaos);
	chaosDelayParam    = apvts.getRawParameterValue (kParamChaosD);
	chaosAmtParam      = apvts.getRawParameterValue (kParamChaosAmt);
	chaosSpdParam      = apvts.getRawParameterValue (kParamChaosSpd);
	chaosAmtFilterParam = apvts.getRawParameterValue (kParamChaosAmtFilter);
	chaosSpdFilterParam = apvts.getRawParameterValue (kParamChaosSpdFilter);
	modeInParam   = apvts.getRawParameterValue (kParamModeIn);
	modeOutParam  = apvts.getRawParameterValue (kParamModeOut);
	sumBusParam   = apvts.getRawParameterValue (kParamSumBus);
	invPolParam        = apvts.getRawParameterValue (kParamInvPol);
	invStrParam        = apvts.getRawParameterValue (kParamInvStr);
	limThresholdParam = apvts.getRawParameterValue (kParamLimThreshold);
	limModeParam      = apvts.getRawParameterValue (kParamLimMode);
	mixModeParam   = apvts.getRawParameterValue (kParamMixMode);
	dryLevelParam  = apvts.getRawParameterValue (kParamDryLevel);
	wetLevelParam  = apvts.getRawParameterValue (kParamWetLevel);
	filterPosParam = apvts.getRawParameterValue (kParamFilterPos);
}

DisperserAudioProcessor::~DisperserAudioProcessor()
{
}

const juce::String DisperserAudioProcessor::getName() const { return JucePlugin_Name; }
bool DisperserAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
	return true;
#else
	return false;
#endif
}
bool DisperserAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
	return true;
#else
	return false;
#endif
}
bool DisperserAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
	return true;
#else
	return false;
#endif
}
double DisperserAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int DisperserAudioProcessor::getNumPrograms() { return 1; }
int DisperserAudioProcessor::getCurrentProgram() { return 0; }
void DisperserAudioProcessor::setCurrentProgram (int) {}
const juce::String DisperserAudioProcessor::getProgramName (int) { return {}; }
void DisperserAudioProcessor::changeProgramName (int, const juce::String&) {}

void DisperserAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
	currentSampleRate = juce::jmax (1.0, sampleRate);
	const int scratchSamples = juce::jmax (samplesPerBlock, kRtScratchMaxSamples);
	const int stages = juce::jlimit (kAmountMin, kAmountMax,
									 loadIntParamOrDefault (amountParam, kAmountDefault));
	const int series = juce::jlimit (kSeriesMin, kSeriesMax,
									 loadIntParamOrDefault (seriesParam, kSeriesDefault));
	const float freq = loadAtomicOrDefault (freqParam, kFreqDefault);
	const float shape = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (shapeParam, kShapeDefault));

	resizeDspState (kAmountMax, kSeriesMax);
	activeStages = stages;
	activeSeries = series;
	coeffUpdateCountdown = 0;

	for (int i = 0; i < kSeriesMax; ++i)
	{
		xfadeChainL[(size_t) i].assign ((size_t) kAmountMax, {});
		xfadeChainR[(size_t) i].assign ((size_t) kAmountMax, {});
	}
	seriesXfadeSamplesRemaining = 0;
	seriesXfadeTotalSamples = 0;
	previousSeries = series;

	stagesSmoothed.reset (currentSampleRate, kStageSmoothingSeconds);
	stagesSmoothed.setCurrentAndTargetValue ((float) stages);
	smoothedFreqValue = freq;
	freqEmaCoeffDefault_ = std::exp (-1.0f / ((float) currentSampleRate * kFreqTauDefault));
	freqEmaCoeff = freqEmaCoeffDefault_;
	shapeSmoothed.reset (currentSampleRate, kShapeSmoothingSeconds);
	shapeSmoothed.setCurrentAndTargetValue (shape);
	jitterSmoothed.reset (currentSampleRate, kJitterSmoothingSeconds);
	jitterSmoothed.setCurrentAndTargetValue (juce::jlimit (kJitterMin, kJitterMax, loadAtomicOrDefault (jitterParam, kJitterDefault)));
	feedbackSmoothed.reset (currentSampleRate, kFeedbackSmoothingSeconds);
	feedbackSmoothed.setCurrentAndTargetValue (juce::jlimit (kFeedbackMin, kFeedbackMax, loadAtomicOrDefault (feedbackParam, kFeedbackDefault)));
	feedbackLastL = 0.0f;
	feedbackLastR = 0.0f;
	fbkDcCoeff = std::exp (-juce::MathConstants<float>::twoPi * kFbkDcBlockHz / (float) currentSampleRate);
	fbkDcStateInL = fbkDcStateInR = 0.0f;
	fbkDcStateOutL = fbkDcStateOutR = 0.0f;

	// Input/Output/Mix utility smoothing init
	const float inputGainDb = juce::jlimit (kInputMin, kInputMax,
		loadAtomicOrDefault (inputParam, kInputDefault));
	const float outputGainDb = juce::jlimit (kOutputMin, kOutputMax,
		loadAtomicOrDefault (outputParam, kOutputDefault));
	smoothedInputGain = gainFaderDecibelsToGain (inputGainDb);
	smoothedOutputGain = gainFaderDecibelsToGain (outputGainDb);
	smoothedMix = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (mixParam, kMixDefault));
	smoothedDryLevel = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (dryLevelParam, kDryLevelDefault));
	smoothedWetLevel = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (wetLevelParam, kWetLevelDefault));
	smoothedPan = juce::jlimit (kPanMin, kPanMax, loadAtomicOrDefault (panParam, kPanDefault));
	smoothedLimThreshold = fastDecibelsToGain (juce::jlimit (kLimThresholdMin, kLimThresholdMax,
		loadAtomicOrDefault (limThresholdParam, kLimThresholdDefault)));

	// Reset tilt state
	tiltDb_ = 0.0f;
	tiltB0_ = 1.0f; tiltB1_ = 0.0f; tiltA1_ = 0.0f;
	tiltTargetB0_ = 1.0f; tiltTargetB1_ = 0.0f; tiltTargetA1_ = 0.0f;
	tiltState_[0] = tiltState_[1] = 0.0f;
	lastTiltDb_ = 0.0f;
	tiltSmoothSc_ = 1.0f - std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.03f));

	// Pre-allocate processBlock scratch buffers; these must not grow on the audio thread.
	dryBuffer.setSize (getTotalNumOutputChannels(), scratchSamples);
	sidechainFrequencyOffsetNorm_.assign ((size_t) scratchSamples, 0.0f);

	lastCoeffFreq = -1.0f;
	lastCoeffShape = -1.0f;
	lastCoeffStages = -1;
	lastCoeffFreqR = -1.0f;
	smoothedJitterSeriesFreq.fill (freq);
	smoothedJitterSeriesShape.fill (shape);
	lastJitterCoeffFreq.fill (-1.0f);
	lastJitterCoeffShape.fill (-1.0f);
	lastJitterCoeffFreqR.fill (-1.0f);
	lastJitterCoeffStages = -1;
	jitterCoeffSmoothingReady_ = false;
	for (int i = 0; i < kSeriesMax; ++i)
		jitterLanes_[(size_t) i].reset (0x44564A4C + (juce::int64) i * 0x10001);
	jitterFeedbackMod_.reset (0x44564A46ll, 0.381f);

	// Reset MIDI note tracking
	clearPendingMidiEvents();
	clearMidiTrackingState();
	resetSidechainRuntime();

	// Reset wet filter state
	wetFilterState_[0].reset();
	wetFilterState_[1].reset();
	smoothedFilterHpFreq_ = loadAtomicOrDefault (filterHpFreqParam, kFilterHpFreqDefault);
	smoothedFilterLpFreq_ = loadAtomicOrDefault (filterLpFreqParam, kFilterLpFreqDefault);
	lastCalcHpFreq_ = -1.0f;
	lastCalcLpFreq_ = -1.0f;
	lastCalcHpSlope_ = -1;
	lastCalcLpSlope_ = -1;
	filterCoeffCountdown_ = 0;
	updateFilterCoeffs (true, true);

	// Reset chaos state
	chaosFilterEnabled_ = false;
	chaosDelayEnabled_  = false;
	chaosStereo_ = false;
	chaosAmtD_ = 0.0f; chaosAmtNormD_ = 0.0f; chaosAmtF_ = 0.0f;
	for (int c = 0; c < 2; ++c)
	{
		chaosDPrev_[c] = chaosDCurr_[c] = chaosDNext_[c] = 0.0f;
		chaosDPhase_[c] = 0.0f; chaosDDriftPhase_[c] = 0.0f; chaosDDriftFreqHz_[c] = 0.0f; chaosDOut_[c] = 0.0f;
		chaosGPrev_[c] = chaosGCurr_[c] = chaosGNext_[c] = 0.0f;
		chaosGPhase_[c] = 0.0f; chaosGDriftPhase_[c] = 0.0f; chaosGDriftFreqHz_[c] = 0.0f; chaosGOut_[c] = 0.0f;
	}
	chaosFPrev_ = chaosFCurr_ = chaosFNext_ = 0.0f;
	chaosFPhase_ = 0.0f; chaosFDriftPhase_ = 0.0f; chaosFDriftFreqHz_ = 0.0f;
	chaosFOut_[0] = chaosFOut_[1] = 0.0f;
	chaosDelayMaxSamples_ = 0.0f;
	smoothedChaosDelayMaxSamples_ = 0.0f;
	smoothedChaosGainMaxDb_ = 0.0f;
	smoothedChaosFilterMaxOct_ = 0.0f;
	chaosDelaySmoothedSamples_[0] = chaosDelaySmoothedSamples_[1] = 0.0f;
	chaosDelaySmoothReady_[0] = chaosDelaySmoothReady_[1] = false;
	for (auto& channel : chaosDelayBuf_)
		for (float& sample : channel)
			sample = 0.0f;
	chaosDelayWritePos_ = 0;
	chaosParamSmoothCoeff_ = 0.999f;
	chaosDriveAmtSmoothed_ = 0.0f;
	chaosDriveSpdSmoothed_ = kChaosSpdDefault;
	chaosDriveParamSmoothReady_ = false;
	chaosFilterAmtSmoothed_ = 0.0f;
	chaosFilterSpdSmoothed_ = kChaosSpdDefault;
	chaosFilterParamSmoothReady_ = false;

	// Precompute chaos smooth coefficients (sampleRate-dependent but constant between prepareToPlay)
	cachedChaosParamSmoothCoeff_ = std::exp (-1.0f / ((float) currentSampleRate * 0.010f));
	chaosDelaySmoothStep_ = 1.0f - std::exp (-1.0f / ((float) currentSampleRate * 0.002f));

	// Reset limiter state and precompute coefficients
	limEnv1_[0] = limEnv1_[1] = kLimFloor;
	limEnv2_[0] = limEnv2_[1] = kLimFloor;
	{
		const float sr = (float) getSampleRate();
		limAtt1_ = std::exp (-1.0f / (sr * 0.002f));
		limRel1_ = std::exp (-1.0f / (sr * 0.010f));
		limRel2_ = std::exp (-1.0f / (sr * 0.100f));
	}

#if JUCE_DEBUG
	// Developer diagnostics stay available in Debug, but must not write files in Release.
	dspLog.enableDesktopAutoDump();
#endif
}

void DisperserAudioProcessor::updateFilterCoeffs (bool forceHp, bool forceLp)
{
	const float sr = (float) currentSampleRate;
	const int hpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
									  loadIntParamOrDefault (filterHpSlopeParam, kFilterSlopeDefault));
	const int lpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
									  loadIntParamOrDefault (filterLpSlopeParam, kFilterSlopeDefault));

	const float hpFreq = juce::jlimit (kFilterFreqMin, juce::jmin (kFilterFreqMax, 0.49f * sr), smoothedFilterHpFreq_);
	const float lpFreq = juce::jlimit (kFilterFreqMin, juce::jmin (kFilterFreqMax, 0.49f * sr), smoothedFilterLpFreq_);

	if (forceHp || hpSlope != lastCalcHpSlope_ || std::abs (hpFreq - lastCalcHpFreq_) > 0.01f)
	{
		lastCalcHpFreq_  = hpFreq;
		lastCalcHpSlope_ = hpSlope;

		if (hpSlope == 0)      // 6 dB/oct - single 1-pole
		{
			hpCoeffs_[0] = calcOnePoleHP (hpFreq, sr);
			hpCoeffs_[1] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };  // pass-through
		}
		else if (hpSlope == 1) // 12 dB/oct - single Butterworth biquad
		{
			hpCoeffs_[0] = calcBiquadHP (hpFreq, sr, kBW2_Q);
			hpCoeffs_[1] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		}
		else                   // 24 dB/oct - two cascaded Butterworth biquads
		{
			hpCoeffs_[0] = calcBiquadHP (hpFreq, sr, kBW4_Q1);
			hpCoeffs_[1] = calcBiquadHP (hpFreq, sr, kBW4_Q2);
		}
	}

	if (forceLp || lpSlope != lastCalcLpSlope_ || std::abs (lpFreq - lastCalcLpFreq_) > 0.01f)
	{
		lastCalcLpFreq_  = lpFreq;
		lastCalcLpSlope_ = lpSlope;

		if (lpSlope == 0)
		{
			lpCoeffs_[0] = calcOnePoleLP (lpFreq, sr);
			lpCoeffs_[1] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		}
		else if (lpSlope == 1)
		{
			lpCoeffs_[0] = calcBiquadLP (lpFreq, sr, kBW2_Q);
			lpCoeffs_[1] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		}
		else
		{
			lpCoeffs_[0] = calcBiquadLP (lpFreq, sr, kBW4_Q1);
			lpCoeffs_[1] = calcBiquadLP (lpFreq, sr, kBW4_Q2);
		}
	}
}

void DisperserAudioProcessor::releaseResources()
{
	for (auto& c : chainL) c.clear();
	for (auto& c : chainR) c.clear();
	for (auto& c : xfadeChainL) c.clear();
	for (auto& c : xfadeChainR) c.clear();
	stageCoeff.clear();
	stageCoeffR.clear();
	for (auto& c : jitterStageCoeff) c.clear();
	for (auto& c : jitterStageCoeffR) c.clear();
	clearPendingMidiEvents();
	clearMidiTrackingState();
	feedbackLastL = feedbackLastR = 0.0f;
	fbkDcStateInL = fbkDcStateInR = 0.0f;
	fbkDcStateOutL = fbkDcStateOutR = 0.0f;
	resetSidechainRuntime();
	sidechainFrequencyOffsetNorm_.clear();
}

#if ! JucePlugin_PreferredChannelConfigurations
bool DisperserAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
	const auto out = layouts.getMainOutputChannelSet();
	if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
		return false;

	const auto in = layouts.getMainInputChannelSet();
	if (in != out)
		return false;

	const auto sidechain = layouts.getChannelSet (true, 1);
	if (! sidechain.isDisabled()
		&& sidechain != juce::AudioChannelSet::mono()
		&& sidechain != juce::AudioChannelSet::stereo())
		return false;

	return true;
}
#endif

float DisperserAudioProcessor::calcAllPassCoeff (float frequency, float sampleRate) noexcept
{
	const float maxFreq = juce::jmax (kFreqMin, juce::jmin (kFreqEffectiveMax, 0.49f * sampleRate));
	const float f = juce::jlimit (kFreqMin, maxFreq, frequency);
	const float t = std::tan (juce::MathConstants<float>::pi * f / sampleRate);
	if (! std::isfinite (t))
		return 0.0f;
	return (1.0f - t) / (1.0f + t);
}

void DisperserAudioProcessor::resizeDspState (int stages, int series)
{
	juce::ignoreUnused (stages, series);
	const int nStages = kAmountMax;

	const size_t coeffSize = (size_t) nStages;
	if (stageCoeff.size() != coeffSize)
		stageCoeff.assign (coeffSize, 0.0f);
	if (stageCoeffR.size() != coeffSize)
		stageCoeffR.assign (coeffSize, 0.0f);

	for (int i = 0; i < kSeriesMax; ++i)
	{
		const size_t newSize = (size_t) nStages;
		if (jitterStageCoeff[(size_t) i].size() != coeffSize)
			jitterStageCoeff[(size_t) i].assign (coeffSize, 0.0f);
		if (jitterStageCoeffR[(size_t) i].size() != coeffSize)
			jitterStageCoeffR[(size_t) i].assign (coeffSize, 0.0f);
		if (chainL[(size_t) i].size() != newSize)
			chainL[(size_t) i].assign (newSize, {});
		if (chainR[(size_t) i].size() != newSize)
			chainR[(size_t) i].assign (newSize, {});
	}
}

void DisperserAudioProcessor::clearStageRange (int fromStageInclusive,
													   int toStageExclusive,
													   int seriesCount) noexcept
{
	const int fromStage = juce::jlimit (0, kAmountMax, fromStageInclusive);
	const int toStage = juce::jlimit (0, kAmountMax, toStageExclusive);
	const int nSeries = juce::jlimit (kSeriesMin, kSeriesMax, seriesCount);

	if (toStage <= fromStage)
		return;

	for (int s = 0; s < nSeries; ++s)
	{
		auto* left = chainL[(size_t) s].data();
		auto* right = chainR[(size_t) s].data();
		for (int st = fromStage; st < toStage; ++st)
		{
			left[st].z1 = 0.0f;
			right[st].z1 = 0.0f;
		}
	}
}

void DisperserAudioProcessor::updateCoefficientsInto (float freqHz, float shapeNorm, int stages, std::vector<float>& dest)
{
	const int nStages = juce::jmax (1, stages);
	if ((int) dest.size() < nStages)
		dest.assign ((size_t) kAmountMax, 0.0f);

	const float sr = (float) currentSampleRate;
	const float minFreq = kFreqMin;
	const float maxFreq = juce::jmax (minFreq, juce::jmin (kFreqEffectiveMax, 0.49f * sr));
	const float center = juce::jlimit (minFreq, maxFreq, freqHz);
	const float shape = juce::jlimit (0.0f, 1.0f, shapeNorm);

	const float logPos = std::log2 (center / minFreq) / std::log2 (maxFreq / minFreq);
	const float lowComp = std::pow (juce::jlimit (0.0f, 1.0f, 1.0f - logPos), 1.15f);
	const float shapeStrength = 1.0f + (0.95f * lowComp);
	const float shapeComp = juce::jlimit (0.0f, 1.0f, 0.5f + ((shape - 0.5f) * shapeStrength));

	const float spreadMax = 4.0f + (1.1f * lowComp);
	const float spreadMin = 0.12f;
	const float spreadOct = juce::jmap (shapeComp, spreadMax, spreadMin);
	const float warpGamma = juce::jmap (shapeComp, 0.45f, 3.0f + (0.8f * lowComp));

	if (nStages == 1)
	{
		dest[0] = calcAllPassCoeff (center, sr);
		return;
	}

	const float denom = (float) juce::jmax (1, nStages - 1);
	for (int i = 0; i < nStages; ++i)
	{
		const float u = (2.0f * ((float) i / denom)) - 1.0f;
		const float absWarped = std::pow (std::abs (u), warpGamma);
		const float warped = std::copysign (absWarped, u);
		const float oct = 0.5f * spreadOct * warped;
		const float f = juce::jlimit (minFreq, maxFreq, center * std::pow (2.0f, oct));
		dest[(size_t) i] = calcAllPassCoeff (f, sr);
	}
}

void DisperserAudioProcessor::updateCoefficients (float freqHz, float shapeNorm, int stages)
{
	updateCoefficientsInto (freqHz, shapeNorm, stages, stageCoeff);
}

void DisperserAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
	juce::ScopedNoDenormals noDenormals;
	DSP_LOG_BLOCK_BEGIN();

	const int numSamples = buffer.getNumSamples();
	const int numChannels = juce::jmin (buffer.getNumChannels(), getTotalNumOutputChannels());
	if (numSamples <= 0 || numChannels <= 0)
		return;

	for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
		buffer.clear (ch, 0, numSamples);

	const bool sidechainEnabled = loadBoolParamOrDefault (sidechainParam, false);
	const float* sidechainReadL = nullptr;
	const float* sidechainReadR = nullptr;
	int sidechainChannels = 0;
	float sidechainPeak = 0.0f;

	if (sidechainEnabled && getBusCount (true) > 1)
	{
		auto sidechainBuffer = getBusBuffer (buffer, true, 1);
		sidechainChannels = sidechainBuffer.getNumChannels();
		if (sidechainChannels > 0)
		{
			sidechainReadL = sidechainBuffer.getReadPointer (0);
			sidechainReadR = (sidechainChannels > 1) ? sidechainBuffer.getReadPointer (1) : sidechainReadL;

			for (int ch = 0; ch < juce::jmin (sidechainChannels, 2); ++ch)
				sidechainPeak = juce::jmax (sidechainPeak, sidechainBuffer.getMagnitude (ch, 0, numSamples));
		}
	}
	const bool sidechainBusAvailable = sidechainEnabled && sidechainReadL != nullptr;
	const bool sidechainCarrierDetected = sidechainBusAvailable && sidechainPeak > 1.0e-6f;

	const int targetStages = juce::jlimit (kAmountMin, kAmountMax, loadIntParamOrDefault (amountParam, kAmountDefault));
	const int targetSeries = juce::jlimit (kSeriesMin, kSeriesMax, loadIntParamOrDefault (seriesParam, kSeriesDefault));
	float targetFreq = loadAtomicOrDefault (freqParam, kFreqDefault);
	float targetShape = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (shapeParam, kShapeDefault));
	const float targetJitter = juce::jlimit (kJitterMin, kJitterMax, loadAtomicOrDefault (jitterParam, kJitterDefault));

	// Debug overrides preserved.
	if (loadBoolParamOrDefault (s0Param, false))
		targetShape = 0.0f;
	if (loadBoolParamOrDefault (s100Param, false))
		targetShape = 1.0f;
	const bool altEnabled = loadBoolParamOrDefault (altParam, false);

	// MIDI note tracking
	const bool midiEnabled = loadBoolParamOrDefault (midiParam, false);
	const int midiDelaySamples = juce::jmax (0, (int) std::lround ((double) currentSampleRate
		* (double) juce::jlimit (0, 100, getMidiDelayMs()) / 1000.0));
	if (midiEnabled && ! midi.isEmpty())
	{
		const int ch = midiChannel.load (std::memory_order_relaxed);
		for (const auto metadata : midi)
		{
			const auto msg = metadata.getMessage();
			if (ch != 0 && msg.getChannel() != ch)
				continue;

			auto queueMidiEvent = [this, midiDelaySamples, metadata, numSamples] (PendingMidiEvent event)
			{
				const int eventSampleInBlock = juce::jlimit (0, juce::jmax (0, numSamples - 1), metadata.samplePosition);
				event.samplesRemaining = juce::jmax (0, eventSampleInBlock + midiDelaySamples);
				enqueuePendingMidiEvent (event);
			};

			if (msg.isAllNotesOff() || msg.isAllSoundOff())
			{
				queueMidiEvent ({ PendingMidiEventType::allNotesOff, -1, 0, 0 });
			}
			else if (msg.isNoteOn())
			{
				queueMidiEvent ({ PendingMidiEventType::noteOn, msg.getNoteNumber(), msg.getVelocity(), 0 });
			}
			else if (msg.isNoteOff())
			{
				queueMidiEvent ({ PendingMidiEventType::noteOff, msg.getNoteNumber(), 0, 0 });
			}
		}
	}
	else if (! midiEnabled)
	{
		clearPendingMidiEvents();
		clearMidiTrackingState();
	}

	const int midiNote = lastMidiNote.load (std::memory_order_relaxed);
	const bool midiNoteActive = midiEnabled && (midiNote >= 0);
	const float midiFreq = currentMidiFrequency.load (std::memory_order_relaxed);
	if (midiNoteActive && midiFreq > 0.0f)
		targetFreq = midiFreq;

	// MOD frequency multiplier (hyperbolic below centre, linear above)
	const float modValue = loadAtomicOrDefault (modParam, kModDefault);
	const bool modHarm = loadBoolParamOrDefault (modHarmParam, false);
	const float freqMultiplier = modSliderToEffectiveMultiplier (modValue, modHarm);
	targetFreq *= freqMultiplier;
	{
		const float effectiveFreqMax = juce::jmax (kFreqMin, juce::jmin (kFreqEffectiveMax, 0.49f * (float) currentSampleRate));
		targetFreq = juce::jlimit (kFreqMin, effectiveFreqMax, targetFreq);
	}

	jassert ((int) sidechainFrequencyOffsetNorm_.size() >= numSamples);
	jassert (dryBuffer.getNumSamples() >= numSamples);

	float* sidechainOffsetNorm = sidechainFrequencyOffsetNorm_.data();
	const float* sidechainOffsetNormRead = nullptr;
	if (sidechainEnabled)
	{
		const float sidechainSmoothTarget = juce::jlimit (kSidechainSmoothMin, kSidechainSmoothMax,
			loadAtomicOrDefault (sidechainSmoothParam, kSidechainSmoothDefault));
		float sidechainSmoothEffective = sidechainSmoothTarget;
		if (sidechainSmoothTarget <= 0.25f)
		{
			const float t = sidechainSmoothTarget / 0.25f;
			sidechainSmoothEffective = 0.25f * (2.0f * t * t - t * t * t);
		}
		const bool sidechainDirectAtZeroSmooth = sidechainSmoothEffective <= 0.000001f;
		const float sidechainSmoothCurve = juce::jmin (1.0f, sidechainSmoothEffective * 2.0f);
		const float sidechainExtendedBlend = juce::jlimit (0.0f, 1.0f, (sidechainSmoothEffective - 0.5f) * 2.0f);
		const float sidechainGateSmoothShape = (sidechainSmoothEffective <= 0.5f)
			? sidechainSmoothCurve
			: (1.0f + 0.5f * sidechainExtendedBlend);
		const float sidechainGain = gainFaderDecibelsToGain (juce::jlimit (kSidechainGainMin, kSidechainGainMax,
			loadAtomicOrDefault (sidechainGainParam, kSidechainGainDefault)));
		const float sidechainPol = juce::jlimit (kSidechainPolMin, kSidechainPolMax,
			loadAtomicOrDefault (sidechainPolParam, kSidechainPolDefault));
		const bool sidechainHpOn = loadBoolParamOrDefault (sidechainHpOnParam, kSidechainHpOnDefault);
		const bool sidechainLpOn = loadBoolParamOrDefault (sidechainLpOnParam, kSidechainLpOnDefault);
		const float sidechainHpTarget = juce::jlimit (kSidechainFilterFreqMin, kSidechainFilterFreqMax,
			loadAtomicOrDefault (sidechainHpParam, kSidechainHpDefault));
		const float sidechainLpTarget = juce::jlimit (kSidechainFilterFreqMin, kSidechainFilterFreqMax,
			loadAtomicOrDefault (sidechainLpParam, kSidechainLpDefault));
		const int sidechainHpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
			loadIntParamOrDefault (sidechainHpSlopeParam, kSidechainHpSlopeDefault));
		const int sidechainLpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
			loadIntParamOrDefault (sidechainLpSlopeParam, kSidechainLpSlopeDefault));
		const int sidechainHpSections = sidechainHpOn ? (sidechainHpSlope == 2 ? 2 : 1) : 0;
		const int sidechainLpSections = sidechainLpOn ? (sidechainLpSlope == 2 ? 2 : 1) : 0;
		WetFilterBiquadCoeffs sidechainHpCoeffs[2];
		WetFilterBiquadCoeffs sidechainLpCoeffs[2];
		if (sidechainHpSections == 1)
			sidechainHpCoeffs[0] = (sidechainHpSlope == 0) ? calcOnePoleHP (sidechainHpTarget, (float) currentSampleRate)
				: calcBiquadHP (sidechainHpTarget, (float) currentSampleRate, kBW2_Q);
		else if (sidechainHpSections == 2)
		{
			sidechainHpCoeffs[0] = calcBiquadHP (sidechainHpTarget, (float) currentSampleRate, kBW4_Q1);
			sidechainHpCoeffs[1] = calcBiquadHP (sidechainHpTarget, (float) currentSampleRate, kBW4_Q2);
		}
		if (sidechainLpSections == 1)
			sidechainLpCoeffs[0] = (sidechainLpSlope == 0) ? calcOnePoleLP (sidechainLpTarget, (float) currentSampleRate)
				: calcBiquadLP (sidechainLpTarget, (float) currentSampleRate, kBW2_Q);
		else if (sidechainLpSections == 2)
		{
			sidechainLpCoeffs[0] = calcBiquadLP (sidechainLpTarget, (float) currentSampleRate, kBW4_Q1);
			sidechainLpCoeffs[1] = calcBiquadLP (sidechainLpTarget, (float) currentSampleRate, kBW4_Q2);
		}
		const float sidechainControlHz = sidechainLpOn ? sidechainLpTarget : kSidechainLpDefault;
		constexpr float kSidechainMinGateTau = 0.00025f;
		constexpr float kSidechainMaxGateTau = 0.040f;
		const float sidechainGateTau = kSidechainMinGateTau * sidechainGateSmoothShape
			+ sidechainGateSmoothShape * sidechainGateSmoothShape * kSidechainMaxGateTau;
		const float sidechainGateCoeff = sidechainDirectAtZeroSmooth
			? 0.0f
			: std::exp (-1.0f / ((float) currentSampleRate * sidechainGateTau));
		const float sidechainDcCoeff = std::exp (-juce::MathConstants<float>::twoPi * 20.0f / (float) currentSampleRate);
		const float sidechainSmoothCurveSq = sidechainSmoothCurve * sidechainSmoothCurve;
		const float sidechainMaxSmoothHz = juce::jmax (20.0f, (float) currentSampleRate * 0.45f);
		const float sidechainCarrierSmoothMul = (sidechainSmoothEffective <= 0.5f)
			? (sidechainMaxSmoothHz / juce::jmax (20.0f, sidechainControlHz)
				+ sidechainSmoothCurveSq * (0.25f - sidechainMaxSmoothHz / juce::jmax (20.0f, sidechainControlHz)))
			: juce::jmap (sidechainExtendedBlend, 0.25f, 0.10f);
		const float sidechainCarrierSmoothHz = juce::jlimit (20.0f, (float) currentSampleRate * 0.45f,
			sidechainControlHz * sidechainCarrierSmoothMul);
		const float sidechainCarrierSmoothCoeff = sidechainDirectAtZeroSmooth
			? 0.0f
			: std::exp (-juce::MathConstants<float>::twoPi * sidechainCarrierSmoothHz / (float) currentSampleRate);
		constexpr float kSidechainMinDepthTau = 0.0005f;
		constexpr float kSidechainMaxDepthTau = 0.040f;
		const float sidechainDepthTau = kSidechainMinDepthTau * sidechainGateSmoothShape
			+ sidechainGateSmoothShape * sidechainGateSmoothShape * kSidechainMaxDepthTau;
		const float sidechainDepthCoeff = sidechainDirectAtZeroSmooth
			? 0.0f
			: std::exp (-1.0f / ((float) currentSampleRate * sidechainDepthTau));

		auto processSidechainFilters = [&] (float x,
			WetFilterBiquadState (&hpState)[2],
			WetFilterBiquadState (&lpState)[2]) noexcept
		{
			float y = x * sidechainGain;
			for (int s = 0; s < sidechainHpSections; ++s)
				y = processBiquad (sidechainHpCoeffs[s], hpState[s], y);
			for (int s = 0; s < sidechainLpSections; ++s)
				y = processBiquad (sidechainLpCoeffs[s], lpState[s], y);
			return y;
		};
		for (int n = 0; n < numSamples; ++n)
		{
			const float sidechainGateTarget = sidechainCarrierDetected ? 1.0f : 0.0f;
			sidechainGateSmoothed_ = sidechainGateCoeff * sidechainGateSmoothed_
				+ (1.0f - sidechainGateCoeff) * sidechainGateTarget;

			const float sidechainRawL = sidechainBusAvailable ? sidechainReadL[n] : 0.0f;
			const float sidechainRawR = sidechainBusAvailable ? sidechainReadR[n] : sidechainRawL;
			const float sidechainDcL = sidechainRawL - sidechainDcPrevInL_ + sidechainDcCoeff * sidechainDcPrevOutL_;
			const float sidechainDcR = sidechainRawR - sidechainDcPrevInR_ + sidechainDcCoeff * sidechainDcPrevOutR_;
			sidechainDcPrevInL_ = sidechainRawL;
			sidechainDcPrevInR_ = sidechainRawR;
			sidechainDcPrevOutL_ = sidechainDcL;
			sidechainDcPrevOutR_ = sidechainDcR;

			const float sidechainFilteredL = processSidechainFilters (sidechainDcL, sidechainHpFilterL_, sidechainLpFilterL_);
			const float sidechainFilteredR = processSidechainFilters (sidechainDcR, sidechainHpFilterR_, sidechainLpFilterR_);
			sidechainCarrierSmoothL_ = sidechainCarrierSmoothCoeff * sidechainCarrierSmoothL_
				+ (1.0f - sidechainCarrierSmoothCoeff) * sidechainFilteredL;
			sidechainCarrierSmoothR_ = sidechainCarrierSmoothCoeff * sidechainCarrierSmoothR_
				+ (1.0f - sidechainCarrierSmoothCoeff) * sidechainFilteredR;

			const float sidechainCarrierEnergy = 0.5f
				* (sidechainCarrierSmoothL_ * sidechainCarrierSmoothL_
				   + sidechainCarrierSmoothR_ * sidechainCarrierSmoothR_);
			sidechainRmsEnv_ = sidechainDepthCoeff * sidechainRmsEnv_
				+ (1.0f - sidechainDepthCoeff) * sidechainCarrierEnergy;
			const float sidechainDepthTarget = juce::jlimit (0.0f, 1.0f,
				std::sqrt (juce::jmax (0.0f, sidechainRmsEnv_)) * 2.0f);
			sidechainDepthSmoothed_ = sidechainDepthCoeff * sidechainDepthSmoothed_
				+ (1.0f - sidechainDepthCoeff) * sidechainDepthTarget;
			sidechainOffsetNorm[n] = sidechainGateSmoothed_ * sidechainDepthSmoothed_ * sidechainPol;
		}
		sidechainOffsetNormRead = sidechainOffsetNorm;
	}
	else
	{
		resetSidechainRuntime();
		sidechainOffsetNormRead = nullptr;
	}

	// Smoothstep feedback mapping (sign-preserving bipolar)
	float rawFeedback = juce::jlimit (kFeedbackMin, kFeedbackMax, loadAtomicOrDefault (feedbackParam, kFeedbackDefault));
	const float sign = rawFeedback < 0.0f ? -1.0f : 1.0f;
	const float af   = std::abs (rawFeedback);
	const float targetFeedback = sign * af * af * (3.0f - 2.0f * af);

	stagesSmoothed.setTargetValue ((float) targetStages);
	shapeSmoothed.setTargetValue (targetShape);
	jitterSmoothed.setTargetValue (targetJitter);
	feedbackSmoothed.setTargetValue (targetFeedback);

	// MIX (dry/wet)
	const float mixValue = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (mixParam, kMixDefault));
	const int   mixMode  = loadIntParamOrDefault (mixModeParam, kMixModeDefault);
	const float dryLevelTarget = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (dryLevelParam, kDryLevelDefault));
	const float wetLevelTarget = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (wetLevelParam, kWetLevelDefault));
	const int limMode = loadIntParamOrDefault (limModeParam, kLimModeDefault);
	const float limThreshDbTarget = juce::jlimit (kLimThresholdMin, kLimThresholdMax,
		loadAtomicOrDefault (limThresholdParam, kLimThresholdDefault));
	const float limThreshLinTarget = fastDecibelsToGain (limThreshDbTarget);

	// Filter / Tilt position
	{
		const int fltPos = loadIntParamOrDefault (filterPosParam, kFilterPosDefault);
		// 0=F-post T-post  1=F-pre T-pre  2=F-pre T-post  3=F-post T-pre
		filterPre_ = (fltPos == 1 || fltPos == 2);
		tiltPre_   = (fltPos == 1 || fltPos == 3);
	}

	// TILT EQ parameter load
	tiltDb_ = loadAtomicOrDefault (tiltParam, kTiltDefault);

	// INPUT / OUTPUT gain (dB to linear, same as ECHO-TR)
	const float inputGainDb  = juce::jlimit (kInputMin,  kInputMax,  loadAtomicOrDefault (inputParam,  kInputDefault));
	const float outputGainDb = juce::jlimit (kOutputMin, kOutputMax, loadAtomicOrDefault (outputParam, kOutputDefault));
	const float inputGain    = gainFaderDecibelsToGain (inputGainDb);
	const float outputGain   = gainFaderDecibelsToGain (outputGainDb);
	float inputMeterPeak = 0.0f;

	// STYLE: 0=MONO, 1=STEREO, 2=WIDE, 3=DUAL
	const int style = juce::jlimit (kStyleMin, kStyleMax, loadIntParamOrDefault (styleParam, (int) kStyleDefault));

	// Sum Bus (needed early for needsDryBlend)
	const int sumBusVal  = juce::jlimit (0, 2, (int) sumBusParam->load());

	// Save dry input for dry/wet blend (only when mix < 1 or sum bus active or SEND mode)
	const bool needsDryBlend = (mixValue < 0.999f) || (sumBusVal != 0) || (mixMode == 1);
	if (needsDryBlend)
	{
		for (int ch = 0; ch < numChannels; ++ch)
			dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
	}

	// MIDI glide: velocity-dependent EMA coefficient
	if (midiNoteActive)
	{
		const float vel  = (float) lastMidiVelocity.load (std::memory_order_relaxed);
		const float tLin = juce::jlimit (0.0f, 1.0f, (vel - 1.0f) / 126.0f);
		const float t    = std::pow (tLin, 0.05f);
		const float tau  = kMidiGlideTauMax - t * (kMidiGlideTauMax - kMidiGlideTauMin);
		freqEmaCoeff = std::exp (-1.0f / ((float) currentSampleRate * tau));
	}
	else
	{
		freqEmaCoeff = freqEmaCoeffDefault_;
	}

	// Direct mode (per-sample all-pass)

	// Detect series change -> start crossfade
	if (targetSeries != activeSeries)
	{
		for (int s = 0; s < kSeriesMax; ++s)
		{
			xfadeChainL[(size_t) s] = chainL[(size_t) s];
			xfadeChainR[(size_t) s] = chainR[(size_t) s];
		}
		previousSeries = activeSeries;
		seriesXfadeTotalSamples = (int) std::round (currentSampleRate * kSeriesCrossfadeMs / 1000.0);
		seriesXfadeSamplesRemaining = seriesXfadeTotalSamples;
		DSP_LOG_CROSSFADE(dspLog, previousSeries, targetSeries, seriesXfadeTotalSamples);

		if (targetSeries > activeSeries)
			clearStageRange (0, kAmountMax, targetSeries);
		activeSeries = targetSeries;
		coeffUpdateCountdown = 0;
		lastJitterCoeffStages = -1;
	}

	auto* ch0 = buffer.getWritePointer (0);
	float* ch1 = (numChannels > 1) ? buffer.getWritePointer (1) : nullptr;
	const bool hasStereo = (ch1 != nullptr);

	// Mode In: 0=L+R, 1=M/S, 2=MID, 3=SIDE
	const int modeInVal  = juce::jlimit (0, 3, (int) modeInParam->load());
	const int modeOutVal = juce::jlimit (0, 3, (int) modeOutParam->load());

	if (modeInVal > 0 && hasStereo)
	{
		const auto modeIn = TR::DSP::channelModeFromInt (modeInVal);
		for (int n = 0; n < numSamples; ++n)
		{
			const auto routed = TR::DSP::applyModeIn ({ ch0[n], ch1[n] }, modeIn);
			ch0[n] = routed.l;
			ch1[n] = routed.r;
		}
	}

	const bool processR     = (style >= 1 && hasStereo);
	const bool crossFbk     = (style == 2);  // WIDE
	const bool negateCoeffR = (style == 2);  // WIDE: complementary phase
	const bool dualCoeffR   = (style == 3);  // DUAL: separate R coefficients
	const bool crossfading = (seriesXfadeSamplesRemaining > 0);
	float runtimeTargetFreq = targetFreq;
	float runtimeFreqEmaCoeff = freqEmaCoeff;

	auto refreshRuntimeMidiDerivedState = [&]()
	{
		const int runtimeMidiNote = lastMidiNote.load (std::memory_order_relaxed);
		const bool runtimeMidiNoteActive = midiEnabled && (runtimeMidiNote >= 0);
		runtimeTargetFreq = loadAtomicOrDefault (freqParam, kFreqDefault);
		if (runtimeMidiNoteActive)
		{
			const float runtimeMidiFreq = currentMidiFrequency.load (std::memory_order_relaxed);
			if (runtimeMidiFreq > 0.0f)
				runtimeTargetFreq = runtimeMidiFreq;
		}

		runtimeTargetFreq *= freqMultiplier;
		{
			const float effectiveFreqMax = juce::jmax (kFreqMin, juce::jmin (kFreqEffectiveMax, 0.49f * (float) currentSampleRate));
			runtimeTargetFreq = juce::jlimit (kFreqMin, effectiveFreqMax, runtimeTargetFreq);
		}

		if (runtimeMidiNoteActive)
		{
			const float vel  = (float) lastMidiVelocity.load (std::memory_order_relaxed);
			const float tLin = juce::jlimit (0.0f, 1.0f, (vel - 1.0f) / 126.0f);
			const float t    = std::pow (tLin, 0.05f);
			const float tau  = kMidiGlideTauMax - t * (kMidiGlideTauMax - kMidiGlideTauMin);
			runtimeFreqEmaCoeff = std::exp (-1.0f / ((float) currentSampleRate * tau));
		}
		else
		{
			runtimeFreqEmaCoeff = freqEmaCoeffDefault_;
		}
	};

	// Chaos per-block parameter read
	chaosFilterEnabled_ = loadBoolParamOrDefault (chaosParam, false);
	chaosDelayEnabled_  = loadBoolParamOrDefault (chaosDelayParam, false);
	const bool anyChaos = chaosFilterEnabled_ || chaosDelayEnabled_;
	if (anyChaos)
	{
		if (chaosDelayEnabled_)
		{
			const float rawAmtD = juce::jlimit (kChaosAmtMin, kChaosAmtMax,
				loadAtomicOrDefault (chaosAmtParam, kChaosAmtDefault));
			const float rawSpdD = juce::jlimit (kChaosSpdMin, kChaosSpdMax,
				loadAtomicOrDefault (chaosSpdParam, kChaosSpdDefault));
			chaosAmtD_       = rawAmtD;
			chaosAmtNormD_   = rawAmtD * 0.01f;
			chaosShPeriodD_  = (float) currentSampleRate / rawSpdD;
			chaosDelayMaxSamples_ = chaosAmtNormD_ * 0.005f * (float) currentSampleRate;
			chaosGainMaxDb_  = chaosAmtNormD_ * 1.0f;    // +/-1 dB at 100%
		}
		else
		{
			chaosDelayMaxSamples_ = 0.0f;
			smoothedChaosDelayMaxSamples_ = 0.0f;
			chaosGainMaxDb_ = 0.0f;
			chaosDelaySmoothedSamples_[0] = chaosDelaySmoothedSamples_[1] = 0.0f;
			chaosDelaySmoothReady_[0] = chaosDelaySmoothReady_[1] = false;
			chaosDriveAmtSmoothed_ = 0.0f;
			chaosDriveSpdSmoothed_ = kChaosSpdDefault;
			chaosDriveParamSmoothReady_ = false;
		}

		if (chaosFilterEnabled_)
		{
			const float rawAmtF = juce::jlimit (kChaosAmtMin, kChaosAmtMax,
				loadAtomicOrDefault (chaosAmtFilterParam, kChaosAmtDefault));
			const float rawSpdF = juce::jlimit (kChaosSpdMin, kChaosSpdMax,
				loadAtomicOrDefault (chaosSpdFilterParam, kChaosSpdDefault));
			chaosAmtF_       = rawAmtF;
			chaosShPeriodF_  = (float) currentSampleRate / rawSpdF;
			const float amtNormF = rawAmtF * 0.01f;
			chaosFilterMaxOct_ = amtNormF * 2.0f;  // +/-2 oct at 100%
		}
		else
		{
			chaosFilterMaxOct_ = 0.0f;
			chaosFilterAmtSmoothed_ = 0.0f;
			chaosFilterSpdSmoothed_ = kChaosSpdDefault;
			chaosFilterParamSmoothReady_ = false;
		}

		chaosParamSmoothCoeff_ = cachedChaosParamSmoothCoeff_;
	}
	else
	{
		chaosAmtD_ = 0.0f; chaosAmtF_ = 0.0f;
		chaosDelayMaxSamples_ = 0.0f;
		smoothedChaosDelayMaxSamples_ = 0.0f;
		chaosGainMaxDb_ = 0.0f;
		chaosFilterMaxOct_ = 0.0f;
		chaosDelaySmoothedSamples_[0] = chaosDelaySmoothedSamples_[1] = 0.0f;
		chaosDelaySmoothReady_[0] = chaosDelaySmoothReady_[1] = false;
		chaosDriveAmtSmoothed_ = 0.0f;
		chaosDriveSpdSmoothed_ = kChaosSpdDefault;
		chaosDriveParamSmoothReady_ = false;
		chaosFilterAmtSmoothed_ = 0.0f;
		chaosFilterSpdSmoothed_ = kChaosSpdDefault;
		chaosFilterParamSmoothReady_ = false;
	}

	chaosStereo_ = (style >= 1);

	// Wet-signal HP/LP filter (PRE position - only runs if filterPre_)
	if (filterPre_)
	{
		const bool hpOn = loadBoolParamOrDefault (filterHpOnParam, false);
		const bool lpOn = loadBoolParamOrDefault (filterLpOnParam, false);

		if (hpOn || lpOn)
		{
			const float targetHpFreq = juce::jlimit (kFilterFreqMin, kFilterFreqMax,
				loadAtomicOrDefault (filterHpFreqParam, kFilterHpFreqDefault));
			const float targetLpFreq = juce::jlimit (kFilterFreqMin, kFilterFreqMax,
				loadAtomicOrDefault (filterLpFreqParam, kFilterLpFreqDefault));
			const int hpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
				loadIntParamOrDefault (filterHpSlopeParam, kFilterSlopeDefault));
			const int lpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
				loadIntParamOrDefault (filterLpSlopeParam, kFilterSlopeDefault));

			const int numSections_hp = (hpSlope == 2) ? 2 : 1;
			const int numSections_lp = (lpSlope == 2) ? 2 : 1;

			for (int ch = 0; ch < numChannels; ++ch)
			{
				float* wet = buffer.getWritePointer (ch);
				auto& fs = wetFilterState_[ch < 2 ? ch : 0];

				for (int n = 0; n < numSamples; ++n)
				{
					if (ch == 0)
					{
						smoothedFilterHpFreq_ = smoothedFilterHpFreq_ * kGainSmoothCoeff
							+ targetHpFreq * (1.0f - kGainSmoothCoeff);
						smoothedFilterLpFreq_ = smoothedFilterLpFreq_ * kGainSmoothCoeff
							+ targetLpFreq * (1.0f - kGainSmoothCoeff);

						if (chaosFilterEnabled_) advanceChaosF();

						--filterCoeffCountdown_;
						if (filterCoeffCountdown_ <= 0)
						{
							filterCoeffCountdown_ = kFilterCoeffUpdateInterval;
							if (chaosFilterEnabled_
								&& (chaosAmtF_ > 0.01f || (chaosFilterParamSmoothReady_ && chaosFilterAmtSmoothed_ > 0.01f)))
							{
								const float sHp = smoothedFilterHpFreq_;
								const float sLp = smoothedFilterLpFreq_;
								const float hpBase = hpOn ? sHp : kFilterFreqMin;
								const float lpBase = lpOn ? sLp : kFilterFreqMax;

								// L channel coefficients
								const float octL = chaosFOut_[0] * smoothedChaosFilterMaxOct_;
								const float multL = std::exp2 (octL);
								smoothedFilterHpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBase * multL);
								smoothedFilterLpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBase * multL);
								updateFilterCoeffs (true, true);

								if (chaosStereo_)
								{
									auto hpL0 = hpCoeffs_[0]; auto hpL1 = hpCoeffs_[1];
									auto lpL0 = lpCoeffs_[0]; auto lpL1 = lpCoeffs_[1];

									const float octR = chaosFOut_[1] * smoothedChaosFilterMaxOct_;
									const float multR = std::exp2 (octR);
									smoothedFilterHpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBase * multR);
									smoothedFilterLpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBase * multR);
									updateFilterCoeffs (true, true);

									hpCoeffsR_[0] = hpCoeffs_[0]; hpCoeffsR_[1] = hpCoeffs_[1];
									lpCoeffsR_[0] = lpCoeffs_[0]; lpCoeffsR_[1] = lpCoeffs_[1];
									hpCoeffs_[0] = hpL0; hpCoeffs_[1] = hpL1;
									lpCoeffs_[0] = lpL0; lpCoeffs_[1] = lpL1;
								}
								else
								{
									hpCoeffsR_[0] = hpCoeffs_[0]; hpCoeffsR_[1] = hpCoeffs_[1];
									lpCoeffsR_[0] = lpCoeffs_[0]; lpCoeffsR_[1] = lpCoeffs_[1];
								}

								smoothedFilterHpFreq_ = sHp;
								smoothedFilterLpFreq_ = sLp;
							}
							else
							{
								updateFilterCoeffs (false, false);
								hpCoeffsR_[0] = hpCoeffs_[0]; hpCoeffsR_[1] = hpCoeffs_[1];
								lpCoeffsR_[0] = lpCoeffs_[0]; lpCoeffsR_[1] = lpCoeffs_[1];
							}
						}
					}

					float x = wet[n];
					const auto& hpC = (ch == 0) ? hpCoeffs_ : hpCoeffsR_;
					const auto& lpC = (ch == 0) ? lpCoeffs_ : lpCoeffsR_;

					if (hpOn)
						for (int s = 0; s < numSections_hp; ++s)
							x = processBiquad (hpC[s], fs.hp[s], x);

					if (lpOn)
						for (int s = 0; s < numSections_lp; ++s)
							x = processBiquad (lpC[s], fs.lp[s], x);

					wet[n] = x;
				}
			}
		}
		else if (chaosFilterEnabled_)
		{
			for (int n = 0; n < numSamples; ++n)
			{
				advanceChaosF();
			}
		}
	}

	// TILT filter lambda (1-pole shelving, pivot 1 kHz)
	auto applyTilt = [&]()
	{
		if (std::abs (tiltDb_) > 0.05f)
		{
			if (std::abs (tiltDb_ - lastTiltDb_) > 0.02f)
			{
				lastTiltDb_ = tiltDb_;
				const double pivot = 1000.0;
				const double octToNy = std::log2 ((currentSampleRate * 0.5) / pivot);
				const double gainNyDb = static_cast<double> (tiltDb_) * octToNy;
				const double gNy = std::pow (10.0, gainNyDb / 20.0);
				const double wc = 2.0 * currentSampleRate
				                * std::tan (juce::MathConstants<double>::pi * pivot / currentSampleRate);
				const double K = wc / (2.0 * currentSampleRate);
				const double g = std::sqrt (gNy);
				const double norm = 1.0 / (1.0 + K * g);
				tiltTargetB0_ = static_cast<float> ((g + K) * norm);
				tiltTargetB1_ = static_cast<float> ((K - g) * norm);
				tiltTargetA1_ = static_cast<float> ((K * g - 1.0) * norm);
			}

			const float sc = tiltSmoothSc_;
			const int tiltChannels = juce::jmin (numChannels, 2);
			float* leftData = tiltChannels > 0 ? buffer.getWritePointer (0) : nullptr;
			float* rightData = tiltChannels > 1 ? buffer.getWritePointer (1) : nullptr;
			float leftState = tiltState_[0];
			float rightState = tiltState_[1];

			if (rightData != nullptr)
			{
				for (int n = 0; n < numSamples; ++n)
				{
					tiltB0_ += (tiltTargetB0_ - tiltB0_) * sc;
					tiltB1_ += (tiltTargetB1_ - tiltB1_) * sc;
					tiltA1_ += (tiltTargetA1_ - tiltA1_) * sc;

					const float leftX = leftData[n];
					const float leftY = tiltB0_ * leftX + leftState;
					leftState = tiltB1_ * leftX - tiltA1_ * leftY;
					leftData[n] = leftY;

					const float rightX = rightData[n];
					const float rightY = tiltB0_ * rightX + rightState;
					rightState = tiltB1_ * rightX - tiltA1_ * rightY;
					rightData[n] = rightY;
				}
			}
			else if (leftData != nullptr)
			{
				for (int n = 0; n < numSamples; ++n)
				{
					tiltB0_ += (tiltTargetB0_ - tiltB0_) * sc;
					tiltB1_ += (tiltTargetB1_ - tiltB1_) * sc;
					tiltA1_ += (tiltTargetA1_ - tiltA1_) * sc;

					const float x = leftData[n];
					const float y = tiltB0_ * x + leftState;
					leftState = tiltB1_ * x - tiltA1_ * y;
					leftData[n] = y;
				}
			}

			tiltState_[0] = leftState;
			tiltState_[1] = rightState;
		}
		else if (std::abs (lastTiltDb_) > 0.05f)
		{
			lastTiltDb_ = 0.0f;
			tiltB0_ = 1.0f; tiltB1_ = 0.0f; tiltA1_ = 0.0f;
			tiltTargetB0_ = 1.0f; tiltTargetB1_ = 0.0f; tiltTargetA1_ = 0.0f;
			tiltState_[0] = tiltState_[1] = 0.0f;
		}
	};

	if (tiltPre_) applyTilt();

	const bool freqConverged = std::abs (smoothedFreqValue - targetFreq) < 0.01f;
	const bool jitterActive = targetJitter > 0.0001f
	                       || jitterSmoothed.isSmoothing()
	                       || jitterSmoothed.getCurrentValue() > 0.0001f;
	if (! jitterActive)
	{
		lastJitterCoeffStages = -1;
		jitterCoeffSmoothingReady_ = false;
	}

	// Fast path: parameters converged + no crossfade -> tight inner loop
	// without per-sample smoothing, coefficient checks, or fractional stages.
	if (!crossfading
		&& !stagesSmoothed.isSmoothing()
		&& freqConverged
		&& !shapeSmoothed.isSmoothing()
		&& !jitterActive
		&& !feedbackSmoothed.isSmoothing()
		&& sidechainOffsetNormRead == nullptr
		&& pendingMidiEventCount_ == 0)
	{
		smoothedFreqValue = targetFreq;   // snap EMA to avoid drift
		const int stgs = activeStages;
		const float fb = feedbackSmoothed.getCurrentValue();

		// DUAL: update R coefficients for fast path
		if (dualCoeffR && stgs > 0)
		{
			const float freqR = targetFreq * 0.5f;
			if (std::abs (freqR - lastCoeffFreqR) > 0.001f || lastCoeffStages != stgs)
			{
				updateCoefficientsInto (freqR, targetShape, stgs, stageCoeffR);
				lastCoeffFreqR = freqR;
			}
		}

		if (stgs > 0)
		{
			for (int n = 0; n < numSamples; ++n)
			{
				float sourceL = ch0[n];
				float sourceR = hasStereo ? ch1[n] : sourceL;
				if (chaosDelayEnabled_)
				{
					advanceChaosD();
					applyChaosDelay (sourceL, sourceR);
				}

				// Feedback routing: cross for WIDE, independent otherwise
				float xL = sourceL + fb * (crossFbk ? feedbackLastR : feedbackLastL);
				float xR = processR ? (sourceR + fb * (crossFbk ? feedbackLastL : feedbackLastR)) : xL;

				for (int s = 0; s < activeSeries; ++s)
				{
					auto* lS = chainL[(size_t) s].data();
					auto* rS = chainR[(size_t) s].data();

					for (int st = 0; st < stgs; ++st)
					{
						const float aRaw = stageCoeff[(size_t) st];
						const float a = (altEnabled && (st & 1)) ? -aRaw : aRaw;

						auto& sl = lS[st];
						const float yL = (-a * xL) + sl.z1;
						sl.z1 = xL + (a * yL);
						xL = yL;

						if (processR)
						{
							// WIDE: -a (complementary phase), DUAL: separate coeffs, STEREO: same a
							const float aR = negateCoeffR ? -a : (dualCoeffR ? ((altEnabled && (st & 1)) ? -stageCoeffR[(size_t) st] : stageCoeffR[(size_t) st]) : a);
							auto& sr = rS[st];
							const float yR = (-aR * xR) + sr.z1;
							sr.z1 = xR + (aR * yR);
							xR = yR;
						}
					}
				}

				ch0[n] = xL;
				feedbackLastL = sanitiseFeedbackWrite (dcBlockTick (xL, fbkDcStateInL, fbkDcStateOutL, fbkDcCoeff));
				if (hasStereo)
				{
					ch1[n] = processR ? xR : xL;
					const float fbWriteR = processR ? xR : xL;
					feedbackLastR = sanitiseFeedbackWrite (dcBlockTick (fbWriteR, fbkDcStateInR, fbkDcStateOutR, fbkDcCoeff));
				}
			}
		}
		else
		{
			feedbackLastL = 0.0f;
			feedbackLastR = 0.0f;
			fbkDcStateInL = fbkDcStateInR = 0.0f;
			fbkDcStateOutL = fbkDcStateOutR = 0.0f;

			if (chaosDelayEnabled_)
			{
				for (int n = 0; n < numSamples; ++n)
				{
					float sourceL = ch0[n];
					float sourceR = hasStereo ? ch1[n] : sourceL;
					advanceChaosD();
					applyChaosDelay (sourceL, sourceR);
					ch0[n] = sourceL;
					if (hasStereo)
						ch1[n] = sourceR;
				}
			}
		}

	}
	else
	{
	// Slow path: smoothing active or crossfade in progress
	for (int n = 0; n < numSamples; ++n)
	{
		if (pendingMidiEventCount_ > 0)
		{
			bool appliedMidiEvent = false;
			int writeIndex = 0;
			for (int eventIndex = 0; eventIndex < pendingMidiEventCount_; ++eventIndex)
			{
				const auto event = pendingMidiEvents_[(size_t) eventIndex];
				if (event.samplesRemaining == n)
				{
					applyPendingMidiEvent (event);
					appliedMidiEvent = true;
				}
				else
				{
					pendingMidiEvents_[(size_t) writeIndex++] = event;
				}
			}
			pendingMidiEventCount_ = writeIndex;

			if (appliedMidiEvent)
				refreshRuntimeMidiDerivedState();
		}

		const float smoothedStages = juce::jlimit (0.0f, (float) kAmountMax, stagesSmoothed.getNextValue());
		smoothedFreqValue += (runtimeTargetFreq - smoothedFreqValue) * (1.0f - runtimeFreqEmaCoeff);
		float effectiveFreq = smoothedFreqValue;
		if (sidechainOffsetNormRead != nullptr)
		{
			const float effectiveFreqMax = juce::jmax (kFreqMin, juce::jmin (kFreqEffectiveMax, 0.49f * (float) currentSampleRate));
			effectiveFreq = juce::jlimit (kFreqMin, effectiveFreqMax,
				effectiveFreq + sidechainOffsetNormRead[n] * kSidechainFrequencyOffsetMaxHz);
		}
		float effectiveShape = shapeSmoothed.getNextValue();
		float feedbackJitterOut = 0.0f;
		float feedbackJitterDepth = 0.0f;
		const float baseFb = feedbackSmoothed.getNextValue();
		const int jitterCoeffSeriesCount = jitterActive
			? juce::jlimit (0, kSeriesMax, juce::jmax (activeSeries, crossfading ? previousSeries : activeSeries))
			: 0;
		if (jitterActive)
		{
			const float jitterAmt = jitterSmoothed.getNextValue();
			const float equivalentDelaySamples = calcJitterEquivalentDelaySamples (
				effectiveFreq, smoothedStages, juce::jmax (1, jitterCoeffSeriesCount));
			const float absFeedbackForJitter = juce::jlimit (0.0f, 1.0f, std::abs (baseFb));
			const float fbEnergy = 0.9f * absFeedbackForJitter + 0.1f * smoothStep01 (absFeedbackForJitter);
			const float jitterCoeffTau = kJitterCoeffSmoothMaxSeconds
				+ (kJitterCoeffSmoothMinSeconds - kJitterCoeffSmoothMaxSeconds) * fbEnergy;
			const float jitterCoeffAlpha = (jitterCoeffTau <= 0.000001f)
				? 1.0f
				: (1.0f - std::exp (-1.0f / (juce::jmax (1.0f, (float) currentSampleRate) * jitterCoeffTau)));
			const float jitterFreqMax = juce::jmax (kFreqMin, juce::jmin (kFreqEffectiveMax, 0.49f * (float) currentSampleRate));

			for (int s = 0; s < jitterCoeffSeriesCount; ++s)
			{
				const size_t idx = (size_t) s;
				float freqOctOffset = 0.0f;
				float shapeOffset = 0.0f;
				advanceJitterLane (jitterLanes_[idx], jitterAmt, equivalentDelaySamples,
				                   s, freqOctOffset, shapeOffset);
				const float targetJitterFreq = juce::jlimit (kFreqMin, jitterFreqMax, effectiveFreq * std::exp2 (freqOctOffset));
				const float targetJitterShape = juce::jlimit (0.0f, 1.0f, effectiveShape + shapeOffset);

				if (! jitterCoeffSmoothingReady_)
				{
					smoothedJitterSeriesFreq[idx] = targetJitterFreq;
					smoothedJitterSeriesShape[idx] = targetJitterShape;
				}
				else
				{
					smoothedJitterSeriesFreq[idx] += (targetJitterFreq - smoothedJitterSeriesFreq[idx]) * jitterCoeffAlpha;
					smoothedJitterSeriesShape[idx] += (targetJitterShape - smoothedJitterSeriesShape[idx]) * jitterCoeffAlpha;
				}

				jitterSeriesFreq[idx] = juce::jlimit (kFreqMin, jitterFreqMax, smoothedJitterSeriesFreq[idx]);
				jitterSeriesShape[idx] = juce::jlimit (0.0f, 1.0f, smoothedJitterSeriesShape[idx]);
			}
			jitterCoeffSmoothingReady_ = true;

			advanceJitterFeedback (jitterAmt, equivalentDelaySamples, feedbackJitterOut, feedbackJitterDepth);
		}
		const float fb = jitterActive ? applyJitterToFeedback (baseFb, feedbackJitterOut, feedbackJitterDepth) : baseFb;
		float sourceL = ch0[n];
		float sourceR = hasStereo ? ch1[n] : sourceL;

		if (chaosDelayEnabled_)
		{
			advanceChaosD();
			applyChaosDelay (sourceL, sourceR);
		}

		const int baseStages = juce::jlimit (0, kAmountMax, (int) std::floor (smoothedStages));
		const float stageFrac = juce::jlimit (0.0f, 1.0f, smoothedStages - (float) baseStages);
		const bool useFractionalStage = (stageFrac > 0.0001f && baseStages < kAmountMax);
		const int coeffStages = juce::jlimit (0, kAmountMax, baseStages + (useFractionalStage ? 1 : 0));

		if (coeffStages > activeStages)
			clearStageRange (activeStages, coeffStages, activeSeries);
		activeStages = coeffStages;

		if (coeffStages > 0)
		{
			// Batched coefficient update (every kCoeffUpdateInterval samples or on stage change)
			--coeffUpdateCountdown;
			if (coeffUpdateCountdown <= 0
				|| lastCoeffStages != coeffStages
				|| (jitterActive && lastJitterCoeffStages != coeffStages)
				|| (jitterActive && jitterCoeffSeriesCount > 0 && lastJitterCoeffFreq[(size_t) (jitterCoeffSeriesCount - 1)] < 0.0f))
			{
				coeffUpdateCountdown = kCoeffUpdateInterval;
				const bool baseStageChanged = (lastCoeffStages != coeffStages);
				if (baseStageChanged
					|| std::abs (effectiveFreq - lastCoeffFreq) > 0.001f
					|| std::abs (effectiveShape - lastCoeffShape) > 0.0002f)
				{
					updateCoefficients (effectiveFreq, effectiveShape, coeffStages);
					lastCoeffStages = coeffStages;
					lastCoeffFreq = effectiveFreq;
					lastCoeffShape = effectiveShape;
				}

				// DUAL: update R coefficients in slow path
				if (dualCoeffR)
				{
					const float freqR = effectiveFreq * 0.5f;
					if (baseStageChanged || std::abs (freqR - lastCoeffFreqR) > 0.001f)
					{
						updateCoefficientsInto (freqR, effectiveShape, coeffStages, stageCoeffR);
						lastCoeffFreqR = freqR;
					}
				}

				if (jitterActive)
				{
					const bool jitterStageChanged = (lastJitterCoeffStages != coeffStages);
					for (int s = 0; s < jitterCoeffSeriesCount; ++s)
					{
						const size_t idx = (size_t) s;
						if (jitterStageChanged
							|| std::abs (jitterSeriesFreq[idx] - lastJitterCoeffFreq[idx]) > 0.001f
							|| std::abs (jitterSeriesShape[idx] - lastJitterCoeffShape[idx]) > 0.0002f)
						{
							updateCoefficientsInto (jitterSeriesFreq[idx], jitterSeriesShape[idx], coeffStages, jitterStageCoeff[idx]);
							lastJitterCoeffFreq[idx] = jitterSeriesFreq[idx];
							lastJitterCoeffShape[idx] = jitterSeriesShape[idx];
						}

						if (dualCoeffR)
						{
							const float freqR = jitterSeriesFreq[idx] * 0.5f;
							if (jitterStageChanged || std::abs (freqR - lastJitterCoeffFreqR[idx]) > 0.001f)
							{
								updateCoefficientsInto (freqR, jitterSeriesShape[idx], coeffStages, jitterStageCoeffR[idx]);
								lastJitterCoeffFreqR[idx] = freqR;
							}
						}
					}
					lastJitterCoeffStages = coeffStages;
				}
			}

			const float inputL = sourceL + fb * (crossFbk ? feedbackLastR : feedbackLastL);
			const float inputR = processR ? (sourceR + fb * (crossFbk ? feedbackLastL : feedbackLastR)) : inputL;

			// Process through current (new) topology
			float xL = inputL;
			float xR = inputR;

			for (int s = 0; s < activeSeries; ++s)
			{
				const size_t seriesIdx = (size_t) s;
				const auto& coeffs = jitterActive ? jitterStageCoeff[seriesIdx] : stageCoeff;
				const auto& coeffsR = jitterActive ? jitterStageCoeffR[seriesIdx] : stageCoeffR;
				auto* lStages = chainL[seriesIdx].data();
				auto* rStages = chainR[seriesIdx].data();

				for (int st = 0; st < baseStages; ++st)
				{
					const float aRaw = coeffs[(size_t) st];
					const float a = (altEnabled && (st & 1)) ? -aRaw : aRaw;

					auto& sl = lStages[st];
					const float yL = (-a * xL) + sl.z1;
					sl.z1 = xL + (a * yL);
					xL = yL;

					if (processR)
					{
						const float aR = negateCoeffR ? -a : (dualCoeffR ? ((altEnabled && (st & 1)) ? -coeffsR[(size_t) st] : coeffsR[(size_t) st]) : a);
						auto& sr = rStages[st];
						const float yR = (-aR * xR) + sr.z1;
						sr.z1 = xR + (aR * yR);
						xR = yR;
					}
				}

				if (useFractionalStage)
				{
					const int st = baseStages;
					const float aRaw = coeffs[(size_t) st];
					const float a = (altEnabled && (st & 1)) ? -aRaw : aRaw;

					const float inL = xL;
					auto& sl = lStages[st];
					const float yL = (-a * inL) + sl.z1;
					sl.z1 = inL + (a * yL);
					xL = inL + (stageFrac * (yL - inL));

					if (processR)
					{
						const float aR = negateCoeffR ? -a : (dualCoeffR ? ((altEnabled && (st & 1)) ? -coeffsR[(size_t) st] : coeffsR[(size_t) st]) : a);
						const float inR = xR;
						auto& sr = rStages[st];
						const float yR = (-aR * inR) + sr.z1;
						sr.z1 = inR + (aR * yR);
						xR = inR + (stageFrac * (yR - inR));
					}
				}
			}

			// Series crossfade: blend old topology output during transition
			if (crossfading && seriesXfadeSamplesRemaining > 0)
			{
				float xfL = inputL;
				float xfR = inputR;

				for (int s = 0; s < previousSeries; ++s)
				{
					const size_t seriesIdx = (size_t) s;
					const auto& coeffs = jitterActive ? jitterStageCoeff[seriesIdx] : stageCoeff;
					const auto& coeffsR = jitterActive ? jitterStageCoeffR[seriesIdx] : stageCoeffR;
					auto* lStages = xfadeChainL[seriesIdx].data();
					auto* rStages = xfadeChainR[seriesIdx].data();

					for (int st = 0; st < baseStages; ++st)
					{
						const float aRaw = coeffs[(size_t) st];
						const float a = (altEnabled && (st & 1)) ? -aRaw : aRaw;

						auto& sl = lStages[st];
						const float yL = (-a * xfL) + sl.z1;
						sl.z1 = xfL + (a * yL);
						xfL = yL;

						if (processR)
						{
							const float aR = negateCoeffR ? -a : (dualCoeffR ? ((altEnabled && (st & 1)) ? -coeffsR[(size_t) st] : coeffsR[(size_t) st]) : a);
							auto& sr = rStages[st];
							const float yR = (-aR * xfR) + sr.z1;
							sr.z1 = xfR + (aR * yR);
							xfR = yR;
						}
					}

					if (useFractionalStage)
					{
						const int st = baseStages;
						const float aRaw = coeffs[(size_t) st];
						const float a = (altEnabled && (st & 1)) ? -aRaw : aRaw;

						const float inL = xfL;
						auto& sl = lStages[st];
						const float yL = (-a * inL) + sl.z1;
						sl.z1 = inL + (a * yL);
						xfL = inL + (stageFrac * (yL - inL));

						if (processR)
						{
							const float aR = negateCoeffR ? -a : (dualCoeffR ? ((altEnabled && (st & 1)) ? -coeffsR[(size_t) st] : coeffsR[(size_t) st]) : a);
							const float inR = xfR;
							auto& sr = rStages[st];
							const float yR = (-aR * inR) + sr.z1;
							sr.z1 = inR + (aR * yR);
							xfR = inR + (stageFrac * (yR - inR));
						}
					}
				}

				const float alpha = (float) seriesXfadeSamplesRemaining / (float) seriesXfadeTotalSamples;
				xL += alpha * (xfL - xL);
				xR += alpha * (xfR - xR);
				--seriesXfadeSamplesRemaining;
			}

			ch0[n] = xL;
			feedbackLastL = sanitiseFeedbackWrite (dcBlockTick (xL, fbkDcStateInL, fbkDcStateOutL, fbkDcCoeff));
			if (hasStereo)
			{
				ch1[n] = processR ? xR : xL;
				const float fbWriteR = processR ? xR : xL;
				feedbackLastR = sanitiseFeedbackWrite (dcBlockTick (fbWriteR, fbkDcStateInR, fbkDcStateOutR, fbkDcCoeff));
			}
		}
		else
		{
			feedbackLastL = 0.0f;
			feedbackLastR = 0.0f;
			fbkDcStateInL = fbkDcStateInR = 0.0f;
			fbkDcStateOutL = fbkDcStateOutR = 0.0f;

			if (chaosDelayEnabled_)
			{
				ch0[n] = sourceL;
				if (hasStereo)
					ch1[n] = sourceR;
			}
		}
	}
	} // end else (slow path)

	// (ALT coefficient alternation is applied inside the allpass loops)

	// Wet-signal HP/LP filter (POST position - only runs if !filterPre_)
	if (! filterPre_)
	{
		const bool hpOn = loadBoolParamOrDefault (filterHpOnParam, false);
		const bool lpOn = loadBoolParamOrDefault (filterLpOnParam, false);

		if (hpOn || lpOn)
		{
			const float targetHpFreq = juce::jlimit (kFilterFreqMin, kFilterFreqMax,
				loadAtomicOrDefault (filterHpFreqParam, kFilterHpFreqDefault));
			const float targetLpFreq = juce::jlimit (kFilterFreqMin, kFilterFreqMax,
				loadAtomicOrDefault (filterLpFreqParam, kFilterLpFreqDefault));
			const int hpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
				loadIntParamOrDefault (filterHpSlopeParam, kFilterSlopeDefault));
			const int lpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
				loadIntParamOrDefault (filterLpSlopeParam, kFilterSlopeDefault));

			const int numSections_hp = (hpSlope == 2) ? 2 : 1;
			const int numSections_lp = (lpSlope == 2) ? 2 : 1;

			for (int ch = 0; ch < numChannels; ++ch)
			{
				float* wet = buffer.getWritePointer (ch);
				auto& fs = wetFilterState_[ch < 2 ? ch : 0];

				for (int n = 0; n < numSamples; ++n)
				{
					// Per-sample EMA smoothing on filter frequencies (only on channel 0)
					if (ch == 0)
					{
						smoothedFilterHpFreq_ = smoothedFilterHpFreq_ * kGainSmoothCoeff
							+ targetHpFreq * (1.0f - kGainSmoothCoeff);
						smoothedFilterLpFreq_ = smoothedFilterLpFreq_ * kGainSmoothCoeff
							+ targetLpFreq * (1.0f - kGainSmoothCoeff);

						if (chaosFilterEnabled_) advanceChaosF();

						--filterCoeffCountdown_;
						if (filterCoeffCountdown_ <= 0)
						{
							filterCoeffCountdown_ = kFilterCoeffUpdateInterval;
							if (chaosFilterEnabled_
								&& (chaosAmtF_ > 0.01f || (chaosFilterParamSmoothReady_ && chaosFilterAmtSmoothed_ > 0.01f)))
							{
								const float sHp = smoothedFilterHpFreq_;
								const float sLp = smoothedFilterLpFreq_;
								const float hpBase = hpOn ? sHp : kFilterFreqMin;
								const float lpBase = lpOn ? sLp : kFilterFreqMax;

								// L channel coefficients
								const float octL = chaosFOut_[0] * smoothedChaosFilterMaxOct_;
								const float multL = std::exp2 (octL);
								smoothedFilterHpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBase * multL);
								smoothedFilterLpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBase * multL);
								updateFilterCoeffs (true, true);

								if (chaosStereo_)
								{
									auto hpL0 = hpCoeffs_[0]; auto hpL1 = hpCoeffs_[1];
									auto lpL0 = lpCoeffs_[0]; auto lpL1 = lpCoeffs_[1];

									const float octR = chaosFOut_[1] * smoothedChaosFilterMaxOct_;
									const float multR = std::exp2 (octR);
									smoothedFilterHpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBase * multR);
									smoothedFilterLpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBase * multR);
									updateFilterCoeffs (true, true);

									hpCoeffsR_[0] = hpCoeffs_[0]; hpCoeffsR_[1] = hpCoeffs_[1];
									lpCoeffsR_[0] = lpCoeffs_[0]; lpCoeffsR_[1] = lpCoeffs_[1];
									hpCoeffs_[0] = hpL0; hpCoeffs_[1] = hpL1;
									lpCoeffs_[0] = lpL0; lpCoeffs_[1] = lpL1;
								}
								else
								{
									hpCoeffsR_[0] = hpCoeffs_[0]; hpCoeffsR_[1] = hpCoeffs_[1];
									lpCoeffsR_[0] = lpCoeffs_[0]; lpCoeffsR_[1] = lpCoeffs_[1];
								}

								smoothedFilterHpFreq_ = sHp;
								smoothedFilterLpFreq_ = sLp;
							}
							else
							{
								updateFilterCoeffs (false, false);
								hpCoeffsR_[0] = hpCoeffs_[0]; hpCoeffsR_[1] = hpCoeffs_[1];
								lpCoeffsR_[0] = lpCoeffs_[0]; lpCoeffsR_[1] = lpCoeffs_[1];
							}
						}
					}

					float x = wet[n];
					const auto& hpC = (ch == 0) ? hpCoeffs_ : hpCoeffsR_;
					const auto& lpC = (ch == 0) ? lpCoeffs_ : lpCoeffsR_;

					if (hpOn)
						for (int s = 0; s < numSections_hp; ++s)
							x = processBiquad (hpC[s], fs.hp[s], x);

					if (lpOn)
						for (int s = 0; s < numSections_lp; ++s)
							x = processBiquad (lpC[s], fs.lp[s], x);

					wet[n] = x;
				}
			}
		}
		else if (chaosFilterEnabled_)
		{
			// Filters off but chaos F enabled: advance S&H to keep phase continuous
			for (int n = 0; n < numSamples; ++n)
			{
				advanceChaosF();
			}
		}
	}

	// TILT filter (POST position)
	if (!tiltPre_) applyTilt();

	// Mode Out: 0=L+R, 1=M/S decode, 2=MID, 3=SIDE
	if (modeOutVal > 0 && numChannels >= 2)
	{
		const auto modeOut = TR::DSP::channelModeFromInt (modeOutVal);
		float* wL = buffer.getWritePointer (0);
		float* wR = buffer.getWritePointer (1);
		for (int n = 0; n < numSamples; ++n)
		{
			const auto routed = TR::DSP::applyModeOut ({ wL[n], wR[n] }, modeOut);
			wL[n] = routed.l;
			wR[n] = routed.r;
		}
	}

	if (limMode == 0)
		smoothedLimThreshold = limThreshLinTarget;

	// Invert Polarity / Stereo (WET path: before wet gain/limiter and mix)
	{
		const int invPol = loadIntParamOrDefault (invPolParam, kInvPolDefault);
		const int invStr = loadIntParamOrDefault (invStrParam, kInvStrDefault);
		if (invPol == 1)
			for (int ch = 0; ch < numChannels; ++ch)
				juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), -1.0f, numSamples);
		if (invStr == 1 && numChannels >= 2)
		{
			float* sL = buffer.getWritePointer (0);
			float* sR = buffer.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
				std::swap (sL[n], sR[n]);
		}
	}

	// Apply wet Input/Output gain before WET limiting and dry/wet blending.
	{
		auto* const* wetChannels = buffer.getArrayOfWritePointers();
		for (int n = 0; n < numSamples; ++n)
		{
			smoothedInputGain  = smoothedInputGain  * kGainSmoothCoeff + inputGain  * (1.0f - kGainSmoothCoeff);
			smoothedOutputGain = smoothedOutputGain * kGainSmoothCoeff + outputGain * (1.0f - kGainSmoothCoeff);
			for (int ch = 0; ch < numChannels; ++ch)
			{
				const float inputApplied = wetChannels[ch][n] * smoothedInputGain;
				TR::DSP::observePeak (inputMeterPeak, inputApplied);
				wetChannels[ch][n] = inputApplied * smoothedOutputGain;
			}
		}
	}

	// Limiter (WET mode: after wet gain staging, before dry/wet mix)
	if (limMode == 1)
	{
		auto* chL = buffer.getWritePointer (0);
		if (numChannels >= 2)
		{
			auto* chR = buffer.getWritePointer (1);
			for (int i = 0; i < numSamples; ++i)
			{
				smoothedLimThreshold = smoothedLimThreshold * kGainSmoothCoeff
					+ limThreshLinTarget * (1.0f - kGainSmoothCoeff);
				applyLimiter (chL[i], chR[i], smoothedLimThreshold);
			}
		}
		else
		{
			for (int i = 0; i < numSamples; ++i)
			{
				smoothedLimThreshold = smoothedLimThreshold * kGainSmoothCoeff
					+ limThreshLinTarget * (1.0f - kGainSmoothCoeff);
				applyLimiterMono (chL[i], smoothedLimThreshold);
			}
		}
	}

	// Per-sample smoothed Mix + Dry/Wet blend.
	if (needsDryBlend)
	{
		if (sumBusVal == 0 || numChannels < 2)
		{
			// ST (stereo passthrough)
			const int blendChannels = juce::jmin (numChannels, dryBuffer.getNumChannels());
			auto* const* dryChannels = dryBuffer.getArrayOfReadPointers();
			auto* const* wetChannels = buffer.getArrayOfWritePointers();

			for (int n = 0; n < numSamples; ++n)
			{
				smoothedMix        = smoothedMix        * kGainSmoothCoeff + mixValue   * (1.0f - kGainSmoothCoeff);
				smoothedDryLevel   = smoothedDryLevel   * kGainSmoothCoeff + dryLevelTarget * (1.0f - kGainSmoothCoeff);
				smoothedWetLevel   = smoothedWetLevel   * kGainSmoothCoeff + wetLevelTarget * (1.0f - kGainSmoothCoeff);

				for (int ch = 0; ch < blendChannels; ++ch)
				{
					const float dryS = dryChannels[ch][n];
					const float wetS = wetChannels[ch][n];
					if (mixMode == 0)
						wetChannels[ch][n] = dryS + smoothedMix * (wetS - dryS);
					else
						wetChannels[ch][n] = dryS * smoothedDryLevel + wetS * smoothedWetLevel;
				}
			}
		}
		else
		{
			// ->M or ->S bus: dry preserves stereo image, only wet goes through bus
			const float* dryL = dryBuffer.getReadPointer (0);
			const float* dryR = dryBuffer.getReadPointer (1);
			float* outL = buffer.getWritePointer (0);
			float* outR = buffer.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
			{
				smoothedMix        = smoothedMix        * kGainSmoothCoeff + mixValue   * (1.0f - kGainSmoothCoeff);
				smoothedDryLevel   = smoothedDryLevel   * kGainSmoothCoeff + dryLevelTarget * (1.0f - kGainSmoothCoeff);
				smoothedWetLevel   = smoothedWetLevel   * kGainSmoothCoeff + wetLevelTarget * (1.0f - kGainSmoothCoeff);

				const auto mixed = TR::DSP::mixDryWet ({ dryL[n], dryR[n] }, { outL[n], outR[n] },
					{ TR::DSP::mixModeFromInt (mixMode),
					  TR::DSP::sumBusFromInt (sumBusVal),
					  smoothedMix,
					  smoothedDryLevel,
					  smoothedWetLevel });
				outL[n] = mixed.l;
				outR[n] = mixed.r;
			}
		}
	}
	else
	{
		// Full wet already has input/output gain and optional WET limiting applied.
	}
	{
		constexpr float kSnapEpsilon = 1e-5f;
		if (std::abs (smoothedInputGain  - inputGain)  < kSnapEpsilon) smoothedInputGain  = inputGain;
		if (std::abs (smoothedOutputGain - outputGain) < kSnapEpsilon) smoothedOutputGain = outputGain;
		if (std::abs (smoothedMix        - mixValue)   < kSnapEpsilon) smoothedMix        = mixValue;
		if (std::abs (smoothedDryLevel   - dryLevelTarget) < kSnapEpsilon) smoothedDryLevel = dryLevelTarget;
		if (std::abs (smoothedWetLevel   - wetLevelTarget) < kSnapEpsilon) smoothedWetLevel = wetLevelTarget;
	}

	// Pan (equal-power, stereo only)
	const float panTarget = juce::jlimit (kPanMin, kPanMax, loadAtomicOrDefault (panParam, kPanDefault));
	if (numChannels >= 2)
	{
		if (std::abs (panTarget - 0.5f) > 0.001f || std::abs (smoothedPan - 0.5f) > 0.001f)
		{
			float* left = buffer.getWritePointer (0);
			float* right = buffer.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
			{
				smoothedPan = smoothedPan * kGainSmoothCoeff + panTarget * (1.0f - kGainSmoothCoeff);
				const float angle = smoothedPan * 1.5707963f; // pi/2
				left[n] *= std::cos (angle);
				right[n] *= std::sin (angle);
			}
		}
	}
	else
	{
		smoothedPan = panTarget;
	}
	if (std::abs (smoothedPan - panTarget) < 1e-5f)
		smoothedPan = panTarget;

	// Limiter (GLOBAL mode: after pan, before safety clip)
	{
		if (limMode == 2)
		{
			auto* chL = buffer.getWritePointer (0);
			if (numChannels >= 2)
			{
				auto* chR = buffer.getWritePointer (1);
				for (int i = 0; i < numSamples; ++i)
				{
					smoothedLimThreshold = smoothedLimThreshold * kGainSmoothCoeff
						+ limThreshLinTarget * (1.0f - kGainSmoothCoeff);
					applyLimiter (chL[i], chR[i], smoothedLimThreshold);
				}
			}
			else
			{
				for (int i = 0; i < numSamples; ++i)
				{
					smoothedLimThreshold = smoothedLimThreshold * kGainSmoothCoeff
						+ limThreshLinTarget * (1.0f - kGainSmoothCoeff);
					applyLimiterMono (chL[i], smoothedLimThreshold);
				}
			}
		}
	}
	if (std::abs (smoothedLimThreshold - limThreshLinTarget) < 1e-5f)
		smoothedLimThreshold = limThreshLinTarget;

	if (pendingMidiEventCount_ > 0)
	{
		for (int eventIndex = 0; eventIndex < pendingMidiEventCount_; ++eventIndex)
			pendingMidiEvents_[(size_t) eventIndex].samplesRemaining -= numSamples;
	}

	// Invert Polarity / Stereo (GLOBAL mode: after Limiter GLOBAL, before safety clip)
	{
		const int invPol = loadIntParamOrDefault (invPolParam, kInvPolDefault);
		const int invStr = loadIntParamOrDefault (invStrParam, kInvStrDefault);
		if (invPol == 2)
			for (int ch = 0; ch < numChannels; ++ch)
				juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), -1.0f, numSamples);
		if (invStr == 2 && numChannels >= 2)
		{
			float* sL = buffer.getWritePointer (0);
			float* sR = buffer.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
				std::swap (sL[n], sR[n]);
		}
	}

	// Safety limiter (+48 dBFS ~= 251.19)
	for (int ch = 0; ch < numChannels; ++ch)
	{
		float* data = buffer.getWritePointer (ch);
		juce::FloatVectorOperations::clip (data, data, -251.19f, 251.19f, numSamples);
	}

	const float outputMeterPeak = TR::DSP::bufferPeak (buffer, numChannels, numSamples);
	TR::DSP::publishPeak (inputMeterPeak_, inputMeterPeak);
	TR::DSP::publishPeak (outputMeterPeak_, outputMeterPeak);

	DSP_LOG_BLOCK_END(dspLog, numSamples, currentSampleRate,
		targetStages, targetSeries, targetFreq, targetShape, altEnabled);
}

bool DisperserAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* DisperserAudioProcessor::createEditor()
{
	return new DisperserAudioProcessorEditor (*this);
}

juce::AudioProcessorValueTreeState::ParameterLayout DisperserAudioProcessor::createParameterLayout()
{
	std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

	params.push_back (std::make_unique<juce::AudioParameterInt> (
		kParamAmount, "Stages", kAmountMin, kAmountMax, kAmountDefault));

	params.push_back (std::make_unique<juce::AudioParameterInt> (
		kParamSeries, "Series", kSeriesMin, kSeriesMax, kSeriesDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFreq, "Frequency",
		juce::NormalisableRange<float> (kFreqMin, kFreqBaseMax, 0.0f, 0.35f), kFreqDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamShape, "Shape",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kShapeDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamJitter, "Jitter",
		juce::NormalisableRange<float> (kJitterMin, kJitterMax, 0.001f), kJitterDefault));

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamAlt, "Alt", false));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFeedback, "Feedback",
		juce::NormalisableRange<float> (kFeedbackMin, kFeedbackMax, 0.0f, 1.0f), kFeedbackDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamMod, "Mod",
		juce::NormalisableRange<float> (0.0f, kModMax, 0.0f, 1.0f), kModDefault));

	params.push_back (std::make_unique<juce::AudioParameterBool> (
		 kParamModHarm, "Mod Harm", false));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamMix, "Mix",
		juce::NormalisableRange<float> (0.0f, kMixMax, 0.0f, 1.0f), kMixDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamTilt, "Tilt",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f), kTiltDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamPan, "Pan",
		juce::NormalisableRange<float> (kPanMin, kPanMax, 0.01f), kPanDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamInput, "Input",
		makeGainFaderRange(), kInputDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamOutput, "Output",
		makeGainFaderRange(), kOutputDefault));

		// Style: 0 = Mono, 1 = Stereo, 2 = Wide, 3 = Dual
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamStyle, "Style",
		juce::NormalisableRange<float> ((float) kStyleMin, (float) kStyleMax, 1.0f, 1.0f), kStyleDefault));

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamMidi, "MIDI", false));

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamSidechain, "Sidechain", false));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainGain, "Sidechain Gain", makeGainFaderRange(), kSidechainGainDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainSmooth, "Sidechain Smooth",
		juce::NormalisableRange<float> (kSidechainSmoothMin, kSidechainSmoothMax, 0.001f, 1.0f),
		kSidechainSmoothDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainPol, "Sidechain Pol",
		juce::NormalisableRange<float> (kSidechainPolMin, kSidechainPolMax, 0.001f, 1.0f),
		kSidechainPolDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainHp, "Sidechain HP",
		juce::NormalisableRange<float> (kSidechainFilterFreqMin, kSidechainFilterFreqMax, 0.0f, 0.35f),
		kSidechainHpDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamSidechainLp, "Sidechain LP",
		juce::NormalisableRange<float> (kSidechainFilterFreqMin, kSidechainFilterFreqMax, 0.0f, 0.35f),
		kSidechainLpDefault));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamSidechainHpOn, "Sidechain HP On", kSidechainHpOnDefault));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamSidechainLpOn, "Sidechain LP On", kSidechainLpOnDefault));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamSidechainHpSlope, "Sidechain HP Slope", kFilterSlopeMin, kFilterSlopeMax, kSidechainHpSlopeDefault));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamSidechainLpSlope, "Sidechain LP Slope", kFilterSlopeMin, kFilterSlopeMax, kSidechainLpSlopeDefault));

	// Wet filter
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFilterHpFreq, "Filter HP Freq",
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.0f, 0.35f), kFilterHpFreqDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFilterLpFreq, "Filter LP Freq",
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.0f, 0.35f), kFilterLpFreqDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFilterHpSlope, "Filter HP Slope",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f), (float) kFilterSlopeDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFilterLpSlope, "Filter LP Slope",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f), (float) kFilterSlopeDefault));

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamFilterHpOn, "Filter HP On", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamFilterLpOn, "Filter LP On", false));

	// Chaos
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamChaos, "Chaos Filter", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamChaosD, "Chaos Delay", false));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmt, "Chaos Amount",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpd, "Chaos Speed",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtFilter, "Chaos Amt Filter",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdFilter, "Chaos Spd Filter",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));

	// Mode In / Mode Out / Sum Bus
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamModeIn, "Mode In",
		juce::StringArray { "L+R", "M/S", "MID", "SIDE" }, kModeInOutDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOut, "Mode Out",
		juce::StringArray { "L+R", "M/S", "MID", "SIDE" }, kModeInOutDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBus, "Sum Bus",
		juce::StringArray { "ST", juce::String::fromUTF8 (u8"\u2192M"), juce::String::fromUTF8 (u8"\u2192S") }, kSumBusDefault));

	// Invert Polarity / Invert Stereo
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamInvPol, "Invert Polarity",
		juce::StringArray { "NONE", "WET", "GLOBAL" }, kInvPolDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamInvStr, "Invert Stereo",
		juce::StringArray { "NONE", "WET", "GLOBAL" }, kInvStrDefault));

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamS0, "S0", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamS100, "S100", false));

	// Limiter
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamLimThreshold, "Lim Threshold",
		juce::NormalisableRange<float> (kLimThresholdMin, kLimThresholdMax, 0.1f), kLimThresholdDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamLimMode, "Lim Mode", juce::StringArray { "NONE", "WET", "GLOBAL" }, kLimModeDefault));

	// Mix Mode (INSERT / SEND) + Dry/Wet levels for SEND mode
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamMixMode, "Mix Mode",
		juce::StringArray { "INSERT", "SEND" }, kMixModeDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamDryLevel, "Dry Level",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kDryLevelDefault));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamWetLevel, "Wet Level",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kWetLevelDefault));

	// Filter / Tilt position (PRE / POST effect)
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamFilterPos, "Filter Position",
		juce::StringArray { juce::String::fromUTF8 (u8"F\u25bc T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25b2"),
		                    juce::String::fromUTF8 (u8"F\u25b2 T\u25bc"),
		                    juce::String::fromUTF8 (u8"F\u25bc T\u25b2") },
		kFilterPosDefault));

	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiWidth, "UI Width", 360, 720, 360));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiHeight, "UI Height", 240, 1200, 752));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiPalette, "UI Palette", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiFxTail, "UI FX Tail", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiIoFx, "UI I/O FX", true));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor0, "UI Color 0", 0, 0xFFFFFF, 0x00FF00));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor1, "UI Color 1", 0, 0xFFFFFF, 0x000000));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor2, "UI Color 2", 0, 0xFFFFFF, 0x0000FF));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor3, "UI Color 3", 0, 0xFFFFFF, 0xFF0000));

	return { params.begin(), params.end() };
}

void DisperserAudioProcessor::setUiEditorSize (int width, int height)
{
	const int w = juce::jlimit (360, 720, width);
	const int h = juce::jlimit (752, 752, height);
	uiEditorWidth.store (w, std::memory_order_relaxed);
	uiEditorHeight.store (h, std::memory_order_relaxed);
	apvts.state.setProperty (UiStateKeys::editorWidth, w, nullptr);
	apvts.state.setProperty (UiStateKeys::editorHeight, h, nullptr);
	setParameterPlainValue (apvts, kParamUiWidth, (float) w);
	setParameterPlainValue (apvts, kParamUiHeight, (float) h);
	updateHostDisplay();
}

int DisperserAudioProcessor::getUiEditorWidth() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::editorWidth);
	if (! fromState.isVoid())
		return juce::jlimit (360, 720, (int) fromState);
	if (uiWidthParam != nullptr)
		return juce::jlimit (360, 720, (int) std::lround (uiWidthParam->load (std::memory_order_relaxed)));
	return juce::jlimit (360, 720, uiEditorWidth.load (std::memory_order_relaxed));
}

int DisperserAudioProcessor::getUiEditorHeight() const noexcept
{
	return 752;
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

void DisperserAudioProcessor::setUiIoFxEnabled (bool shouldEnableIoFx)
{
	apvts.state.setProperty (UiStateKeys::ioFxEnabled, shouldEnableIoFx, nullptr);
	setParameterPlainValue (apvts, kParamUiIoFx, shouldEnableIoFx ? 1.0f : 0.0f);
	updateHostDisplay();
}

bool DisperserAudioProcessor::getUiIoFxEnabled() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::ioFxEnabled);
	if (! fromState.isVoid())
		return (bool) fromState;
	if (uiIoFxParam != nullptr)
		return uiIoFxParam->load (std::memory_order_relaxed) > 0.5f;
	return true;
}

void DisperserAudioProcessor::setUiCustomPaletteColour (int index, juce::Colour colour)
{
	const int safeIndex = juce::jlimit (0, 3, index);
	const auto argb = colour.getARGB();
	const int rgb = ((int) colour.getRed() << 16) | ((int) colour.getGreen() << 8) | (int) colour.getBlue();

	uiCustomPalette[(size_t) safeIndex].store (argb, std::memory_order_relaxed);
	apvts.state.setProperty (UiStateKeys::customPalette[(size_t) safeIndex], (int) argb, nullptr);

	const char* colorParamIds[4] { kParamUiColor0, kParamUiColor1, kParamUiColor2, kParamUiColor3 };
	setParameterPlainValue (apvts, colorParamIds[safeIndex], (float) rgb);
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

void DisperserAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
	auto state = apvts.copyState();
	state.setProperty (UiStateKeys::editorWidth, getUiEditorWidth(), nullptr);
	state.setProperty (UiStateKeys::editorHeight, getUiEditorHeight(), nullptr);
	state.setProperty (UiStateKeys::useCustomPalette, getUiUseCustomPalette(), nullptr);
	state.setProperty (UiStateKeys::fxTailEnabled, getUiFxTailEnabled(), nullptr);
	state.setProperty (UiStateKeys::ioFxEnabled, getUiIoFxEnabled(), nullptr);
	state.setProperty (UiStateKeys::midiPort, getMidiChannel(), nullptr);
	state.setProperty (UiStateKeys::midiDelayMs, getMidiDelayMs(), nullptr);
	for (int i = 0; i < 4; ++i)
		state.setProperty (UiStateKeys::customPalette[(size_t) i], (int) getUiCustomPaletteColour (i).getARGB(), nullptr);

	if (auto xml = state.createXml())
		copyXmlToBinary (*xml, destData);
}

void DisperserAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	if (auto xmlState = getXmlFromBinary (data, sizeInBytes))
	{
		if (xmlState->hasTagName (apvts.state.getType()))
			apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
	}

	const auto w = apvts.state.getProperty (UiStateKeys::editorWidth);
	const auto h = apvts.state.getProperty (UiStateKeys::editorHeight);
	const auto cp = apvts.state.getProperty (UiStateKeys::useCustomPalette);
	const auto fx = apvts.state.getProperty (UiStateKeys::fxTailEnabled);

	if (! w.isVoid()) uiEditorWidth.store ((int) w, std::memory_order_relaxed);
	if (! h.isVoid()) uiEditorHeight.store ((int) h, std::memory_order_relaxed);
	if (! cp.isVoid()) uiUseCustomPalette.store ((bool) cp ? 1 : 0, std::memory_order_relaxed);
	if (! fx.isVoid()) uiFxTailEnabled.store ((bool) fx ? 1 : 0, std::memory_order_relaxed);

	const auto mp = apvts.state.getProperty (UiStateKeys::midiPort);
	if (! mp.isVoid()) midiChannel.store (juce::jlimit (0, 16, (int) mp), std::memory_order_relaxed);
	const auto md = apvts.state.getProperty (UiStateKeys::midiDelayMs);
	if (! md.isVoid()) midiDelayMs.store (juce::jlimit (0, 100, (int) md), std::memory_order_relaxed);

	clearPendingMidiEvents();
	clearMidiTrackingState();
	resetSidechainRuntime();

	for (int i = 0; i < 4; ++i)
	{
		const auto c = apvts.state.getProperty (UiStateKeys::customPalette[(size_t) i]);
		if (! c.isVoid())
			uiCustomPalette[(size_t) i].store ((juce::uint32) (int) c, std::memory_order_relaxed);
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

void DisperserAudioProcessor::setUiIoExpanded (bool expanded)
{
	apvts.state.setProperty (UiStateKeys::ioExpanded, expanded, nullptr);
}

bool DisperserAudioProcessor::getUiIoExpanded() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::ioExpanded);
	if (! fromState.isVoid()) return (bool) fromState;
	return false;
}

void DisperserAudioProcessor::setMidiChannel (int channel)
{
	const int ch = juce::jlimit (0, 16, channel);
	midiChannel.store (ch, std::memory_order_relaxed);
	apvts.state.setProperty (UiStateKeys::midiPort, ch, nullptr);
}

int DisperserAudioProcessor::getMidiChannel() const noexcept
{
	return midiChannel.load (std::memory_order_relaxed);
}

void DisperserAudioProcessor::clearMidiTrackingState() noexcept
{
	lastMidiNote.store (-1, std::memory_order_relaxed);
	lastMidiVelocity.store (0, std::memory_order_relaxed);
	currentMidiFrequency.store (0.0f, std::memory_order_relaxed);
}

void DisperserAudioProcessor::resetSidechainRuntime() noexcept
{
	sidechainDcPrevInL_ = 0.0f;
	sidechainDcPrevInR_ = 0.0f;
	sidechainDcPrevOutL_ = 0.0f;
	sidechainDcPrevOutR_ = 0.0f;
	for (auto& st : sidechainHpFilterL_) st = {};
	for (auto& st : sidechainHpFilterR_) st = {};
	for (auto& st : sidechainLpFilterL_) st = {};
	for (auto& st : sidechainLpFilterR_) st = {};
	sidechainCarrierSmoothL_ = 0.0f;
	sidechainCarrierSmoothR_ = 0.0f;
	sidechainRmsEnv_ = 0.0f;
	sidechainGateSmoothed_ = 0.0f;
	sidechainDepthSmoothed_ = 0.0f;
	std::fill (sidechainFrequencyOffsetNorm_.begin(), sidechainFrequencyOffsetNorm_.end(), 0.0f);
}

void DisperserAudioProcessor::clearPendingMidiEvents() noexcept
{
	pendingMidiEventCount_ = 0;
}

void DisperserAudioProcessor::enqueuePendingMidiEvent (const PendingMidiEvent& event) noexcept
{
	if (pendingMidiEventCount_ >= kPendingMidiEventCapacity)
		return;

	pendingMidiEvents_[(size_t) pendingMidiEventCount_++] = event;
}

void DisperserAudioProcessor::applyPendingMidiEvent (const PendingMidiEvent& event) noexcept
{
	switch (event.type)
	{
		case PendingMidiEventType::allNotesOff:
			clearMidiTrackingState();
			return;

		case PendingMidiEventType::noteOn:
		{
			lastMidiNote.store (event.note, std::memory_order_relaxed);
			lastMidiVelocity.store (event.velocity, std::memory_order_relaxed);
			currentMidiFrequency.store (440.0f * std::exp2 ((event.note - 69) * (1.0f / 12.0f)),
				std::memory_order_relaxed);
			return;
		}

		case PendingMidiEventType::noteOff:
			if (event.note == lastMidiNote.load (std::memory_order_relaxed))
				clearMidiTrackingState();
			return;
	}
}

void DisperserAudioProcessor::setMidiDelayMs (int delayMsValue)
{
	const int clamped = juce::jlimit (0, 100, delayMsValue);
	midiDelayMs.store (clamped, std::memory_order_relaxed);
	apvts.state.setProperty (UiStateKeys::midiDelayMs, clamped, nullptr);
}

int DisperserAudioProcessor::getMidiDelayMs() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::midiDelayMs);
	if (! fromState.isVoid()) return juce::jlimit (0, 100, (int) fromState);
	return midiDelayMs.load (std::memory_order_relaxed);
}

juce::String DisperserAudioProcessor::getMidiNoteName (int midiNote)
{
	if (midiNote < 0 || midiNote > 127)
		return "";

	const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	const int octave = (midiNote / 12) - 1;
	const int noteIndex = midiNote % 12;

	return juce::String (noteNames[noteIndex]) + juce::String (octave);
}

juce::String DisperserAudioProcessor::getCurrentFreqDisplay() const
{
	const bool midiEnabled = loadBoolParamOrDefault (midiParam, false);
	const int midiNote = lastMidiNote.load (std::memory_order_relaxed);
	const bool midiNoteActive = midiEnabled && (midiNote >= 0);

	if (midiNoteActive)
		return getMidiNoteName (midiNote);

	return "";
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new DisperserAudioProcessor();
}

