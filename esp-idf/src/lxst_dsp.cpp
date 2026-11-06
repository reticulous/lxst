#include "lxst_dsp.h"

#include <cmath>
#include <cstring>

namespace lxsess {

float blockRms(const int16_t* pcm, int n)
{
    if (n <= 0) return 0.0f;
    int64_t acc = 0;
    for (int i = 0; i < n; i++) acc += (int32_t)pcm[i] * pcm[i];
    return (float)(std::sqrt((double)acc / n) / 32768.0);
}

float rmsDbfs(float rms)
{
    if (rms <= 0) return -120.0f;
    float db = (float)(20.0 * std::log10(rms));
    return db < -120.0f ? -120.0f : db;
}

void Tone::fill(int16_t* pcm, int n, int hz, int rate)
{
    /* A rotating phasor: one sin/cos per block, two multiplies a sample. */
    const float amp = 32767.0f * powf(10.0f, (peakDb > 0 ? 0.0f : peakDb) / 20.0f);
    const float step = 2.0f * (float)M_PI * (float)hz / (float)rate;
    const float cr = cosf(step), ci = sinf(step);
    float re = cosf((float)phase), im = sinf((float)phase);
    for (int i = 0; i < n; i++) {
        pcm[i] = (int16_t)lrintf(amp * im);
        float r = re * cr - im * ci;
        im = re * ci + im * cr;
        re = r;
    }
    phase = std::fmod(phase + (double)step * n, 2.0 * M_PI);
}

static float onePole(float ms, int rate)
{
    return 1.0f - expf(-1000.0f / (ms * (float)rate));
}

void MicChain::reset(int r)
{
    rate = r < 8000 ? 8000 : r;
    clamped = 0;
    inRms = 0;
    z1 = z2 = 0;
    ms = msGate = 0;
    gain = want = 1;
    pos = qh = qn = 0;
    idx = 0;
    lim = 1;
    std::memset(dly, 0, sizeof(dly));

    const float q = 0.70710678f;
    float w0 = 2.0f * (float)M_PI * HPF_HZ / (float)rate;
    float c = cosf(w0), al = sinf(w0) / (2.0f * q), a0 = 1.0f + al;
    b0 = (1.0f + c) / 2.0f / a0;
    b1 = -(1.0f + c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - al) / a0;

    aLevel = onePole(LEVEL_MS, rate);
    aGate = onePole(GATE_MS, rate);
    aAtt = onePole(ATTACK_MS, rate);
    aRel = onePole(RELEASE_MS, rate);
    aLimRel = onePole(LIM_RELEASE_MS, rate);
    float g = 32768.0f * powf(10.0f, GATE_DBFS / 20.0f);
    float t = 32768.0f * powf(10.0f, TARGET_DBFS / 20.0f);
    gateMs = g * g;
    targetMs = t * t;
    maxGain = powf(10.0f, MAX_GAIN_DB / 20.0f);
    sub = rate / 1000;
    subLeft = 0;

    look = rate / 1000 + 1;
    if (look > MAX_LOOK) look = MAX_LOOK;
    for (int i = 0; i < look; i++) box[i] = 65536;
    boxSum = 65536u * (uint32_t)look;
}

float MicChain::gainDb() const
{
    return 20.0f * log10f(gain);
}

void MicChain::process(int16_t* pcm, int n)
{
    if (!rate) reset(16000);
    float sumsq = 0;
    for (int i = 0; i < n; i++) {
        /* high-pass */
        float x = (float)pcm[i];
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;

        /* AGC: the wanted gain is revisited once a millisecond */
        float y2 = y * y;
        sumsq += y2;
        ms += aLevel * (y2 - ms);
        msGate += aGate * (y2 - msGate);
        if (--subLeft <= 0) {
            subLeft = sub;
            if (!agc) want = 1.0f;
            else if (msGate < gateMs) want = gain;
            else {
                float w = ms > 0 ? sqrtf(targetMs / ms) : maxGain;
                want = w < 1.0f ? 1.0f : w > maxGain ? maxGain : w;
            }
        }
        gain += (want < gain ? aAtt : aRel) * (want - gain);
        float v = y * gain;

        /* limiter: the gain each sample needs, its minimum over the look-ahead
         * window, and the box average of that minimum, which is never above
         * what the delayed sample needs */
        float av = fabsf(v);
        uint32_t need = av <= (float)CEILING ? 65536u
                      : (uint32_t)((float)CEILING / av * 65536.0f);
        while (qn > 0 && qv[(qh + qn - 1) % MAX_LOOK] >= need) qn--;
        int tail = (qh + qn) % MAX_LOOK;
        qv[tail] = need;
        qi[tail] = idx;
        qn++;
        while (idx - qi[qh] >= (uint32_t)look) { qh = (qh + 1) % MAX_LOOK; qn--; }
        uint32_t hmin = qv[qh];
        idx++;

        boxSum += hmin - box[pos];
        box[pos] = hmin;
        dly[pos] = v;
        pos = pos + 1 == look ? 0 : pos + 1;
        float out = dly[pos];                     /* look − 1 samples old */

        float b = (float)(boxSum / (uint32_t)look) * (1.0f / 65536.0f);
        lim = b < lim ? b : lim + aLimRel * (b - lim);
        long s = lrintf(out * lim);
        if (s > CEILING || s < -CEILING) {
            clamped++;
            s = s > 0 ? CEILING : -CEILING;
        }
        pcm[i] = (int16_t)s;
    }
    inRms = n > 0 ? sqrtf(sumsq / (float)n) / 32768.0f : 0.0f;
    if (fabsf(z1) < 1e-20f) z1 = 0;
    if (fabsf(z2) < 1e-20f) z2 = 0;
    if (ms < 1e-20f) ms = 0;
    if (msGate < 1e-20f) msGate = 0;
}

void SpurtStats::addBlock(const int16_t* pcm, int n)
{
    if (n <= 0) return;
    int64_t acc = 0;
    for (int i = 0; i < n; i++) {
        int v = pcm[i];
        acc += (int32_t)v * v;
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
    }
    samples += (uint64_t)n;
    sumsq += (double)acc;
    static const double g = 32768.0 * std::pow(10.0, GATE_DBFS / 20.0);   /* in LSB */
    if ((double)acc / n < g * g) return;
    for (int i = 0; i < n; i++) {
        int s = pcm[i] > HYST ? 1 : pcm[i] < -HYST ? -1 : 0;
        if (s && s != zsign) {
            if (zsign) zc++;
            zsign = s;
        }
    }
    zcSamples += (uint64_t)n;
}

float SpurtStats::rmsDbfs() const
{
    if (!samples || sumsq <= 0) return -120.0f;
    double rms = std::sqrt(sumsq / (double)samples) / 32768.0;
    return (float)(20.0 * std::log10(rms));
}

float SpurtStats::peakDbfs() const
{
    if (!peak) return -120.0f;
    return (float)(20.0 * std::log10(peak / 32768.0));
}

int SpurtStats::freqHz(int rate) const
{
    if (!zcSamples || rate <= 0) return 0;
    return (int)std::lrint((double)zc * rate / (2.0 * (double)zcSamples));
}

} // namespace lxsess
