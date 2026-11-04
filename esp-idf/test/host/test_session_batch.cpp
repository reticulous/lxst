/* Host test for the voice session's transmit batcher: partyline's
 * push-to-talk rule (8 + Σ(len + 3) ≤ MDU − 16, MDU the Link's maximum data
 * unit, at most min(64, jitter/frame)
 * frames) and one frame per message outside push-to-talk rooms.
 *
 *   g++ -std=c++17 -Wall -I../../src -I../../include -Istub test_session_batch.cpp ../../src/lxst_batch.cpp -o /tmp/test_session_batch && /tmp/test_session_batch
 */
#include "lxst_batch.h"
#include "lxst.h"
#include "lxst_mem.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

void* lxstAlloc(size_t n) { return std::malloc(n); }
void  lxstFree(void* p) { std::free(p); }

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct Msg { std::vector<std::vector<uint8_t>> frames; size_t wire = 0; };
static std::vector<Msg> out;

static void emit(void*, const uint8_t* m, size_t len)
{
    Msg msg;
    if (len < 2 || m[0] != LXST_EV_BATCH) { failures++; return; }
    size_t p = 2;
    for (int i = 0; i < m[1]; i++) {
        uint16_t l = (uint16_t)(m[p] | (m[p + 1] << 8));
        p += 2;
        msg.frames.emplace_back(m + p, m + p + l);
        p += l;
    }
    if (p != len) failures++;
    msg.wire = 8;
    for (auto& f : msg.frames) msg.wire += f.size() + 3;
    out.push_back(msg);
}

static std::vector<uint8_t> frame(int len, uint8_t tag)
{
    std::vector<uint8_t> f((size_t)len, tag);
    f[0] = 0x02;
    return f;
}

int main()
{
    /* c2-700 in a push-to-talk room at MDU 431: 42-byte frames, budget 415,
     * frame cap min(64, 6000 / 400) = 15. 8 + n × 45 ≤ 415 → 9 frames. */
    {
        out.clear();
        LxstBatcher b;
        CHECK(b.init(431 - 16, 8, 3, 15, 42));
        for (int i = 0; i < 20; i++) {
            auto f = frame(42, (uint8_t)i);
            b.add(f.data(), (uint16_t)f.size(), emit, nullptr);
        }
        CHECK(out.size() == 2);
        CHECK(out[0].frames.size() == 9);
        CHECK(out[0].wire <= 415);
        CHECK(out[1].frames.size() == 9);
        CHECK(b.count() == 2);
        b.flush(emit, nullptr);
        CHECK(out.size() == 3);
        CHECK(out[2].frames.size() == 2);
        CHECK(out[2].frames[1][1] == 19);
        b.flush(emit, nullptr);                  /* nothing pending: no message */
        CHECK(out.size() == 3);
    }

    /* The frame-count cap binds before the byte budget. */
    {
        out.clear();
        LxstBatcher b;
        b.init(415, 8, 3, 4, 82);
        for (int i = 0; i < 9; i++) {
            auto f = frame(20, (uint8_t)i);
            b.add(f.data(), (uint16_t)f.size(), emit, nullptr);
        }
        CHECK(out.size() == 2);
        CHECK(out[0].frames.size() == 4 && out[1].frames.size() == 4);
        CHECK(b.count() == 1);
    }

    /* batch_max_frames 1: every frame on its own. */
    {
        out.clear();
        LxstBatcher b;
        b.init(0, 0, 0, 1, 41);
        for (int i = 0; i < 3; i++) {
            auto f = frame(41, (uint8_t)i);
            b.add(f.data(), (uint16_t)f.size(), emit, nullptr);
        }
        CHECK(out.size() == 3);
        CHECK(b.count() == 0);
    }

    /* Exactly at the budget still fits; one byte over flushes first. */
    {
        out.clear();
        LxstBatcher b;
        b.init(8 + 2 * 45, 8, 3, 64, 42);
        auto f = frame(42, 1);
        b.add(f.data(), 42, emit, nullptr);
        b.add(f.data(), 42, emit, nullptr);
        CHECK(out.empty() && b.count() == 2);
        b.add(f.data(), 42, emit, nullptr);
        CHECK(out.size() == 1 && out[0].frames.size() == 2 && b.count() == 1);
    }

    /* clear() drops the partial batch; an oversized frame is refused. */
    {
        out.clear();
        LxstBatcher b;
        b.init(415, 8, 3, 15, 42);
        auto f = frame(42, 1);
        b.add(f.data(), 42, emit, nullptr);
        b.clear();
        b.flush(emit, nullptr);
        CHECK(out.empty());
        auto g = frame(43, 1);
        b.add(g.data(), 43, emit, nullptr);
        CHECK(b.count() == 0);
    }

    /* The frame cap is clamped to LXST_MAX_BATCH. */
    {
        out.clear();
        LxstBatcher b;
        b.init(0, 0, 0, 200, 4);
        auto f = frame(4, 1);
        for (int i = 0; i < 64; i++) b.add(f.data(), 4, emit, nullptr);
        CHECK(out.size() == 1 && out[0].frames.size() == (size_t)LXST_MAX_BATCH);
    }

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
