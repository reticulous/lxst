/**
 * lxst_endpoint_device — the session endpoint on the device's audio engine:
 * two ITS client connections to the audio task's stream port, both pcm16.
 *
 *   playback  "pcm16,playback"  open for the whole session; carries the mix,
 *                               one [av_hdr][pcm16] block per send; does not
 *                               hold the microphone
 *   capture   "pcm16"           open only while the session wants the mic;
 *                               delivers [av_hdr][pcm16] mic blocks
 *
 * A connection that is down is (re)made by service() only, at most every 5 s.
 */
#include "lxst_endpoint.h"
#include "audio.h"
#include "av_hdr.h"
#include "codec.h"
#include "its.h"
#include "log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <cstring>

namespace {

constexpr int    MAX_BLOCK = LXST_EP_MAX_BLOCK;
constexpr size_t MAX_MSG = sizeof(av_hdr_t) + MAX_BLOCK * 2;
constexpr int    REF_PLAY = 0, REF_CAP = 1;
constexpr int64_t RETRY_US = 5 * 1000 * 1000;

class DeviceEndpoint : public LxstEndpoint {
public:
    bool open(LxstEndpointSink* sink, int rate, int blockMs) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    void capture(bool on) override;
    bool capturing() const override { return m_cap >= 0; }
    bool play(const int16_t* pcm, int n) override;
    void service() override;

    static void onPlayRecv(int h, size_t);
    static void onCapRecv(int h, size_t);
    static void onDisc(int ref);

    LxstEndpointSink* m_sink = nullptr;
    bool    m_open = false;
    int     m_play = -1, m_cap = -1;
    bool    m_wantCap = false;
    int64_t m_retryPlayUs = 0, m_retryCapUs = 0;
    uint8_t* m_buf = nullptr;      /* one message either direction */
    uint8_t* m_out = nullptr;

    bool connectPlay();
    bool connectCap();
};

DeviceEndpoint s_dev;

bool DeviceEndpoint::connectPlay()
{
    static const char kPlay[] = "pcm16,playback";
    m_play = itsConnect("audio", AUDIO_STREAM_PORT, kPlay, sizeof(kPlay) - 1,
                        pdMS_TO_TICKS(500), REF_PLAY, onPlayRecv, onDisc);
    if (m_play < 0) {
        m_retryPlayUs = esp_timer_get_time() + RETRY_US;
        warn("lxst: audio playback connect failed");
        return false;
    }
    return true;
}

bool DeviceEndpoint::connectCap()
{
    static const char kCap[] = "pcm16";
    m_cap = itsConnect("audio", AUDIO_STREAM_PORT, kCap, sizeof(kCap) - 1,
                       pdMS_TO_TICKS(500), REF_CAP, onCapRecv, onDisc);
    if (m_cap < 0) {
        m_retryCapUs = esp_timer_get_time() + RETRY_US;
        warn("lxst: audio capture connect failed");
        return false;
    }
    return true;
}

bool DeviceEndpoint::open(LxstEndpointSink* sink, int, int)
{
    if (m_open) return true;
    if (!m_buf) m_buf = (uint8_t*)heap_caps_malloc(MAX_MSG, MALLOC_CAP_SPIRAM);
    if (!m_out) m_out = (uint8_t*)heap_caps_malloc(MAX_MSG, MALLOC_CAP_SPIRAM);
    if (!m_buf || !m_out) return false;
    m_sink = sink;
    m_wantCap = false;
    if (esp_timer_get_time() < m_retryPlayUs || !connectPlay()) return false;
    m_retryCapUs = 0;
    m_open = true;
    return true;
}

void DeviceEndpoint::close()
{
    if (m_cap >= 0) itsDisconnect(m_cap);
    if (m_play >= 0) itsDisconnect(m_play);
    m_cap = m_play = -1;
    m_wantCap = false;
    m_open = false;
    m_sink = nullptr;
}

void DeviceEndpoint::capture(bool on)
{
    m_wantCap = on && m_open;
    if (!m_wantCap && m_cap >= 0) {
        itsDisconnect(m_cap);
        m_cap = -1;
    }
}

void DeviceEndpoint::service()
{
    if (!m_open) return;
    int64_t now = esp_timer_get_time();
    if (m_play < 0 && now >= m_retryPlayUs) connectPlay();
    if (m_wantCap && m_cap < 0 && now >= m_retryCapUs) connectCap();
}

bool DeviceEndpoint::play(const int16_t* pcm, int n)
{
    if (!m_open || m_play < 0 || n <= 0 || n > MAX_BLOCK) return false;
    avHdrFill((av_hdr_t*)m_out, AV_TYPE_AUDIO, AUDIO_PCM16, 0, 0);
    memcpy(m_out + sizeof(av_hdr_t), pcm, (size_t)n * 2);
    return itsSend(m_play, m_out, sizeof(av_hdr_t) + (size_t)n * 2, 0) > 0;
}

/* The playback connection carries nothing back; drain whatever arrives. */
void DeviceEndpoint::onPlayRecv(int h, size_t)
{
    while (itsRecv(h, s_dev.m_buf, MAX_MSG, 0) > 0) {}
}

void DeviceEndpoint::onCapRecv(int h, size_t)
{
    size_t n;
    while ((n = itsRecv(h, s_dev.m_buf, MAX_MSG, 0)) > 0) {
        if (h != s_dev.m_cap || !s_dev.m_sink || n <= sizeof(av_hdr_t)) continue;
        const av_hdr_t* hd = (const av_hdr_t*)s_dev.m_buf;
        if (hd->type != AV_TYPE_AUDIO || hd->codec != AUDIO_PCM16) continue;
        s_dev.m_sink->onMic((const int16_t*)(s_dev.m_buf + sizeof(av_hdr_t)),
                            (int)((n - sizeof(av_hdr_t)) / 2));
    }
}

void DeviceEndpoint::onDisc(int ref)
{
    if (ref == REF_PLAY) {
        s_dev.m_play = -1;
        s_dev.m_retryPlayUs = esp_timer_get_time() + RETRY_US;
    } else if (ref == REF_CAP) {
        s_dev.m_cap = -1;
        s_dev.m_retryCapUs = esp_timer_get_time() + RETRY_US;
    }
}

} // namespace

LxstEndpoint* lxstDeviceEndpoint() { return &s_dev; }
