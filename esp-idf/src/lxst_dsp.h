/**
 * lxst_dsp.h — the session's small signal helpers: block level for the voice
 * gate, the test-tone source, and the per-spurt receive analysis (level, peak
 * and dominant frequency by zero-crossing rate). Host-portable.
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace lxsess {

/** Root-mean-square (RMS) level of a PCM16 block, linear, 0 (silence) to 1 (full scale). */
float blockRms(const int16_t* pcm, int n);

/** A linear RMS level in dB relative to full scale (−120 for silence). */
float rmsDbfs(float rms);

/** RMS level of a PCM16 block in dB relative to full scale (−120 for silence). */
inline float blockDbfs(const int16_t* pcm, int n) { return rmsDbfs(blockRms(pcm, n)); }

/** Sine source peaking at peakDb dBFS (−12 unless set). */
struct Tone {
    double phase = 0;
    float  peakDb = -12.0f;
    void fill(int16_t* pcm, int n, int hz, int rate);
};

/**
 * Microphone conditioner, run on each capture block in place, in this order:
 * a 100 Hz high-pass (2nd-order Butterworth), automatic gain control (AGC)
 * towards TARGET_DBFS RMS between 0 dB and MAX_GAIN_DB, and a look-ahead peak
 * limiter whose output never passes CEILING. The AGC gain holds while the
 * high-passed level is below GATE_DBFS, so pauses are not pumped up. State
 * carries across blocks; reset() at every new capture stream or rate.
 * With agc false the gain stays at unity (high-pass and limiter still run).
 */
struct MicChain {
    static constexpr float TARGET_DBFS = -18.0f;
    static constexpr float MAX_GAIN_DB = 24.0f;
    static constexpr float GATE_DBFS   = -50.0f;
    static constexpr float HPF_HZ      = 100.0f;
    static constexpr float ATTACK_MS   = 15.0f;
    static constexpr float RELEASE_MS  = 400.0f;
    static constexpr float LEVEL_MS    = 100.0f;   /* AGC level detector */
    static constexpr float GATE_MS     = 20.0f;    /* gate's short-term level */
    static constexpr float LIM_RELEASE_MS = 50.0f;
    static constexpr int   CEILING     = 29204;    /* −1 dBFS */
    static constexpr int   MAX_LOOK    = 64;       /* look-ahead samples, ≥ 1 ms at 48 kHz */

    bool agc = true;
    uint32_t clamped = 0;                          /* samples the final clamp changed */
    float inRms = 0;                               /* last block's RMS after the high-pass,
                                                      before the gain, linear 0..1 */

    void  reset(int rate);
    void  process(int16_t* pcm, int n);
    float gainDb() const;                          /* the AGC gain in force */

    int   rate = 0;
    /* high-pass biquad (transposed direct form II) */
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    /* AGC */
    float ms = 0, msGate = 0, gain = 1, want = 1;
    float aLevel = 0, aGate = 0, aAtt = 0, aRel = 0;
    float gateMs = 0, targetMs = 0, maxGain = 1;
    int   sub = 16, subLeft = 0;
    /* limiter: delay line, sliding minimum (monotonic queue) of the needed
     * gain in Q16, and a box average of that minimum over the look-ahead */
    int      look = 1, pos = 0;
    float    dly[MAX_LOOK] = {};
    uint32_t box[MAX_LOOK] = {};
    uint32_t boxSum = 0;
    uint32_t qv[MAX_LOOK] = {};
    uint32_t qi[MAX_LOOK] = {};
    int      qh = 0, qn = 0;
    uint32_t idx = 0;
    float    lim = 1, aLimRel = 0;
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
