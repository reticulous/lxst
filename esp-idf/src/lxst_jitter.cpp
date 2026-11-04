#include "lxst_jitter.h"
#include "lxst_mem.h"

#include <cstring>

bool LxstJitter::init(const LxstJitterCfg& cfg)
{
    release();
    m_cfg = cfg;
    if (m_cfg.frameMs < 1) m_cfg.frameMs = 1;
    if (m_cfg.maxFrameBytes < 1) m_cfg.maxFrameBytes = 1;
    m_startSlots = (m_cfg.startMs + m_cfg.frameMs - 1) / m_cfg.frameMs;
    if (m_startSlots < 1) m_startSlots = 1;
    if (m_startSlots > MAX_SLOTS) m_startSlots = MAX_SLOTS;
    int cap = (m_cfg.maxMs + m_cfg.frameMs - 1) / m_cfg.frameMs;
    if (cap < m_startSlots) cap = m_startSlots;
    if (cap > MAX_SLOTS) cap = MAX_SLOTS;
    m_slots = (Slot*)lxstAlloc(sizeof(Slot) * (size_t)cap);
    m_data = (uint8_t*)lxstAlloc((size_t)cap * (size_t)m_cfg.maxFrameBytes);
    if (!m_slots || !m_data) { release(); return false; }
    m_cap = cap;
    return true;
}

void LxstJitter::release()
{
    if (m_slots) lxstFree(m_slots);
    if (m_data) lxstFree(m_data);
    m_slots = nullptr;
    m_data = nullptr;
    m_cap = m_head = m_count = 0;
    m_haveRef = m_playing = m_ended = false;
}

void LxstJitter::appendSlot(uint16_t seq, const uint8_t* f, uint16_t len, int* dropped)
{
    if (m_count == m_cap) {               /* full: the oldest slot goes */
        if (dropped && m_slots[m_head].len) (*dropped)++;
        m_head = (m_head + 1) % m_cap;
        m_count--;
    }
    int i = idx(m_count);
    m_slots[i].seq = seq;
    m_slots[i].len = f ? len : 0;
    if (f) memcpy(m_data + (size_t)i * m_cfg.maxFrameBytes, f, len);
    m_count++;
    m_next = (uint16_t)(seq + 1);
    m_haveRef = true;
}

LxstJitter::Push LxstJitter::push(uint16_t seq, const uint8_t* f, uint16_t len,
                                  uint32_t nowMs, int* dropped)
{
    if (!m_cap || !f || !len || len > m_cfg.maxFrameBytes) return PUSH_BAD;
    /* Only a frame that extends the sequence is news of a following spurt;
     * a back-fill, late or duplicate frame belongs to the one that ended. */
    if (!m_cfg.sequenced || !m_haveRef) {
        appendSlot(m_cfg.sequenced ? seq : m_next, f, len, dropped);
        m_lastArrival = nowMs;
        m_ended = false;
        return PUSH_OK;
    }
    int d = (int16_t)(uint16_t)(seq - m_next);
    if (d < 0) {
        int back = -d;
        if (back <= m_count) {
            int i = idx(m_count - back);
            if (m_slots[i].seq != seq || m_slots[i].len) return PUSH_DUP;
            m_slots[i].len = len;
            memcpy(m_data + (size_t)i * m_cfg.maxFrameBytes, f, len);
            m_lastArrival = nowMs;
            return PUSH_OK;
        }
        if (back <= LATE_WINDOW) return PUSH_LATE;
    }
    if (d >= 0 && d <= MAX_GAP) {
        for (int k = 0; k < d; k++) appendSlot((uint16_t)(m_next), nullptr, 0, dropped);
    }                                     /* else the sender restarted its count */
    appendSlot(seq, f, len, dropped);
    m_lastArrival = nowMs;
    m_ended = false;
    return PUSH_OK;
}

LxstJitter::Pop LxstJitter::pop(uint32_t nowMs, const uint8_t** f, uint16_t* len, bool* underrun)
{
    if (underrun) *underrun = false;
    bool quiet = (uint32_t)(nowMs - m_lastArrival) >= (uint32_t)m_cfg.holdMs;
    if (!m_playing) {
        if (m_count >= m_startSlots || (m_count && (m_ended || quiet))) m_playing = true;
        else if (!m_count && (m_ended || quiet)) return POP_RELEASE;
        else return POP_WAIT;
    }
    if (!m_count) {
        if (m_ended || quiet) { m_playing = false; return POP_RELEASE; }
        m_playing = false;
        if (underrun) *underrun = true;
        return POP_WAIT;
    }
    int i = m_head;
    m_head = (m_head + 1) % m_cap;
    m_count--;
    if (!m_slots[i].len) return POP_CONCEAL;
    *f = m_data + (size_t)i * m_cfg.maxFrameBytes;
    *len = m_slots[i].len;
    return POP_FRAME;
}
