#pragma once

// ============================================================================
// DspDebugLog.h — RT-safe DSP event logger for DISP-TR diagnostics
//
// Captures per-block parameter snapshots, reverse rebuild timings,
// crossfade events, and coefficient updates into a lock-free ring buffer.
// Auto-dumps to CSV on Desktop when the processor is destroyed.
//
// Usage:
//   #define DISPTR_DSP_DEBUG_LOG 1   // enable logging (0 = no overhead)
//
// In processBlock:
//   DSP_LOG_BLOCK(log, blockSize, sampleRate, stages, series, freq, shape, decay, reverse, inv);
//
// In rebuildRvsConvolutionIfNeeded:
//   DSP_LOG_REBUILD_BEGIN();
//   ... build IR ...
//   DSP_LOG_REBUILD_END(log, irLength, stages, series, freq, shape, decay);
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
    enum Type : uint8_t { Block = 0, Rebuild, Crossfade };

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
    float    decay;
    bool     reverse;
    bool     inv;

    // Rebuild fields
    double   rebuildDurationUs;
    int      irLength;

    // Crossfade fields
    int      prevSeries;
    int      newSeries;
    int      fadeSamples;
};

class DspDebugLog
{
public:
    static constexpr int kRingSize = 16384;  // power-of-2

    DspDebugLog() = default;

    void logBlock (double durationUs, int blockSize, double sampleRate,
                   int stages, int series, float freq, float shape, float decay,
                   bool reverse, bool inv) noexcept
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
        e.decay           = decay;
        e.reverse         = reverse;
        e.inv             = inv;
        e.rebuildDurationUs = 0.0;
        e.irLength        = 0;
        e.prevSeries      = 0;
        e.newSeries       = 0;
        e.fadeSamples     = 0;
    }

    void logRebuild (double rebuildDurationUs, int irLength,
                     int stages, int series, float freq, float shape, float decay) noexcept
    {
        const int idx = writeIndex.fetch_add (1, std::memory_order_relaxed) & (kRingSize - 1);
        auto& e = ring[idx];
        e.timestampSec      = juce::Time::getMillisecondCounterHiRes() * 0.001;
        e.type              = DspLogEntry::Rebuild;
        e.blockDurationUs   = 0.0;
        e.blockSize         = 0;
        e.cpuPercent        = 0.0f;
        e.stages            = stages;
        e.series            = series;
        e.freq              = freq;
        e.shape             = shape;
        e.decay             = decay;
        e.reverse           = true;
        e.inv               = false;
        e.rebuildDurationUs = rebuildDurationUs;
        e.irLength          = irLength;
        e.prevSeries        = 0;
        e.newSeries         = 0;
        e.fadeSamples       = 0;
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
        e.decay             = 0.0f;
        e.reverse           = false;
        e.inv               = false;
        e.rebuildDurationUs = 0.0;
        e.irLength          = 0;
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
                               "stages,series,freq,shape,decay,reverse,inv,"
                               "rebuild_duration_us,ir_length,prev_series,new_series,fade_samples\n",
                               false, false, nullptr);

            const int total = juce::jmin (writeIndex.load (std::memory_order_relaxed), kRingSize);
            const int startIdx = writeIndex.load (std::memory_order_relaxed) - total;

            for (int i = 0; i < total; ++i)
            {
                const auto& e = ring[(startIdx + i) & (kRingSize - 1)];
                const char* typeName = (e.type == DspLogEntry::Block)     ? "BLOCK"
                                     : (e.type == DspLogEntry::Rebuild)   ? "REBUILD"
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
                     << juce::String (e.decay, 4) << ","
                     << (e.reverse ? 1 : 0) << ","
                     << (e.inv ? 1 : 0) << ","
                     << juce::String (e.rebuildDurationUs, 2) << ","
                     << e.irLength << ","
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
#define DSP_LOG_BLOCK_END(log, nSamples, sr, stg, ser, frq, shp, dcy, rvs, inv)  do { \
        const auto _dspLogEnd = juce::Time::getHighResolutionTicks(); \
        const double _dspLogUs = juce::Time::highResolutionTicksToSeconds (_dspLogEnd - _dspLogStart) * 1000000.0; \
        (log).logBlock (_dspLogUs, (nSamples), (sr), (stg), (ser), (frq), (shp), (dcy), (rvs), (inv)); \
    } while(0)

#define DSP_LOG_REBUILD_BEGIN()  const auto _rebuildStart = juce::Time::getHighResolutionTicks()
#define DSP_LOG_REBUILD_END(log, irLen, stg, ser, frq, shp, dcy)  do { \
        const auto _rebuildEnd = juce::Time::getHighResolutionTicks(); \
        const double _rebuildUs = juce::Time::highResolutionTicksToSeconds (_rebuildEnd - _rebuildStart) * 1000000.0; \
        (log).logRebuild (_rebuildUs, (irLen), (stg), (ser), (frq), (shp), (dcy)); \
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
#define DSP_LOG_BLOCK_END(log, nSamples, sr, stg, ser, frq, shp, dcy, rvs, inv) ((void)0)
#define DSP_LOG_REBUILD_BEGIN()                                            ((void)0)
#define DSP_LOG_REBUILD_END(log, irLen, stg, ser, frq, shp, dcy)          ((void)0)
#define DSP_LOG_CROSSFADE(log, prev, next, samples)                        ((void)0)

#endif // DISPTR_DSP_DEBUG_LOG
