/* Host test for the LXST frame helpers against partyline 1.0.5's own frame rules.
 *
 *   g++ -std=c++17 -Wall -Wextra -I../../include test_frame.cpp ../../src/lxst_frame.cpp -o /tmp/test_frame && /tmp/test_frame
 */
#include "lxst_frame.h"
#include "frame_vectors.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main()
{
    /* Codec2 sub-frames: 20 ms for 3200/2400, 40 ms otherwise; bytes per pycodec2. */
    struct { uint8_t mode; int ms; int bytes; } c2[] = {
        { LXST_C2_3200, 20, 8 }, { LXST_C2_2400, 20, 6 }, { LXST_C2_1600, 40, 8 }, { LXST_C2_1400, 40, 7 },
        { LXST_C2_1300, 40, 7 }, { LXST_C2_1200, 40, 6 }, { LXST_C2_700C, 40, 4 },
    };
    for (const auto& m : c2) {
        CHECK(lxstC2SubframeMs(m.mode) == m.ms);
        CHECK(lxstC2SubframeBytes(m.mode) == m.bytes);
    }
    CHECK(lxstC2SubframeMs(7) == 0 && lxstC2SubframeBytes(7) == 0);
    CHECK(lxstC2SubframeMs(0xff) == 0 && lxstC2SubframeBytes(0xff) == 0);

    /* Opus TOC durations, every TOC byte, with and without a count byte. */
    size_t tocs = 0;
    for (const auto& t : TOC_VECS) {
        int32_t got = lxstOpusDurationUs(t.b, t.n);
        if (got != t.us) {
            std::printf("FAIL toc %02x/%02x n=%u: got %d want %d\n", t.b[0], t.b[1], t.n, got, t.us);
            failures++;
        }
        tocs++;
    }
    CHECK(lxstOpusDurationUs(nullptr, 0) == -1);
    {
        const uint8_t celt_2_5[] = { 0x80 };          /* config 16: CELT 2.5 ms, code 0 */
        CHECK(lxstOpusDurationUs(celt_2_5, 1) == 2500);
        const uint8_t code3[] = { 0x0b, 0x03 };       /* config 1: SILK 20 ms, code 3, 3 frames */
        CHECK(lxstOpusDurationUs(code3, 2) == 60000);
        const uint8_t code3_short[] = { 0x0b };
        CHECK(lxstOpusDurationUs(code3_short, 1) == -1);
    }

    /* LXST's Opus ceilings, and the partyline byte caps they give. */
    for (const auto& p : OPUS_PROFILE_VECS) CHECK(lxstOpusBitrateCeiling(p.profile) == p.ceiling);
    CHECK(lxstOpusBitrateCeiling(9) == 0);
    CHECK(1 + lxstOpusMaxBytes(16000, 20) == 41);
    CHECK(1 + lxstOpusMaxBytes(8000, 60) == 61);
    CHECK(1 + lxstOpusMaxBytes(6000, 60) == 46);
    CHECK(1 + lxstOpusMaxBytes(14000, 40) == 71);
    CHECK(1 + lxstOpusMaxBytes(28000, 40) == 141);
    CHECK(1 + lxstOpusMaxBytes(56000, 40) == 281);
    CHECK(lxstOpusMaxBytes(6000, 25) == 19);          /* 18.75 rounds up */

    /* Frame duration and validity for every profile, as partyline decides them. */
    size_t frames = 0, valid = 0;
    std::vector<uint8_t> f;
    for (const auto& t : FRAME_VECS) {
        f.assign(t.n, 0);
        if (t.prefix_n) std::memcpy(f.data(), t.prefix, t.prefix_n);
        int32_t us = lxstFrameDurationUs(f.data(), f.size());
        bool ok = lxstFrameValid(f.data(), f.size(), t.codec, t.frame_ms, t.limit);
        if (us != t.us || ok != t.valid) {
            std::printf("FAIL frame codec %u %u ms limit %u [%02x %02x %02x] n=%u: us %d/%d valid %d/%d\n",
                        t.codec, t.frame_ms, t.limit, t.prefix[0], t.prefix[1], t.prefix[2], t.n,
                        us, t.us, ok, t.valid);
            failures++;
        }
        frames++;
        valid += t.valid;
    }
    CHECK(valid > 100);

    /* The worked sizes: a c2-3200 frame is 82 bytes, a c2-700 frame 42. */
    {
        std::vector<uint8_t> c3200(82, 0), c700(42, 0);
        c3200[0] = LXST_CODEC_CODEC2; c3200[1] = LXST_C2_3200;
        c700[0] = LXST_CODEC_CODEC2;  c700[1] = LXST_C2_700C;
        CHECK(lxstFrameDurationUs(c3200.data(), 82) == 200000);
        CHECK(lxstFrameValid(c3200.data(), 82, LXST_CODEC_CODEC2, 200, 400));
        CHECK(lxstFrameValid(c700.data(), 42, LXST_CODEC_CODEC2, 400, 400));
        CHECK(!lxstFrameValid(c700.data(), 42, LXST_CODEC_CODEC2, 400, 41));
        CHECK(!lxstFrameValid(c700.data(), 42, LXST_CODEC_OPUS, 400, 400));
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("test_frame: all passed (%zu TOC cases, %zu frame cases)\n", tocs, frames);
    return 0;
}
