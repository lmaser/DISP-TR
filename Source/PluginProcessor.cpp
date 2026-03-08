#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DspDebugLog.h"
#include <cstring>

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

	inline void fillStageCoefficients (std::vector<float>& coeffOut,
										 double sampleRate,
										 float freqHz,
										 float shapeNorm,
										 int stages)
	{
		const int nStages = juce::jmax (1, stages);
		coeffOut.assign ((size_t) nStages, 0.0f);

		const float sr = (float) juce::jmax (1.0, sampleRate);
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
			const float t = std::tan (juce::MathConstants<float>::pi * center / sr);
			coeffOut[0] = std::isfinite (t) ? (1.0f - t) / (1.0f + t) : 0.0f;
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
			const float t = std::tan (juce::MathConstants<float>::pi * f / sr);
			coeffOut[(size_t) i] = std::isfinite (t) ? (1.0f - t) / (1.0f + t) : 0.0f;
		}
	}

	inline float mapReverseFrequencyFromControl (float frequencyHz) noexcept
	{
		const juce::NormalisableRange<float> frequencyRange (20.0f, 20000.0f, 0.0f, 0.35f);
		const float clamped = juce::jlimit (frequencyRange.start, frequencyRange.end, frequencyHz);
		const float normalised = juce::jlimit (0.0f, 1.0f, frequencyRange.convertTo0to1 (clamped));
		const float invertedNormalised = 1.0f - normalised;
		return frequencyRange.convertFrom0to1 (invertedNormalised);
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
	reverseParam = apvts.getRawParameterValue (kParamReverse);
	invParam = apvts.getRawParameterValue (kParamInv);
	s0Param = apvts.getRawParameterValue (kParamS0);
	s100Param = apvts.getRawParameterValue (kParamS100);
	rvsDecayParam = apvts.getRawParameterValue (kParamRvsDecay);
	uiWidthParam = apvts.getRawParameterValue (kParamUiWidth);
	uiHeightParam = apvts.getRawParameterValue (kParamUiHeight);
	uiPaletteParam = apvts.getRawParameterValue (kParamUiPalette);
	uiFxTailParam = apvts.getRawParameterValue (kParamUiFxTail);
	uiColorParams[0] = apvts.getRawParameterValue (kParamUiColor0);
	uiColorParams[1] = apvts.getRawParameterValue (kParamUiColor1);
	uiColorParams[2] = apvts.getRawParameterValue (kParamUiColor2);
	uiColorParams[3] = apvts.getRawParameterValue (kParamUiColor3);
}

DisperserAudioProcessor::~DisperserAudioProcessor()
{
	if (rvsRebuildThread)
	{
		rvsRebuildThread->stopThread (500);
		rvsRebuildThread.reset();
	}
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
	freqSmoothed.reset (currentSampleRate, kFreqSmoothingSeconds);
	freqSmoothed.setCurrentAndTargetValue (freq);
	shapeSmoothed.reset (currentSampleRate, kShapeSmoothingSeconds);
	shapeSmoothed.setCurrentAndTargetValue (shape);

	lastCoeffFreq = -1.0f;
	lastCoeffShape = -1.0f;
	lastCoeffStages = -1;

	rvsConvL.prepare (kRvsPartitionSize, kRvsMaxIrLength);
	rvsConvR.prepare (kRvsPartitionSize, kRvsMaxIrLength);

	// Pre-allocate shared IR buffer for thread-safe handoff
	sharedIrBuffer.assign ((size_t) kRvsMaxIrLength, 0.0f);
	sharedIrReady.store (0, std::memory_order_relaxed);

	// Stop any prior rebuild thread before resetting state
	if (rvsRebuildThread)
	{
		rvsRebuildThread->stopThread (500);
		rvsRebuildThread.reset();
	}

	rvsRebuildPending = false;
	rvsRebuildCooldownSamples = 0;
	rvsStableSamples = 0;
	pendingRvsStages = -1;
	pendingRvsSeries = -1;
	pendingRvsFreq = -1.0f;
	pendingRvsShape = -1.0f;
	lastRvsStages = -1;
	lastRvsSeries = -1;
	lastRvsFreq = -1.0f;
	lastRvsShape = -1.0f;
	lastRvsDecay = -1.0f;
	lastRvsIrLength = -1;

	// Start background rebuild thread
	rvsRebuildThread = std::make_unique<RvsRebuildThread> (*this);
	rvsRebuildThread->startThread (juce::Thread::Priority::normal);

	dspLog.enableDesktopAutoDump();
}

void DisperserAudioProcessor::releaseResources()
{
	if (rvsRebuildThread)
	{
		rvsRebuildThread->stopThread (500);
		rvsRebuildThread.reset();
	}

	for (auto& c : chainL) c.clear();
	for (auto& c : chainR) c.clear();
	for (auto& c : xfadeChainL) c.clear();
	for (auto& c : xfadeChainR) c.clear();
	stageCoeff.clear();
	rvsConvL.reset();
	rvsConvR.reset();
	rvsRebuildPending = false;
	rvsRebuildCooldownSamples = 0;
	rvsStableSamples = 0;
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

void DisperserAudioProcessor::updateCoefficients (float freqHz, float shapeNorm, int stages)
{
	const int nStages = juce::jmax (1, stages);
	if ((int) stageCoeff.size() < nStages)
		stageCoeff.assign ((size_t) kAmountMax, 0.0f);

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
		stageCoeff[0] = calcAllPassCoeff (center, sr);
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
		stageCoeff[(size_t) i] = calcAllPassCoeff (f, sr);
	}
}

int DisperserAudioProcessor::computeRvsIrLengthSamples (int stages, int series, float decay) const noexcept
{
	const int nStages = juce::jlimit (kAmountMin, kAmountMax, stages);
	const int nSeries = juce::jlimit (kSeriesMin, kSeriesMax, series);
	const int complexity = juce::jmax (1, nStages * nSeries);
	const int fullLen = 192 + (complexity * 10);
	const float d = juce::jlimit (0.0f, 1.0f, decay);
	const float lengthMult = 0.25f + d * 1.5f; // 0→0.25×, 0.5→1.0× (default), 1.0→1.75×
	const int len = (int) std::round ((float) fullLen * lengthMult);
	return juce::jlimit (192, 2048, len);
}

// ── Background IR rebuild thread ─────────────────────────────
void DisperserAudioProcessor::RvsRebuildThread::run()
{
	coeffScratch.reserve ((size_t) kAmountMax);
	for (auto& sv : stateScratch)
		sv.assign ((size_t) kAmountMax, {});
	fwdIrScratch.reserve (2048u);
	revIrScratch.reserve (2048u);

	while (! threadShouldExit())
	{
		if (! pending.exchange (false, std::memory_order_acquire))
		{
			wait (100);
			continue;
		}

		busy.store (true, std::memory_order_release);

		const int stages  = reqStages.load (std::memory_order_relaxed);
		const int series  = reqSeries.load (std::memory_order_relaxed);
		const float freq  = reqFreq  .load (std::memory_order_relaxed);
		const float shape = reqShape .load (std::memory_order_relaxed);
		const float decay = reqDecay .load (std::memory_order_relaxed);

		DSP_LOG_REBUILD_BEGIN();

		const float d = juce::jlimit (0.0f, 1.0f, decay);
		const int irLength = proc.computeRvsIrLengthSamples (stages, series, d);
		const int nStages = juce::jlimit (kAmountMin, kAmountMax, stages);
		const int nSeries = juce::jlimit (kSeriesMin, kSeriesMax, series);

		// Build forward IR using thread-local scratch buffers
		fwdIrScratch.assign ((size_t) irLength, 0.0f);

		if (nStages > 0)
		{
			fillStageCoefficients (coeffScratch, proc.currentSampleRate, freq, shape, nStages);
			for (int s = 0; s < nSeries; ++s)
				for (int st = 0; st < nStages; ++st)
					stateScratch[(size_t) s][(size_t) st].z1 = 0.0f;

			for (int n = 0; n < irLength; ++n)
			{
				float x = (n == 0) ? 1.0f : 0.0f;
				for (int s = 0; s < nSeries; ++s)
				{
					auto* states = stateScratch[(size_t) s].data();
					for (int st = 0; st < nStages; ++st)
					{
						const float a = coeffScratch[(size_t) st];
						auto& z = states[st];
						const float y = (-a * x) + z.z1;
						z.z1 = x + (a * y);
						x = y;
					}
				}
				fwdIrScratch[(size_t) n] = x;
			}
		}
		else
		{
			fwdIrScratch[0] = 1.0f;
		}

		// Reverse
		revIrScratch.resize ((size_t) irLength);
		for (int i = 0; i < irLength; ++i)
			revIrScratch[(size_t) i] = fwdIrScratch[(size_t) (irLength - 1 - i)];

		// Tukey taper
		const int taperLen = juce::jmin (irLength / 4, 128);
		if (taperLen > 1)
		{
			for (int i = 0; i < taperLen; ++i)
			{
				const int idx = irLength - taperLen + i;
				const float phase = (float) i / (float) (taperLen - 1);
				const float window = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * phase));
				revIrScratch[(size_t) idx] *= window;
			}
		}

		// Peak normalise
		float peak = 0.0f;
		for (const float sample : revIrScratch)
			peak = juce::jmax (peak, std::abs (sample));
		if (peak > 1.0f)
		{
			const float gain = 1.0f / peak;
			for (float& sample : revIrScratch)
				sample *= gain;
		}

		// Store reversed IR in shared buffer for audio thread pickup
		const int copyLen = juce::jmin (irLength, kRvsMaxIrLength);
		std::memcpy (proc.sharedIrBuffer.data(), revIrScratch.data(),
					 (size_t) copyLen * sizeof (float));
		proc.sharedIrReady.store (copyLen, std::memory_order_release);

		DSP_LOG_REBUILD_END(proc.dspLog, irLength, stages, series, freq, shape, d);

		busy.store (false, std::memory_order_release);
	}
}

void DisperserAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
	juce::ignoreUnused (midi);
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
	const bool reverseEnabled = loadBoolParamOrDefault (reverseParam, false);
	const float rvsDecay = juce::jlimit (0.0f, 1.0f, loadAtomicOrDefault (rvsDecayParam, kRvsDecayDefault));
	if (reverseEnabled)
		targetFreq = mapReverseFrequencyFromControl (targetFreq);

	// Debug overrides preserved.
	if (loadBoolParamOrDefault (s0Param, false))
		targetShape = 0.0f;
	if (loadBoolParamOrDefault (s100Param, false))
		targetShape = 1.0f;
	const bool invEnabled = loadBoolParamOrDefault (invParam, false);

	stagesSmoothed.setTargetValue ((float) targetStages);
	freqSmoothed.setTargetValue (targetFreq);
	shapeSmoothed.setTargetValue (targetShape);

	// ── Reverse mode (convolution) ───────────────────────────
	if (reverseEnabled)
	{
		const float smoothedStages = juce::jlimit (0.0f, (float) kAmountMax, stagesSmoothed.skip (numSamples));
		const float smoothedFreq = freqSmoothed.skip (numSamples);
		const float smoothedShape = juce::jlimit (0.0f, 1.0f, shapeSmoothed.skip (numSamples));
		const int smoothedStageCount = juce::jlimit (kAmountMin, kAmountMax,
			(int) std::lround (smoothedStages));

		const float quantShape = std::round (smoothedShape * 100.0f) * 0.01f;
		const float freqOct = std::log2 (juce::jmax (20.0f, smoothedFreq) / 20.0f);
		const float quantFreq = 20.0f * std::pow (2.0f, std::round (freqOct * 24.0f) / 24.0f);
		const int candidateStages = smoothedStageCount;
		const int candidateSeries = targetSeries;
		const float candidateFreq = quantFreq;
		const float candidateShape = quantShape;
		const float candidateDecay = rvsDecay;

		auto differsFrom = [] (int stagesA, int seriesA, float freqA, float shapeA, float decayA,
							   int stagesB, int seriesB, float freqB, float shapeB, float decayB) noexcept
		{
			return stagesA != stagesB
				|| seriesA != seriesB
				|| std::abs (freqA - freqB) > 0.01f
				|| std::abs (shapeA - shapeB) > 0.0001f
				|| std::abs (decayA - decayB) > 0.005f;
		};

		if (! rvsRebuildPending)
		{
			const bool differsFromLast = differsFrom (candidateStages,
				candidateSeries,
				candidateFreq,
				candidateShape,
				candidateDecay,
				lastRvsStages,
				lastRvsSeries,
				lastRvsFreq,
				lastRvsShape,
				lastRvsDecay);

			if (differsFromLast)
			{
				rvsRebuildPending = true;
				pendingRvsStages = candidateStages;
				pendingRvsSeries = candidateSeries;
				pendingRvsFreq = candidateFreq;
				pendingRvsShape = candidateShape;
				pendingRvsDecay = candidateDecay;
				rvsStableSamples = 0;
			}
		}
		else
		{
			const bool differsFromPending = differsFrom (candidateStages,
				candidateSeries,
				candidateFreq,
				candidateShape,
				candidateDecay,
				pendingRvsStages,
				pendingRvsSeries,
				pendingRvsFreq,
				pendingRvsShape,
				pendingRvsDecay);

			if (differsFromPending)
			{
				pendingRvsStages = candidateStages;
				pendingRvsSeries = candidateSeries;
				pendingRvsFreq = candidateFreq;
				pendingRvsShape = candidateShape;
				pendingRvsDecay = candidateDecay;
				rvsStableSamples = 0;
			}
			else
			{
				rvsStableSamples += numSamples;
			}
		}

		rvsRebuildCooldownSamples = juce::jmax (0, rvsRebuildCooldownSamples - numSamples);
		const int settleSamples = (int) std::round ((currentSampleRate * kRvsSettleWindowMs) / 1000.0);
		const bool allowInitialRebuild = (lastRvsStages < 0);
		const bool settleReached = (rvsStableSamples >= settleSamples);
		const bool threadBusy = rvsRebuildThread && rvsRebuildThread->isBusy();
		if (rvsRebuildPending && rvsRebuildCooldownSamples <= 0
			&& ! threadBusy && (allowInitialRebuild || settleReached))
		{
			if (rvsRebuildThread)
				rvsRebuildThread->requestRebuild (pendingRvsStages,
					pendingRvsSeries,
					pendingRvsFreq,
					pendingRvsShape,
					pendingRvsDecay);

			// Update lastRvs* on the audio thread so we don't re-post the same request
			lastRvsStages   = pendingRvsStages;
			lastRvsSeries   = pendingRvsSeries;
			lastRvsFreq     = pendingRvsFreq;
			lastRvsShape    = pendingRvsShape;
			lastRvsDecay    = pendingRvsDecay;
			lastRvsIrLength = computeRvsIrLengthSamples (pendingRvsStages, pendingRvsSeries, pendingRvsDecay);

			rvsRebuildPending = false;
			rvsStableSamples = 0;
			rvsRebuildCooldownSamples = (int) std::round ((currentSampleRate * kRvsRebuildMinIntervalMs) / 1000.0);
		}

		// Pick up new IR from rebuild thread if available
		const int newIrLen = sharedIrReady.exchange (0, std::memory_order_acquire);
		if (newIrLen > 0)
		{
			rvsConvL.loadIR (sharedIrBuffer.data(), newIrLen);
			rvsConvR.loadIR (sharedIrBuffer.data(), newIrLen);
		}

		// Process convolution (uniform cost per partition)
		bool stereoIdentical = false;
		if (numChannels > 1)
		{
			stereoIdentical = (std::memcmp (buffer.getReadPointer (0),
											buffer.getReadPointer (1),
											(size_t) numSamples * sizeof (float)) == 0);
		}

		auto* ch0w = buffer.getWritePointer (0);
		rvsConvL.processInPlace (ch0w, numSamples);

		if (numChannels > 1)
		{
			if (! stereoIdentical)
				rvsConvR.processInPlace (buffer.getWritePointer (1), numSamples);
			else
				std::memcpy (buffer.getWritePointer (1), ch0w, (size_t) numSamples * sizeof (float));
		}

		if (invEnabled)
			buffer.applyGain (-1.0f);

		DSP_LOG_BLOCK_END(dspLog, numSamples, currentSampleRate,
			targetStages, targetSeries, targetFreq, targetShape, rvsDecay, true, invEnabled);
		return;
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
	const bool crossfading = (seriesXfadeSamplesRemaining > 0);

	for (int n = 0; n < numSamples; ++n)
	{
		const float smoothedStages = juce::jlimit (0.0f, (float) kAmountMax, stagesSmoothed.getNextValue());
		const float smoothedFreq = freqSmoothed.getNextValue();
		const float smoothedShape = shapeSmoothed.getNextValue();

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

			const float inputL = ch0[n];
			const float inputR = hasStereo ? ch1[n] : inputL;

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

					if (hasStereo)
					{
						auto& sr = rStages[st];
						const float yR = (-a * xR) + sr.z1;
						sr.z1 = xR + (a * yR);
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

					if (hasStereo)
					{
						const float inR = xR;
						auto& sr = rStages[st];
						const float yR = (-a * inR) + sr.z1;
						sr.z1 = inR + (a * yR);
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

						if (hasStereo)
						{
							auto& sr = rStages[st];
							const float yR = (-a * xfR) + sr.z1;
							sr.z1 = xfR + (a * yR);
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

						if (hasStereo)
						{
							const float inR = xfR;
							auto& sr = rStages[st];
							const float yR = (-a * inR) + sr.z1;
							sr.z1 = inR + (a * yR);
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
			if (hasStereo)
				ch1[n] = xR;
		}
	}

	if (invEnabled)
		buffer.applyGain (-1.0f);

	DSP_LOG_BLOCK_END(dspLog, numSamples, currentSampleRate,
		targetStages, targetSeries, targetFreq, targetShape, 0.0f, false, invEnabled);
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

	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamReverse, "Reverse", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamInv, "Inv", false));
	params.push_back (std::make_unique<juce::AudioParameterFloat> (
		kParamRvsDecay, "RVS Decay",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kRvsDecayDefault));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamS0, "S0", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamS100, "S100", false));

	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiWidth, "UI Width", 360, 1600, 360));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiHeight, "UI Height", 240, 1200, 360));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiPalette, "UI Palette", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiFxTail, "UI FX Tail", false));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiColor0, "UI Color 0", 0, 0xFFFFFF, 0xFFFFFF));
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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new DisperserAudioProcessor();
}

