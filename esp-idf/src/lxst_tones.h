/**
 * lxst_tones.h — the call progress tones (LXST_TONE_*), generated block by
 * block and added into the playout mix. Host-portable.
 *
 *   ring       400 Hz + 450 Hz   0.4 s on, 0.2 s off, 0.4 s on, 2 s off
 *   ring-back  382 Hz            2 s on, 5 s off
 *   busy       382 Hz            0.25 s on, 0.25 s off
 *
 * Peak about −12 dBFS (the ring's two components share it). Every burst is
 * ramped in and out over a few milliseconds so the cadence does not click. A
 * tone starts at the beginning of its first burst when set.
 */
#pragma once

#include <cstdint>

namespace lxsess {

class CallTone {
public:
    static constexpr int RAMP_MS = 5;
    static constexpr float PEAK = 32767.0f * 0.251f;   /* −12 dBFS */

    /** Select LXST_TONE_*; an unknown value is none. Setting a tone restarts
     *  its cadence, even the same one. */
    void set(uint8_t kind);
    uint8_t kind() const { return m_kind; }
    bool active() const { return m_kind != 0; }

    /** Add the next n samples at `rate` into acc. */
    void mix(int32_t* acc, int n, int rate);

    /** The cadence's period, and whether ms into it is inside a burst. */
    static uint32_t periodMs(uint8_t kind);
    static bool onAt(uint8_t kind, uint32_t ms);

private:
    uint8_t  m_kind = 0;
    uint64_t m_pos = 0;           /* samples since the tone started */
    double   m_ph[2] = { 0, 0 };
};

}  // namespace lxsess
