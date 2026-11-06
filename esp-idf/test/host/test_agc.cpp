/* Host test for the microphone conditioner (lxsess::MicChain): quiet speech is
 * brought up to the target, loud input never passes the limiter's ceiling, hum
 * is cut by the high-pass, and silence, room noise and pauses are not pumped
 * up. Synthetic signals at 16 kHz and 8 kHz, 20 ms blocks.
 *
 *   g++ -std=c++17 -Wall -Wextra -O2 -I../../src test_agc.cpp ../../src/lxst_dsp.cpp -o /tmp/test_agc && /tmp/test_agc
 */
#include "lxst_dsp.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using lxsess::MicChain;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static double dbfs(const std::vector<int16_t>& v, size_t from, size_t to)
{
    double acc = 0;
    for (size_t i = from; i < to; i++) acc += (double)v[i] * v[i];
    double rms = std::sqrt(acc / (double)(to - from)) / 32768.0;
    return rms > 0 ? 20.0 * std::log10(rms) : -120.0;
}

static int maxAbs(const std::vector<int16_t>& v)
{
    int m = 0;
    for (int16_t s : v) { int a = s < 0 ? -s : s; if (a > m) m = a; }
    return m;
}

static uint32_t s_seed = 12345;
static float white()
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)((int32_t)s_seed) / 2147483648.0f;
}

/* Noise band-limited to roughly 200–2500 Hz, amplitude-modulated at amRate Hz
 * (0: steady), scaled to rmsDb dBFS RMS and clipped to full scale. */
static std::vector<int16_t> speechLike(int rate, double secs, double rmsDb, double amRate)
{
    size_t n = (size_t)(rate * secs);
    std::vector<double> x(n);
    double lp = 0, hp = 0, prev = 0;
    double aLp = 1.0 - std::exp(-2.0 * M_PI * 2500.0 / rate);
    double aHp = std::exp(-2.0 * M_PI * 200.0 / rate);
    for (size_t i = 0; i < n; i++) {
        lp += aLp * (white() - lp);
        hp = aHp * (hp + lp - prev);
        prev = lp;
        double env = amRate > 0 ? 0.5 - 0.5 * std::cos(2.0 * M_PI * amRate * i / rate) : 1.0;
        x[i] = hp * env;
    }
    double acc = 0;
    for (double v : x) acc += v * v;
    double k = 32768.0 * std::pow(10.0, rmsDb / 20.0) / std::sqrt(acc / n);
    std::vector<int16_t> out(n);
    for (size_t i = 0; i < n; i++) {
        double v = x[i] * k;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : std::lrint(v));
    }
    return out;
}

static std::vector<int16_t> sine(int rate, double secs, double hz, double peakDb)
{
    size_t n = (size_t)(rate * secs);
    std::vector<int16_t> out(n);
    double a = 32767.0 * std::pow(10.0, peakDb / 20.0);
    for (size_t i = 0; i < n; i++) out[i] = (int16_t)std::lrint(a * std::sin(2.0 * M_PI * hz * i / rate));
    return out;
}

static void run(MicChain& c, std::vector<int16_t>& v, int rate)
{
    int blk = rate / 50;
    for (size_t i = 0; i < v.size(); i += (size_t)blk) {
        int n = (int)(v.size() - i < (size_t)blk ? v.size() - i : (size_t)blk);
        c.process(v.data() + i, n);
    }
}

static void testRate(int rate)
{
    std::printf("-- %d Hz\n", rate);
    size_t r = (size_t)rate;

    /* (a) quiet speech-like bursts rise to the target */
    {
        MicChain c; c.reset(rate);
        auto v = speechLike(rate, 8.0, -40.0, 4.0);
        double in = dbfs(v, 4 * r, 8 * r);
        run(c, v, rate);
        double out = dbfs(v, 4 * r, 8 * r);
        std::printf("quiet speech  in %.1f dBFS  out %.1f dBFS  gain %.1f dB\n", in, out, c.gainDb());
        CHECK(std::fabs(out - MicChain::TARGET_DBFS) <= 3.0);
    }
    /* (a') steady quiet noise above the gate */
    {
        MicChain c; c.reset(rate);
        auto v = speechLike(rate, 6.0, -35.0, 0);
        double in = dbfs(v, 3 * r, 6 * r);
        run(c, v, rate);
        double out = dbfs(v, 3 * r, 6 * r);
        std::printf("steady -35    in %.1f dBFS  out %.1f dBFS  gain %.1f dB\n", in, out, c.gainDb());
        CHECK(std::fabs(out - MicChain::TARGET_DBFS) <= 3.0);
    }
    /* (b) loud input is limited, never past the ceiling, never clamped */
    {
        MicChain c; c.reset(rate);
        auto v = speechLike(rate, 4.0, -3.0, 4.0);
        int inPk = maxAbs(v);
        double in = dbfs(v, 0, v.size());
        run(c, v, rate);
        std::printf("loud speech   in %.1f dBFS peak %d  out %.1f dBFS peak %d  clamped %u\n",
                    in, inPk, dbfs(v, 0, v.size()), maxAbs(v), (unsigned)c.clamped);
        CHECK(maxAbs(v) <= MicChain::CEILING);
        CHECK(c.clamped == 0);
    }
    {
        MicChain c; c.reset(rate);
        size_t n = 2 * r;
        std::vector<int16_t> v(n);
        for (size_t i = 0; i < n; i++) v[i] = ((i * 2000 / r) & 1) ? -32768 : 32767;   /* 1 kHz */
        run(c, v, rate);
        std::printf("full square   out %.1f dBFS peak %d  clamped %u\n", dbfs(v, 0, n), maxAbs(v),
                    (unsigned)c.clamped);
        CHECK(maxAbs(v) <= MicChain::CEILING);
        CHECK(c.clamped == 0);
    }
    {
        /* quiet speech then a sudden loud onset with the gain still high */
        MicChain c; c.reset(rate);
        auto v = speechLike(rate, 3.0, -40.0, 4.0);
        auto l = speechLike(rate, 1.0, 0.0, 0);
        v.insert(v.end(), l.begin(), l.end());
        run(c, v, rate);
        std::printf("quiet->loud   out peak %d  clamped %u\n", maxAbs(v), (unsigned)c.clamped);
        CHECK(maxAbs(v) <= MicChain::CEILING);
        CHECK(c.clamped == 0);
    }
    /* (c) hum, gain at unity */
    {
        double lvl[2];
        const double hz[2] = {50, 1000};
        for (int k = 0; k < 2; k++) {
            MicChain c; c.agc = false; c.reset(rate);
            auto v = sine(rate, 2.0, hz[k], -20.0);
            run(c, v, rate);
            lvl[k] = dbfs(v, r, 2 * r);
        }
        std::printf("hum           50 Hz %.1f dBFS  1 kHz %.1f dBFS  attenuation %.1f dB\n",
                    lvl[0], lvl[1], lvl[1] - lvl[0]);
        CHECK(lvl[1] - lvl[0] >= 10.0);
    }
    /* (d) silence and room noise are not boosted */
    {
        MicChain c; c.reset(rate);
        std::vector<int16_t> v(3 * r, 0);
        run(c, v, rate);
        std::printf("silence       out peak %d  gain %.1f dB\n", maxAbs(v), c.gainDb());
        CHECK(maxAbs(v) == 0 && c.gainDb() < 0.01f);
    }
    {
        MicChain c; c.reset(rate);
        auto v = speechLike(rate, 4.0, -60.0, 0);
        double in = dbfs(v, r, 4 * r);
        run(c, v, rate);
        double out = dbfs(v, r, 4 * r);
        std::printf("-60 noise     in %.1f dBFS  out %.1f dBFS  gain %.1f dB\n", in, out, c.gainDb());
        CHECK(out < -55.0 && c.gainDb() < 0.01f);
    }
    {
        /* a pause after speech: the noise floor stays where the speech left it */
        MicChain c; c.reset(rate);
        auto v = speechLike(rate, 4.0, -40.0, 4.0);
        run(c, v, rate);
        float gSpeech = c.gainDb();
        auto p = speechLike(rate, 6.0, -60.0, 0);
        int blk = rate / 50;
        float inMax = -120.0f;                  /* the voice gate's level: pre-gain */
        for (size_t i = 0; i + (size_t)blk <= p.size(); i += (size_t)blk) {
            c.process(p.data() + i, blk);
            float d = lxsess::rmsDbfs(c.inRms);
            if (d > inMax) inMax = d;
        }
        double early = dbfs(p, r / 5, r), late = dbfs(p, 5 * r, 6 * r);
        std::printf("pause         gain %.1f -> %.1f dB  noise out %.1f -> %.1f dBFS  gate level max %.1f dBFS\n",
                    gSpeech, c.gainDb(), early, late, inMax);
        CHECK(c.gainDb() <= gSpeech + 0.1f);
        CHECK(late <= early + 1.0);
        CHECK(inMax < -45.0f);
    }
    /* host time per 20 ms block */
    {
        MicChain c; c.reset(rate);
        auto v = speechLike(rate, 20.0, -30.0, 4.0);
        int blk = rate / 50;
        auto t0 = std::chrono::steady_clock::now();
        run(c, v, rate);
        auto t1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / (v.size() / blk);
        std::printf("host time     %.2f us per %d-sample block\n", us, blk);
    }
}

int main()
{
    testRate(16000);
    testRate(8000);
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("all agc tests passed\n");
    return 0;
}
