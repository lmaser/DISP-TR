#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cstring>
#include <cmath>

// ── Uniform-partition overlap-add convolver ──────────────────
// Uses JUCE FFT internally.  All partitions are the same size,
// so the per-block cost is constant — no periodic spikes.
//
// Usage:
//   prepare(128, 2048);          // partition size, max IR length
//   loadIR(irData, irLength);    // set the impulse response
//   process(in, out, numSamples);// per-block processing

class UniformConvolver
{
public:
	UniformConvolver() = default;

	// Call from prepareToPlay (non-RT).  Allocates all memory.
	void prepare (int partitionSize, int maxIRLength)
	{
		jassert (partitionSize > 0 && juce::isPowerOfTwo (partitionSize));

		partSize = partitionSize;
		fftOrder = (int) std::round (std::log2 ((double) partitionSize * 2));
		fftLen   = partitionSize * 2;
		fft = std::make_unique<juce::dsp::FFT> (fftOrder);

		// Number of complex bins produced by real-only FFT
		numBins = fftLen / 2 + 1;
		// JUCE stores complex as interleaved [re,im] pairs; buffer size = fftLen * 2
		const int bufSize = fftLen * 2;

		maxParts = juce::jmax (1, (maxIRLength + partSize - 1) / partSize);
		numParts = 0;

		irFreq.resize ((size_t) maxParts);
		for (auto& seg : irFreq)
			seg.assign ((size_t) bufSize, 0.0f);

		dlFreq.resize ((size_t) maxParts);
		for (auto& dl : dlFreq)
			dl.assign ((size_t) bufSize, 0.0f);

		dlPos = 0;

		accumBuf.assign ((size_t) bufSize, 0.0f);
		inputAccum.assign ((size_t) partSize, 0.0f);
		outputBuf.assign ((size_t) partSize, 0.0f);
		overlapBuf.assign ((size_t) partSize, 0.0f);
		bufPos = 0;
		loaded = false;
	}

	// Load a new impulse response.  Performs FFT of each IR segment.
	// Not strictly RT-safe (FFT work), but bounded and fast
	// (~50-100 µs for typical IRs).  Called rarely (on IR swap).
	void loadIR (const float* ir, int irLength)
	{
		if (ir == nullptr || irLength <= 0 || partSize <= 0 || fft == nullptr)
			return;

		numParts = juce::jmin (maxParts, (irLength + partSize - 1) / partSize);
		const int bufSize = fftLen * 2;

		for (int k = 0; k < numParts; ++k)
		{
			auto& seg = irFreq[(size_t) k];
			std::fill (seg.begin(), seg.end(), 0.0f);

			const int offset = k * partSize;
			const int count  = juce::jmin (partSize, irLength - offset);
			std::memcpy (seg.data(), ir + offset, (size_t) count * sizeof (float));

			fft->performRealOnlyForwardTransform (seg.data(), true);
		}

		for (int k = numParts; k < maxParts; ++k)
			std::fill (irFreq[(size_t) k].begin(), irFreq[(size_t) k].end(), 0.0f);

		reset();
		loaded = true;
	}

	// Process audio.  Handles any block size (internally buffers
	// to partition-sized chunks).  Cost is uniform per partition.
	void process (const float* input, float* output, int numSamples)
	{
		if (! loaded || numParts == 0)
		{
			if (input != output)
				std::memcpy (output, input, (size_t) numSamples * sizeof (float));
			return;
		}

		int pos = 0;
		while (pos < numSamples)
		{
			const int toCopy = juce::jmin (numSamples - pos, partSize - bufPos);

			std::memcpy (inputAccum.data() + bufPos, input + pos,
						 (size_t) toCopy * sizeof (float));
			std::memcpy (output + pos, outputBuf.data() + bufPos,
						 (size_t) toCopy * sizeof (float));

			bufPos += toCopy;
			pos    += toCopy;

			if (bufPos >= partSize)
			{
				processPartition();
				bufPos = 0;
			}
		}
	}

	// Process in-place (convenience).
	void processInPlace (float* data, int numSamples)
	{
		process (data, data, numSamples);
	}

	void reset()
	{
		for (auto& dl : dlFreq)
			std::fill (dl.begin(), dl.end(), 0.0f);
		dlPos = 0;

		std::fill (inputAccum.begin(), inputAccum.end(), 0.0f);
		std::fill (outputBuf.begin(), outputBuf.end(), 0.0f);
		std::fill (overlapBuf.begin(), overlapBuf.end(), 0.0f);
		bufPos = 0;
	}

	int getLatency() const noexcept { return partSize; }

private:
	void processPartition()
	{
		const int bufSize = fftLen * 2;

		// ── 1. FFT the input partition ───────────────────────
		auto& curInput = dlFreq[(size_t) dlPos];
		std::fill (curInput.begin(), curInput.end(), 0.0f);
		std::memcpy (curInput.data(), inputAccum.data(),
					 (size_t) partSize * sizeof (float));
		fft->performRealOnlyForwardTransform (curInput.data(), true);

		// ── 2. Multiply-accumulate across all IR partitions ──
		std::fill (accumBuf.begin(), accumBuf.end(), 0.0f);
		float* Y = accumBuf.data();

		for (int k = 0; k < numParts; ++k)
		{
			const int dlIdx = ((dlPos - k) % maxParts + maxParts) % maxParts;
			const float* X = dlFreq[(size_t) dlIdx].data();
			const float* H = irFreq[(size_t) k].data();

			for (int b = 0; b < numBins; ++b)
			{
				const int ri = b * 2;
				const int ii = ri + 1;
				Y[ri] += X[ri] * H[ri] - X[ii] * H[ii];
				Y[ii] += X[ri] * H[ii] + X[ii] * H[ri];
			}
		}

		// ── 3. IFFT ─────────────────────────────────────────
		fft->performRealOnlyInverseTransform (accumBuf.data());

		// ── 4. Overlap-add ──────────────────────────────────
		for (int i = 0; i < partSize; ++i)
			outputBuf[(size_t) i] = accumBuf[(size_t) i] + overlapBuf[(size_t) i];

		std::memcpy (overlapBuf.data(), accumBuf.data() + partSize,
					 (size_t) partSize * sizeof (float));

		// ── 5. Advance delay line ───────────────────────────
		dlPos = (dlPos + 1) % maxParts;
	}

	int partSize  = 0;
	int fftOrder  = 0;
	int fftLen    = 0;
	int numBins   = 0;
	int maxParts  = 0;
	int numParts  = 0;
	bool loaded   = false;

	std::unique_ptr<juce::dsp::FFT> fft;

	std::vector<std::vector<float>> irFreq;   // IR segments (freq domain)
	std::vector<std::vector<float>> dlFreq;   // Input delay line (freq domain)
	int dlPos = 0;

	std::vector<float> accumBuf;              // FFT scratch for multiply-accum
	std::vector<float> inputAccum;            // time-domain input partition
	std::vector<float> outputBuf;             // time-domain output partition
	std::vector<float> overlapBuf;            // overlap-add tail
	int bufPos = 0;                           // position within current partition
};
