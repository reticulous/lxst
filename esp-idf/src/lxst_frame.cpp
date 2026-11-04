#include "lxst_frame.h"

int lxstC2SubframeMs(uint8_t mode)
{
    switch (mode) {
    case LXST_C2_3200: case LXST_C2_2400: return 20;
    case LXST_C2_1600: case LXST_C2_1400: case LXST_C2_1300:
    case LXST_C2_1200: case LXST_C2_700C: return 40;
    default: return 0;
    }
}

int lxstC2SubframeBytes(uint8_t mode)
{
    switch (mode) {
    case LXST_C2_3200: return 8;
    case LXST_C2_2400: return 6;
    case LXST_C2_1600: return 8;
    case LXST_C2_1400: return 7;
    case LXST_C2_1300: return 7;
    case LXST_C2_1200: return 6;
    case LXST_C2_700C: return 4;
    default: return 0;
    }
}

int32_t lxstOpusDurationUs(const uint8_t* packet, size_t len)
{
    if (len < 1) return -1;
    uint8_t toc = packet[0];
    unsigned config = toc >> 3, code = toc & 3;
    int32_t size_us;
    if (config < 12) {                       /* SILK */
        static const int32_t silk[4] = { 10000, 20000, 40000, 60000 };
        size_us = silk[config % 4];
    } else if (config < 16) {                /* hybrid */
        size_us = (config % 2) ? 20000 : 10000;
    } else {                                 /* CELT */
        static const int32_t celt[4] = { 2500, 5000, 10000, 20000 };
        size_us = celt[config % 4];
    }
    int32_t count;
    if (code == 0) count = 1;
    else if (code < 3) count = 2;
    else {
        if (len < 2) return -1;
        count = packet[1] & 0x3F;
    }
    return size_us * count;
}

uint32_t lxstOpusBitrateCeiling(uint8_t profile)
{
    switch (profile) {
    case LXST_OPUS_VOICE_LOW:    return 6000;
    case LXST_OPUS_VOICE_MEDIUM: return 8000;
    case LXST_OPUS_VOICE_HIGH:   return 16000;
    case LXST_OPUS_VOICE_MAX:    return 32000;
    case LXST_OPUS_AUDIO_MIN:    return 8000;
    case LXST_OPUS_AUDIO_LOW:    return 14000;
    case LXST_OPUS_AUDIO_MEDIUM: return 28000;
    case LXST_OPUS_AUDIO_HIGH:   return 56000;
    case LXST_OPUS_AUDIO_MAX:    return 128000;
    default: return 0;
    }
}

uint32_t lxstOpusMaxBytes(uint32_t bitrate_ceiling, uint32_t frame_ms)
{
    uint64_t bits_ms = (uint64_t)bitrate_ceiling * frame_ms;   /* bytes = bits_ms / 8000 */
    return (uint32_t)((bits_ms + 7999) / 8000);
}

int32_t lxstFrameDurationUs(const uint8_t* frame, size_t len)
{
    if (len < 1) return -1;
    if (frame[0] == LXST_CODEC_OPUS) return lxstOpusDurationUs(frame + 1, len - 1);
    if (frame[0] != LXST_CODEC_CODEC2) return -1;
    if (len < 3) return -1;                  /* mode byte and at least one body byte */
    int bytes = lxstC2SubframeBytes(frame[1]);
    if (bytes == 0) return -1;
    size_t body = len - 2;
    if (body % (size_t)bytes) return -1;
    return (int32_t)(body / (size_t)bytes) * lxstC2SubframeMs(frame[1]) * 1000;
}

bool lxstFrameValid(const uint8_t* frame, size_t len, uint8_t codec, uint32_t frame_ms, size_t max_bytes)
{
    if (len < 2 || len > max_bytes) return false;
    if (frame[0] != codec) return false;
    return lxstFrameDurationUs(frame, len) == (int32_t)(frame_ms * 1000);
}
