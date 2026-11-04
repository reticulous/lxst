#include "lxst_sess_profile.h"
#include "lxst.h"

#include <cstdio>

namespace lxsess {

bool opusPreset(uint8_t mode, OpusPreset* o)
{
    uint32_t br = lxstOpusBitrateCeiling(mode);
    if (!br) return false;
    o->bitrate = (int)br;
    o->voip = mode <= LXST_OPUS_VOICE_MAX;
    o->narrow = mode == LXST_OPUS_VOICE_LOW || mode == LXST_OPUS_AUDIO_MIN;
    return true;
}

bool profileValid(const lxst_profile_t& p)
{
    if (p.frame_ms == 0 || p.max_frame_bytes < 2 || p.max_frame_bytes > MAX_FRAME_BYTES)
        return false;
    if (p.batch_max_frames < 1 || p.batch_max_frames > LXST_MAX_BATCH) return false;
    int depth = p.max_jitter_ms ? p.max_jitter_ms : DEFAULT_MAX_JITTER_MS;
    if (p.max_jitter_ms > MAX_JITTER_MS || p.jitter_ms > depth) return false;
    if (p.codec == LXST_CODEC_CODEC2) {
        int sub = lxstC2SubframeMs(p.mode);
        if (!sub || p.frame_ms % sub) return false;
        return 2 + (p.frame_ms / sub) * lxstC2SubframeBytes(p.mode) <= p.max_frame_bytes;
    }
    if (p.codec == LXST_CODEC_OPUS) {
        if (!lxstOpusBitrateCeiling(p.mode)) return false;
        switch (p.frame_ms) {
        case 10: case 20: case 40: case 60: case 80: case 100: case 120: return true;
        default: return false;
        }
    }
    return false;
}

int frameBytesMax(const lxst_profile_t& p)
{
    if (p.rx_any) return MAX_FRAME_BYTES;
    int cap = p.max_frame_bytes < MAX_FRAME_BYTES ? p.max_frame_bytes : MAX_FRAME_BYTES;
    if (p.codec == LXST_CODEC_CODEC2) {
        int sub = lxstC2SubframeMs(p.mode);
        if (sub) {
            int n = 2 + (p.frame_ms / sub) * lxstC2SubframeBytes(p.mode);
            if (n < cap) cap = n;
        }
    }
    return cap < 1 ? 1 : cap;
}

JitterSize jitterSize(const lxst_profile_t& p, int startMs)
{
    JitterSize j;
    j.frameBytes = frameBytesMax(p);
    int frameMs = p.frame_ms ? p.frame_ms : 1;
    long long fit = (long long)(SPEAKER_JITTER_BYTES / (size_t)j.frameBytes) * frameMs;
    j.maxMs = p.max_jitter_ms ? p.max_jitter_ms : DEFAULT_MAX_JITTER_MS;
    if (j.maxMs > fit) j.maxMs = (int)fit;
    if (startMs < 0) startMs = 0;
    j.startMs = startMs < j.maxMs ? startMs : j.maxMs;
    return j;
}

bool rxFrameOk(const lxst_profile_t& p, const uint8_t* f, size_t len)
{
    if (!f || len < 2) return false;
    if (!p.rx_any) {
        size_t cap = p.max_frame_bytes < MAX_FRAME_BYTES ? p.max_frame_bytes : MAX_FRAME_BYTES;
        return lxstFrameValid(f, len, p.codec, p.frame_ms, cap)
               && (p.codec != LXST_CODEC_CODEC2 || f[1] == p.mode);
    }
    if (len > (size_t)MAX_FRAME_BYTES) return false;
    int32_t us = lxstFrameDurationUs(f, len);
    if (us <= 0) return false;
    return f[0] != LXST_CODEC_OPUS || us <= 120000;   /* one decoded unit holds 120 ms */
}

TxSel txSelect(uint8_t txMode, TxSel setting)
{
    if (txMode == LXST_TX_OPEN) return TXSEL_OPEN;
    if (txMode == LXST_TX_PTT) return TXSEL_PTT;
    return setting;
}

void profileName(const lxst_profile_t& p, char* out, size_t n)
{
    if (!n) return;
    if (p.codec == LXST_CODEC_CODEC2) {
        static const char* names[] = { "700C", "1200", "1300", "1400", "1600", "2400", "3200" };
        const char* m = p.mode < 7 ? names[p.mode] : "?";
        snprintf(out, n, "Codec2 %s, %u ms", m, (unsigned)p.frame_ms);
        return;
    }
    if (p.codec == LXST_CODEC_OPUS) {
        OpusPreset o;
        if (opusPreset(p.mode, &o)) {
            snprintf(out, n, "Opus %s %d kbps, %u ms", o.voip ? "voice" : "music",
                     o.bitrate / 1000, (unsigned)p.frame_ms);
            return;
        }
    }
    snprintf(out, n, "codec %u mode %u, %u ms", (unsigned)p.codec, (unsigned)p.mode,
             (unsigned)p.frame_ms);
}

} // namespace lxsess
