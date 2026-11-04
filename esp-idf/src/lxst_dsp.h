/**
 * lxst_dsp.h — the session's small signal helpers: block level for the voice
 * gate, the test-tone source, and the per-spurt receive analysis (level, peak
 * and dominant frequency by zero-crossing rate). Host-portable.
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace lxsess {

/** Root-mean-square (RMS) level of a PCM16 block in dB relative to full scale (−120 for silence). */
float blockDbfs(const int16_t* pcm, int n);

/** Sine source at about −12 dBFS peak. */
struct Tone {
    double phase = 0;
    void fill(int16_t* pcm, int n, int hz, int rate);
};

/** What one talk spurt sounded like. Blocks below GATE_DBFS count towards
 *  duration and level but not towards the frequency estimate, so pauses do not
 *  drag it down. */
struct SpurtStats {
    static constexpr float GATE_DBFS = -50.0f;
    static constexpr int   HYST = 64;      /* zero-crossing hysteresis, LSB */

    uint64_t samples = 0;
    double   sumsq = 0;
    int      peak = 0;
    uint64_t zcSamples = 0, zc = 0;
    int      zsign = 0;
    uint32_t frames = 0, concealed = 0, late = 0;

    void reset() { *this = SpurtStats(); }
    void addBlock(const int16_t* pcm, int n);
    float rmsDbfs() const;
    float peakDbfs() const;
    int   freqHz(int rate) const;
};

} // namespace lxsess
