/**
 * lxst_batch.h — the transmit batcher. Frames are collected straight into an
 * LXST_EV_BATCH message ([op][count] then count × [len u16][frame]) and the
 * message is handed out when the profile's rule says so:
 *
 *   - before adding a frame that would take base + Σ(len + per_frame) past
 *     budget (budget 0: no byte limit); a frame always goes into an empty batch;
 *   - once the batch holds max_frames frames;
 *   - on flush() (end of a talk spurt).
 *
 * Host-portable.
 */
#pragma once

#include <cstdint>
#include <cstddef>

class LxstBatcher {
public:
    typedef void (*EmitFn)(void* ctx, const uint8_t* msg, size_t len);

    ~LxstBatcher() { release(); }

    /** (Re)configure; drops any partial batch. maxFrames is clamped to
     *  1..LXST_MAX_BATCH. Returns false when the buffer cannot be allocated. */
    bool init(uint16_t budget, uint8_t base, uint8_t perFrame, uint8_t maxFrames,
              uint16_t maxFrameBytes);
    void release();

    /** Add one frame, emitting whatever the rule completes. */
    void add(const uint8_t* frame, uint16_t len, EmitFn emit, void* ctx);
    /** Emit the partial batch, if any. */
    void flush(EmitFn emit, void* ctx);
    /** Drop the partial batch. */
    void clear();

    int count() const { return m_count; }

private:
    uint8_t* m_buf = nullptr;
    size_t   m_cap = 0, m_len = 0;
    uint32_t m_cost = 0;
    int      m_count = 0;
    uint16_t m_budget = 0, m_maxFrameBytes = 0;
    uint8_t  m_base = 0, m_per = 0, m_max = 1;
};
