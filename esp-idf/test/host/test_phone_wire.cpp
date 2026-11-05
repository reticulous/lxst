/* Host test for the LXST telephony wire (lxst_phone_wire.h) against packets
 * packed by RNS's umsgpack and LXST 0.5.4's call profiles (gen_phone_vectors.py).
 *
 *   g++ -std=c++17 -Wall -Wextra -I../../src test_phone_wire.cpp ../../src/lxst_phone_wire.cpp -o /tmp/test_phone_wire && /tmp/test_phone_wire
 */
#include "lxst_phone_wire.h"
#include "phone_vectors.h"

#include <cstdio>
#include <cstring>

using namespace lxphone;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void vectors()
{
    for (const auto& v : PHONE_VECS) {
        Packet p;
        bool ok = decode(v.b, v.n, p);
        if (!ok) { std::printf("FAIL decode %s\n", v.name); failures++; continue; }
        CHECK(p.hasSignals == (v.nsig >= 0));
        if (v.nsig >= 0) {
            CHECK(p.nSignals == v.nsig);
            for (int i = 0; i < v.nsig && i < p.nSignals; ++i) CHECK(p.signals[i] == v.sig[i]);
        }
        CHECK(p.hasFrames == (v.nframes >= 0));
        if (v.nframes >= 0) {
            CHECK(p.nFrames == v.nframes);
            for (int i = 0; i < v.nframes && i < p.nFrames; ++i)
                CHECK(p.frames[i].n == v.fn[i] && !std::memcmp(p.frames[i].p, v.f[i], v.fn[i]));
        }
        /* every proper prefix fails */
        for (size_t k = 0; k < v.n; ++k) {
            Packet q;
            if (decode(v.b, k, q)) { std::printf("FAIL %s truncated to %zu decoded\n", v.name, k); failures++; break; }
        }
        if (!v.encode) continue;
        uint8_t out[512];
        size_t n = 0;
        if (v.nsig >= 0) n = encodeSignals(v.sig, v.nsig, out, sizeof out);
        else n = encodeFrame(v.f[0], v.fn[0], out, sizeof out);
        if (n != v.n || std::memcmp(out, v.b, n)) { std::printf("FAIL encode %s\n", v.name); failures++; }
    }
}

static void malformed()
{
    Packet p;
    const uint8_t notMap[] = { 0x91, 0x03 };
    CHECK(!decode(notMap, sizeof notMap, p));
    const uint8_t trailing[] = { 0x81, 0x00, 0x91, 0x03, 0x00 };
    CHECK(!decode(trailing, sizeof trailing, p));
    const uint8_t reserved[] = { 0x81, 0x00, 0xc1 };
    CHECK(!decode(reserved, sizeof reserved, p));
    const uint8_t empty[] = { 0x80 };
    CHECK(decode(empty, sizeof empty, p) && !p.hasSignals && !p.hasFrames);
    /* a signal list with a str and a negative int keeps only the integers */
    const uint8_t mixed[] = { 0x81, 0x00, 0x93, 0xa1, 'x', 0xff, 0x04 };
    CHECK(decode(mixed, sizeof mixed, p) && p.nSignals == 1 && p.signals[0] == 4);
    /* frames that are not bin are left out */
    const uint8_t strFrame[] = { 0x81, 0x01, 0xa2, 0x01, 0x02 };
    CHECK(decode(strFrame, sizeof strFrame, p) && p.hasFrames && p.nFrames == 0);
    /* a buffer too small to encode into */
    uint16_t s[2] = { 0x13F, 0xF1 };
    uint8_t small[4];
    CHECK(encodeSignals(s, 2, small, sizeof small) == 0);
}

static void signalsAndProfiles()
{
    CHECK(sigKind(0x13F) == SIG_PROFILE && sigValue(0x13F) == 0x40);
    CHECK(sigKind(0x17F) == SIG_PROFILE && sigValue(0x17F) == 0x80);
    CHECK(sigKind(0xF2) == SIG_MODE && sigValue(0xF2) == 2);
    CHECK(sigKind(0x06) == SIG_STATUS && sigValue(0x06) == 6);
    CHECK(sigKind(0xFF) == SIG_PROFILE && sigValue(0xFF) == 0);

    int n;
    const ProfileInfo* t = profileTable(n);
    CHECK(n == (int)(sizeof REF_PROFILES / sizeof REF_PROFILES[0]));
    for (const auto& r : REF_PROFILES) {
        const ProfileInfo* p = profileInfo(r.id);
        CHECK(p != nullptr);
        if (!p) continue;
        CHECK(p->codec == r.codec && p->mode == r.mode && p->frame_ms == r.frame_ms);
        CHECK(p->frame_bytes == r.frame_bytes && p->buffer == r.buffer);
        CHECK(profileByKey(p->key) == profileInfo(runProfile(p->id)));
    }
    (void)t;
    CHECK(!profileInfo(0x00) && !profileInfo(0x45) && !profileInfo(0x90));
    CHECK(!profileByKey("auto"));
    /* 0x70 decodes, but the node runs it, and a stored "ull", at 0x80 */
    CHECK(runProfile(0x70) == 0x80 && runProfile(0x40) == 0x40 && runProfile(0x80) == 0x80);
    CHECK(profileByKey("ull") == profileInfo(0x80) && profileByKey("ll") == profileInfo(0x80));

    uint16_t j, m;
    receiveBuffer(*profileInfo(0x40), j, m);
    CHECK(j == 180 && m == 360 + JITTER_MARGIN_MS);
    receiveBuffer(*profileInfo(0x30), j, m);
    CHECK(j == 200 && m == 600 + JITTER_MARGIN_MS);
    receiveBuffer(*profileInfo(0x70), j, m);
    CHECK(j == JITTER_FLOOR_MS && m == 30 + JITTER_MARGIN_MS);
    receiveBuffer(*profileInfo(0x80), j, m);
    CHECK(j == JITTER_FLOOR_MS && m == 80 + JITTER_MARGIN_MS);
    /* a batched (LoRa) hop: start at the batch wait in whole frames, room
     * for a whole batch on top */
    receiveBuffer(*profileInfo(0x10), j, m, true);
    CHECK(j == 3200 && m == 3200 + 12 * 400 + JITTER_MARGIN_MS);
    receiveBuffer(*profileInfo(0x30), j, m, true);
    CHECK(j == 3000 && m == 3000 + 12 * 200 + JITTER_MARGIN_MS);
    receiveBuffer(*profileInfo(0x10), j, m, false);
    CHECK(j == 400);

    const uint8_t F = MODE_FULL_DUPLEX, H = MODE_HALF_DUPLEX;
    auto is = [](CallFit f, uint8_t p, uint8_t m) { return f.profile == p && f.mode == m; };
    /* rtt only, on an unknown (0) or fast first hop */
    CHECK(is(autoProfile(0, 200), 0x40, F));
    CHECK(is(autoProfile(10000000, 200), 0x40, F));
    CHECK(is(autoProfile(0, 4000), 0x30, F));
    CHECK(is(autoProfile(0, 1501), 0x30, F));
    CHECK(is(autoProfile(0, 1500), 0x40, F));
    CHECK(is(autoProfile(0, 4001), 0x10, F));
    /* LoRa SF7/125 kHz registers 4000 bit/s: no full-duplex call fits its
     * 60 %, one direction of 700C does: half duplex */
    CHECK(is(autoProfile(4000, 200), 0x10, H));
    CHECK(is(autoProfile(4000, 4001), 0x10, H));
    /* SF8 (2000 bit/s registered): nothing fits */
    CHECK(is(autoProfile(2000, 200), 0, 0));
    /* 30 kbit/s: Opus medium both ways needs 39200 > 18000, Codec2 3200
     * both ways 13040 fits */
    CHECK(is(autoProfile(30000, 200), 0x30, F));
    /* 8 kbit/s: 700C both ways (4600 <= 4800) beats 1600 one way */
    CHECK(is(autoProfile(8000, 200), 0x10, F));
}

static void hopCost()
{
    /* The payload is exactly what encodeFrames packs for full frames. */
    int count = 0;
    const ProfileInfo* t = profileTable(count);
    for (int i = 0; i < count; ++i) {
        const ProfileInfo& p = t[i];
        uint8_t frame[512] = { p.codec };
        Bytes f[MAX_FRAMES];
        int n = framesPerPacket(p);
        for (int k = 0; k < n; ++k) f[k] = Bytes{ frame, p.frame_bytes };
        uint8_t out[2048];
        CHECK((int)encodeFrames(f, n, out, sizeof out) == payloadBytes(p));
    }
    /* Link packet = 19 header + 48 token + payload padded to 16 */
    CHECK(packetBytes(*profileInfo(0x10)) == 115 && packetMs(*profileInfo(0x10)) == 400);
    CHECK(packetBytes(*profileInfo(0x20)) == 147 && packetMs(*profileInfo(0x20)) == 320);
    CHECK(packetBytes(*profileInfo(0x30)) == 163 && packetMs(*profileInfo(0x30)) == 200);
    CHECK(packetBytes(*profileInfo(0x40)) == 147 && packetMs(*profileInfo(0x40)) == 60);
    CHECK(packetBytes(*profileInfo(0x50)) == 195);
    CHECK(packetBytes(*profileInfo(0x60)) == 323);
    CHECK(packetBytes(*profileInfo(0x80)) == 147 && packetMs(*profileInfo(0x80)) == 60);
    CHECK(txBps(*profileInfo(0x10)) == 2300);
    CHECK(txBps(*profileInfo(0x20)) == 3675);
    CHECK(txBps(*profileInfo(0x30)) == 6520);
    CHECK(txBps(*profileInfo(0x40)) == 19600);
    CHECK(txBps(*profileInfo(0x50)) == 26000);
    CHECK(txBps(*profileInfo(0x60)) == 43067);
    CHECK(txBps(*profileInfo(0x80)) == 19600);

    const uint8_t F = MODE_FULL_DUPLEX, H = MODE_HALF_DUPLEX;
    CHECK(modeDirections(F) == 2 && modeDirections(H) == 1);
    CHECK(fitsHop(*profileInfo(0x60), 0, F));            /* unknown hop */
    CHECK(fitsHop(*profileInfo(0x10), 4000, H));          /* 2300 <= 2400 */
    CHECK(!fitsHop(*profileInfo(0x10), 3833, H));         /* 2300 > 2299.8 */
    CHECK(!fitsHop(*profileInfo(0x20), 4000, H));
    CHECK(!fitsHop(*profileInfo(0x10), 4000, F));         /* 4600 > 2400 */
    CHECK(fitsHop(*profileInfo(0x10), 7667, F));          /* 4600 <= 4600.2 */
    CHECK(!fitsHop(*profileInfo(0x10), 7666, F));
    CHECK(fitProfile(0x40, 4000, H) == 0x10);
    CHECK(fitProfile(0x40, 4000, F) == 0);
    CHECK(fitProfile(0x60, 50000, H) == 0x50);            /* 26000 <= 30000 */
    CHECK(fitProfile(0x60, 50000, F) == 0x30);            /* 13040 <= 30000 */
    CHECK(fitProfile(0x60, 80000, H) == 0x60);
    CHECK(fitProfile(0x60, 150000, F) == 0x60);           /* 86134 <= 90000 */
    CHECK(fitProfile(0x70, 1000000, F) == 0x80);          /* never runs 0x70 */
    CHECK(fitProfile(0x10, 3000, H) == 0);

    /* fitCall: full duplex first, half duplex where only one way fits, and
     * no full duplex unless allowed */
    auto is = [](CallFit f, uint8_t p, uint8_t m) { return f.profile == p && f.mode == m; };
    CHECK(is(fitCall(0x40, 0, true), 0x40, F));
    CHECK(is(fitCall(0x40, 4000, true), 0x10, H));
    CHECK(is(fitCall(0x30, 8000, true), 0x10, F));
    CHECK(is(fitCall(0x30, 8000, false), 0x20, H));       /* 3675 <= 4800 */
    CHECK(is(fitCall(0x10, 3000, true), 0, 0));
    CHECK(is(fitCall(0x40, 100000, false), 0x40, H));

    /* hourly budget: 360 s/h less the 15 s reserve at 700C over 4000 bit/s,
     * 57.5 % airtime */
    CHECK(talkSecondsLeft(*profileInfo(0x10), 4000, 0, 360000) == 600);
    CHECK(talkSecondsLeft(*profileInfo(0x10), 4000, 335000, 360000) == 17);
    CHECK(talkSecondsLeft(*profileInfo(0x10), 4000, 345000, 360000) == 0);
    CHECK(talkSecondsLeft(*profileInfo(0x10), 4000, 350000, 360000) == 0);
    CHECK(talkSecondsLeft(*profileInfo(0x10), 4000, 360000, 360000) == 0);
    CHECK(talkSecondsLeft(*profileInfo(0x10), 4000, 0, 10000) == 0);  /* a limit within the reserve */
    CHECK(talkSecondsLeft(*profileInfo(0x10), 4000, 400000, 360000) == 0);
    CHECK(talkSecondsLeft(*profileInfo(0x10), 4000, 0, 0) == -1);   /* no budget */
    CHECK(talkSecondsLeft(*profileInfo(0x10), 0, 0, 360000) == -1); /* unknown hop */
}

static void batching()
{
    /* frames per packet: at least 60 ms of audio, one for 60 ms and longer */
    const struct { uint8_t id; int n; } want[] = {
        { 0x10, 1 }, { 0x20, 1 }, { 0x30, 1 }, { 0x40, 1 }, { 0x50, 1 }, { 0x60, 1 }, { 0x70, 6 }, { 0x80, 3 },
    };
    for (const auto& w : want) CHECK(framesPerPacket(*profileInfo(w.id)) == w.n);

    /* a list matches umsgpack's packing of {1: [frame, frame]} */
    for (const auto& v : PHONE_VECS) {
        if (std::strcmp(v.name, "frame_list")) continue;
        Bytes f[2] = { { v.f[0], v.fn[0] }, { v.f[1], v.fn[1] } };
        uint8_t out[512];
        size_t n = encodeFrames(f, 2, out, sizeof out);
        CHECK(n == v.n && !std::memcmp(out, v.b, n));
        CHECK(encodeFrames(f, 2, out, v.n - 1) == 0);
    }

    /* one frame is a bare bin, as encodeFrame */
    const uint8_t a[] = { 0x01, 0xaa, 0xbb };
    Bytes one[1] = { { a, sizeof a } };
    uint8_t x[64], y[64];
    size_t nx = encodeFrames(one, 1, x, sizeof x);
    size_t ny = encodeFrame(a, sizeof a, y, sizeof y);
    CHECK(nx && nx == ny && !std::memcmp(x, y, nx));
    CHECK(encodeFrames(one, 0, x, sizeof x) == 0);

    /* six 0x70 frames round-trip as a list, in order */
    uint8_t fb[6][11];
    Bytes six[6];
    for (int i = 0; i < 6; ++i) {
        fb[i][0] = 0x01;
        for (int k = 1; k < 11; ++k) fb[i][k] = (uint8_t)(i * 16 + k);
        six[i] = Bytes{ fb[i], 11 };
    }
    uint8_t out[256];
    size_t n = encodeFrames(six, 6, out, sizeof out);
    CHECK(n == 3 + 6 * 13);
    CHECK(out[0] == 0x81 && out[1] == FIELD_FRAMES && out[2] == 0x96);
    Packet p;
    CHECK(decode(out, n, p) && p.hasFrames && p.nFrames == 6 && !p.hasSignals);
    for (int i = 0; i < 6 && i < p.nFrames; ++i)
        CHECK(p.frames[i].n == 11 && !std::memcmp(p.frames[i].p, fb[i], 11));
}

static void text()
{
    CHECK(durationText(0) == "0:00");
    CHECK(durationText(59) == "0:59");
    CHECK(durationText(61) == "1:01");
    CHECK(durationText(3725) == "62:05");
    CHECK(normHash("0123456789ABCDEF0123456789abcdef") == "0123456789abcdef0123456789abcdef");
    CHECK(normHash(" 01234567 89abcdef 01234567 89abcdef ") == "0123456789abcdef0123456789abcdef");
    CHECK(normHash("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") ==
          "0123456789abcdef0123456789abcdef");
    CHECK(normHash("0123456789abcdef0123456789abcde").empty());
    CHECK(normHash("0123456789abcdef0123456789abcdeg").empty());
    std::string list = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    CHECK(listHas(list, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
    CHECK(listHas(list, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    CHECK(!listHas(list, "cccccccccccccccccccccccccccccccc"));
    CHECK(!listHas("", "cccccccccccccccccccccccccccccccc"));
}

int main()
{
    vectors();
    malformed();
    signalsAndProfiles();
    hopCost();
    batching();
    text();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("test_phone_wire: all passed\n");
    return 0;
}
