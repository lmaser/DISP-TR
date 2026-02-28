#include "PluginProcessor.h"
#include "PluginEditor.h"
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
	uiWidthParam = apvts.getRawParameterValue (kParamUiWidth);
	uiHeightParam = apvts.getRawParameterValue (kParamUiHeight);
	uiPaletteParam = apvts.getRawParameterValue (kParamUiPalette);
	uiFxTailParam = apvts.getRawParameterValue (kParamUiFxTail);
	uiColorParams[0] = apvts.getRawParameterValue (kParamUiColor0);
	uiColorParams[1] = apvts.getRawParameterValue (kParamUiColor1);
	uiColorParams[2] = apvts.getRawParameterValue (kParamUiColor2);
	uiColorParams[3] = apvts.getRawParameterValue (kParamUiColor3);
}

DisperserAudioProcessor::~DisperserAudioProcessor() = default;

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

	stagesSmoothed.reset (currentSampleRate, kStageSmoothingSeconds);
	stagesSmoothed.setCurrentAndTargetValue ((float) stages);
	freqSmoothed.reset (currentSampleRate, kFreqSmoothingSeconds);
	freqSmoothed.setCurrentAndTargetValue (freq);
	shapeSmoothed.reset (currentSampleRate, kShapeSmoothingSeconds);
	shapeSmoothed.setCurrentAndTargetValue (shape);

	lastCoeffFreq = -1.0f;
	lastCoeffShape = -1.0f;
	lastCoeffStages = -1;

	juce::dsp::ProcessSpec spec;
	spec.sampleRate = currentSampleRate;
	spec.maximumBlockSize = (juce::uint32) juce::jmax (1, samplesPerBlock);
	spec.numChannels = 1;

	rvsConvL.reset();
	rvsConvR.reset();
	rvsConvL.prepare (spec);
	rvsConvR.prepare (spec);

	rvsCoeffScratch.reserve ((size_t) kAmountMax);
	rvsForwardIrScratch.reserve (2048u);
	rvsReverseIrScratch.reserve (2048u);
	for (auto& stateVec : rvsStateScratch)
		stateVec.assign ((size_t) kAmountMax, {});
	rvsIrBufferL.setSize (1, 2048, false, false, true);
	rvsIrBufferR.setSize (1, 2048, false, false, true);

	juce::AudioBuffer<float> identityL (1, 1);
	juce::AudioBuffer<float> identityR (1, 1);
	identityL.setSample (0, 0, 1.0f);
	identityR.setSample (0, 0, 1.0f);
	rvsConvL.loadImpulseResponse (std::move (identityL), currentSampleRate,
		juce::dsp::Convolution::Stereo::no,
		juce::dsp::Convolution::Trim::no,
		juce::dsp::Convolution::Normalise::no);
	rvsConvR.loadImpulseResponse (std::move (identityR), currentSampleRate,
		juce::dsp::Convolution::Stereo::no,
		juce::dsp::Convolution::Trim::no,
		juce::dsp::Convolution::Normalise::no);

	rvsConvPrepared = true;
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
	lastRvsIrLength = -1;
}

void DisperserAudioProcessor::releaseResources()
{
	for (auto& c : chainL) c.clear();
	for (auto& c : chainR) c.clear();
	stageCoeff.clear();
	rvsConvL.reset();
	rvsConvR.reset();
	rvsConvPrepared = false;
	rvsRebuildPending = false;
	rvsRebuildCooldownSamples = 0;
	rvsStableSamples = 0;
	rvsCoeffScratch.clear();
	rvsForwardIrScratch.clear();
	rvsReverseIrScratch.clear();
	for (auto& stateVec : rvsStateScratch)
		stateVec.clear();
	rvsIrBufferL.setSize (0, 0);
	rvsIrBufferR.setSize (0, 0);
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

int DisperserAudioProcessor::computeRvsIrLengthSamples (int stages, int series) const noexcept
{
	const int nStages = juce::jlimit (kAmountMin, kAmountMax, stages);
	const int nSeries = juce::jlimit (kSeriesMin, kSeriesMax, series);
	const int complexity = juce::jmax (1, nStages * nSeries);
	const int len = 192 + (complexity * 10);
	return juce::jlimit (192, 4096, len);
}

void DisperserAudioProcessor::buildForwardImpulseResponse (std::vector<float>& ir,
													  int irLength,
													  int stages,
													  int series,
													  float freqHz,
													  float shapeNorm)
{
	const int nStages = juce::jlimit (kAmountMin, kAmountMax, stages);
	const int nSeries = juce::jlimit (kSeriesMin, kSeriesMax, series);
	ir.resize ((size_t) juce::jmax (1, irLength));
	std::fill (ir.begin(), ir.end(), 0.0f);

	if (nStages <= 0)
	{
		ir[0] = 1.0f;
		return;
	}

	fillStageCoefficients (rvsCoeffScratch, currentSampleRate, freqHz, shapeNorm, nStages);

	for (int s = 0; s < nSeries; ++s)
	{
		auto* states = rvsStateScratch[(size_t) s].data();
		for (int st = 0; st < nStages; ++st)
			states[st].z1 = 0.0f;
	}

	for (int n = 0; n < irLength; ++n)
	{
		float x = (n == 0) ? 1.0f : 0.0f;
		for (int s = 0; s < nSeries; ++s)
		{
			auto* states = rvsStateScratch[(size_t) s].data();
			for (int st = 0; st < nStages; ++st)
			{
				const float a = rvsCoeffScratch[(size_t) st];
				auto& z = states[st];
				const float y = (-a * x) + z.z1;
				z.z1 = x + (a * y);
				x = y;
			}
		}
		ir[(size_t) n] = x;
	}
}

void DisperserAudioProcessor::rebuildRvsConvolutionIfNeeded (int stages, int series, float freqHz, float shapeNorm, bool forceRebuild)
{
	if (! rvsConvPrepared)
		return;

	const int irLength = computeRvsIrLengthSamples (stages, series);
	const bool changed = forceRebuild
		|| stages != lastRvsStages
		|| series != lastRvsSeries
		|| irLength != lastRvsIrLength
		|| std::abs (freqHz - lastRvsFreq) > 0.5f
		|| std::abs (shapeNorm - lastRvsShape) > 0.002f;

	if (! changed)
		return;

	rvsForwardIrScratch.resize ((size_t) irLength);
	buildForwardImpulseResponse (rvsForwardIrScratch, irLength, stages, series, freqHz, shapeNorm);

	rvsReverseIrScratch.resize ((size_t) irLength);
	for (int i = 0; i < irLength; ++i)
		rvsReverseIrScratch[(size_t) i] = rvsForwardIrScratch[(size_t) (irLength - 1 - i)];

	float peak = 0.0f;
	for (const float sample : rvsReverseIrScratch)
		peak = juce::jmax (peak, std::abs (sample));
	if (peak > 1.0f)
	{
		const float gain = 1.0f / peak;
		for (float& sample : rvsReverseIrScratch)
			sample *= gain;
	}

	rvsIrBufferL.setSize (1, irLength, false, false, true);
	rvsIrBufferR.setSize (1, irLength, false, false, true);
	std::memcpy (rvsIrBufferL.getWritePointer (0), rvsReverseIrScratch.data(), (size_t) irLength * sizeof (float));
	std::memcpy (rvsIrBufferR.getWritePointer (0), rvsReverseIrScratch.data(), (size_t) irLength * sizeof (float));

	rvsConvL.loadImpulseResponse (std::move (rvsIrBufferL), currentSampleRate,
		juce::dsp::Convolution::Stereo::no,
		juce::dsp::Convolution::Trim::no,
		juce::dsp::Convolution::Normalise::no);
	rvsConvR.loadImpulseResponse (std::move (rvsIrBufferR), currentSampleRate,
		juce::dsp::Convolution::Stereo::no,
		juce::dsp::Convolution::Trim::no,
		juce::dsp::Convolution::Normalise::no);

	lastRvsStages = stages;
	lastRvsSeries = series;
	lastRvsFreq = freqHz;
	lastRvsShape = shapeNorm;
	lastRvsIrLength = irLength;
}

void DisperserAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
	juce::ignoreUnused (midi);
	juce::ScopedNoDenormals noDenormals;

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

		auto differsFrom = [] (int stagesA, int seriesA, float freqA, float shapeA,
							   int stagesB, int seriesB, float freqB, float shapeB) noexcept
		{
			return stagesA != stagesB
				|| seriesA != seriesB
				|| std::abs (freqA - freqB) > 0.01f
				|| std::abs (shapeA - shapeB) > 0.0001f;
		};

		if (! rvsRebuildPending)
		{
			const bool differsFromLast = differsFrom (candidateStages,
				candidateSeries,
				candidateFreq,
				candidateShape,
				lastRvsStages,
				lastRvsSeries,
				lastRvsFreq,
				lastRvsShape);

			if (differsFromLast)
			{
				rvsRebuildPending = true;
				pendingRvsStages = candidateStages;
				pendingRvsSeries = candidateSeries;
				pendingRvsFreq = candidateFreq;
				pendingRvsShape = candidateShape;
				rvsStableSamples = 0;
			}
		}
		else
		{
			const bool differsFromPending = differsFrom (candidateStages,
				candidateSeries,
				candidateFreq,
				candidateShape,
				pendingRvsStages,
				pendingRvsSeries,
				pendingRvsFreq,
				pendingRvsShape);

			if (differsFromPending)
			{
				pendingRvsStages = candidateStages;
				pendingRvsSeries = candidateSeries;
				pendingRvsFreq = candidateFreq;
				pendingRvsShape = candidateShape;
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
		if (rvsRebuildPending && rvsRebuildCooldownSamples <= 0 && (allowInitialRebuild || settleReached))
		{
			rebuildRvsConvolutionIfNeeded (pendingRvsStages,
				pendingRvsSeries,
				pendingRvsFreq,
				pendingRvsShape,
				false);
			rvsRebuildPending = false;
			rvsStableSamples = 0;
			rvsRebuildCooldownSamples = (int) std::round ((currentSampleRate * kRvsRebuildMinIntervalMs) / 1000.0);
		}

		auto block = juce::dsp::AudioBlock<float> (buffer);
		auto left = block.getSingleChannelBlock (0);
		juce::dsp::ProcessContextReplacing<float> ctxL (left);

		bool processRightIndependently = (numChannels > 1);
		if (numChannels > 1)
		{
			const auto* leftRead = buffer.getReadPointer (0);
			const auto* rightRead = buffer.getReadPointer (1);
			if (std::memcmp (leftRead, rightRead, (size_t) numSamples * sizeof (float)) == 0)
				processRightIndependently = false;
		}

		rvsConvL.process (ctxL);

		if (processRightIndependently)
		{
			auto right = block.getSingleChannelBlock (1);
			juce::dsp::ProcessContextReplacing<float> ctxR (right);
			rvsConvR.process (ctxR);
		}
		else if (numChannels > 1)
		{
			std::memcpy (buffer.getWritePointer (1), buffer.getReadPointer (0), (size_t) numSamples * sizeof (float));
		}

		if (invEnabled)
			buffer.applyGain (-1.0f);

		return;
	}

	if (targetSeries > activeSeries)
		clearStageRange (0, kAmountMax, targetSeries);
	activeSeries = targetSeries;

	auto* ch0 = buffer.getWritePointer (0);
	float* ch1 = (numChannels > 1) ? buffer.getWritePointer (1) : nullptr;
	const bool hasStereo = (ch1 != nullptr);

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
			if (lastCoeffStages != coeffStages
				|| std::abs (smoothedFreq - lastCoeffFreq) > 0.001f
				|| std::abs (smoothedShape - lastCoeffShape) > 0.0002f)
			{
				updateCoefficients (smoothedFreq, smoothedShape, coeffStages);
				lastCoeffStages = coeffStages;
				lastCoeffFreq = smoothedFreq;
				lastCoeffShape = smoothedShape;
			}

			float xL = ch0[n];
			float xR = hasStereo ? ch1[n] : xL;

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

			ch0[n] = xL;
			if (hasStereo)
				ch1[n] = xR;
		}
	}

	if (invEnabled)
		buffer.applyGain (-1.0f);
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
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamS0, "S0", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamS100, "S100", false));

	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiWidth, "UI Width", 360, 1600, 360));
	params.push_back (std::make_unique<juce::AudioParameterInt> (kParamUiHeight, "UI Height", 240, 1200, 360));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiPalette, "UI Palette", false));
	params.push_back (std::make_unique<juce::AudioParameterBool> (kParamUiFxTail, "UI FX Tail", true));
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

