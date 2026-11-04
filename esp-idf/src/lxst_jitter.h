/**
 * lxst_jitter.h — one speaker's receive jitter buffer. It holds ENCODED
 * frames, one slot per frame time, so a gap in the sequence is a slot with no
 * frame (played as concealment) and a frame that arrives late but before its
 * slot has played fills it.
 *
 * Ordering. Sequenced: frames are placed by their 16-bit sequence number,
 * wrap-aware. A frame up to MAX_GAP ahead of the next expected one leaves
 * empty slots for the missing ones; one behind the playhead (up to LATE_WINDOW
 * back) is late and dropped; a jump beyond either window is taken as the
 * sender restarting its count and is appended. Unsequenced: arrival order.
 *
 * Playout, one frame per pop():
 *   PREBUF   waits until start_ms of slots are buffered, then plays. It also
 *            starts at once with what it has when the talker ended (talkEnd),
 *            or when nothing has arrived for hold_ms (a spurt shorter than
 *            the start depth).
 *   PLAYING  pops the head slot: a frame, or concealment for an empty slot.
 *            Drained: released if the talker ended or nothing has arrived for
 *            hold_ms; otherwise an underrun, back to PREBUF.
 *   A frame that extends the sequence after talkEnd clears it (the next
 *   spurt followed on); a back-fill, late or duplicate frame does not, and
 *   neither late nor duplicate frames count as arrivals for the hold.
 *   The buffer never holds more than max_ms: the oldest slot is dropped.
 *
 * Host-portable.
 */
#pragma once

#include <cstdint>
#include <cstddef>

struct LxstJitterCfg {
    int  frameMs;
    int  maxFrameBytes;
    int  startMs;
    int  maxMs;
    int  holdMs;
    bool sequenced;
};

class LxstJitter {
public:
    static constexpr int MAX_GAP     = 64;
    static constexpr int LATE_WINDOW = 1024;
    static constexpr int MAX_SLOTS   = 4096;

    enum Push { PUSH_OK, PUSH_LATE, PUSH_DUP, PUSH_BAD };
    enum Pop  { POP_WAIT, POP_FRAME, POP_CONCEAL, POP_RELEASE };

    ~LxstJitter() { release(); }

    bool init(const LxstJitterCfg& cfg);
    void release();

    /** Add a frame received at now_ms. `dropped` counts frames evicted to stay
     *  within max_ms (an evicted empty slot is not counted). */
    Push push(uint16_t seq, const uint8_t* frame, uint16_t len, uint32_t nowMs, int* dropped);
    /** The talker signalled the end of its spurt. */
    void talkEnd() { m_ended = true; }

    /** One frame time of playout. On POP_FRAME, *frame and *len give the frame,
     *  valid until the next push(). *underrun is set when a drained buffer
     *  went back to PREBUF. */
    Pop pop(uint32_t nowMs, const uint8_t** frame, uint16_t* len, bool* underrun);

    bool playing() const   { return m_playing; }
    int  slots() const     { return m_count; }
    int  bufferedMs() const { return m_count * m_cfg.frameMs; }
    int  capacity() const  { return m_cap; }

private:
    struct Slot { uint16_t seq; uint16_t len; };   /* len 0: missing */

    void appendSlot(uint16_t seq, const uint8_t* f, uint16_t len, int* dropped);
    int  idx(int i) const { return (m_head + i) % m_cap; }

    LxstJitterCfg m_cfg{};
    Slot*    m_slots = nullptr;
    uint8_t* m_data = nullptr;
    int      m_cap = 0, m_head = 0, m_count = 0;
    int      m_startSlots = 1;
    bool     m_haveRef = false;
    uint16_t m_next = 0;          /* sequence the next appended slot takes */
    bool     m_playing = false;
    bool     m_ended = false;
    uint32_t m_lastArrival = 0;
};
