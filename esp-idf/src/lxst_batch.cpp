#include "lxst_batch.h"
#include "lxst.h"
#include "lxst_mem.h"

#include <cstring>

bool LxstBatcher::init(uint16_t budget, uint8_t base, uint8_t perFrame, uint8_t maxFrames,
                       uint16_t maxFrameBytes)
{
    if (maxFrames < 1) maxFrames = 1;
    if (maxFrames > LXST_MAX_BATCH) maxFrames = LXST_MAX_BATCH;
    size_t cap = 2 + (size_t)maxFrames * (2 + maxFrameBytes);
    if (cap > m_cap) {
        release();
        m_buf = (uint8_t*)lxstAlloc(cap);
        if (!m_buf) return false;
        m_cap = cap;
    }
    m_budget = budget;
    m_base = base;
    m_per = perFrame;
    m_max = maxFrames;
    m_maxFrameBytes = maxFrameBytes;
    clear();
    return true;
}

void LxstBatcher::release()
{
    if (m_buf) lxstFree(m_buf);
    m_buf = nullptr;
    m_cap = 0;
    m_len = 0;
    m_count = 0;
    m_cost = 0;
}

void LxstBatcher::clear()
{
    m_count = 0;
    m_cost = 0;
    m_len = 2;
    if (m_buf) { m_buf[0] = LXST_EV_BATCH; m_buf[1] = 0; }
}

void LxstBatcher::flush(EmitFn emit, void* ctx)
{
    if (!m_buf || !m_count) return;
    m_buf[1] = (uint8_t)m_count;
    emit(ctx, m_buf, m_len);
    clear();
}

void LxstBatcher::add(const uint8_t* frame, uint16_t len, EmitFn emit, void* ctx)
{
    if (!m_buf || len > m_maxFrameBytes) return;
    uint32_t cost = (uint32_t)len + m_per;
    if (m_count && m_budget && m_base + m_cost + cost > m_budget) flush(emit, ctx);
    m_buf[m_len++] = (uint8_t)(len & 0xFF);
    m_buf[m_len++] = (uint8_t)(len >> 8);
    memcpy(m_buf + m_len, frame, len);
    m_len += len;
    m_cost += cost;
    m_count++;
    if (m_count >= m_max) flush(emit, ctx);
}
