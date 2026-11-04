/* Host test for the voice session's profile helpers and signal helpers: Opus
 * presets, tail and hold, profile validity and names, the voice gate level, the
 * test tone and the receive-spurt analysis.
 *
 *   g++ -std=c++17 -Wall -I../../src -I../../include -Istub test_session_dsp.cpp ../../src/lxst_sess_profile.cpp ../../src/lxst_dsp.cpp ../../src/lxst_frame.cpp -o /tmp/test_session_dsp && /tmp/test_session_dsp
 */
#include "lxst.h"
#include "lxst_sess_profile.h"
#include "lxst_dsp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static lxst_profile_t prof(uint8_t codec, uint8_t mode, uint16_t frameMs, uint16_t maxBytes)
{
    lxst_profile_t p{};
    p.codec = codec;
    p.mode = mode;
    p.frame_ms = frameMs;
    p.max_frame_bytes = maxBytes;
    p.batch_max_frames = 1;
    return p;
}

int main()
{
    using namespace lxsess;

    OpusPreset o{};
    CHECK(opusPreset(LXST_OPUS_VOICE_LOW, &o) && o.bitrate == 6000 && o.voip && o.narrow);
    CHECK(opusPreset(LXST_OPUS_VOICE_HIGH, &o) && o.bitrate == 16000 && o.voip && !o.narrow);
    CHECK(opusPreset(LXST_OPUS_AUDIO_MIN, &o) && o.bitrate == 8000 && !o.voip && o.narrow);
    CHECK(opusPreset(LXST_OPUS_AUDIO_MAX, &o) && o.bitrate == 128000 && !o.voip);
    CHECK(!opusPreset(0x09, &o));

    CHECK(tailMs(20) == 250 && tailMs(200) == 300 && tailMs(400) == 500);
    CHECK(holdMs(200) == 1000 && holdMs(6000) == 6000);

    /* partyline's room profiles are valid at their byte limits. */
    CHECK(profileValid(prof(LXST_CODEC_CODEC2, LXST_C2_3200, 200, 82)));
    CHECK(profileValid(prof(LXST_CODEC_CODEC2, LXST_C2_2400, 200, 62)));
    CHECK(profileValid(prof(LXST_CODEC_CODEC2, LXST_C2_1200, 400, 62)));
    CHECK(profileValid(prof(LXST_CODEC_CODEC2, LXST_C2_700C, 400, 42)));
    CHECK(profileValid(prof(LXST_CODEC_OPUS, LXST_OPUS_VOICE_HIGH, 20, 41)));
    CHECK(profileValid(prof(LXST_CODEC_OPUS, LXST_OPUS_AUDIO_HIGH, 40, 281)));
    CHECK(!profileValid(prof(LXST_CODEC_CODEC2, LXST_C2_3200, 200, 81)));   /* cap too small */
    CHECK(!profileValid(prof(LXST_CODEC_CODEC2, LXST_C2_1200, 220, 80)));   /* not whole sub-frames */
    CHECK(!profileValid(prof(LXST_CODEC_OPUS, LXST_OPUS_VOICE_HIGH, 30, 41)));
    CHECK(!profileValid(prof(LXST_CODEC_RAW, 0, 20, 41)));
    lxst_profile_t bad = prof(LXST_CODEC_OPUS, LXST_OPUS_VOICE_HIGH, 20, 41);
    bad.batch_max_frames = 0;
    CHECK(!profileValid(bad));

    /* Receive buffer limits. */
    lxst_profile_t j = prof(LXST_CODEC_CODEC2, LXST_C2_3200, 200, 400);
    j.jitter_ms = 30000;
    j.max_jitter_ms = 30000;
    CHECK(profileValid(j));
    j.max_jitter_ms = 30001;
    CHECK(!profileValid(j));                              /* deeper than MAX_JITTER_MS */
    j.max_jitter_ms = 2000;
    CHECK(!profileValid(j));                              /* start deeper than the buffer */
    j.max_jitter_ms = 0;
    j.jitter_ms = DEFAULT_MAX_JITTER_MS + 1;
    CHECK(!profileValid(j));
    j.jitter_ms = 200;
    CHECK(profileValid(j));

    /* Slots are the codec's real frame, not the cap. */
    CHECK(frameBytesMax(prof(LXST_CODEC_CODEC2, LXST_C2_3200, 200, 400)) == 82);
    CHECK(frameBytesMax(prof(LXST_CODEC_CODEC2, LXST_C2_700C, 400, 400)) == 42);
    CHECK(frameBytesMax(prof(LXST_CODEC_OPUS, LXST_OPUS_AUDIO_HIGH, 40, 281)) == 281);

    /* Codec2 3200, 30 s: 150 slots of 82 B fit. */
    j = prof(LXST_CODEC_CODEC2, LXST_C2_3200, 200, 400);
    j.max_jitter_ms = 30000;
    JitterSize js = jitterSize(j, 30000);
    CHECK(js.frameBytes == 82 && js.maxMs == 30000 && js.startMs == 30000);
    /* Opus 281 B at 40 ms, 30 s: cut to 466 slots (18.64 s) within 128 KB. */
    j = prof(LXST_CODEC_OPUS, LXST_OPUS_AUDIO_HIGH, 40, 281);
    j.max_jitter_ms = 30000;
    js = jitterSize(j, 30000);
    CHECK(js.maxMs == 466 * 40 && js.startMs == js.maxMs);
    CHECK((size_t)((js.maxMs + 39) / 40) * js.frameBytes <= SPEAKER_JITTER_BYTES);
    /* No max: the default depth; a negative start is 0. */
    js = jitterSize(prof(LXST_CODEC_OPUS, LXST_OPUS_VOICE_HIGH, 20, 41), -5);
    CHECK(js.maxMs == DEFAULT_MAX_JITTER_MS && js.startMs == 0);

    char name[64];
    profileName(prof(LXST_CODEC_CODEC2, LXST_C2_3200, 200, 82), name, sizeof(name));
    CHECK(!std::strcmp(name, "Codec2 3200, 200 ms"));
    profileName(prof(LXST_CODEC_CODEC2, LXST_C2_700C, 400, 42), name, sizeof(name));
    CHECK(!std::strcmp(name, "Codec2 700C, 400 ms"));
    profileName(prof(LXST_CODEC_OPUS, LXST_OPUS_VOICE_HIGH, 20, 41), name, sizeof(name));
    CHECK(!std::strcmp(name, "Opus voice 16 kbps, 20 ms"));

    /* Levels. */
    std::vector<int16_t> z(320, 0);
    CHECK(blockDbfs(z.data(), 320) == -120.0f);
    std::vector<int16_t> fs(320, 32767);
    CHECK(std::fabs(blockDbfs(fs.data(), 320)) < 0.01f);

    /* The tone: about −12 dBFS peak, continuous across blocks, and the analysis
     * finds its frequency and level. */
    {
        Tone t;
        SpurtStats s;
        std::vector<int16_t> blk(320);
        int peak = 0;
        for (int b = 0; b < 200; b++) {          /* 4 s at 16 kHz */
            t.fill(blk.data(), 320, 440, 16000);
            for (int16_t v : blk) peak = std::max(peak, (int)std::abs(v));
            s.addBlock(blk.data(), 320);
        }
        CHECK(peak > 8100 && peak < 8300);
        CHECK(std::abs(s.freqHz(16000) - 440) <= 2);
        CHECK(std::fabs(s.peakDbfs() - (-12.0f)) < 0.2f);
        CHECK(std::fabs(s.rmsDbfs() - (-15.0f)) < 0.2f);
        CHECK(s.samples == 64000);
    }

    /* Silence in the spurt counts towards level but not frequency. */
    {
        Tone t;
        SpurtStats s;
        std::vector<int16_t> blk(320);
        for (int b = 0; b < 50; b++) {
            t.fill(blk.data(), 320, 1000, 16000);
            s.addBlock(blk.data(), 320);
            s.addBlock(z.data(), 320);
        }
        CHECK(std::abs(s.freqHz(16000) - 1000) <= 3);
        CHECK(std::fabs(s.rmsDbfs() - (-18.0f)) < 0.2f);
    }

    /* Session format: the engine's clamps, and one block within the ceiling. */
    {
        int r = 0, b = 0;
        sessionFormat(16000, 20, 1920, &r, &b);
        CHECK(r == 16000 && b == 20);
        sessionFormat(4000, 5, 1920, &r, &b);
        CHECK(r == 8000 && b == 10);
        sessionFormat(96000, 80, 1920, &r, &b);
        CHECK(r == 48000 && b == 40);
        sessionFormat(48000, 40, 1920, &r, &b);
        CHECK(r == 48000 && 48000 * b / 1000 <= 1920);
    }

    /* Received frames: the profile's rules, or any well-formed frame with rx_any. */
    {
        lxst_profile_t c2 = prof(LXST_CODEC_CODEC2, LXST_C2_3200, 200, 82);
        std::vector<uint8_t> f3200(82, 0);
        f3200[0] = LXST_CODEC_CODEC2;
        f3200[1] = LXST_C2_3200;
        std::vector<uint8_t> f700(42, 0);                 /* 10 × 40 ms of 700C */
        f700[0] = LXST_CODEC_CODEC2;
        f700[1] = LXST_C2_700C;
        std::vector<uint8_t> f1600(66, 0);                /* 8 × 40 ms of 1600 */
        f1600[0] = LXST_CODEC_CODEC2;
        f1600[1] = LXST_C2_1600;
        /* Opus code 0, config 1 (SILK NB 20 ms); config 3 (SILK NB 60 ms);
         * config 15 hybrid FB 20 ms, stereo flag; code 3 × 3 frames of 60 ms. */
        uint8_t o20[] = { LXST_CODEC_OPUS, (1 << 3), 0xAA, 0x55 };
        uint8_t o60[] = { LXST_CODEC_OPUS, (3 << 3), 0xAA };
        uint8_t o20st[] = { LXST_CODEC_OPUS, (15 << 3) | 0x04, 0xAA };
        uint8_t o180[] = { LXST_CODEC_OPUS, (3 << 3) | 3, 3, 0xAA };
        uint8_t oEmpty[] = { LXST_CODEC_OPUS };
        uint8_t raw[] = { LXST_CODEC_RAW, 1, 2 };
        uint8_t c2Bad[] = { LXST_CODEC_CODEC2, LXST_C2_3200, 1, 2, 3 };   /* 3 B: not whole */
        uint8_t c2Mode[] = { LXST_CODEC_CODEC2, 0x07, 1, 2, 3, 4 };

        CHECK(rxFrameOk(c2, f3200.data(), f3200.size()));
        CHECK(!rxFrameOk(c2, f3200.data(), 42));          /* not 200 ms */
        CHECK(!rxFrameOk(c2, f700.data(), f700.size()));  /* another mode */
        CHECK(!rxFrameOk(c2, o20, sizeof(o20)));
        CHECK(!rxFrameOk(c2, nullptr, 0));

        lxst_profile_t any = c2;
        any.rx_any = 1;
        CHECK(rxFrameOk(any, f3200.data(), f3200.size()));
        CHECK(rxFrameOk(any, f3200.data(), 42));          /* 100 ms of 3200 */
        CHECK(rxFrameOk(any, f700.data(), f700.size()));
        CHECK(rxFrameOk(any, f1600.data(), f1600.size()));
        CHECK(rxFrameOk(any, o20, sizeof(o20)));
        CHECK(rxFrameOk(any, o60, sizeof(o60)));
        CHECK(rxFrameOk(any, o20st, sizeof(o20st)));
        CHECK(!rxFrameOk(any, o180, sizeof(o180)));       /* longer than 120 ms */
        CHECK(!rxFrameOk(any, oEmpty, sizeof(oEmpty)));
        CHECK(!rxFrameOk(any, raw, sizeof(raw)));
        CHECK(!rxFrameOk(any, c2Bad, sizeof(c2Bad)));
        CHECK(!rxFrameOk(any, c2Mode, sizeof(c2Mode)));
        std::vector<uint8_t> big(2 + 63 * 8, 0);          /* 63 sub-frames of 3200: accepted */
        big[0] = LXST_CODEC_CODEC2;
        big[1] = LXST_C2_3200;
        CHECK(big.size() == 506 && rxFrameOk(any, big.data(), big.size()));
        std::vector<uint8_t> huge(2 + 128 * 4, 0);        /* 514 B: too long */
        huge[0] = LXST_CODEC_CODEC2;
        huge[1] = LXST_C2_700C;
        CHECK(!rxFrameOk(any, huge.data(), huge.size()));
        std::vector<uint8_t> o512(512, 0);
        o512[0] = LXST_CODEC_OPUS;
        o512[1] = (31 << 3);                              /* CELT FB 20 ms */
        CHECK(rxFrameOk(any, o512.data(), o512.size()));

        /* rx_any slots hold the largest frame the session takes. */
        CHECK(frameBytesMax(any) == MAX_FRAME_BYTES);
        any.max_jitter_ms = 30000;
        JitterSize ja = jitterSize(any, 30000);
        CHECK(ja.frameBytes == 512 && (size_t)((ja.maxMs + 199) / 200) * 512 <= SPEAKER_JITTER_BYTES);
    }

    /* Call profiles from the telephony table are valid. */
    {
        struct { uint8_t codec, mode; uint16_t ms, bytes; } t[] = {
            { LXST_CODEC_CODEC2, LXST_C2_700C, 400, 42 },
            { LXST_CODEC_CODEC2, LXST_C2_1600, 320, 66 },
            { LXST_CODEC_CODEC2, LXST_C2_3200, 200, 82 },
            { LXST_CODEC_OPUS, LXST_OPUS_VOICE_MEDIUM, 60, 61 },
            { LXST_CODEC_OPUS, LXST_OPUS_VOICE_HIGH, 60, 121 },
            { LXST_CODEC_OPUS, LXST_OPUS_VOICE_MAX, 60, 241 },
            { LXST_CODEC_OPUS, LXST_OPUS_VOICE_MEDIUM, 10, 11 },
            { LXST_CODEC_OPUS, LXST_OPUS_VOICE_MEDIUM, 20, 21 },
        };
        for (auto& e : t) {
            lxst_profile_t p = prof(e.codec, e.mode, e.ms, e.bytes);
            p.rx_any = 1;
            p.tx_mode = LXST_TX_OPEN;
            CHECK(profileValid(p));
            if (e.codec == LXST_CODEC_OPUS)
                CHECK(1 + lxstOpusMaxBytes(lxstOpusBitrateCeiling(e.mode), e.ms) == e.bytes);
        }
        OpusPreset o;
        CHECK(opusPreset(LXST_OPUS_VOICE_MAX, &o) && o.bitrate == 32000 && o.voip && !o.narrow);
        CHECK(opusPreset(LXST_OPUS_VOICE_MEDIUM, &o) && o.bitrate == 8000 && o.voip && !o.narrow);
        char nm[64];
        profileName(prof(LXST_CODEC_CODEC2, LXST_C2_1600, 320, 66), nm, sizeof(nm));
        CHECK(!std::strcmp(nm, "Codec2 1600, 320 ms"));
    }

    /* Transmit keying: the profile's tx_mode over the setting. */
    CHECK(txSelect(LXST_TX_SETTING, TXSEL_VOX) == TXSEL_VOX);
    CHECK(txSelect(LXST_TX_SETTING, TXSEL_PTT) == TXSEL_PTT);
    CHECK(txSelect(LXST_TX_OPEN, TXSEL_PTT) == TXSEL_OPEN);
    CHECK(txSelect(LXST_TX_OPEN, TXSEL_VOX) == TXSEL_OPEN);
    CHECK(txSelect(LXST_TX_PTT, TXSEL_OPEN) == TXSEL_PTT);
    CHECK(txSelect(9, TXSEL_OPEN) == TXSEL_OPEN);

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
