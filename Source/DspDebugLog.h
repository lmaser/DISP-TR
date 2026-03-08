#pragma once

// ============================================================================
// DspDebugLog.h — RT-safe DSP event logger for DISP-TR diagnostics
//
// Captures per-block parameter snapshots and crossfade events
// into a lock-free ring buffer.
// Auto-dumps to CSV on Desktop when the processor is destroyed.
//
// Usage:
//   #define DISPTR_DSP_DEBUG_LOG 1   // enable logging (0 = no overhead)
//
// In processBlock:
//   DSP_LOG_BLOCK_BEGIN();
//   ... process ...
//   DSP_LOG_BLOCK_END(log, blockSize, sampleRate, stages, series, freq, shape, inv);
//
// On series crossfade start:
//   DSP_LOG_CROSSFADE(log, prevSeries, newSeries, fadeSamples);
//
// ============================================================================

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>

#ifndef DISPTR_DSP_DEBUG_LOG
 #define DISPTR_DSP_DEBUG_LOG 1
#endif

#if DISPTR_DSP_DEBUG_LOG

struct DspLogEntry
{
    enum Type : uint8_t { Block = 0, Crossfade };

    double   timestampSec;
    Type     type;

    // Block fields
    double   blockDurationUs;
    int      blockSize;
    float    cpuPercent;
    int      stages;
    int      series;
    float    freq;
    float    shape;
    bool     inv;

    // Crossfade fields
    int      prevSeries;
    int      newSeries;
    int      fadeSamples;
};

class DspDebugLog
{
public:
    static constexpr int kRingSize = 16384;

    DspDebugLog() = default;

    void logBlock (double durationUs, int blockSize, double sampleRate,
                   int stages, int series, float freq, float shape,
                   bool inv) noexcept
    {
        const double blockDurUs = (static_cast<double>(blockSize) / sampleRate) * 1'000'000.0;
        const float cpuPct = (blockDurUs > 0.0)
                                 ? static_cast<float>((durationUs / blockDurUs) * 100.0)
                                 : 0.0f;

        const int idx = writeIndex.fetch_add (1, std::memory_order_relaxed) & (kRingSize - 1);
        auto& e = ring[idx];
        e.timestampSec    = juce::Time::getMillisecondCounterHiRes() * 0.001;
        e.type            = DspLogEntry::Block;
        e.blockDurationUs = durationUs;
        e.blockSize       = blockSize;
        e.cpuPercent      = cpuPct;
        e.stages          = stages;
        e.series          = series;
        e.freq            = freq;
        e.shape           = shape;
        e.inv             = inv;
        e.prevSeries      = 0;
        e.newSeries       = 0;
        e.fadeSamples     = 0;
    }

    void logCrossfade (int prevSeries, int newSeries, int fadeSamples) noexcept
    {
        const int idx = writeIndex.fetch_add (1, std::memory_order_relaxed) & (kRingSize - 1);
        auto& e = ring[idx];
        e.timestampSec      = juce::Time::getMillisecondCounterHiRes() * 0.001;
        e.type              = DspLogEntry::Crossfade;
        e.blockDurationUs   = 0.0;
        e.blockSize         = 0;
        e.cpuPercent        = 0.0f;
        e.stages            = 0;
        e.series            = 0;
        e.freq              = 0.0f;
        e.shape             = 0.0f;
        e.inv               = false;
        e.prevSeries        = prevSeries;
        e.newSeries         = newSeries;
        e.fadeSamples       = fadeSamples;
    }

    bool dumpToFile (const juce::String& filePath) const
    {
        juce::File f (filePath);
        if (auto stream = f.createOutputStream())
        {
            stream->writeText ("timestamp_s,type,block_duration_us,block_size,cpu_percent,"
                               "stages,series,freq,shape,inv,"
                               "prev_series,new_series,fade_samples\n",
                               false, false, nullptr);

            const int total = juce::jmin (writeIndex.load (std::memory_order_relaxed), kRingSize);
            const int startIdx = writeIndex.load (std::memory_order_relaxed) - total;

            for (int i = 0; i < total; ++i)
            {
                const auto& e = ring[(startIdx + i) & (kRingSize - 1)];
                const char* typeName = (e.type == DspLogEntry::Block)     ? "BLOCK"
                                     : (e.type == DspLogEntry::Crossfade) ? "CROSSFADE"
                                     : "UNKNOWN";

                juce::String line;
                line << juce::String (e.timestampSec, 6) << ","
                     << typeName << ","
                     << juce::String (e.blockDurationUs, 2) << ","
                     << e.blockSize << ","
                     << juce::String (e.cpuPercent, 3) << ","
                     << e.stages << ","
                     << e.series << ","
                     << juce::String (e.freq, 2) << ","
                     << juce::String (e.shape, 4) << ","
                     << (e.inv ? 1 : 0) << ","
                     << e.prevSeries << ","
                     << e.newSeries << ","
                     << e.fadeSamples << "\n";
                stream->writeText (line, false, false, nullptr);
            }
            stream->flush();
            return true;
        }
        return false;
    }

    void enableDesktopAutoDump (const juce::String& filename = "disptr_dsp_debug.csv")
    {
        auto desktop = juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
        autoDumpPath = desktop.getChildFile (filename).getFullPathName();
    }

    ~DspDebugLog()
    {
        if (autoDumpPath.isNotEmpty() && writeIndex.load (std::memory_order_relaxed) > 0)
            dumpToFile (autoDumpPath);
    }

private:
    DspLogEntry ring[kRingSize] {};
    std::atomic<int> writeIndex { 0 };
    juce::String autoDumpPath;
};

#define DSP_LOG_BLOCK_BEGIN()  const auto _dspLogStart = juce::Time::getHighResolutionTicks()
#define DSP_LOG_BLOCK_END(log, nSamples, sr, stg, ser, frq, shp, inv)  do { \
        const auto _dspLogEnd = juce::Time::getHighResolutionTicks(); \
        const double _dspLogUs = juce::Time::highResolutionTicksToSeconds (_dspLogEnd - _dspLogStart) * 1000000.0; \
        (log).logBlock (_dspLogUs, (nSamples), (sr), (stg), (ser), (frq), (shp), (inv)); \
    } while(0)

#define DSP_LOG_CROSSFADE(log, prev, next, samples)  (log).logCrossfade ((prev), (next), (samples))

#else // DISPTR_DSP_DEBUG_LOG == 0

class DspDebugLog
{
public:
    void enableDesktopAutoDump (const juce::String& = {}) {}
    bool dumpToFile (const juce::String&) const { return false; }
};

#define DSP_LOG_BLOCK_BEGIN()                                              ((void)0)
#define DSP_LOG_BLOCK_END(log, nSamples, sr, stg, ser, frq, shp, inv)     ((void)0)
#define DSP_LOG_CROSSFADE(log, prev, next, samples)                        ((void)0)

#endif // DISPTR_DSP_DEBUG_LOG
