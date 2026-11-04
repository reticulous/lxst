#include "lxst_dsp.h"

#include <cmath>

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
    const float amp = 32767.0f * 0.251f;            /* −12 dBFS peak */
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
