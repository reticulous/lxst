#include "lxst_tones.h"
#include "lxst.h"

#include <cmath>

namespace lxsess {

namespace {

struct Spec {
    const uint16_t* seg;      /* burst, gap, burst, gap … in ms */
    int             nSeg;
    float           hz[2];    /* 0: unused */
};

const uint16_t RING_SEG[]     = { 400, 200, 400, 2000 };
const uint16_t RINGBACK_SEG[] = { 2000, 5000 };
const uint16_t BUSY_SEG[]     = { 250, 250 };

const Spec* spec(uint8_t kind)
{
    static const Spec ring     = { RING_SEG, 4, { 400.0f, 450.0f } };
    static const Spec ringback = { RINGBACK_SEG, 2, { 382.0f, 0.0f } };
    static const Spec busy     = { BUSY_SEG, 2, { 382.0f, 0.0f } };
    switch (kind) {
    case LXST_TONE_RING:     return &ring;
    case LXST_TONE_RINGBACK: return &ringback;
    case LXST_TONE_BUSY:     return &busy;
    default:                 return nullptr;
    }
}

/* Gain 0..1 at sample `pos` of the cycle. */
float envelope(const Spec& s, uint64_t pos, int rate)
{
    uint64_t start = 0;
    for (int i = 0; i < s.nSeg; i++) {
        uint64_t len = (uint64_t)s.seg[i] * (uint64_t)rate / 1000;
        if (pos < start + len) {
            if (i & 1) return 0.0f;
            uint64_t t = pos - start;
            float ramp = (float)rate * CallTone::RAMP_MS / 1000.0f;
            float g = 1.0f;
            if ((float)t < ramp) g = (float)t / ramp;
            if ((float)(len - t) < ramp) g = std::fmin(g, (float)(len - t) / ramp);
            return g;
        }
        start += len;
    }
    return 0.0f;
}

}  // namespace

uint32_t CallTone::periodMs(uint8_t kind)
{
    const Spec* s = spec(kind);
    if (!s) return 0;
    uint32_t p = 0;
    for (int i = 0; i < s->nSeg; i++) p += s->seg[i];
    return p;
}

bool CallTone::onAt(uint8_t kind, uint32_t ms)
{
    const Spec* s = spec(kind);
    if (!s) return false;
    ms %= periodMs(kind);
    uint32_t start = 0;
    for (int i = 0; i < s->nSeg; i++) {
        if (ms < start + s->seg[i]) return !(i & 1);
        start += s->seg[i];
    }
    return false;
}

void CallTone::set(uint8_t kind)
{
    m_kind = spec(kind) ? kind : 0;
    m_pos = 0;
    m_ph[0] = m_ph[1] = 0;
}

void CallTone::mix(int32_t* acc, int n, int rate)
{
    const Spec* s = spec(m_kind);
    if (!s || n <= 0 || rate <= 0) return;
    uint64_t period = (uint64_t)periodMs(m_kind) * (uint64_t)rate / 1000;
    int parts = s->hz[1] > 0 ? 2 : 1;
    float amp = PEAK / (float)parts;
    /* A rotating phasor per component: one sin/cos per block. */
    float cr[2], ci[2], re[2], im[2], step[2];
    for (int k = 0; k < parts; k++) {
        step[k] = 2.0f * (float)M_PI * s->hz[k] / (float)rate;
        cr[k] = cosf(step[k]);
        ci[k] = sinf(step[k]);
        re[k] = cosf((float)m_ph[k]);
        im[k] = sinf((float)m_ph[k]);
    }
    uint64_t pos = m_pos % period;
    for (int i = 0; i < n; i++) {
        float v = 0;
        for (int k = 0; k < parts; k++) {
            v += im[k];
            float r = re[k] * cr[k] - im[k] * ci[k];
            im[k] = re[k] * ci[k] + im[k] * cr[k];
            re[k] = r;
        }
        float g = envelope(*s, pos, rate);
        if (g > 0) acc[i] += (int32_t)lrintf(amp * g * v);
        if (++pos == period) pos = 0;
    }
    for (int k = 0; k < parts; k++)
        m_ph[k] = std::fmod(m_ph[k] + (double)step[k] * n, 2.0 * M_PI);
    m_pos += (uint64_t)n;
}

}  // namespace lxsess
