/* Host test for the voice session's per-speaker jitter buffer: ordering,
 * sequence wrap, gaps, late frames, the start and release rules, the cap.
 *
 *   g++ -std=c++17 -Wall -I../../src -I../../include -Istub test_session_jitter.cpp ../../src/lxst_jitter.cpp -o /tmp/test_session_jitter && /tmp/test_session_jitter
 */
#include "lxst_jitter.h"
#include "lxst_mem.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

void* lxstAlloc(size_t n) { return std::malloc(n); }
void  lxstFree(void* p) { std::free(p); }

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* A frame whose one payload byte is its sequence number's low byte. */
static LxstJitter::Push put(LxstJitter& j, uint16_t seq, uint32_t now, int* dropped = nullptr)
{
    uint8_t f[2] = { 0x02, (uint8_t)seq };
    return j.push(seq, f, 2, now, dropped);
}

/* Pop one frame time: returns -1 wait, -2 conceal, -3 release, else the
 * frame's tag byte. */
static int take(LxstJitter& j, uint32_t now, bool* underrun = nullptr)
{
    const uint8_t* f = nullptr;
    uint16_t len = 0;
    bool u = false;
    LxstJitter::Pop r = j.pop(now, &f, &len, &u);
    if (underrun) *underrun = u;
    switch (r) {
    case LxstJitter::POP_WAIT:    return -1;
    case LxstJitter::POP_CONCEAL: return -2;
    case LxstJitter::POP_RELEASE: return -3;
    default:                      return f[1];
    }
}

static LxstJitterCfg cfg(int frameMs, int startMs, int maxMs, bool seq)
{
    LxstJitterCfg c{};
    c.frameMs = frameMs;
    c.maxFrameBytes = 16;
    c.startMs = startMs;
    c.maxMs = maxMs;
    c.holdMs = startMs > 1000 ? startMs : 1000;
    c.sequenced = seq;
    return c;
}

int main()
{
    /* Prebuffer: nothing plays until start_ms is buffered, then in order. */
    {
        LxstJitter j;
        CHECK(j.init(cfg(20, 60, 3000, true)));
        put(j, 10, 0);
        CHECK(take(j, 0) == -1);
        put(j, 11, 0);
        CHECK(take(j, 0) == -1);
        put(j, 12, 0);
        CHECK(take(j, 0) == 10);
        CHECK(j.playing());
        CHECK(take(j, 20) == 11);
        CHECK(take(j, 40) == 12);
    }

    /* Reordering inside the buffer, and a duplicate. */
    {
        LxstJitter j;
        j.init(cfg(20, 80, 3000, true));
        put(j, 1, 0);
        put(j, 3, 0);              /* leaves a hole for 2 */
        CHECK(put(j, 2, 0) == LxstJitter::PUSH_OK);
        CHECK(put(j, 2, 0) == LxstJitter::PUSH_DUP);
        put(j, 4, 0);
        CHECK(take(j, 0) == 1);
        CHECK(take(j, 0) == 2);
        CHECK(take(j, 0) == 3);
        CHECK(take(j, 0) == 4);
    }

    /* A missing frame is concealed; a frame arriving after its slot played is late. */
    {
        LxstJitter j;
        j.init(cfg(20, 60, 3000, true));
        put(j, 100, 0);
        put(j, 102, 0);
        put(j, 103, 0);
        CHECK(take(j, 0) == 100);
        CHECK(take(j, 20) == -2);
        CHECK(put(j, 101, 30) == LxstJitter::PUSH_LATE);
        CHECK(take(j, 40) == 102);
    }

    /* Sequence numbers wrap at 2^16. */
    {
        LxstJitter j;
        j.init(cfg(20, 60, 3000, true));
        put(j, 65534, 0);
        put(j, 0, 0);              /* 65535 missing across the wrap */
        put(j, 65535, 0);          /* ... and arrives */
        put(j, 1, 0);
        CHECK(take(j, 0) == (uint8_t)65534);
        CHECK(take(j, 0) == (uint8_t)65535);
        CHECK(take(j, 0) == 0);
        CHECK(take(j, 0) == 1);
        CHECK(put(j, 65535, 0) == LxstJitter::PUSH_LATE);
    }

    /* A jump far beyond the gap window is the sender restarting: appended. */
    {
        LxstJitter j;
        j.init(cfg(20, 20, 3000, true));
        put(j, 5, 0);
        CHECK(take(j, 0) == 5);
        CHECK(put(j, 30000, 0) == LxstJitter::PUSH_OK);
        CHECK(j.slots() == 1);
        CHECK(take(j, 0) == (uint8_t)30000);
    }

    /* Unsequenced: arrival order whatever the numbers say. */
    {
        LxstJitter j;
        j.init(cfg(20, 40, 3000, false));
        put(j, 9, 0);
        put(j, 3, 0);
        CHECK(take(j, 0) == 9);
        CHECK(take(j, 0) == 3);
    }

    /* The cap drops the oldest frames. */
    {
        LxstJitter j;
        j.init(cfg(100, 200, 500, true));
        CHECK(j.capacity() == 5);
        int dropped = 0;
        for (int s = 0; s < 8; s++) put(j, (uint16_t)s, 0, &dropped);
        CHECK(dropped == 3);
        CHECK(j.slots() == 5);
        CHECK(take(j, 0) == 3);
    }

    /* Talk end: a short spurt starts at once and releases when drained. */
    {
        LxstJitter j;
        j.init(cfg(200, 6000, 30000, true));
        put(j, 1, 0);
        put(j, 2, 0);
        CHECK(take(j, 10) == -1);  /* far from 6 s buffered */
        j.talkEnd();
        CHECK(take(j, 20) == 1);
        CHECK(take(j, 220) == 2);
        CHECK(take(j, 420) == -3);
    }

    /* Without talk end, a stalled short spurt starts after the hold, and a
     * drained buffer with no news for the hold is released. */
    {
        LxstJitter j;
        j.init(cfg(20, 200, 3000, true));       /* hold 1000 ms */
        put(j, 1, 0);
        CHECK(take(j, 500) == -1);
        CHECK(take(j, 1000) == 1);
        CHECK(take(j, 1020) == -3);
    }

    /* An underrun mid-spurt goes back to prebuffering instead of releasing. */
    {
        LxstJitter j;
        j.init(cfg(20, 40, 3000, true));
        put(j, 1, 0);
        put(j, 2, 0);
        CHECK(take(j, 0) == 1);
        CHECK(take(j, 20) == 2);
        bool u = false;
        CHECK(take(j, 40, &u) == -1);
        CHECK(u);
        CHECK(!j.playing());
        put(j, 3, 50);
        CHECK(take(j, 60) == -1);                /* one frame: not yet 40 ms */
        put(j, 4, 70);
        CHECK(take(j, 80) == 3);
        CHECK(take(j, 100) == 4);
        CHECK(take(j, 120) == -1);               /* underrun again */
        CHECK(take(j, 1069) == -1);              /* hold not yet over */
        CHECK(take(j, 1070) == -3);
    }

    /* Talk end, then frames of the next spurt before the drain: no release. */
    {
        LxstJitter j;
        j.init(cfg(20, 20, 3000, true));
        put(j, 1, 0);
        j.talkEnd();
        put(j, 2, 5);
        CHECK(take(j, 10) == 1);
        CHECK(take(j, 30) == 2);
        bool u = false;
        CHECK(take(j, 50, &u) == -1);
        CHECK(u);
    }

    /* Talk end, then a late or duplicate frame of the ended spurt: it still
     * releases on the drain, not after the hold. */
    {
        LxstJitter j;
        j.init(cfg(20, 20, 3000, true));
        put(j, 1, 0);
        put(j, 2, 0);
        CHECK(take(j, 0) == 1);
        j.talkEnd();
        CHECK(put(j, 1, 10) == LxstJitter::PUSH_LATE);
        CHECK(put(j, 2, 10) == LxstJitter::PUSH_DUP);
        CHECK(take(j, 20) == 2);
        CHECK(take(j, 40) == -3);
    }

    /* Talk end, then a back-fill of a hole in the ended spurt: it plays, and
     * the drain still releases. */
    {
        LxstJitter j;
        j.init(cfg(20, 200, 3000, true));
        put(j, 1, 0);
        put(j, 3, 0);
        j.talkEnd();
        CHECK(put(j, 2, 5) == LxstJitter::PUSH_OK);
        CHECK(take(j, 10) == 1);
        CHECK(take(j, 30) == 2);
        CHECK(take(j, 50) == 3);
        CHECK(take(j, 70) == -3);
    }

    /* A stream of late frames (a sender that restarted its count a little
     * behind) does not hold a drained speaker: it releases after the hold. */
    {
        LxstJitter j;
        j.init(cfg(20, 20, 3000, true));
        put(j, 500, 0);
        CHECK(take(j, 0) == (uint8_t)500);
        bool u = false;
        CHECK(take(j, 20, &u) == -1);
        CHECK(u);
        for (uint32_t t = 100; t < 1000; t += 100)
            CHECK(put(j, (uint16_t)(t / 100), t) == LxstJitter::PUSH_LATE);
        CHECK(take(j, 1000) == -3);
    }

    /* A gap wider than the buffer evicts what it must and still lands in order. */
    {
        LxstJitter j;
        j.init(cfg(20, 20, 100, true));          /* 5 slots */
        int dropped = 0;
        put(j, 1, 0, &dropped);
        put(j, 40, 0, &dropped);                 /* 38 empty slots, then 40 */
        CHECK(j.slots() == 5);
        CHECK(dropped == 1);                     /* frame 1; empty slots are not frames */
        CHECK(take(j, 0) == -2);
        CHECK(take(j, 0) == -2);
        CHECK(take(j, 0) == -2);
        CHECK(take(j, 0) == -2);
        CHECK(take(j, 0) == 40);
    }

    /* Oversized and empty frames are refused. */
    {
        LxstJitter j;
        j.init(cfg(20, 20, 100, true));
        uint8_t big[32] = {};
        CHECK(j.push(1, big, sizeof(big), 0, nullptr) == LxstJitter::PUSH_BAD);
        CHECK(j.push(1, big, 0, 0, nullptr) == LxstJitter::PUSH_BAD);
    }

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
