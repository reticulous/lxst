/**
 * lxst_endpoint_browser — the session endpoint on a browser: one ITS packet
 * connection on LXST_PORT_BROWSER, which the webrtc router opens for the
 * DataChannel "lxst:2" (one Stream Control Transmission Protocol (SCTP)
 * message = one packet, both ways).
 *
 *   browser ── [av_hdr][block] … ──▶ microphone
 *   browser ◀── [av_hdr][block] … ── the mix, in the connect payload's codec
 *   browser ── [LXST_DC_PTT][down]   ──▶ talk key
 *   browser ◀── [LXST_DC_OWNER][x]   ─── who owns the endpoint
 *   browser ◀── [LXST_DC_MIC][want]  ─── send the microphone or stop
 *
 * One browser at a time: a newer connect replaces the older, which is sent
 * [LXST_DC_OWNER][0] (it stops wanting the channel) and closed once that has
 * left or after RETIRE_US. Microphone blocks
 * reach the sink re-chunked to the session's block, only while the session is
 * using this endpoint and wants the microphone; a message that is neither a
 * talk key nor an audio av_hdr block is dropped. The endpoint opens only at
 * LXST_BROWSER_RATE. A control message the channel's window refuses is resent
 * from a short one-shot timer.
 */
#include "lxst.h"
#include "lxst_endpoint.h"
#include "lxst_dc.h"
#include "av_hdr.h"
#include "codec.h"
#include "its.h"
#include "log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/time.h>
#include <cstring>

namespace {

constexpr size_t MAX_MSG = 4096;            /* ≥ av_hdr + one pcm16 block at the ceiling */
static_assert(MAX_MSG >= sizeof(av_hdr_t) + LXST_EP_MAX_BLOCK * 2, "MAX_MSG");
/* Browser → lxst: a few microphone blocks in flight (25 pcm16 blocks at
 * 16 kHz / 20 ms). */
constexpr size_t TO_WINDOW = 4 * MAX_MSG;
/* lxst → browser: the mix plus control (12 pcm16 blocks at 16 kHz / 20 ms).
 * A slow channel drops blocks rather than growing latency. */
constexpr size_t FROM_WINDOW = 2 * MAX_MSG;
/* Packets in flight per direction. */
constexpr size_t DEPTH = 32;
/* Decoded inbound: ADPCM gives two samples per payload byte, the most of the
 * accepted codecs. */
constexpr int DEC_SAMPLES = 2 * (int)MAX_MSG;
constexpr uint64_t CTRL_RETRY_US = 50 * 1000;
constexpr int64_t  RETIRE_US = 1000 * 1000;

/* Inbound codecs that decode to the session's rate. */
bool micCodecOk(uint8_t c)
{
    return c == AUDIO_PCM16 || c == AUDIO_ULAW_16K || c == AUDIO_ADPCM;
}

class BrowserEndpoint : public LxstEndpoint {
public:
    bool open(LxstEndpointSink* sink, int rate, int blockMs) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    void capture(bool on) override;
    bool capturing() const override { return m_wantCap && m_h >= 0; }
    bool play(const int16_t* pcm, int n) override;

    static int  onConnect(int handle, const void* data, size_t len);
    static bool onBusy(const void* data, size_t len);
    static void onRecv(int handle, size_t);
    static void onDisc(int ref);
    static void retryCb(void*);

    void flush();
    void mic(const int16_t* pcm, int n);

    LxstBrowserEvents* m_ev = nullptr;
    LxstEndpointSink*  m_sink = nullptr;
    TaskHandle_t m_task = nullptr;
    esp_timer_handle_t m_retry = nullptr;
    int      m_h = -1;
    int      m_ref = 0;               /* generation of the current connection */
    int      m_old = -1, m_oldRef = -1;   /* the replaced connection, until it is closed */
    bool     m_oldSent = false;
    int64_t  m_oldUntilUs = 0;
    bool     m_open = false, m_wantCap = false;
    bool     m_owner = false;
    bool     m_downPending = false;   /* replaced; the successor has not connected yet */
    int      m_sentOwner = -1, m_sentMic = -1;   /* -1: not yet sent on this connection */
    int      m_spb = 320;
    int      m_refusedRate = 0;
    audio_codec_t m_outCodec = AUDIO_PCM16;
    codec_state_t m_enc{}, m_dec{};
    uint8_t  m_inCodec = 0xff;
    uint8_t* m_in = nullptr;          /* one inbound message */
    uint8_t* m_out = nullptr;         /* one outbound block  */
    int16_t* m_pcm = nullptr;         /* decoded inbound     */
    int16_t* m_chunk = nullptr;       /* re-chunking to m_spb */
    int      m_chunkFill = 0;
    bool     m_inConnect = false;     /* nothing is sent from inside onConnect */
};

BrowserEndpoint s_br;

audio_codec_t parseCodec(const void* data, size_t len)
{
    char tmp[24];
    size_t m = 0;
    const char* p = (const char*)data;
    while (data && m < len && m < sizeof(tmp) - 1 && p[m] && p[m] != ',') { tmp[m] = p[m]; m++; }
    tmp[m] = '\0';
    if (!strcmp(tmp, "ulaw16k")) return AUDIO_ULAW_16K;
    return AUDIO_PCM16;
}

int BrowserEndpoint::onConnect(int handle, const void* data, size_t len)
{
    BrowserEndpoint& b = s_br;
    if (b.m_old >= 0) { itsDisconnect(b.m_old); b.m_old = -1; }
    if (b.m_h >= 0) {                             /* told and closed from flush() */
        b.m_old = b.m_h;
        b.m_oldRef = b.m_ref;
        b.m_oldSent = false;
        b.m_oldUntilUs = esp_timer_get_time() + RETIRE_US;
        info("lxst: browser channel replaced");
    }
    b.m_h = handle;
    b.m_downPending = false;
    b.m_ref = (b.m_ref + 1) & 0x7f;
    b.m_outCodec = parseCodec(data, len);
    codecStateReset(&b.m_enc);
    codecStateReset(&b.m_dec);
    b.m_inCodec = 0xff;
    b.m_chunkFill = 0;
    b.m_sentOwner = b.m_sentMic = -1;           /* greeting goes out from flush() */
    info("lxst: browser channel connected (codec %d)", (int)b.m_outCodec);
    b.m_inConnect = true;
    if (b.m_ev) b.m_ev->onBrowserChannel(true);
    b.m_inConnect = false;
    return b.m_ref;
}

/* Both slots are taken (the current connection and one being retired): the
 * retired one goes at once. Failing that, the current one is dropped for the
 * newer, whose onConnect normally follows in the same dispatch; if it does
 * not, lxstBrowserFlush() reports the channel closed. */
bool BrowserEndpoint::onBusy(const void*, size_t)
{
    BrowserEndpoint& b = s_br;
    if (b.m_old >= 0) {
        itsDisconnect(b.m_old);
        b.m_old = -1;
        return false;
    }
    if (b.m_h >= 0) {
        uint8_t m[2] = { LXST_DC_OWNER, 0 };
        itsSend(b.m_h, m, sizeof(m), 0);
        itsDisconnect(b.m_h);
        b.m_h = -1;
        b.m_chunkFill = 0;
        b.m_downPending = true;
        if (b.m_task) xTaskNotifyGive(b.m_task);
        info("lxst: browser channel replaced");
    }
    return false;
}

void BrowserEndpoint::onDisc(int ref)
{
    BrowserEndpoint& b = s_br;
    if (b.m_old >= 0 && ref == b.m_oldRef) { b.m_old = -1; return; }
    if (ref != b.m_ref || b.m_h < 0) return;
    b.m_h = -1;
    b.m_chunkFill = 0;
    info("lxst: browser channel closed");
    if (b.m_ev) b.m_ev->onBrowserChannel(false);
}

void BrowserEndpoint::onRecv(int h, size_t)
{
    BrowserEndpoint& b = s_br;
    size_t n;
    while ((n = itsRecv(h, b.m_in, MAX_MSG, 0)) > 0) {
        if (h != b.m_h) continue;
        switch (lxsess::dcClassify(b.m_in, n, micCodecOk)) {
        case lxsess::DC_PTT:
            if (b.m_ev) b.m_ev->onBrowserPtt(b.m_in[1] != 0);
            break;
        case lxsess::DC_AUDIO: {
            if (!b.m_open || !b.m_wantCap || !b.m_sink) break;
            uint8_t codec = ((const av_hdr_t*)b.m_in)->codec;
            if (codec != b.m_inCodec) { b.m_inCodec = codec; codecStateReset(&b.m_dec); }
            size_t ns = audioDecode((audio_codec_t)codec, &b.m_dec, b.m_in + sizeof(av_hdr_t),
                                    n - sizeof(av_hdr_t), b.m_pcm);
            b.mic(b.m_pcm, (int)ns);
            break;
        }
        default:
            break;
        }
    }
}

void BrowserEndpoint::mic(const int16_t* pcm, int n)
{
    while (n > 0 && m_sink) {
        int take = m_spb - m_chunkFill;
        if (take > n) take = n;
        memcpy(m_chunk + m_chunkFill, pcm, (size_t)take * 2);
        m_chunkFill += take;
        pcm += take;
        n -= take;
        if (m_chunkFill == m_spb) {
            m_chunkFill = 0;
            m_sink->onMic(m_chunk, m_spb);
        }
    }
}

void BrowserEndpoint::retryCb(void*)
{
    if (s_br.m_task) xTaskNotifyGive(s_br.m_task);
}

void BrowserEndpoint::flush()
{
    if (m_inConnect) return;
    bool failed = false;
    if (m_old >= 0) {
        if (!m_oldSent) {
            uint8_t m[2] = { LXST_DC_OWNER, 0 };
            m_oldSent = itsSend(m_old, m, sizeof(m), 0) > 0;
        }
        if ((m_oldSent && itsSendIsEmpty(m_old)) || esp_timer_get_time() >= m_oldUntilUs) {
            itsDisconnect(m_old);
            m_old = -1;
        } else {
            failed = true;                  /* look again from the retry timer */
        }
    }
    if (m_h < 0) {
        if (failed && m_retry && !esp_timer_is_active(m_retry))
            esp_timer_start_once(m_retry, CTRL_RETRY_US);
        return;
    }
    int owner = m_owner ? 1 : 0;
    if (owner != m_sentOwner) {
        uint8_t m[2] = { LXST_DC_OWNER, (uint8_t)owner };
        if (itsSend(m_h, m, sizeof(m), 0) > 0) m_sentOwner = owner;
        else failed = true;
    }
    int want = (m_open && m_wantCap) ? 1 : 0;
    if (want != m_sentMic) {
        uint8_t m[2] = { LXST_DC_MIC, (uint8_t)want };
        if (itsSend(m_h, m, sizeof(m), 0) > 0) m_sentMic = want;
        else failed = true;
    }
    if (failed && m_retry && !esp_timer_is_active(m_retry))
        esp_timer_start_once(m_retry, CTRL_RETRY_US);
}

bool BrowserEndpoint::open(LxstEndpointSink* sink, int rate, int blockMs)
{
    if (m_open) return true;
    if (m_h < 0 || !m_out || !m_chunk) return false;
    if (rate != LXST_BROWSER_RATE) {
        if (rate != m_refusedRate)
            err("lxst: the browser channel carries %d Hz only; the session runs at %d Hz",
                LXST_BROWSER_RATE, rate);
        m_refusedRate = rate;
        return false;
    }
    m_refusedRate = 0;
    m_spb = rate * blockMs / 1000;
    m_sink = sink;
    m_wantCap = false;
    m_chunkFill = 0;
    codecStateReset(&m_enc);
    m_open = true;
    return true;
}

void BrowserEndpoint::close()
{
    m_open = false;
    m_wantCap = false;
    m_sink = nullptr;
    m_chunkFill = 0;
    flush();
}

void BrowserEndpoint::capture(bool on)
{
    bool want = on && m_open;
    if (want == m_wantCap) return;
    m_wantCap = want;
    m_chunkFill = 0;
    flush();
}

bool BrowserEndpoint::play(const int16_t* pcm, int n)
{
    if (!m_open || m_h < 0 || n <= 0 || n > LXST_EP_MAX_BLOCK) return false;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint64_t epoch = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
    avHdrFill((av_hdr_t*)m_out, AV_TYPE_AUDIO, (uint8_t)m_outCodec, epoch, 0);
    size_t pb = audioEncode(m_outCodec, &m_enc, pcm, (size_t)n, m_out + sizeof(av_hdr_t));
    return itsSend(m_h, m_out, sizeof(av_hdr_t) + pb, 0) > 0;
}

} // namespace

LxstEndpoint* lxstBrowserEndpoint() { return &s_br; }

bool lxstBrowserStart(LxstBrowserEvents* ev)
{
    BrowserEndpoint& b = s_br;
    b.m_ev = ev;
    b.m_task = xTaskGetCurrentTaskHandle();
    b.m_in = (uint8_t*)heap_caps_malloc(MAX_MSG, MALLOC_CAP_SPIRAM);
    b.m_out = (uint8_t*)heap_caps_malloc(MAX_MSG, MALLOC_CAP_SPIRAM);
    b.m_pcm = (int16_t*)heap_caps_malloc((size_t)DEC_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    b.m_chunk = (int16_t*)heap_caps_malloc((size_t)LXST_EP_MAX_BLOCK * 2, MALLOC_CAP_SPIRAM);
    if (!b.m_in || !b.m_out || !b.m_pcm || !b.m_chunk) {
        err("lxst: no memory for the browser endpoint");
        return false;
    }
    esp_timer_create_args_t ra = {};
    ra.callback = BrowserEndpoint::retryCb;
    ra.name = "lxst_br_retry";
    if (esp_timer_create(&ra, &b.m_retry) != ESP_OK) b.m_retry = nullptr;
    if (!itsServerPortOpen(LXST_PORT_BROWSER, ITS_PACKET, 2, TO_WINDOW, FROM_WINDOW,
                           DEPTH, MAX_MSG)) {
        err("lxst: browser port open failed");
        return false;
    }
    itsServerOnConnect(LXST_PORT_BROWSER, BrowserEndpoint::onConnect);
    itsServerOnBusy(LXST_PORT_BROWSER, BrowserEndpoint::onBusy);
    itsServerOnRecv(LXST_PORT_BROWSER, BrowserEndpoint::onRecv);
    itsServerOnDisconnect(LXST_PORT_BROWSER, BrowserEndpoint::onDisc);
    return true;
}

bool lxstBrowserConnected() { return s_br.m_h >= 0; }

void lxstBrowserOwner(bool browser)
{
    s_br.m_owner = browser;
    s_br.flush();
}

void lxstBrowserFlush()
{
    BrowserEndpoint& b = s_br;
    if (b.m_downPending && b.m_h < 0) {
        b.m_downPending = false;
        info("lxst: browser channel closed");
        if (b.m_ev) b.m_ev->onBrowserChannel(false);
    }
    b.flush();
}
