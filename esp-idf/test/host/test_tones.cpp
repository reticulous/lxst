/* Host test for the call progress tones (lxst_tones.h): cadences, level,
 * frequency, silence in the gaps, click-free edges and restart on set.
 *
 *   g++ -std=c++17 -Wall -I../../src -I../../include -Istub test_tones.cpp ../../src/lxst_tones.cpp ../../src/lxst_dsp.cpp -o /tmp/test_tones && /tmp/test_tones
 */
#include "lxst.h"
#include "lxst_tones.h"
#include "lxst_dsp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace lxsess;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* Render ms of a tone in blocks of `block` samples. */
static std::vector<int16_t> render(uint8_t kind, int rate, int ms, int block)
{
    CallTone t;
    t.set(kind);
    int n = rate * ms / 1000;
    std::vector<int16_t> out(n);
    std::vector<int32_t> acc(block);
    for (int at = 0; at < n; at += block) {
        int k = n - at < block ? n - at : block;
        std::fill(acc.begin(), acc.end(), 0);
        t.mix(acc.data(), k, rate);
        for (int i = 0; i < k; i++) out[at + i] = (int16_t)acc[i];
    }
    return out;
}

static int peakIn(const std::vector<int16_t>& v, int rate, int fromMs, int toMs)
{
    int p = 0;
    for (int i = rate * fromMs / 1000; i < rate * toMs / 1000 && i < (int)v.size(); i++)
        p = std::max(p, std::abs((int)v[i]));
    return p;
}

int main()
{
    /* Cadences. */
    CHECK(CallTone::periodMs(LXST_TONE_RING) == 3000);
    CHECK(CallTone::periodMs(LXST_TONE_RINGBACK) == 7000);
    CHECK(CallTone::periodMs(LXST_TONE_BUSY) == 500);
    CHECK(CallTone::periodMs(LXST_TONE_NONE) == 0);
    CHECK(CallTone::periodMs(9) == 0);
    CHECK(CallTone::onAt(LXST_TONE_RING, 0) && CallTone::onAt(LXST_TONE_RING, 399));
    CHECK(!CallTone::onAt(LXST_TONE_RING, 400) && !CallTone::onAt(LXST_TONE_RING, 599));
    CHECK(CallTone::onAt(LXST_TONE_RING, 600) && CallTone::onAt(LXST_TONE_RING, 999));
    CHECK(!CallTone::onAt(LXST_TONE_RING, 1000) && !CallTone::onAt(LXST_TONE_RING, 2999));
    CHECK(CallTone::onAt(LXST_TONE_RING, 3000));
    CHECK(CallTone::onAt(LXST_TONE_RINGBACK, 1999) && !CallTone::onAt(LXST_TONE_RINGBACK, 2000));
    CHECK(!CallTone::onAt(LXST_TONE_RINGBACK, 6999) && CallTone::onAt(LXST_TONE_RINGBACK, 7000));
    CHECK(CallTone::onAt(LXST_TONE_BUSY, 249) && !CallTone::onAt(LXST_TONE_BUSY, 250));
    CHECK(!CallTone::onAt(LXST_TONE_BUSY, 499) && CallTone::onAt(LXST_TONE_BUSY, 500));
    CHECK(!CallTone::onAt(LXST_TONE_NONE, 0));

    /* Unknown is none; none adds nothing. */
    {
        CallTone t;
        t.set(7);
        CHECK(!t.active() && t.kind() == LXST_TONE_NONE);
        std::vector<int32_t> acc(320, 5);
        t.mix(acc.data(), 320, 16000);
        bool same = true;
        for (int32_t v : acc) same = same && v == 5;
        CHECK(same);
    }

    const int rate = 16000;
    /* Ring-back: 382 Hz at about −12 dBFS for 2 s, then 5 s of silence. */
    {
        std::vector<int16_t> v = render(LXST_TONE_RINGBACK, rate, 14000, 320);
        int p = peakIn(v, rate, 10, 1990);
        CHECK(p > 8100 && p < 8300);
        CHECK(peakIn(v, rate, 2000, 7000) == 0);
        CHECK(peakIn(v, rate, 7010, 8990) > 8100);          /* the next cycle */
        SpurtStats s;
        s.addBlock(v.data() + rate / 10, rate);             /* 0.1 … 1.1 s */
        CHECK(std::abs(s.freqHz(rate) - 382) <= 2);
        CHECK(std::fabs(s.peakDbfs() - (-12.0f)) < 0.3f);
    }

    /* Busy: 0.25 s on, 0.25 s off, at 8 kHz and odd block sizes too. */
    {
        std::vector<int16_t> v = render(LXST_TONE_BUSY, 8000, 2000, 173);
        for (int c = 0; c < 4; c++) {
            CHECK(peakIn(v, 8000, c * 500 + 10, c * 500 + 240) > 8000);
            CHECK(peakIn(v, 8000, c * 500 + 250, c * 500 + 500) == 0);
        }
        SpurtStats s;
        s.addBlock(v.data() + 80, 1800);
        CHECK(std::abs(s.freqHz(8000) - 382) <= 3);
    }

    /* Ring: two bursts of 400 + 450 Hz, peak about −12 dBFS, 2 s gap. */
    {
        std::vector<int16_t> v = render(LXST_TONE_RING, rate, 6000, 320);
        int p1 = peakIn(v, rate, 0, 400), p2 = peakIn(v, rate, 600, 1000);
        CHECK(p1 > 7800 && p1 <= 8300);
        CHECK(p2 > 7800 && p2 <= 8300);
        CHECK(peakIn(v, rate, 400, 600) == 0);
        CHECK(peakIn(v, rate, 1000, 3000) == 0);
        CHECK(peakIn(v, rate, 3000, 3400) > 7800);
        /* Both components: correlate with each one. */
        for (double hz : { 400.0, 450.0, 600.0 }) {
            double re = 0, im = 0;
            int a = rate * 20 / 1000, b = rate * 380 / 1000;
            for (int i = a; i < b; i++) {
                double w = 2 * M_PI * hz * i / rate;
                re += v[i] * std::cos(w);
                im += v[i] * std::sin(w);
            }
            double amp = 2 * std::sqrt(re * re + im * im) / (b - a);
            if (hz == 600.0) CHECK(amp < 300);
            else CHECK(amp > 3700 && amp < 4400);       /* half the peak each */
        }
    }

    /* Edges ramp: no sample jumps by more than a ramped step at a burst
     * start or end. */
    {
        std::vector<int16_t> v = render(LXST_TONE_BUSY, rate, 1000, 320);
        CHECK(std::abs(v[0]) < 50);
        int maxStep = 0;
        for (size_t i = 1; i < v.size(); i++)
            maxStep = std::max(maxStep, std::abs(v[i] - v[i - 1]));
        /* A full-level 382 Hz sine moves at most 2π·382/16000·8225 ≈ 1234 per sample. */
        CHECK(maxStep < 1300);
    }

    /* Setting a tone restarts its cadence; the tone mixes over what is there. */
    {
        CallTone t;
        t.set(LXST_TONE_RINGBACK);
        std::vector<int32_t> acc(rate * 3, 0);
        t.mix(acc.data(), (int)acc.size(), rate);          /* now 1 s into the gap */
        std::vector<int32_t> blk(320, 0);
        t.mix(blk.data(), 320, rate);
        int p = 0;
        for (int32_t x : blk) p = std::max(p, std::abs((int)x));
        CHECK(p == 0);
        t.set(LXST_TONE_RINGBACK);
        std::fill(blk.begin(), blk.end(), 1000);
        t.mix(blk.data(), 320, rate);
        int lo = 1 << 30, hi = -(1 << 30);
        for (int32_t x : blk) { lo = std::min(lo, (int)x); hi = std::max(hi, (int)x); }
        CHECK(hi > 1000 + 5000 && lo < 1000 - 5000);        /* sounding again, over 1000 */
    }

    if (failures) {
        std::printf("test_tones: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_tones: all passed\n");
    return 0;
}
