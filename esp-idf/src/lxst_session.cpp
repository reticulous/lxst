/**
 * lxst_session — the lxst task: the one voice session (see lxst.h).
 *
 * Everything runs on the lxst task, driven by ITS callbacks, storage
 * subscriptions and two esp_timers whose callbacks only flag and notify:
 *
 *   playout clock  periodic at the endpoint's block; runs while a speaker
 *                  exists, a call tone sounds, or the test tone is feeding
 *                  the transmitter
 *   release tail   one-shot after the talk key is released
 *
 *   consumer ─ OP_FRAMES ─▶ per-speaker LxstJitter (encoded) ─ due ─▶ decode
 *            ─▶ per-speaker PCM leftover ─▶ mix one block (+ call tone)
 *            ─▶ endpoint.play
 *   endpoint mic (or tone) ─▶ unit buffer ─▶ audio_fc encode ─▶ LXST frame
 *            ─▶ LxstBatcher ─ EV_BATCH ─▶ consumer
 *
 * The consumer is the room (port 1) or a live call (port 3), lxst_arb.h. The
 * endpoint is the owner's (lxst_owner.h): the device engine or the browser
 * channel. It is open while a consumer has the session or a call tone sounds.
 * A change of owner ends the talk spurt and closes the old endpoint.
 */
#include "lxst.h"
#include "lxst_sess_profile.h"
#include "lxst_batch.h"
#include "lxst_jitter.h"
#include "lxst_dsp.h"
#include "lxst_tones.h"
#include "lxst_arb.h"
#include "lxst_endpoint.h"
#include "lxst_owner.h"
#include "lxst_mem.h"
#include "audio_fc.h"

#include "spangap.h"
#include "mem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#if CONFIG_SPANGAP_LCD
#include "lcd_keys.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ───────────── tuning ───────────── */

/** lxst task stack (PSRAM). Sized for an Opus encode. */
constexpr uint32_t LXST_TASK_STACK = 40 * 1024;
/** On the secondary core, below the LCD render task that shares it. */
constexpr UBaseType_t LXST_TASK_PRIO = 2;
/** Opus encoder complexity, 0..10. */
constexpr int OPUS_COMPLEXITY = 2;
constexpr int MAX_SPEAKERS = 8;
/** Voice-operated transmit (VOX): the gate stays open this long after the
 *  level falls below s.lxst.vox_db. */
constexpr int VOX_HANG_MS = 400;
constexpr int STATS_MS = 1000;
/** Wake period while the wanted microphone connection is down. */
constexpr int RETRY_WAKE_MS = 1000;
/** Retry period for a held-key subscription that failed. */
constexpr int KEY_RETRY_MS = 5000;
#ifdef CONFIG_AUDIO_RATE_DEFAULT
constexpr int ENGINE_RATE_DEFAULT = CONFIG_AUDIO_RATE_DEFAULT;
#else
constexpr int ENGINE_RATE_DEFAULT = 16000;
#endif
#ifdef CONFIG_AUDIO_BLOCK_MS_DEFAULT
constexpr int ENGINE_BLOCK_MS_DEFAULT = CONFIG_AUDIO_BLOCK_MS_DEFAULT;
#else
constexpr int ENGINE_BLOCK_MS_DEFAULT = 20;
#endif
/** Playout ticks processed at once after the task was held up; more are
 *  skipped. */
constexpr int MAX_CATCHUP = 4;
/** Largest decoded unit: 120 ms (Opus's longest frame) at 48 kHz. */
constexpr int MAX_UNIT_SAMPLES = 5760;
constexpr int MAX_FRAME = lxsess::MAX_FRAME_BYTES;
/** Aux port for held-key edges (lcd_keys.h). */
constexpr uint16_t AUX_KEYS = 2;
constexpr size_t SESSION_MAX_MSG = 8 + (size_t)LXST_MAX_BATCH * (2 + MAX_FRAME);

static const char* TAG = LXST_TASK;

/* ───────────── memory ───────────── */

void* lxstAlloc(size_t n)
{
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) warn("lxst: no PSRAM for %u B", (unsigned)n);
    return p;
}

void lxstFree(void* p) { heap_caps_free(p); }

/* ───────────── state (lxst task only) ───────────── */

/* File-local types: an anonymous namespace keeps them from colliding with
 * same-named types in other straddles under the one-definition rule. */
namespace {

enum TxMode  { MODE_PTT = lxsess::TXSEL_PTT, MODE_VOX = lxsess::TXSEL_VOX,
               MODE_OPEN = lxsess::TXSEL_OPEN };
enum TxState { TX_IDLE, TX_ON, TX_TAIL };

struct Speaker {
    bool        used = false;
    uint32_t    id = 0;
    LxstJitter  jb;
    audio_fc_t* dec = nullptr;
    uint8_t     decCodec = 0xFF, decMode = 0;   /* what dec decodes */
    int         unitMs = 20, c2Bytes = 0;       /* its unit: a Codec2 sub-frame, or one
                                                 * concealed Opus frame */
    int16_t*    pcm = nullptr;          /* one decoded unit */
    int         pcmPos = 0, pcmLen = 0;
    uint8_t     cur[MAX_FRAME];         /* the frame being played */
    uint16_t    curLen = 0;
    int         subIdx = 0, subLeft = 0;
    bool        conceal = false;
    uint32_t    decUs = 0;              /* this frame's decode time so far */
    bool        audible = false;
    bool        releasing = false;
    lxsess::SpurtStats sp;
};

struct Stats {
    uint64_t encSum = 0; uint32_t encN = 0, encMax = 0;
    uint64_t decSum = 0; uint32_t decN = 0, decMax = 0;
    uint32_t txFrames = 0, txDropped = 0;
    uint32_t rxFrames = 0, concealed = 0, late = 0, dropped = 0, underruns = 0;
    uint32_t blocksDropped = 0, ticksSkipped = 0;
};

struct TxSpurt {
    uint32_t frames = 0, bytes = 0, encMax = 0;
    uint64_t encSum = 0;
};

class Sink : public LxstEndpointSink {
public:
    void onMic(const int16_t* pcm, int n) override;
};

class BrowserEvents : public LxstBrowserEvents {
public:
    void onBrowserChannel(bool up) override;
    void onBrowserPtt(bool down) override;
};

/* One consumer's handle and its own last profile and state, kept while it
 * does not have the session (a pending call, a held room). */
struct Consumer {
    int            h = -1;
    lxst_profile_t prof{};
    bool           canSpeak = false, muted = false, deaf = false;

    void connect(int handle, const lxst_profile_t& p)
    {
        h = handle;
        prof = p;
        canSpeak = p.can_speak != 0;
        muted = deaf = false;
    }
};

/* The room and call side of the session. */
struct Tel {
    lxsess::Arbiter  arb;
    Consumer         room, call;
    lxsess::CallTone tone;
    bool             audioOpen = false;     /* format resolved and the endpoint open */
    bool             callClosePending = false;
    int8_t           heldOut = -1;          /* LXST_EV_HELD value still to send; -1 none */
    uint8_t          setMode = 0;           /* s.lxst.tx as a TxMode */
};

}  // namespace

static TaskHandle_t   s_task = nullptr;
static Sink           s_sink;
static BrowserEvents  s_brEv;
static LxstEndpoint*  s_ep = nullptr;

PSRAM_BSS static Tel  s_t;
static bool           s_live = false;       /* a consumer has the session */
static bool           s_openPending = false, s_closePending = false;
static lxst_profile_t s_prof{};
static bool           s_profOk = false;
static bool           s_canSpeak = false, s_muted = false, s_deaf = false;
static int            s_rate = 16000, s_blockMs = 20, s_spb = 320;

static TxMode   s_mode = MODE_PTT;
static TxState  s_tx = TX_IDLE;
static int      s_voxDb = -45;
static int64_t  s_voxUntilUs = 0;
static bool     s_voxOpen = false;
static lxsess::Ownership s_own;             /* endpoint owner and the four talk sources */
static int      s_toneHz = 0;
static lxsess::Tone s_tone;
static bool     s_keysSubscribed = false;
static int64_t  s_nextKeyTryUs = 0;

static audio_fc_t* s_enc = nullptr;
static int      s_unitMs = 0, s_unitSamples = 0, s_unitsPerFrame = 1, s_c2Bytes = 0;
static int16_t* s_unitBuf = nullptr;
static int      s_unitFill = 0;
PSRAM_BSS static uint8_t s_frame[MAX_FRAME];
static int      s_frameLen = 0, s_frameUnits = 0;
static uint32_t s_frameEncUs = 0;
static LxstBatcher s_batch;
static TxSpurt  s_txSp;
static int64_t  s_txStartUs = 0;

PSRAM_BSS static Speaker s_spk[MAX_SPEAKERS];
static int32_t* s_acc = nullptr;
static int16_t* s_mix = nullptr;
static int16_t* s_toneBuf = nullptr;
static uint8_t* s_rx = nullptr;

static esp_timer_handle_t s_clock = nullptr, s_tail = nullptr;
static std::atomic<int>   s_ticks{0};
static std::atomic<bool>  s_tailFired{false};
static std::atomic<int>   s_micLevel{0};    /* lxstMicLevel(): 0–100 while a spurt is sent */
static bool     s_clockOn = false;

static Stats    s_st;
static bool     s_statsDirty = false;
static int64_t  s_nextStatsUs = 0;

static void txEval();

static uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* ───────────── events to the consumer ───────────── */

static Consumer* activeConsumer()
{
    switch (s_t.arb.active()) {
    case lxsess::Arbiter::ROOM: return &s_t.room;
    case lxsess::Arbiter::CALL: return &s_t.call;
    default:                    return nullptr;
    }
}

static bool sendEv(const void* m, size_t n)
{
    Consumer* c = activeConsumer();
    if (!c || c->h < 0) return false;
    return itsSend(c->h, m, n, 0) > 0;
}

/* LXST_EV_HELD to the room, from the task loop and receive callbacks (never
 * from a connect callback); one that does not fit is tried again. */
static void flushHeld()
{
    if (s_t.heldOut < 0) return;
    if (s_t.room.h < 0) { s_t.heldOut = -1; return; }
    uint8_t m[2] = { LXST_EV_HELD, (uint8_t)s_t.heldOut };
    if (itsSend(s_t.room.h, m, sizeof(m), 0) > 0) s_t.heldOut = -1;
}

static void publishSessState()
{
    storageBegin();
    storageSet("lxst.state", s_t.arb.state());
    storageSet("lxst.held", s_t.arb.held() ? 1 : 0);
    storageEnd();
}

static void evSpeaking(uint32_t id, bool on)
{
    uint8_t m[6] = { LXST_EV_SPEAKING };
    memcpy(m + 1, &id, 4);
    m[5] = on ? 1 : 0;
    sendEv(m, sizeof(m));
}

static void emitBatch(void*, const uint8_t* msg, size_t len)
{
    if (!sendEv(msg, len)) s_st.txDropped += msg[1];
}

/* ───────────── codecs ───────────── */

static int c2FcMode(uint8_t m)
{
    switch (m) {
    case LXST_C2_3200: return AUDIO_FC_C2_3200;
    case LXST_C2_2400: return AUDIO_FC_C2_2400;
    case LXST_C2_1600: return AUDIO_FC_C2_1600;
    case LXST_C2_1400: return AUDIO_FC_C2_1400;
    case LXST_C2_1300: return AUDIO_FC_C2_1300;
    case LXST_C2_1200: return AUDIO_FC_C2_1200;
    default:           return AUDIO_FC_C2_700C;
    }
}

static audio_fc_t* codecCreate()
{
    if (s_prof.codec == LXST_CODEC_CODEC2)
        return audioFcCreate(AUDIO_FC_CODEC2, c2FcMode(s_prof.mode), s_rate, 1);
    lxsess::OpusPreset o{};
    lxsess::opusPreset(s_prof.mode, &o);
    return audioFcCreate(AUDIO_FC_OPUS, o.voip ? AUDIO_FC_OPUS_VOIP : AUDIO_FC_OPUS_AUDIO,
                         s_rate, 1);
}

/* The speaker's decoder decodes this codec and Codec2 mode, rebuilt when it
 * does not. False when no decoder could be made (the speaker plays silence
 * until a frame it can decode). */
static bool speakerCodec(Speaker& s, uint8_t codec, uint8_t mode)
{
    bool c2 = codec == LXST_CODEC_CODEC2;
    if (s.dec && s.decCodec == codec && (!c2 || s.decMode == mode)) return true;
    audioFcDestroy(s.dec);
    s.dec = c2 ? audioFcCreate(AUDIO_FC_CODEC2, c2FcMode(mode), s_rate, 1)
               : audioFcCreate(AUDIO_FC_OPUS, AUDIO_FC_OPUS_VOIP, s_rate, 1);
    s.decCodec = s.dec ? codec : 0xFF;
    s.decMode = mode;
    s.c2Bytes = c2 ? lxstC2SubframeBytes(mode) : 0;
    s.unitMs = c2 ? lxstC2SubframeMs(mode)
             : s_prof.codec == LXST_CODEC_OPUS ? s_prof.frame_ms : 20;
    if (s.unitMs < 1) s.unitMs = 20;
    return s.dec != nullptr;
}

static void encoderBuild()
{
    audioFcDestroy(s_enc);
    s_enc = nullptr;
    s_unitSamples = 0;
    if (!s_profOk) return;
    /* A unit is what one codec call takes: a Codec2 sub-frame, a whole Opus frame. */
    bool c2 = s_prof.codec == LXST_CODEC_CODEC2;
    s_unitMs = c2 ? lxstC2SubframeMs(s_prof.mode) : s_prof.frame_ms;
    s_unitsPerFrame = s_prof.frame_ms / s_unitMs;
    s_c2Bytes = c2 ? lxstC2SubframeBytes(s_prof.mode) : 0;
    s_enc = codecCreate();
    if (!s_enc) { err("lxst: no encoder for this profile at %d Hz", s_rate); return; }
    if (s_prof.codec == LXST_CODEC_CODEC2) {
        if (audioFcNativeFrameBytes(s_enc) != s_c2Bytes) {
            err("lxst: codec2 frame is %d B, LXST mode says %d",
                audioFcNativeFrameBytes(s_enc), s_c2Bytes);
            audioFcDestroy(s_enc);
            s_enc = nullptr;
            return;
        }
    } else {
        lxsess::OpusPreset o{};
        lxsess::opusPreset(s_prof.mode, &o);
        audioFcSet(s_enc, AUDIO_FC_BITRATE, o.bitrate);
        audioFcSet(s_enc, AUDIO_FC_COMPLEXITY, OPUS_COMPLEXITY);
        audioFcSet(s_enc, AUDIO_FC_BANDWIDTH, o.narrow ? 4000 : 8000);
    }
    s_unitSamples = audioFcFrameSamples(s_enc, s_unitMs);
    if (s_unitSamples <= 0 || s_unitSamples > MAX_UNIT_SAMPLES) {
        err("lxst: encoder refuses %d ms frames", s_unitMs);
        audioFcDestroy(s_enc);
        s_enc = nullptr;
        s_unitSamples = 0;
    }
}

/* ───────────── transmit ───────────── */

static bool micWanted()
{
    if (!s_live || !s_enc || !s_canSpeak || s_muted) return false;
    return s_mode != MODE_PTT || s_tx != TX_IDLE;
}

static bool anySpeaker()
{
    for (auto& s : s_spk) if (s.used) return true;
    return false;
}

static void clockUpdate()
{
    bool want = s_t.audioOpen
             && (anySpeaker() || s_t.tone.active() || (s_toneHz > 0 && micWanted()));
    if (want == s_clockOn || !s_clock) return;
    if (want) {
        s_ticks.store(0);
        esp_timer_start_periodic(s_clock, (uint64_t)s_blockMs * 1000);
    } else {
        esp_timer_stop(s_clock);
    }
    s_clockOn = want;
}

static void micUpdate()
{
    if (!s_ep || !s_ep->isOpen()) return;
    s_ep->capture(micWanted() && s_toneHz <= 0);
}

static void spurtStart()
{
    s_tx = TX_ON;
    s_unitFill = 0;
    s_frameUnits = 0;
    s_frameLen = 0;
    s_frameEncUs = 0;
    s_batch.clear();
    s_txSp = TxSpurt();
    s_txStartUs = esp_timer_get_time();
    evSpeaking(LXST_SPEAKER_SELF, true);
    storageSet("lxst.talking", 1);
    s_statsDirty = true;
}

/* End the talk spurt. `send`: flush the partial batch and mark end-of-talk;
 * otherwise drop it (profile change, consumer gone). */
static void spurtEnd(bool send)
{
    if (s_tx == TX_IDLE) return;
    if (s_tail) esp_timer_stop(s_tail);
    s_tailFired.store(false);
    s_tx = TX_IDLE;
    s_micLevel.store(0, std::memory_order_relaxed);
    s_unitFill = 0;
    s_frameUnits = 0;
    if (send) {
        s_batch.flush(emitBatch, nullptr);
        uint8_t m = LXST_EV_TALK_END;
        sendEv(&m, 1);
    } else {
        s_batch.clear();
    }
    evSpeaking(LXST_SPEAKER_SELF, false);
    storageSet("lxst.talking", 0);
    if (s_txSp.frames) {
        char line[128];
        snprintf(line, sizeof(line), "tx spurt: %.2f s, %u frames, %u B, enc avg %u us max %u us",
                 s_txSp.frames * s_prof.frame_ms / 1000.0, (unsigned)s_txSp.frames,
                 (unsigned)s_txSp.bytes, (unsigned)(s_txSp.encSum / s_txSp.frames),
                 (unsigned)s_txSp.encMax);
        info("%s", line);
        storageSet("lxst.stats.last_tx", line);
    }
    s_statsDirty = true;
}

static void frameDone()
{
    s_st.encSum += s_frameEncUs;
    s_st.encN++;
    if (s_frameEncUs > s_st.encMax) s_st.encMax = s_frameEncUs;
    s_txSp.encSum += s_frameEncUs;
    if (s_frameEncUs > s_txSp.encMax) s_txSp.encMax = s_frameEncUs;
    s_txSp.frames++;
    s_txSp.bytes += s_frameLen;
    s_st.txFrames++;
    s_batch.add(s_frame, (uint16_t)s_frameLen, emitBatch, nullptr);
    s_frameUnits = 0;
    s_frameLen = 0;
    s_frameEncUs = 0;
}

static void encodeUnit()
{
    int64_t t0 = esp_timer_get_time();
    if (s_prof.codec == LXST_CODEC_CODEC2) {
        if (!s_frameUnits) {
            s_frame[0] = LXST_CODEC_CODEC2;
            s_frame[1] = s_prof.mode;
            s_frameLen = 2;
        }
        int r = audioFcEncode(s_enc, s_unitBuf, s_unitSamples, s_frame + s_frameLen,
                              MAX_FRAME - s_frameLen);
        s_frameEncUs += (uint32_t)(esp_timer_get_time() - t0);
        if (r != s_c2Bytes) {               /* a short sub-frame would make the frame invalid */
            s_frameUnits = 0;
            s_frameEncUs = 0;
            return;
        }
        s_frameLen += r;
        if (++s_frameUnits == s_unitsPerFrame) frameDone();
        return;
    }
    s_frame[0] = LXST_CODEC_OPUS;
    int cap = s_prof.max_frame_bytes < MAX_FRAME ? s_prof.max_frame_bytes : MAX_FRAME;
    int r = audioFcEncode(s_enc, s_unitBuf, s_unitSamples, s_frame + 1, cap - 1);
    s_frameEncUs = (uint32_t)(esp_timer_get_time() - t0);
    if (r <= 0) return;
    s_frameLen = 1 + r;
    frameDone();
}

/* Microphone (or test tone) PCM: the voice gate, then the encoder. */
static void txInput(const int16_t* pcm, int n)
{
    float rms = lxsess::blockRms(pcm, n);
    if (s_mode == MODE_VOX) {
        int64_t now = esp_timer_get_time();
        if (lxsess::rmsDbfs(rms) > (float)s_voxDb)
            s_voxUntilUs = now + (int64_t)VOX_HANG_MS * 1000;
        bool open = now < s_voxUntilUs;
        if (open != s_voxOpen) { s_voxOpen = open; txEval(); }
    }
    if (s_tx == TX_IDLE || !s_enc || !s_unitBuf) return;
    float lvl = rms * 4.0f;
    s_micLevel.store(lvl >= 1.0f ? 100 : (int)(lvl * 100.0f + 0.5f), std::memory_order_relaxed);
    while (n > 0) {
        int take = s_unitSamples - s_unitFill;
        if (take > n) take = n;
        memcpy(s_unitBuf + s_unitFill, pcm, (size_t)take * 2);
        s_unitFill += take;
        pcm += take;
        n -= take;
        if (s_unitFill == s_unitSamples) {
            s_unitFill = 0;
            encodeUnit();
        }
    }
}

void Sink::onMic(const int16_t* pcm, int n)
{
    if (s_toneHz > 0) return;               /* the tone stands in for the mic */
    txInput(pcm, n);
}

static void txEval()
{
    bool gateOk = s_live && s_enc && s_canSpeak && !s_muted;
    bool keyed = s_mode == MODE_OPEN ? true
               : s_mode == MODE_VOX  ? s_voxOpen
               : s_own.keyed();
    switch (s_tx) {
    case TX_IDLE:
        if (gateOk && keyed) spurtStart();
        break;
    case TX_ON:
        if (!gateOk) spurtEnd(true);
        else if (!keyed) {
            if (s_mode == MODE_PTT && s_tail) {
                s_tx = TX_TAIL;
                s_tailFired.store(false);
                esp_timer_start_once(s_tail, (uint64_t)lxsess::tailMs(s_prof.frame_ms) * 1000);
            } else {
                spurtEnd(true);
            }
        }
        break;
    case TX_TAIL:
        if (!gateOk) spurtEnd(true);
        else if (keyed) { esp_timer_stop(s_tail); s_tx = TX_ON; }
        break;
    }
    micUpdate();
    clockUpdate();
}

/* ───────────── receive ───────────── */

static void speakerFree(Speaker& s)
{
    s.jb.release();
    audioFcDestroy(s.dec);
    s.dec = nullptr;
    s.decCodec = 0xFF;
    if (s.pcm) lxstFree(s.pcm);
    s.pcm = nullptr;
    s.used = false;
}

static void speakerRelease(Speaker& s, bool notify)
{
    if (s.audible) {
        if (notify) evSpeaking(s.id, false);
        char line[160];
        snprintf(line, sizeof(line),
                 "rx spurt speaker %u: %.2f s, %u frames, %u concealed, %u late, "
                 "rms %.1f dBFS, peak %.1f dBFS, ~%d Hz",
                 (unsigned)s.id, s.sp.samples / (double)s_rate, (unsigned)s.sp.frames,
                 (unsigned)s.sp.concealed, (unsigned)s.sp.late, s.sp.rmsDbfs(),
                 s.sp.peakDbfs(), s.sp.freqHz(s_rate));
        info("%s", line);
        storageSet("lxst.stats.last_rx", line);
    }
    speakerFree(s);
    s_statsDirty = true;
}

static void releaseAll(bool notify)
{
    for (auto& s : s_spk) if (s.used) speakerRelease(s, notify);
    clockUpdate();
}

static Speaker* speakerFind(uint32_t id)
{
    for (auto& s : s_spk) if (s.used && s.id == id) return &s;
    return nullptr;
}

static Speaker* speakerNew(uint32_t id)
{
    Speaker* s = nullptr;
    for (auto& c : s_spk) if (!c.used) { s = &c; break; }
    if (!s) return nullptr;
    int startMs = s_prof.jitter_ms ? s_prof.jitter_ms : storageGetInt("s.lxst.jitter_ms", 200);
    lxsess::JitterSize js = lxsess::jitterSize(s_prof, startMs);
    LxstJitterCfg cfg{ s_prof.frame_ms, js.frameBytes, js.startMs, js.maxMs,
                       lxsess::holdMs(js.startMs), s_prof.sequenced && !s_prof.rx_any };
    s->id = id;
    s->pcm = (int16_t*)lxstAlloc(MAX_UNIT_SAMPLES * sizeof(int16_t));
    if (!s->pcm || !speakerCodec(*s, s_prof.codec, s_prof.mode) || !s->jb.init(cfg)) {
        warn("lxst: no room for speaker %u", (unsigned)id);
        s->used = true;
        speakerFree(*s);
        return nullptr;
    }
    s->used = true;
    s->pcmPos = s->pcmLen = 0;
    s->curLen = 0;
    s->subIdx = s->subLeft = 0;
    s->conceal = false;
    s->decUs = 0;
    s->audible = false;
    s->releasing = false;
    s->sp.reset();
    return s;
}

static void rxFrame(uint32_t id, uint16_t seq, const uint8_t* f, uint16_t len)
{
    s_st.rxFrames++;
    Speaker* s = speakerFind(id);
    if (!s && !(s = speakerNew(id))) { s_st.dropped++; return; }
    int dropped = 0;
    switch (s->jb.push(seq, f, len, nowMs(), &dropped)) {
    case LxstJitter::PUSH_LATE: s_st.late++; s->sp.late++; break;
    case LxstJitter::PUSH_BAD:  s_st.dropped++; break;
    default: break;
    }
    s_st.dropped += dropped;
}

/* Load the next frame of a speaker for decoding, sub-frame by sub-frame. */
static void loadFrame(Speaker& s, const uint8_t* f, uint16_t len)
{
    /* Without rx_any a good frame is in the profile's codec and mode, which
     * the decoder already is; with it the decoder follows the frame. */
    bool ok = f && lxsess::rxFrameOk(s_prof, f, len)
                && speakerCodec(s, f[0], f[0] == LXST_CODEC_CODEC2 ? f[1] : 0);
    s.subIdx = 0;
    s.decUs = 0;
    if (ok) {
        memcpy(s.cur, f, len);
        s.curLen = len;
        s.conceal = false;
        s.subLeft = s.decCodec == LXST_CODEC_CODEC2 ? (len - 2) / s.c2Bytes : 1;
        s.sp.frames++;
        return;
    }
    s.subLeft = s_prof.frame_ms / s.unitMs;    /* one slot's time */
    if (s.subLeft < 1) s.subLeft = 1;
    s.conceal = true;
    s.curLen = 0;
    s.sp.concealed++;
    s_st.concealed++;
}

static void decodeUnit(Speaker& s)
{
    int unit = s_rate * s.unitMs / 1000;
    if (unit > MAX_UNIT_SAMPLES) unit = MAX_UNIT_SAMPLES;
    int64_t t0 = esp_timer_get_time();
    int got;
    if (s.decCodec == LXST_CODEC_CODEC2) {
        const uint8_t* in = s.conceal ? nullptr : s.cur + 2 + s.subIdx * s.c2Bytes;
        got = audioFcDecode(s.dec, in, s.conceal ? 0 : s.c2Bytes, s.pcm, unit);
    } else if (s.conceal) {
        got = audioFcDecode(s.dec, nullptr, 0, s.pcm, unit);
    } else {
        got = audioFcDecode(s.dec, s.cur + 1, s.curLen - 1, s.pcm, MAX_UNIT_SAMPLES);
    }
    s.decUs += (uint32_t)(esp_timer_get_time() - t0);
    if (got <= 0) {                          /* keep time moving */
        memset(s.pcm, 0, (size_t)unit * 2);
        got = unit;
    }
    s.pcmPos = 0;
    s.pcmLen = got;
    s.subIdx++;
    if (--s.subLeft == 0) {
        s_st.decSum += s.decUs;
        s_st.decN++;
        if (s.decUs > s_st.decMax) s_st.decMax = s.decUs;
    }
}

/* Add one block of this speaker into the mix. True when it is playing. */
static bool speakerFill(Speaker& s, int32_t* acc, int n, uint32_t now)
{
    int pos = 0;
    bool played = false;
    while (pos < n) {
        if (s.pcmPos < s.pcmLen) {
            int take = s.pcmLen - s.pcmPos;
            if (take > n - pos) take = n - pos;
            const int16_t* p = s.pcm + s.pcmPos;
            for (int i = 0; i < take; i++) acc[pos + i] += p[i];
            s.sp.addBlock(p, take);
            s.pcmPos += take;
            pos += take;
            played = true;
            continue;
        }
        if (s.subLeft > 0) { decodeUnit(s); continue; }
        const uint8_t* f = nullptr;
        uint16_t len = 0;
        bool underrun = false;
        LxstJitter::Pop r = s.jb.pop(now, &f, &len, &underrun);
        if (underrun) s_st.underruns++;
        if (r == LxstJitter::POP_WAIT) break;
        if (r == LxstJitter::POP_RELEASE) { s.releasing = true; break; }
        if (!s.audible) {
            s.audible = true;
            evSpeaking(s.id, true);
        }
        loadFrame(s, r == LxstJitter::POP_FRAME ? f : nullptr, len);   /* empty slot: conceal */
    }
    return played;
}

static void playoutTick(uint32_t now)
{
    memset(s_acc, 0, (size_t)s_spb * sizeof(int32_t));
    bool any = false;
    for (auto& s : s_spk) if (s.used && speakerFill(s, s_acc, s_spb, now)) any = true;
    for (auto& s : s_spk) if (s.used && s.releasing) speakerRelease(s, true);
    if (s_t.tone.active()) {                 /* its gaps too: the block stream stays even */
        s_t.tone.mix(s_acc, s_spb, s_rate);
        any = true;
    }
    if (!any) return;
    for (int i = 0; i < s_spb; i++) {
        int32_t v = s_acc[i];
        s_mix[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (s_ep && s_ep->isOpen() && !s_ep->play(s_mix, s_spb)) s_st.blocksDropped++;
}

/* ───────────── profile and session ───────────── */

/* The transmit keying in force: the profile's tx_mode over s.lxst.tx. */
static TxMode effMode()
{
    return (TxMode)lxsess::txSelect(s_prof.tx_mode, (lxsess::TxSel)s_t.setMode);
}

static void publishTxMode()
{
    const char* n = s_mode == MODE_OPEN ? "open" : s_mode == MODE_VOX ? "vox" : "ptt";
    storageSet("lxst.tx_mode", s_profOk ? n : "");
}

static void applyProfile(const lxst_profile_t& p)
{
    spurtEnd(false);
    releaseAll(true);
    s_prof = p;
    s_profOk = lxsess::profileValid(p);
    s_canSpeak = p.can_speak != 0;
    TxMode m = effMode();
    if (m != s_mode) {
        s_mode = m;
        s_voxOpen = false;
    }
    char name[64] = "";
    if (s_profOk) {
        lxsess::profileName(p, name, sizeof(name));
        if (!s_batch.init(p.batch_budget, p.batch_base, p.batch_per_frame,
                          p.batch_max_frames, p.max_frame_bytes)) {
            err("lxst: no memory for the batch");
            s_profOk = false;
        }
    } else {
        warn("lxst: invalid profile (codec %u mode %u, %u ms, %u B)", (unsigned)p.codec,
             (unsigned)p.mode, (unsigned)p.frame_ms, (unsigned)p.max_frame_bytes);
    }
    encoderBuild();
    storageSet("lxst.profile", name);
    publishTxMode();
    info("lxst: profile %s", s_profOk ? name : "(invalid)");
    txEval();
}

static LxstEndpoint* ownerEndpoint()
{
    return s_own.browserOwns() ? lxstBrowserEndpoint() : lxstDeviceEndpoint();
}

static void publishOwner()
{
    bool b = s_own.browserOwns();
    lxstBrowserOwner(b);
    storageSet("lxst.endpoint", b ? "browser" : "device");
}

/* The owner changed: end the talk spurt, move the session to the new owner's
 * endpoint (closing the old one, so an unused device engine powers down). */
static void ownerChanged()
{
    publishOwner();
    info("lxst: endpoint is the %s", s_own.browserOwns() ? "browser" : "device");
    if (!s_t.audioOpen) return;
    spurtEnd(true);
    s_voxOpen = false;
    s_voxUntilUs = 0;
    LxstEndpoint* next = ownerEndpoint();
    if (next != s_ep) {
        if (s_ep) s_ep->close();
        s_ep = next;
        if (!s_ep->open(&s_sink, s_rate, s_blockMs)) warn("lxst: %s endpoint not up yet",
                                       s_own.browserOwns() ? "browser" : "audio");
    }
    txEval();
}

static void talkKey(lxsess::TalkSrc src, bool down)
{
    if (s_own.key(src, down)) ownerChanged();
    else if (s_live) txEval();
}

void BrowserEvents::onBrowserChannel(bool up)
{
    storageSet("lxst.browser", up ? 1 : 0);
    bool changed = up ? s_own.channelUp() : s_own.channelDown();
    if (changed) ownerChanged();
    else if (s_live) txEval();              /* a replacing channel starts with its key up */
}

void BrowserEvents::onBrowserPtt(bool down)
{
    talkKey(lxsess::TALK_BROWSER, down);
}

/* The format is resolved and the owner's endpoint opened when the first of a
 * session and a call tone needs them, and closed when neither does. */
static void audioOpen()
{
    if (s_t.audioOpen) return;
    lxsess::sessionFormat(storageGetInt("s.audio.rate", ENGINE_RATE_DEFAULT),
                          storageGetInt("s.audio.block_ms", ENGINE_BLOCK_MS_DEFAULT),
                          LXST_EP_MAX_BLOCK, &s_rate, &s_blockMs);
    s_spb = s_rate * s_blockMs / 1000;
    s_ep = ownerEndpoint();
    if (!s_ep->open(&s_sink, s_rate, s_blockMs)) warn("lxst: audio endpoint not up yet");
    s_t.audioOpen = true;
}

static void audioUpdate()
{
    bool want = s_live || s_t.tone.active();
    if (want && !s_t.audioOpen) {
        audioOpen();
    } else if (!want && s_t.audioOpen) {
        s_t.audioOpen = false;
        clockUpdate();                      /* stops it */
        if (s_ep) s_ep->close();
        return;
    }
    clockUpdate();
}

/* Give the session to a consumer: its own last profile, then its state. */
static void activate(const Consumer& c)
{
    applyProfile(c.prof);
    s_canSpeak = c.canSpeak;
    s_muted = c.muted;
    if (c.deaf && !s_deaf) releaseAll(true);
    s_deaf = c.deaf;
    txEval();
}

static void sessionOpen()
{
    s_openPending = false;
    Consumer* c = activeConsumer();
    if (!c || s_live) return;
    s_st = Stats();
    s_live = true;
    audioOpen();
    s_muted = s_deaf = false;
    s_own.releaseDevice();                    /* the "0" written below lands later */
    s_voxOpen = false;
    s_voxUntilUs = 0;
#if CONFIG_SPANGAP_LCD
    s_keysSubscribed = lcdKeySubscribe(AUX_KEYS);
    s_nextKeyTryUs = esp_timer_get_time() + (int64_t)KEY_RETRY_MS * 1000;
    if (!s_keysSubscribed) warn("lxst: no held-key subscription yet");
    lcdKeyClaim(LCD_KEY_PTT, true);
#endif
    storageSet("lxst.cmd.ptt", "0");
    activate(*c);
    publishSessState();
    s_statsDirty = true;
    info("lxst: session open for the %s, %d Hz, %d ms blocks",
         c == &s_t.call ? "call" : "room", s_rate, s_blockMs);
}

static void publishStats();

/* No consumer has the session any more. */
static void sessionClose()
{
    if (!s_live) return;
    spurtEnd(false);
    releaseAll(false);
    s_live = false;
    audioUpdate();
#if CONFIG_SPANGAP_LCD
    lcdKeyClaim(LCD_KEY_PTT, false);
    lcdKeyUnsubscribe();
    s_keysSubscribed = false;
#endif
    audioFcDestroy(s_enc);
    s_enc = nullptr;
    s_batch.release();
    s_profOk = false;
    s_own.releaseDevice();                    /* every device talk source starts released */
    s_voxOpen = false;
    storageSet("lxst.cmd.ptt", "0");
    storageSet("lxst.talking", 0);
    storageSet("lxst.profile", "");
    storageSet("lxst.tx_mode", "");
    publishSessState();
    publishStats();
    info("lxst: session closed");
}

/* The room left (deferred from its disconnect callback). */
static void roomClose()
{
    s_closePending = false;
    sessionClose();
}

/* The call port left (deferred from its disconnect callback): the tone stops;
 * a live call's session goes back to a held room, or closes. */
static void callClose()
{
    s_t.callClosePending = false;
    s_t.tone.set(LXST_TONE_NONE);
    bool resume = false;
    bool wasLive = s_t.arb.call() && s_t.arb.callLive();
    if (wasLive && s_live) {
        spurtEnd(false);                    /* nobody to send them to */
        releaseAll(false);
    }
    s_t.arb.callDisconnect(&resume);
    if (wasLive && s_live) {
        if (resume) {
            activate(s_t.room);
            s_t.heldOut = 0;
            info("lxst: call over, the room resumes");
        } else {
            sessionClose();
        }
    }
    audioUpdate();
    publishSessState();
}

/* Finish what a disconnect left; a connect callback runs this first. */
static void settleClose()
{
    if (s_closePending) roomClose();
    if (s_t.callClosePending) callClose();
}

/* OP_LIVE: the call takes the session; a room that has it is held. */
static void callLive()
{
    if (!s_t.arb.call() || s_t.arb.callLive()) return;
    bool roomHad = s_live;                  /* only the room can have it now */
    if (roomHad) {
        spurtEnd(false);                    /* the room's spurt and speakers go */
        releaseAll(true);
    }
    bool hold = false;
    s_t.arb.goLive(&hold);
    if (hold) s_t.heldOut = 1;
    if (roomHad) activate(s_t.call);
    else sessionOpen();                     /* also covers a room not yet opened */
    publishSessState();
    info("lxst: call live%s", hold ? ", the room is held" : "");
}

static void setTone(uint8_t kind)
{
    s_t.tone.set(kind);
    audioUpdate();
}

/* One message from a consumer. Profile and state are always kept as the
 * consumer's own; they and its frames reach the session only while it has
 * it. */
static void handleMsg(Consumer& c, bool isCall, const uint8_t* m, size_t n)
{
    if (!n) return;
    bool active = s_live && activeConsumer() == &c;
    switch (m[0]) {
    case LXST_OP_PROFILE:
        if (n >= 1 + sizeof(lxst_profile_t)) {
            lxst_profile_t p;
            memcpy(&p, m + 1, sizeof(p));
            c.prof = p;
            c.canSpeak = p.can_speak != 0;
            if (active) applyProfile(p);
        }
        break;
    case LXST_OP_FRAMES: {
        if (!active || n < 8 || !s_profOk || s_deaf) break;
        uint32_t id = rd32(m + 1);
        uint16_t seq = rd16(m + 5);
        int count = m[7];
        const uint8_t* p = m + 8;
        const uint8_t* end = m + n;
        for (int i = 0; i < count && p + 2 <= end; i++) {
            uint16_t len = rd16(p);
            p += 2;
            if (p + len > end) break;
            rxFrame(id, (uint16_t)(seq + i), p, len);
            p += len;
        }
        s_statsDirty = true;
        clockUpdate();
        break;
    }
    case LXST_OP_TALK_END:
        if (active && n >= 5) {
            if (Speaker* s = speakerFind(rd32(m + 1))) s->jb.talkEnd();
        }
        break;
    case LXST_OP_STATE:
        if (n >= 4) {
            c.canSpeak = m[1] != 0;
            c.muted = m[2] != 0;
            c.deaf = m[3] != 0;
            if (!active) break;
            s_canSpeak = c.canSpeak;
            s_muted = c.muted;
            if (c.deaf && !s_deaf) releaseAll(true);
            s_deaf = c.deaf;
            txEval();
        }
        break;
    case LXST_OP_TONE:
        if (isCall && n >= 2) setTone(m[1]);
        break;
    case LXST_OP_LIVE:
        if (isCall) callLive();
        break;
    default:
        break;
    }
}

/* ───────────── ITS callbacks ───────────── */

static bool profileFrom(const void* data, size_t len, lxst_profile_t* p)
{
    if (len < sizeof(lxst_profile_t)) return false;
    memcpy(p, data, sizeof(*p));
    return lxsess::profileValid(*p);
}

static int onSessConnect(int handle, const void* data, size_t len)
{
    settleClose();                              /* the last consumer left; finish that first */
    if (s_t.arb.room()) {
        warn("lxst: a session is already open; refusing another");
        return -1;
    }
    lxst_profile_t p;
    if (!profileFrom(data, len, &p)) {
        warn("lxst: session refused: invalid profile");
        return -1;
    }
    bool held = false;
    s_t.arb.roomConnect(&held);
    s_t.room.connect(handle, p);
    if (held) {
        s_t.heldOut = 1;                        /* sent from the task loop */
        info("lxst: room connected during a call: held");
    } else {
        s_openPending = true;
    }
    publishSessState();
    xTaskNotifyGive(s_task);
    return handle;                              /* the disconnect callback's ref */
}

static void onSessRecv(int handle, size_t)
{
    size_t n;
    while ((n = itsRecv(handle, s_rx, SESSION_MAX_MSG, 0)) > 0) {
        if (handle != s_t.room.h) continue;
        if (s_openPending) sessionOpen();
        flushHeld();
        handleMsg(s_t.room, false, s_rx, n);
    }
}

static void onSessDisconnect(int handle)
{
    if (handle != s_t.room.h) return;
    if (s_live && s_t.arb.active() == lxsess::Arbiter::ROOM) s_closePending = true;
    s_t.arb.roomDisconnect();
    s_t.room.h = -1;
    s_t.heldOut = -1;
    s_openPending = false;
    publishSessState();
    xTaskNotifyGive(s_task);
}

static int onCallConnect(int handle, const void* data, size_t len)
{
    settleClose();
    if (s_t.arb.call()) {
        warn("lxst: a call is already connected; refusing another");
        return -1;
    }
    lxst_profile_t p;
    if (!profileFrom(data, len, &p)) {
        warn("lxst: call refused: invalid profile");
        return -1;
    }
    s_t.arb.callConnect();
    s_t.call.connect(handle, p);
    info("lxst: call connected, pending");
    return handle;                              /* the disconnect callback's ref */
}

static void onCallRecv(int handle, size_t)
{
    size_t n;
    while ((n = itsRecv(handle, s_rx, SESSION_MAX_MSG, 0)) > 0) {
        if (handle != s_t.call.h) continue;
        if (s_openPending) sessionOpen();
        handleMsg(s_t.call, true, s_rx, n);
    }
    flushHeld();
}

static void onCallDisconnect(int handle)
{
    if (handle != s_t.call.h) return;
    s_t.call.h = -1;
    s_t.callClosePending = true;
    xTaskNotifyGive(s_task);
}

static void onKeyAux(TaskHandle_t, const void* data, size_t len)
{
#if CONFIG_SPANGAP_LCD
    if (len < sizeof(lcd_key_edge_t)) return;
    const lcd_key_edge_t* e = (const lcd_key_edge_t*)data;
    if (e->key == LCD_KEY_PTT || e->key == LCD_KEY_SPACE)
        talkKey(e->key == LCD_KEY_PTT ? lxsess::TALK_DEV_PTT : lxsess::TALK_DEV_SPACE,
                e->down != 0);
#else
    (void)data; (void)len;
#endif
}

static TxMode modeFrom(const std::string& v)
{
    if (v == "vox") return MODE_VOX;
    if (v == "open") return MODE_OPEN;
    return MODE_PTT;
}

static void onTxMode(const char*, const char*)
{
    s_t.setMode = (uint8_t)modeFrom(storageGetStr("s.lxst.tx", "ptt"));
    TxMode m = effMode();
    if (m == s_mode) return;
    spurtEnd(true);
    s_mode = m;
    s_voxOpen = false;
    publishTxMode();
    txEval();
}

static void onClaim(const char*, const char* val)
{
    if (!val || strcmp(val, "device")) return;
    if (s_own.claimDevice()) ownerChanged();
    storageSet("lxst.cmd.claim", "");
}

static void onVoxDb(const char*, const char*)
{
    s_voxDb = storageGetInt("s.lxst.vox_db", -45);
}

static void onCmdPtt(const char*, const char* val)
{
    talkKey(lxsess::TALK_DEV_CMD, val && val[0] == '1');
}

static void onTone(const char*, const char*)
{
    int hz = storageGetInt("lxst.test_tone", 0);
    if (hz < 0) hz = 0;
    if (hz > 0 && hz >= s_rate / 2) hz = s_rate / 2 - 1;
    if (hz == s_toneHz) return;
    if (!s_toneHz) s_tone = lxsess::Tone();
    s_toneHz = hz;
    if (s_live) txEval();
}

/* ───────────── timers ───────────── */

static void clockCb(void*)
{
    s_ticks.fetch_add(1, std::memory_order_relaxed);
    xTaskNotifyGive(s_task);
}

static void tailCb(void*)
{
    s_tailFired.store(true);
    xTaskNotifyGive(s_task);
}

/* ───────────── stats ───────────── */

static void publishStats()
{
    int speakers = 0;
    for (auto& s : s_spk) if (s.used) speakers++;
    storageBegin();
    storageSet("lxst.stats.enc_us_avg", (int)(s_st.encN ? s_st.encSum / s_st.encN : 0));
    storageSet("lxst.stats.enc_us_max", (int)s_st.encMax);
    storageSet("lxst.stats.dec_us_avg", (int)(s_st.decN ? s_st.decSum / s_st.decN : 0));
    storageSet("lxst.stats.dec_us_max", (int)s_st.decMax);
    storageSet("lxst.stats.frames_tx", (int)s_st.txFrames);
    storageSet("lxst.stats.tx_dropped", (int)s_st.txDropped);
    storageSet("lxst.stats.frames_rx", (int)s_st.rxFrames);
    storageSet("lxst.stats.concealed", (int)s_st.concealed);
    storageSet("lxst.stats.late", (int)s_st.late);
    storageSet("lxst.stats.dropped", (int)s_st.dropped);
    storageSet("lxst.stats.underruns", (int)s_st.underruns);
    storageSet("lxst.stats.blocks_dropped", (int)s_st.blocksDropped);
    storageSet("lxst.stats.ticks_skipped", (int)s_st.ticksSkipped);
    storageSet("lxst.stats.speakers", speakers);
    storageSet("lxst.stats.heap_int", (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    storageSet("lxst.stats.heap_psram", (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    storageEnd();
}

/* ───────────── task ───────────── */

static void serviceSession()
{
    settleClose();
    if (s_openPending) sessionOpen();
    flushHeld();
    if (!s_t.audioOpen) return;

    if (s_ep && !s_ep->isOpen() && s_ep->open(&s_sink, s_rate, s_blockMs))
        info("lxst: audio endpoint up");
#if CONFIG_SPANGAP_LCD
    if (s_live && !s_keysSubscribed && esp_timer_get_time() >= s_nextKeyTryUs) {
        s_keysSubscribed = lcdKeySubscribe(AUX_KEYS);
        if (s_keysSubscribed) info("lxst: held-key subscription up");
        else s_nextKeyTryUs = esp_timer_get_time() + (int64_t)KEY_RETRY_MS * 1000;
    }
#endif
    micUpdate();
    if (s_tailFired.exchange(false) && s_tx == TX_TAIL) {
        spurtEnd(true);
        micUpdate();
        clockUpdate();
    }

    int t = s_ticks.exchange(0);
    if (t > MAX_CATCHUP) { s_st.ticksSkipped += t - MAX_CATCHUP; t = MAX_CATCHUP; }
    for (int i = 0; i < t && s_t.audioOpen; i++) {
        if (s_toneHz > 0 && micWanted()) {
            s_tone.fill(s_toneBuf, s_spb, s_toneHz, s_rate);
            txInput(s_toneBuf, s_spb);
        }
        if (anySpeaker() || s_t.tone.active()) playoutTick(nowMs());
    }
    if (t) {
        if (s_live) s_statsDirty = true;
        clockUpdate();
    }
    if (s_t.audioOpen && s_ep && s_ep->isOpen()) s_ep->service();

    int64_t now = esp_timer_get_time();
    if (s_statsDirty && now >= s_nextStatsUs) {
        publishStats();
        s_nextStatsUs = now + (int64_t)STATS_MS * 1000;
        s_statsDirty = s_clockOn || s_tx != TX_IDLE;
    }
}

static void service()
{
    serviceSession();
    lxstBrowserFlush();             /* owner and mic changes made in the callbacks */
}

static void lxstTask(void*)
{
    s_task = xTaskGetCurrentTaskHandle();
    s_t = Tel();                    /* PSRAM_BSS is zeroed at boot, never initialised */
    s_rx = (uint8_t*)lxstAlloc(SESSION_MAX_MSG);
    s_unitBuf = (int16_t*)lxstAlloc(MAX_UNIT_SAMPLES * sizeof(int16_t));
    s_acc = (int32_t*)lxstAlloc(MAX_UNIT_SAMPLES * sizeof(int32_t));
    s_mix = (int16_t*)lxstAlloc(MAX_UNIT_SAMPLES * sizeof(int16_t));
    s_toneBuf = (int16_t*)lxstAlloc(MAX_UNIT_SAMPLES * sizeof(int16_t));
    if (!s_rx || !s_unitBuf || !s_acc || !s_mix || !s_toneBuf) {
        err("lxst: buffer alloc failed");
        killSelf();
    }

    if (!itsServerInit()) err("lxst: itsServerInit failed");
    itsServerPortOpen(LXST_PORT_SESSION, ITS_PACKET, 1, 2 * SESSION_MAX_MSG, 2 * SESSION_MAX_MSG, 32,
                      SESSION_MAX_MSG);
    itsServerOnConnect(LXST_PORT_SESSION, onSessConnect);
    itsServerOnRecv(LXST_PORT_SESSION, onSessRecv);
    itsServerOnDisconnect(LXST_PORT_SESSION, onSessDisconnect);
    itsServerPortOpen(LXST_PORT_CALL, ITS_PACKET, 1, 2 * SESSION_MAX_MSG, 2 * SESSION_MAX_MSG, 32,
                      SESSION_MAX_MSG);
    itsServerOnConnect(LXST_PORT_CALL, onCallConnect);
    itsServerOnRecv(LXST_PORT_CALL, onCallRecv);
    itsServerOnDisconnect(LXST_PORT_CALL, onCallDisconnect);
    lxstBrowserStart(&s_brEv);
    itsOnAux(AUX_KEYS, onKeyAux);
    itsClientInit(4);

    esp_timer_create_args_t ca = {};
    ca.callback = clockCb;
    ca.name = "lxst_clock";
    esp_timer_create(&ca, &s_clock);
    esp_timer_create_args_t ta = {};
    ta.callback = tailCb;
    ta.name = "lxst_tail";
    esp_timer_create(&ta, &s_tail);

    storageSubscribeChanges("s.lxst.tx", onTxMode);
    storageSubscribeChanges("s.lxst.vox_db", onVoxDb);
    storageSubscribeChanges("lxst.cmd.ptt", onCmdPtt);
    storageSubscribeChanges("lxst.test_tone", onTone);
    storageSubscribeChanges("lxst.cmd.claim", onClaim);
    s_t.setMode = (uint8_t)modeFrom(storageGetStr("s.lxst.tx", "ptt"));
    s_mode = (TxMode)s_t.setMode;
    onVoxDb(nullptr, nullptr);
    onTone(nullptr, nullptr);

    storageBegin();
    storageSet("lxst.state", "idle");
    storageSet("lxst.held", 0);
    storageSet("lxst.cmd.claim", "");
    storageSet("lxst.talking", 0);
    storageSet("lxst.profile", "");
    storageSet("lxst.tx_mode", "");
    storageSet("lxst.endpoint", "device");
    storageSet("lxst.browser", 0);
    storageEnd();

    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_live && s_statsDirty) {
            int64_t ms = (s_nextStatsUs - esp_timer_get_time()) / 1000;
            wait = pdMS_TO_TICKS(ms > 0 ? ms : 0) + 1;
        }
        /* The microphone is wanted but its connection is down (refused, or the
         * engine dropped it): come back to retry, since nothing else may wake us. */
        if (s_live && s_ep && s_toneHz <= 0 && micWanted()
            && !(s_ep->isOpen() && s_ep->capturing())
            && wait > pdMS_TO_TICKS(RETRY_WAKE_MS))
            wait = pdMS_TO_TICKS(RETRY_WAKE_MS);
        if (s_t.heldOut >= 0 && wait > pdMS_TO_TICKS(RETRY_WAKE_MS))
            wait = pdMS_TO_TICKS(RETRY_WAKE_MS);      /* an EV_HELD the port refused */
#if CONFIG_SPANGAP_LCD
        if (s_live && !s_keysSubscribed) {
            int64_t ms = (s_nextKeyTryUs - esp_timer_get_time()) / 1000;
            TickType_t w = pdMS_TO_TICKS(ms > 0 ? ms : 0) + 1;
            if (w < wait) wait = w;
        }
#endif
        itsPoll(wait);
        while (itsPoll(0)) {}
        service();
    }
}

int lxstMicLevel() { return s_micLevel.load(std::memory_order_relaxed); }

/* ───────────── CLI ───────────── */

static void cliStat(const char* key)
{
    cliPrintf("  %-15s %d\n", key + strlen("lxst.stats."), storageGetInt(key, 0));
}

static void cliLxst(const char* args)
{
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s voice session status, test tone, talk key\n", CLI_HELP_COL, "lxst");
        if (args && args[0] == '-') {
            cliPrintf("  lxst                 status and stats\n");
            cliPrintf("  lxst tone <hz>|off   transmit a test tone instead of the mic\n");
            cliPrintf("  lxst ptt 1|0         hold / release the device talk key\n");
        }
        return;
    }
    while (args && *args == ' ') args++;
    if (args && !strncmp(args, "tone", 4)) {
        const char* v = args + 4;
        while (*v == ' ') v++;
        int hz = (!*v || !strcmp(v, "off")) ? 0 : atoi(v);
        storageSet("lxst.test_tone", hz);
        cliPrintf("test tone %s\n", hz ? v : "off");
        return;
    }
    if (args && !strncmp(args, "ptt", 3)) {
        const char* v = args + 3;
        while (*v == ' ') v++;
        storageSet("lxst.cmd.ptt", (*v == '1') ? "1" : "0");
        return;
    }
    cliPrintf("session   %s%s\n", storageGetStr("lxst.state", "idle").c_str(),
              storageGetInt("lxst.held", 0) ? " (room held)" : "");
    cliPrintf("endpoint  %s%s\n", storageGetStr("lxst.endpoint", "device").c_str(),
              storageGetInt("lxst.browser", 0) ? " (browser channel connected)" : "");
    cliPrintf("profile   %s\n", storageGetStr("lxst.profile", "").c_str());
    std::string tx = storageGetStr("lxst.tx_mode", "");
    if (tx.empty()) tx = storageGetStr("s.lxst.tx", "ptt");
    std::string vox = tx == "vox" ? " (vox " + std::to_string(storageGetInt("s.lxst.vox_db", -45)) + " dBFS)" : "";
    cliPrintf("tx mode   %s%s, talking %d, mic level %d%%, tone %d Hz\n", tx.c_str(), vox.c_str(),
              storageGetInt("lxst.talking", 0), lxstMicLevel(), storageGetInt("lxst.test_tone", 0));
    cliPrintf("jitter    %d ms when the profile names none\n",
              storageGetInt("s.lxst.jitter_ms", 200));
    cliPrintf("stats\n");
    static const char* keys[] = {
        "lxst.stats.speakers", "lxst.stats.frames_tx", "lxst.stats.tx_dropped",
        "lxst.stats.frames_rx", "lxst.stats.concealed", "lxst.stats.late",
        "lxst.stats.dropped", "lxst.stats.underruns", "lxst.stats.blocks_dropped",
        "lxst.stats.ticks_skipped", "lxst.stats.enc_us_avg", "lxst.stats.enc_us_max",
        "lxst.stats.dec_us_avg", "lxst.stats.dec_us_max", "lxst.stats.heap_int",
        "lxst.stats.heap_psram",
    };
    for (const char* k : keys) cliStat(k);
    cliPrintf("last tx   %s\n", storageGetStr("lxst.stats.last_tx", "").c_str());
    cliPrintf("last rx   %s\n", storageGetStr("lxst.stats.last_rx", "").c_str());
}

/* ───────────── service ───────────── */

void LxstService::onInit()
{
    storageBegin();
    storageDefault("s.lxst.tx", "ptt");
    storageDefault("s.lxst.vox_db", -45);
    storageDefault("s.lxst.jitter_ms", 200);
    storageEnd();
    cliRegisterCmd("lxst", cliLxst);
    s_task = spawnTask(lxstTask, TAG, LXST_TASK_STACK, nullptr, LXST_TASK_PRIO, CORE_SECONDARY,
                       STACK_PSRAM);
    if (!s_task) err("lxst: task spawn failed");
}
