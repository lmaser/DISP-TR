#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DspDebugLog.h"

namespace
{
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

	// ── Biquad coefficient calculators for wet HP/LP filters ──
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

	// 4th-order Butterworth Q values
	constexpr float kBW4_Q1 = 0.54119610f;   // 1 / (2 cos(3π/8))
	constexpr float kBW4_Q2 = 1.30656296f;   // 1 / (2 cos(π/8))

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
	invParam = apvts.getRawParameterValue (kParamInv);
	feedbackParam = apvts.getRawParameterValue (kParamFeedback);
	modParam = apvts.getRawParameterValue (kParamMod);
	mixParam = apvts.getRawParameterValue (kParamMix);
	tiltParam = apvts.getRawParameterValue (kParamTilt);
	panParam  = apvts.getRawParameterValue (kParamPan);
	styleParam = apvts.getRawParameterValue (kParamStyle);
	midiParam = apvts.getRawParameterValue (kParamMidi);
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
	juce::ignoreUnused (samplesPerBlock);
	currentSampleRate = juce::jmax (1.0, sampleRate);
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
	feedbackSmoothed.reset (currentSampleRate, kFeedbackSmoothingSeconds);
	feedbackSmoothed.setCurrentAndTargetValue (juce::jlimit (kFeedbackMin, kFeedbackMax, loadAtomicOrDefault (feedbackParam, kFeedbackDefault)));
	feedbackLastL = 0.0f;
	feedbackLastR = 0.0f;

	// Input/Output/Mix gain smoothing init (same as ECHO-TR)
	smoothedInputGain = 1.0f;
	smoothedOutputGain = 1.0f;
	smoothedMix = 1.0f;

	// Reset tilt state
	tiltDb_ = 0.0f;
	tiltB0_ = 1.0f; tiltB1_ = 0.0f; tiltA1_ = 0.0f;
	tiltTargetB0_ = 1.0f; tiltTargetB1_ = 0.0f; tiltTargetA1_ = 0.0f;
	tiltState_[0] = tiltState_[1] = 0.0f;
	lastTiltDb_ = 0.0f;
	tiltSmoothSc_ = 1.0f - std::exp (-1.0f / (static_cast<float> (currentSampleRate) * 0.03f));

	// Pre-allocate dry buffer (avoids malloc in processBlock)
	dryBuffer.setSize (getTotalNumOutputChannels(), samplesPerBlock);

	lastCoeffFreq = -1.0f;
	lastCoeffShape = -1.0f;
	lastCoeffStages = -1;

	// Reset MIDI note tracking
	lastMidiNote.store (-1, std::memory_order_relaxed);
	currentMidiFrequency.store (0.0f, std::memory_order_relaxed);

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
	chaosAmtD_ = 0.0f; chaosAmtF_ = 0.0f;
	chaosDPhase_ = 0.0f; chaosDTarget_ = 0.0f; chaosDSmoothed_ = 0.0f;
	chaosGPhase_ = 0.0f; chaosGTarget_ = 0.0f; chaosGSmoothed_ = 0.0f;
	chaosFPhase_ = 0.0f; chaosFTarget_ = 0.0f; chaosFSmoothed_ = 0.0f;
	smoothedChaosFreqMaxOct_ = 0.0f;
	smoothedChaosGainMaxDb_ = 0.0f;
	smoothedChaosFilterMaxOct_ = 0.0f;
	chaosParamSmoothCoeff_ = 0.999f;

	// Precompute chaos smooth coefficients (sampleRate-dependent but constant between prepareToPlay)
	cachedChaosDSmoothCoeff_ = std::exp (-1.0f / ((float) currentSampleRate * 0.030f));
	cachedChaosGSmoothCoeff_ = std::exp (-1.0f / ((float) currentSampleRate * 0.015f));
	cachedChaosFSmoothCoeff_ = std::exp (-1.0f / ((float) currentSampleRate * 0.060f));
	cachedChaosParamSmoothCoeff_ = std::exp (-1.0f / ((float) currentSampleRate * 0.010f));

	dspLog.enableDesktopAutoDump();
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

		if (hpSlope == 0)      // 6 dB/oct — single 1-pole
		{
			hpCoeffs_[0] = calcOnePoleHP (hpFreq, sr);
			hpCoeffs_[1] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };  // pass-through
		}
		else if (hpSlope == 1) // 12 dB/oct — single Butterworth biquad
		{
			constexpr float kBW2_Q = 0.70710678f;  // 1/sqrt(2)
			hpCoeffs_[0] = calcBiquadHP (hpFreq, sr, kBW2_Q);
			hpCoeffs_[1] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		}
		else                   // 24 dB/oct — two cascaded Butterworth biquads
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
			constexpr float kBW2_Q = 0.70710678f;
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
}

#if ! JucePlugin_PreferredChannelConfigurations
bool DisperserAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
	const auto in = layouts.getMainInputChannelSet();
	const auto out = layouts.getMainOutputChannelSet();
	if (in != out)
		return false;
	return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}
#endif

float DisperserAudioProcessor::calcAllPassCoeff (float frequency, float sampleRate) noexcept
{
	const float f = juce::jlimit (20.0f, 0.49f * sampleRate, frequency);
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
	const float minFreq = 20.0f;
	const float maxFreq = 0.49f * sr;
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
	const int numChannels = buffer.getNumChannels();
	if (numSamples <= 0 || numChannels <= 0)
		return;

	for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
		buffer.clear (ch, 0, numSamples);

	const int targetStages = juce::jlimit (kAmountMin, kAmountMax, loadIntParamOrDefault (amountParam, kAmountDefault));
	const int targetSeries = juce::jlimit (kSeriesMin, kSeriesMax, loadIntParamOrDefault (seriesParam, kSeriesDefault));
	float targetFreq = loadAtomicOrDefault (freqParam, kFreqDefault);
	float targetShape = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (shapeParam, kShapeDefault));

	// Debug overrides preserved.
	if (loadBoolParamOrDefault (s0Param, false))
		targetShape = 0.0f;
	if (loadBoolParamOrDefault (s100Param, false))
		targetShape = 1.0f;
	const bool invEnabled = loadBoolParamOrDefault (invParam, false);

	// ── MIDI note tracking ───────────────────────────────────
	const bool midiEnabled = loadBoolParamOrDefault (midiParam, false);
	if (midiEnabled && ! midi.isEmpty())
	{
		const int ch = midiChannel.load (std::memory_order_relaxed);
		for (const auto metadata : midi)
		{
			const auto msg = metadata.getMessage();
			if (ch != 0 && msg.getChannel() != ch)
				continue;
			if (msg.isNoteOn())
			{
				const int note = msg.getNoteNumber();
				lastMidiNote.store (note, std::memory_order_relaxed);
				lastMidiVelocity.store (msg.getVelocity(), std::memory_order_relaxed);
				currentMidiFrequency.store (440.0f * std::exp2 ((note - 69) * (1.0f / 12.0f)),
					std::memory_order_relaxed);
			}
			else if (msg.isNoteOff())
			{
				if (msg.getNoteNumber() == lastMidiNote.load (std::memory_order_relaxed))
				{
					lastMidiNote.store (-1, std::memory_order_relaxed);
					currentMidiFrequency.store (0.0f, std::memory_order_relaxed);
				}
			}
		}
	}
	else if (! midiEnabled && lastMidiNote.load (std::memory_order_relaxed) >= 0)
	{
		lastMidiNote.store (-1, std::memory_order_relaxed);
		currentMidiFrequency.store (0.0f, std::memory_order_relaxed);
	}

	// MIDI frequency override (priority: MIDI > manual slider)
	const float midiFreq = currentMidiFrequency.load (std::memory_order_relaxed);
	if (midiEnabled && midiFreq > 0.0f)
		targetFreq = midiFreq;

	// ── MOD frequency multiplier (hyperbolic below centre, linear above) ──
	const float modValue = loadAtomicOrDefault (modParam, kModDefault);
	const float freqMultiplier = (modValue < 0.5f)
		? 1.0f / (4.0f - 6.0f * modValue)
		: (1.0f + (modValue - 0.5f) * 6.0f);
	targetFreq *= freqMultiplier;

	// ── Smoothstep feedback mapping (sign-preserving bipolar) ─
	float rawFeedback = juce::jlimit (kFeedbackMin, kFeedbackMax, loadAtomicOrDefault (feedbackParam, kFeedbackDefault));
	const float sign = rawFeedback < 0.0f ? -1.0f : 1.0f;
	const float af   = std::abs (rawFeedback);
	const float targetFeedback = sign * af * af * (3.0f - 2.0f * af);

	stagesSmoothed.setTargetValue ((float) targetStages);
	shapeSmoothed.setTargetValue (targetShape);
	feedbackSmoothed.setTargetValue (targetFeedback);

	// ── MIX (dry/wet) ───────────────────────────────────────
	const float mixValue = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (mixParam, kMixDefault));

	// ── TILT EQ parameter load ──
	tiltDb_ = loadAtomicOrDefault (tiltParam, kTiltDefault);

	// ── INPUT / OUTPUT gain (dB → linear, same as ECHO-TR) ───
	const float inputGainDb  = juce::jlimit (kInputMin,  kInputMax,  loadAtomicOrDefault (inputParam,  kInputDefault));
	const float outputGainDb = juce::jlimit (kOutputMin, kOutputMax, loadAtomicOrDefault (outputParam, kOutputDefault));
	const float inputGain    = fastDecibelsToGain (inputGainDb);
	const float outputGain   = fastDecibelsToGain (outputGainDb);

	// ── STYLE: 0=MONO, 1=STEREO, 2=WIDE, 3=DUAL ────────────
	const int style = juce::jlimit (kStyleMin, kStyleMax, loadIntParamOrDefault (styleParam, (int) kStyleDefault));

	// ── Sum Bus (needed early for needsDryBlend) ────────────
	const int sumBusVal  = juce::jlimit (0, 2, (int) sumBusParam->load());

	// Save dry input for dry/wet blend (only when mix < 1 or sum bus active)
	const bool needsDryBlend = (mixValue < 0.999f) || (sumBusVal != 0);
	if (needsDryBlend)
	{
		if (dryBuffer.getNumChannels() < numChannels || dryBuffer.getNumSamples() < numSamples)
			dryBuffer.setSize (numChannels, numSamples, false, false, true);
		for (int ch = 0; ch < numChannels; ++ch)
			dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
	}

	// ── MIDI glide: velocity-dependent EMA coefficient ──────
	if (midiEnabled && lastMidiNote.load (std::memory_order_relaxed) >= 0)
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

	// ── Direct mode (per-sample all-pass) ────────────────────

	// Detect series change → start crossfade
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
	}

	auto* ch0 = buffer.getWritePointer (0);
	float* ch1 = (numChannels > 1) ? buffer.getWritePointer (1) : nullptr;
	const bool hasStereo = (ch1 != nullptr);

	// ── Mode In: M/S encode input before effect processing ──
	const int modeInVal  = juce::jlimit (0, 2, (int) modeInParam->load());
	const int modeOutVal = juce::jlimit (0, 2, (int) modeOutParam->load());

	if (modeInVal > 0 && hasStereo)
	{
		for (int n = 0; n < numSamples; ++n)
		{
			const float L = ch0[n];
			const float R = ch1[n];
			const float M = (L + R) * kSqrt2Over2;
			const float S = (L - R) * kSqrt2Over2;
			if (modeInVal == 1) { ch0[n] = M; ch1[n] = M; }
			else                { ch0[n] = S; ch1[n] = S; }
		}
	}

	const bool processR     = (style >= 1 && hasStereo);
	const bool crossFbk     = (style == 2);  // WIDE
	const bool negateCoeffR = (style == 2);  // WIDE: complementary phase
	const bool dualCoeffR   = (style == 3);  // DUAL: separate R coefficients
	const bool crossfading = (seriesXfadeSamplesRemaining > 0);

	// ── Chaos per-block parameter read ──
	chaosFilterEnabled_ = loadBoolParamOrDefault (chaosParam, false);
	chaosDelayEnabled_  = loadBoolParamOrDefault (chaosDelayParam, false);
	const bool anyChaos = chaosFilterEnabled_ || chaosDelayEnabled_;
	if (anyChaos)
	{
		if (chaosDelayEnabled_)
		{
			const float rawAmtD = loadAtomicOrDefault (chaosAmtParam, kChaosAmtDefault);
			const float rawSpdD = loadAtomicOrDefault (chaosSpdParam, kChaosSpdDefault);
			chaosAmtD_       = rawAmtD;
			chaosShPeriodD_  = (float) currentSampleRate / rawSpdD;
			const float amtNormD = rawAmtD * 0.01f;
			chaosFreqMaxOct_ = amtNormD * 2.0f;   // ±2 oct at 100%
			chaosGainMaxDb_  = amtNormD * 1.0f;    // ±1 dB at 100%
			chaosDSmoothCoeff_ = cachedChaosDSmoothCoeff_;
			chaosGSmoothCoeff_ = cachedChaosGSmoothCoeff_;
		}
		else
		{
			chaosFreqMaxOct_ = 0.0f;
			chaosGainMaxDb_ = 0.0f;
		}

		if (chaosFilterEnabled_)
		{
			const float rawAmtF = loadAtomicOrDefault (chaosAmtFilterParam, kChaosAmtDefault);
			const float rawSpdF = loadAtomicOrDefault (chaosSpdFilterParam, kChaosSpdDefault);
			chaosAmtF_       = rawAmtF;
			chaosShPeriodF_  = (float) currentSampleRate / rawSpdF;
			const float amtNormF = rawAmtF * 0.01f;
			chaosFilterMaxOct_ = amtNormF * 2.0f;  // ±2 oct at 100%
			chaosFSmoothCoeff_ = cachedChaosFSmoothCoeff_;
		}
		else
		{
			chaosFilterMaxOct_ = 0.0f;
		}

		chaosParamSmoothCoeff_ = cachedChaosParamSmoothCoeff_;
	}
	else
	{
		chaosAmtD_ = 0.0f; chaosAmtF_ = 0.0f;
		chaosFreqMaxOct_ = 0.0f;
		chaosGainMaxDb_ = 0.0f;
		chaosFilterMaxOct_ = 0.0f;
	}

	const bool freqConverged = std::abs (smoothedFreqValue - targetFreq) < 0.01f;

	// Fast path: parameters converged + no crossfade → tight inner loop
	// without per-sample smoothing, coefficient checks, or fractional stages.
	// Chaos D forces slow path because it needs per-sample coefficient modulation.
	if (!crossfading
		&& !stagesSmoothed.isSmoothing()
		&& freqConverged
		&& !shapeSmoothed.isSmoothing()
		&& !feedbackSmoothed.isSmoothing()
		&& !chaosDelayEnabled_)
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
				// Feedback routing: cross for WIDE, independent otherwise
				float xL = ch0[n] + fb * (crossFbk ? feedbackLastR : feedbackLastL);
				float xR = processR ? (ch1[n] + fb * (crossFbk ? feedbackLastL : feedbackLastR)) : xL;

				for (int s = 0; s < activeSeries; ++s)
				{
					auto* lS = chainL[(size_t) s].data();
					auto* rS = chainR[(size_t) s].data();

					for (int st = 0; st < stgs; ++st)
					{
						const float a = stageCoeff[(size_t) st];

						auto& sl = lS[st];
						const float yL = (-a * xL) + sl.z1;
						sl.z1 = xL + (a * yL);
						xL = yL;

						if (processR)
						{
							// WIDE: -a (complementary phase), DUAL: separate coeffs, STEREO: same a
							const float aR = negateCoeffR ? -a : (dualCoeffR ? stageCoeffR[(size_t) st] : a);
							auto& sr = rS[st];
							const float yR = (-aR * xR) + sr.z1;
							sr.z1 = xR + (aR * yR);
							xR = yR;
						}
					}
				}

				ch0[n] = xL;
				feedbackLastL = xL;
				if (hasStereo)
				{
					ch1[n] = processR ? xR : xL;
					feedbackLastR = processR ? xR : xL;
				}
			}
		}

	}
	else
	{
	// Slow path: smoothing active or crossfade in progress
	for (int n = 0; n < numSamples; ++n)
	{
		const float smoothedStages = juce::jlimit (0.0f, (float) kAmountMax, stagesSmoothed.getNextValue());
		smoothedFreqValue += (targetFreq - smoothedFreqValue) * (1.0f - freqEmaCoeff);
		float smoothedFreq = smoothedFreqValue;
		const float smoothedShape = shapeSmoothed.getNextValue();
		const float fb = feedbackSmoothed.getNextValue();

		// Chaos D: advance S&H and modulate allpass centre frequency
		if (chaosDelayEnabled_)
		{
			advanceChaosD();
			if (chaosAmtD_ > 0.01f)
			{
				const float oct = chaosDSmoothed_ * smoothedChaosFreqMaxOct_;
				smoothedFreq = juce::jlimit (20.0f, 20000.0f, smoothedFreq * std::exp2 (oct));
			}
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
			if (coeffUpdateCountdown <= 0 || lastCoeffStages != coeffStages)
			{
				coeffUpdateCountdown = kCoeffUpdateInterval;
				if (lastCoeffStages != coeffStages
					|| std::abs (smoothedFreq - lastCoeffFreq) > 0.001f
					|| std::abs (smoothedShape - lastCoeffShape) > 0.0002f)
				{
					updateCoefficients (smoothedFreq, smoothedShape, coeffStages);
					lastCoeffStages = coeffStages;
					lastCoeffFreq = smoothedFreq;
					lastCoeffShape = smoothedShape;
				}
			}

			// DUAL: update R coefficients in slow path
			if (dualCoeffR)
			{
				const float freqR = smoothedFreq * 0.5f;
				if (std::abs (freqR - lastCoeffFreqR) > 0.001f || lastCoeffStages != coeffStages)
				{
					updateCoefficientsInto (freqR, smoothedShape, coeffStages, stageCoeffR);
					lastCoeffFreqR = freqR;
				}
			}

			const float inputL = ch0[n] + fb * (crossFbk ? feedbackLastR : feedbackLastL);
			const float inputR = processR ? (ch1[n] + fb * (crossFbk ? feedbackLastL : feedbackLastR)) : inputL;

			// Process through current (new) topology
			float xL = inputL;
			float xR = inputR;

			for (int s = 0; s < activeSeries; ++s)
			{
				auto* lStages = chainL[(size_t) s].data();
				auto* rStages = chainR[(size_t) s].data();

				for (int st = 0; st < baseStages; ++st)
				{
					const float a = stageCoeff[(size_t) st];

					auto& sl = lStages[st];
					const float yL = (-a * xL) + sl.z1;
					sl.z1 = xL + (a * yL);
					xL = yL;

					if (processR)
					{
						const float aR = negateCoeffR ? -a : (dualCoeffR ? stageCoeffR[(size_t) st] : a);
						auto& sr = rStages[st];
						const float yR = (-aR * xR) + sr.z1;
						sr.z1 = xR + (aR * yR);
						xR = yR;
					}
				}

				if (useFractionalStage)
				{
					const int st = baseStages;
					const float a = stageCoeff[(size_t) st];

					const float inL = xL;
					auto& sl = lStages[st];
					const float yL = (-a * inL) + sl.z1;
					sl.z1 = inL + (a * yL);
					xL = inL + (stageFrac * (yL - inL));

					if (processR)
					{
						const float aR = negateCoeffR ? -a : (dualCoeffR ? stageCoeffR[(size_t) st] : a);
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
					auto* lStages = xfadeChainL[(size_t) s].data();
					auto* rStages = xfadeChainR[(size_t) s].data();

					for (int st = 0; st < baseStages; ++st)
					{
						const float a = stageCoeff[(size_t) st];

						auto& sl = lStages[st];
						const float yL = (-a * xfL) + sl.z1;
						sl.z1 = xfL + (a * yL);
						xfL = yL;

						if (processR)
						{
							const float aR = negateCoeffR ? -a : (dualCoeffR ? stageCoeffR[(size_t) st] : a);
							auto& sr = rStages[st];
							const float yR = (-aR * xfR) + sr.z1;
							sr.z1 = xfR + (aR * yR);
							xfR = yR;
						}
					}

					if (useFractionalStage)
					{
						const int st = baseStages;
						const float a = stageCoeff[(size_t) st];

						const float inL = xfL;
						auto& sl = lStages[st];
						const float yL = (-a * inL) + sl.z1;
						sl.z1 = inL + (a * yL);
						xfL = inL + (stageFrac * (yL - inL));

						if (processR)
						{
							const float aR = negateCoeffR ? -a : (dualCoeffR ? stageCoeffR[(size_t) st] : a);
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
			feedbackLastL = xL;
			if (hasStereo)
			{
				ch1[n] = processR ? xR : xL;
				feedbackLastR = processR ? xR : xL;
			}
		}

		// Chaos D gain modulation (applied per-sample after allpass)
		if (chaosDelayEnabled_ && chaosAmtD_ > 0.01f)
		{
			const float gainDb  = chaosGSmoothed_ * smoothedChaosGainMaxDb_;
			const float gainLin = std::exp2 (gainDb * 0.16609640474f);  // log2(10)/20
			ch0[n] *= gainLin;
			if (hasStereo)
				ch1[n] *= gainLin;
		}
	}
	} // end else (slow path)

	if (invEnabled)
		buffer.applyGain (-1.0f);

	// ── Wet-signal HP/LP filter (applied before dry/wet blend) ──────────
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
							if (chaosFilterEnabled_ && chaosAmtF_ > 0.01f)
							{
								const float oct = chaosFSmoothed_ * smoothedChaosFilterMaxOct_;
								const float mult = std::exp2 (oct);
								const float sHp = smoothedFilterHpFreq_;
								const float sLp = smoothedFilterLpFreq_;
								smoothedFilterHpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax,
									(hpOn ? sHp : kFilterFreqMin) * mult);
								smoothedFilterLpFreq_ = juce::jlimit (kFilterFreqMin, kFilterFreqMax,
									(lpOn ? sLp : kFilterFreqMax) * mult);
								updateFilterCoeffs (true, true);
								smoothedFilterHpFreq_ = sHp;
								smoothedFilterLpFreq_ = sLp;
							}
							else
							{
								updateFilterCoeffs (false, false);
							}
						}
					}

					float x = wet[n];

					if (hpOn)
						for (int s = 0; s < numSections_hp; ++s)
							x = processBiquad (hpCoeffs_[s], fs.hp[s], x);

					if (lpOn)
						for (int s = 0; s < numSections_lp; ++s)
							x = processBiquad (lpCoeffs_[s], fs.lp[s], x);

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

	// ── TILT filter (1-pole shelving, pivot 1 kHz) ──
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
		tiltB0_ += (tiltTargetB0_ - tiltB0_) * sc;
		tiltB1_ += (tiltTargetB1_ - tiltB1_) * sc;
		tiltA1_ += (tiltTargetA1_ - tiltA1_) * sc;

		for (int ch = 0; ch < juce::jmin (numChannels, 2); ++ch)
		{
			float* data = buffer.getWritePointer (ch);
			for (int n = 0; n < numSamples; ++n)
			{
				const float x = data[n];
				const float y = tiltB0_ * x + tiltState_[ch];
				tiltState_[ch] = tiltB1_ * x - tiltA1_ * y;
				data[n] = y;
			}
		}
	}
	else if (std::abs (lastTiltDb_) > 0.05f)
	{
		lastTiltDb_ = 0.0f;
		tiltB0_ = 1.0f; tiltB1_ = 0.0f; tiltA1_ = 0.0f;
		tiltTargetB0_ = 1.0f; tiltTargetB1_ = 0.0f; tiltTargetA1_ = 0.0f;
		tiltState_[0] = tiltState_[1] = 0.0f;
	}

	// ── Mode Out: M/S decode wet signal ──
	if (modeOutVal > 0 && numChannels >= 2)
	{
		float* wL = buffer.getWritePointer (0);
		float* wR = buffer.getWritePointer (1);
		for (int n = 0; n < numSamples; ++n)
		{
			const float L = wL[n];
			const float R = wR[n];
			const float M = (L + R) * kSqrt2Over2;
			const float S = (L - R) * kSqrt2Over2;
			if (modeOutVal == 1) { wL[n] = M; wR[n] = M; }
			else                 { wL[n] = S; wR[n] = S; }
		}
	}

	// ── Per-sample smoothed Input/Output/Mix + Dry/Wet blend (fused loop) ──
	if (needsDryBlend)
	{
		if (sumBusVal == 0 || numChannels < 2)
		{
			// ST (stereo passthrough)
			for (int ch = 0; ch < juce::jmin (numChannels, dryBuffer.getNumChannels()); ++ch)
			{
				const float* dry = dryBuffer.getReadPointer (ch);
				float* wet = buffer.getWritePointer (ch);
				for (int n = 0; n < numSamples; ++n)
				{
					smoothedInputGain  = smoothedInputGain  * kGainSmoothCoeff + inputGain  * (1.0f - kGainSmoothCoeff);
					smoothedOutputGain = smoothedOutputGain * kGainSmoothCoeff + outputGain * (1.0f - kGainSmoothCoeff);
					smoothedMix        = smoothedMix        * kGainSmoothCoeff + mixValue   * (1.0f - kGainSmoothCoeff);

					const float dryS = dry[n];
					const float wetS = wet[n] * smoothedInputGain * smoothedOutputGain;
					wet[n] = dryS + smoothedMix * (wetS - dryS);
				}
			}
		}
		else
		{
			// →M or →S bus: dry preserves stereo image, only wet goes through bus
			const float* dryL = dryBuffer.getReadPointer (0);
			const float* dryR = dryBuffer.getReadPointer (1);
			float* outL = buffer.getWritePointer (0);
			float* outR = buffer.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
			{
				smoothedInputGain  = smoothedInputGain  * kGainSmoothCoeff + inputGain  * (1.0f - kGainSmoothCoeff);
				smoothedOutputGain = smoothedOutputGain * kGainSmoothCoeff + outputGain * (1.0f - kGainSmoothCoeff);
				smoothedMix        = smoothedMix        * kGainSmoothCoeff + mixValue   * (1.0f - kGainSmoothCoeff);

				const float dL = dryL[n] * (1.0f - smoothedMix);
				const float dR = dryR[n] * (1.0f - smoothedMix);
				const float wL = outL[n] * smoothedInputGain * smoothedOutputGain * smoothedMix;
				const float wR = outR[n] * smoothedInputGain * smoothedOutputGain * smoothedMix;

				if (sumBusVal == 1) // →M
				{
					const float midBus = (wL + wR) * 0.5f;
					outL[n] = dL + midBus;
					outR[n] = dR + midBus;
				}
				else // →S
				{
					const float sideBus = (wL - wR) * 0.5f;
					outL[n] = dL + sideBus;
					outR[n] = dR - sideBus;
				}
			}
		}
	}
	else
	{
		// Full wet — apply input * output gain
		for (int ch = 0; ch < numChannels; ++ch)
		{
			float* data = buffer.getWritePointer (ch);
			for (int n = 0; n < numSamples; ++n)
			{
				smoothedInputGain  = smoothedInputGain  * kGainSmoothCoeff + inputGain  * (1.0f - kGainSmoothCoeff);
				smoothedOutputGain = smoothedOutputGain * kGainSmoothCoeff + outputGain * (1.0f - kGainSmoothCoeff);
				data[n] = data[n] * smoothedInputGain * smoothedOutputGain;
			}
		}
	}
	{
		constexpr float kSnapEpsilon = 1e-5f;
		if (std::abs (smoothedInputGain  - inputGain)  < kSnapEpsilon) smoothedInputGain  = inputGain;
		if (std::abs (smoothedOutputGain - outputGain) < kSnapEpsilon) smoothedOutputGain = outputGain;
		if (std::abs (smoothedMix        - mixValue)   < kSnapEpsilon) smoothedMix        = mixValue;
	}

	// ── Pan (equal-power, stereo only) ──
	if (numChannels >= 2)
	{
		const float pan = panParam->load();
		if (std::abs (pan - lastPan_) > 0.001f)
		{
			lastPan_ = pan;
			const float angle = pan * 1.5707963f; // π/2
			lastPanLeft_  = std::cos (angle);
			lastPanRight_ = std::sin (angle);
		}
		if (std::abs (lastPan_ - 0.5f) > 0.001f)
		{
			juce::FloatVectorOperations::multiply (buffer.getWritePointer (0), lastPanLeft_,  numSamples);
			juce::FloatVectorOperations::multiply (buffer.getWritePointer (1), lastPanRight_, numSamples);
		}
	}

	// ── Safety limiter (+48 dBFS ≈ 251.19) ──
	for (int ch = 0; ch < numChannels; ++ch)
	{
		float* data = buffer.getWritePointer (ch);
		juce::FloatVectorOperations::clip (data, data, -251.19f, 251.19f, numSamples);
	}

	DSP_LOG_BLOCK_END(dspLog, numSamples, currentSampleRate,
		targetStages, targetSeries, targetFreq, targetShape, invEnabled);
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
		juce::NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.35f), kFreqDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamShape, "Shape",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kShapeDefault));

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamInv, "Inv", false));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamFeedback, "Feedback",
		juce::NormalisableRange<float> (kFeedbackMin, kFeedbackMax, 0.0f, 1.0f), kFeedbackDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamMod, "Mod",
		juce::NormalisableRange<float> (0.0f, kModMax, 0.0f, 1.0f), kModDefault));

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
		juce::NormalisableRange<float> (kInputMin, kInputMax, 0.0f, 2.5f), kInputDefault));

	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamOutput, "Output",
		juce::NormalisableRange<float> (kOutputMin, kOutputMax, 0.0f, 3.23f), kOutputDefault));

		// Style: 0 = Mono, 1 = Stereo, 2 = Wide, 3 = Dual
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamStyle, "Style",
		juce::NormalisableRange<float> ((float) kStyleMin, (float) kStyleMax, 1.0f, 1.0f), kStyleDefault));

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamMidi, "MIDI", false));

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
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamChaosD, "Chaos Disperser", false));
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
		juce::StringArray { "L+R", "MID", "SIDE" }, kModeInOutDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOut, "Mode Out",
		juce::StringArray { "L+R", "MID", "SIDE" }, kModeInOutDefault));
	params.push_back (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBus, "Sum Bus",
		juce::StringArray { "ST", juce::String::fromUTF8 (u8"\u2192M"), juce::String::fromUTF8 (u8"\u2192S") }, kSumBusDefault));

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamS0, "S0", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamS100, "S100", false));

	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiWidth, "UI Width", 360, 1600, 360));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiHeight, "UI Height", 240, 1200, 360));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiPalette, "UI Palette", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiFxTail, "UI FX Tail", false));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor0, "UI Color 0", 0, 0xFFFFFF, 0x00FF00));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor1, "UI Color 1", 0, 0xFFFFFF, 0x000000));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor2, "UI Color 2", 0, 0xFFFFFF, 0xFFFFFF));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor3, "UI Color 3", 0, 0xFFFFFF, 0x000000));

	return { params.begin(), params.end() };
}

void DisperserAudioProcessor::setUiEditorSize (int width, int height)
{
	const int w = juce::jlimit (360, 1600, width);
	const int h = juce::jlimit (240, 1200, height);
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
		return (int) fromState;
	if (uiWidthParam != nullptr)
		return (int) std::lround (uiWidthParam->load (std::memory_order_relaxed));
	return uiEditorWidth.load (std::memory_order_relaxed);
}

int DisperserAudioProcessor::getUiEditorHeight() const noexcept
{
	const auto fromState = apvts.state.getProperty (UiStateKeys::editorHeight);
	if (! fromState.isVoid())
		return (int) fromState;
	if (uiHeightParam != nullptr)
		return (int) std::lround (uiHeightParam->load (std::memory_order_relaxed));
	return uiEditorHeight.load (std::memory_order_relaxed);
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
	state.setProperty (UiStateKeys::midiPort, getMidiChannel(), nullptr);
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

