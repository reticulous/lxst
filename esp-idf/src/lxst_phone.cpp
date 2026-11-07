/**
 * lxst_phone — the LXST telephony task (ITS name "lxphone"). Storage map,
 * commands and the wire sketch: lxst_phone.h. The call itself is the state
 * machine in lxst_phone_call.h; this file is its I/O: the hosted
 * lxst.telephony destination, the Links, the session's call port, storage
 * and the CLI.
 *
 * Everything runs on the task: ITS callbacks and storage subscriptions
 * dispatch through its one itsPoll, whose timeout is the next deadline.
 */
#include "lxst_phone.h"
#include "lxst.h"
#include "lxst_phone_call.h"
#include "lxst_phone_wire.h"

#include "spangap.h"
#include "ports.h"
#include "rnsd.h"
#include "mem.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

using namespace lxphone;

static const char* TAG = LXST_PHONE_TASK;

#define PH_ASPECT        "lxst.telephony"
#define PH_TAG_PFX       "ph"
#define PH_TAGS          4
#define PH_LXMF_SLOTS    10
#define PH_NODE_IDENT    "secrets.rnsd.identity"
#define PH_LOG_KEEP      30
#define PH_PEERS_MAX     32
#define PH_PENDING       4
#define PH_DYING         6
#define PH_BUF           2048
#define PH_ANNOUNCE_MS   (3LL * 3600 * 1000)
#define PH_IDENT_MS      60000
#define PH_IDENT_WAIT_MS 60000
#define PH_IDENT_RECHECK_MS 2000
#define PH_RETRY_MS      5000
#define PH_ENDED_MS      3000
#define PH_ENDED_PTT_MS  10000   /* ended with lxst.phone.ptt_redial: time to take the offer */
#define PH_RESOLVE_MS    500
#define PH_PATH_ASK_MS   15000
#define PH_SPEAKER       1u
#define ELLIPSIS         "\xE2\x80\xA6"

/* ─────────────── small helpers ─────────────── */

static int64_t nowMs() { return esp_timer_get_time() / 1000; }
static uint32_t wallS()
{
    time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0;
}

static std::string hexOf(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}

static bool hexTo(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

/** The next packet on `h` into buf: its length, 0 when none is queued. */
static size_t recvNext(int h, void* buf, size_t cap)
{
    for (;;) {
        size_t before = itsBytesAvailable(h);
        if (!before) return 0;
        size_t n = itsRecv(h, buf, cap, 0);
        if (n) return n;
        if (itsBytesAvailable(h) >= before) return 0;
    }
}

/* ─────────────── identities, contacts, names ─────────────── */

static bool lxmfSlotLive(int n)
{
    char k[48];
    std::snprintf(k, sizeof k, "s.lxmf.id.%d.kind", n);
    if (storageGetStr(k, "") == "group") return false;
    std::snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
    uint8_t b[16];
    return hexTo(storageGetStr(k, ""), b, 16);
}

static std::string slotPath(int n) { return "secrets.lxmf.id." + std::to_string(n) + ".privkey"; }

/** The slot's key is stored but lxmf has not published it yet: right after
 *  boot, before lxmf has loaded its identities. */
static bool lxmfSlotLoading(int n)
{
    char k[48];
    std::snprintf(k, sizeof k, "s.lxmf.id.%d.kind", n);
    if (storageGetStr(k, "") == "group" || lxmfSlotLive(n)) return false;
    return storageExists(slotPath(n).c_str());
}

/** The identity the phone is, as a storage path; "" when the chosen one is
 *  not on this device. `wait` is set, and the result is not a choice, while
 *  `loading` and a stored LXMF identity lxmf has not loaded yet could decide it. */
static std::string phoneIdentity(bool loading, bool& wait)
{
    wait = false;
    std::string v = trim(storageGetStr("s.lxst.phone.identity", ""));
    if (v == "node") return PH_NODE_IDENT;
    if (!v.empty() && v != "auto") {
        char* e = nullptr;
        long n = std::strtol(v.c_str(), &e, 10);
        if (!e || *e || n < 0 || n >= PH_LXMF_SLOTS) return "";
        if (lxmfSlotLive((int)n)) return slotPath((int)n);
        wait = loading && lxmfSlotLoading((int)n);
        return "";
    }
    for (int n = 0; n < PH_LXMF_SLOTS; ++n) {
        if (lxmfSlotLive(n)) return slotPath(n);
        if (loading && lxmfSlotLoading(n)) { wait = true; return ""; }
    }
    return PH_NODE_IDENT;
}

static std::string contactKey(int n, const std::string& peer, const char* f)
{
    return "s.lxmf.id." + std::to_string(n) + ".contacts." + peer + "." + f;
}

/** A contact of any LXMF identity here: a record with messages, or kept;
 *  never a group. */
static bool isContact(const std::string& lxDest)
{
    for (int n = 0; n < PH_LXMF_SLOTS; ++n) {
        if (!storageExists(contactKey(n, lxDest, "hash").c_str())) continue;
        if (storageGetInt(contactKey(n, lxDest, "group").c_str(), 0)) continue;
        if (storageGetInt(contactKey(n, lxDest, "count").c_str(), 0) > 0 ||
            storageGetInt(contactKey(n, lxDest, "kept").c_str(), 0))
            return true;
    }
    return false;
}

static bool contactPubkey(const std::string& lxDest, uint8_t pk[RNSD_PUBKEY_LEN])
{
    for (int n = 0; n < PH_LXMF_SLOTS; ++n) {
        std::string h = storageGetStr(contactKey(n, lxDest, "pubkey").c_str(), "");
        if (hexTo(h, pk, RNSD_PUBKEY_LEN)) return true;
    }
    return false;
}

static std::string lxmfDestOf(const uint8_t ih[16])
{
    uint8_t d[16];
    return rnsdDestinationHashFromIdentityHash(ih, "lxmf", "delivery", d) ? hexOf(d, 16) : "";
}

/** A display name for an identity: its LXMF contact's nick or name, else the
 *  name in its last LXMF announce. */
static std::string nameFor(const uint8_t ih[16])
{
    std::string lx = lxmfDestOf(ih);
    if (lx.empty()) return "";
    for (int n = 0; n < PH_LXMF_SLOTS; ++n) {
        std::string v = storageGetStr(contactKey(n, lx, "nick").c_str(), "");
        if (v.empty()) v = storageGetStr(contactKey(n, lx, "display_name").c_str(), "");
        if (!v.empty()) return v;
    }
    uint8_t d[16], ad[256];
    size_t n = sizeof ad;
    if (!hexTo(lx, d, 16) || !rnsdRecallAppData(d, ad, &n)) return "";
    char nm[64];
    rnsdAnnounceName(ad, n, nm, sizeof nm);
    return nm;
}

static bool callerAllowed(const std::string& identHex)
{
    if (listHas(storageGetStr("s.lxst.phone.blocked", ""), identHex)) return false;
    std::string a = storageGetStr("s.lxst.phone.allow", "all");
    if (a == "none") return false;
    if (a != "contacts") return true;
    uint8_t ih[16];
    return hexTo(identHex, ih, 16) && isContact(lxmfDestOf(ih));
}

/* ─────────────── state ─────────────── */

namespace {

struct Ctx {
    int         link = -1;          /* the call's Link handle */
    int         ref = -1;           /* outbound: our ITS ref on it */
    bool        inbound = false;
    std::string tag;
    std::string typed;              /* outbound: what was dialled */
    std::string peerIdent, peerDest, peerName;
    uint32_t    at = 0;
    bool        resolving = false;
    int64_t     resolveAt = 0, askAt = 0;
    const char* why = "";           /* a failure's reason, for the status line */
    bool        pttRedial = false;  /* outbound: ended for a route that carries only
                                       half duplex; lxst.phone.ptt_redial */
    int64_t     heardAt = 0;        /* when the call's link last heard its peer: the
                                       call's start, then each move of
                                       rnsd.links.<tag>.heard_s */
};

struct Pending { int h = -1; char tag[24] = {}; int64_t idBy = 0; bool fresh = false; std::string rid; };
struct Dying { int h = -1; int ref = -1; bool inbound = false; int64_t at = 0; };
struct Peer { std::string ident; std::string name; int64_t seen = 0; };

}  // namespace

static Call*    s_call = nullptr;
static Ctx*     s_ctx = nullptr;
static Pending* s_pend = nullptr;
static Dying*   s_dying = nullptr;
static Packet*  s_pkt = nullptr;
static std::map<std::string, Peer>* s_peers = nullptr;

static uint8_t* s_linkBuf = nullptr;   /* received Link packets */
static uint8_t* s_txBuf = nullptr;     /* encoded Link packets */
static uint8_t* s_lxBuf = nullptr;     /* session events */
static uint8_t* s_fwd = nullptr;       /* OP_FRAMES to the session */

static int  s_dest = -1;
static std::string s_identKey;
static bool s_enabled = true;
static int  s_lx = -1;
static bool s_muted = false;
static int  s_tagNext = 0, s_refNext = 0;
static int  s_annTel = -1, s_annLx = -1;
static int  s_logNext = 0;

static int64_t s_announceAt = 0, s_identAt = 0, s_retryAt = 0, s_endedUntil = 0, s_timerAt = 0;
static int64_t s_startAt = 0;
static bool    s_identWait = false;   /* the identity waits for lxmf to load its identities */
static bool    s_announceWanted = false, s_reconcile = true;
static std::string s_endStatus;

static void runActs();
static void publish();

/* ─────────────── links ─────────────── */

static bool linkSend(int h, const uint8_t* b, size_t n)
{
    return h >= 0 && n && itsSend(h, b, n, 0) > 0;
}

static void sendSignal(int h, uint16_t s)
{
    size_t n = encodeSignals(&s, 1, s_txBuf, PH_BUF);
    if (!linkSend(h, s_txBuf, n)) warn("lxphone: signal %u not sent", (unsigned)s);
}

/** Tear a link down after `delayMs`: a signal sent just before has to leave
 *  the ITS ring for rnsd before the handle closes, or it is dropped with it. */
static void dyingAdd(int h, int ref, bool inbound, int64_t at)
{
    if (h < 0) return;
    if (at <= nowMs()) { itsDisconnect(h); return; }
    for (int i = 0; i < PH_DYING; ++i)
        if (s_dying[i].h < 0) { s_dying[i] = Dying{ h, ref, inbound, at }; return; }
    itsDisconnect(h);
}

static void dyingRun(int64_t now)
{
    for (int i = 0; i < PH_DYING; ++i)
        if (s_dying[i].h >= 0 && now >= s_dying[i].at) {
            itsDisconnect(s_dying[i].h);
            s_dying[i] = Dying{};
        }
}

/* ─────────────── the session's call port ─────────────── */

/* The session's receive buffer was last sized for a batched (LoRa) first hop
 * (receiveBuffer); hopCheck resends the profile when the hop flips. */
static bool s_batched = false;

static bool batchedHop()
{
    return !s_ctx->tag.empty() &&
        storageGetStr(("rnsd.links." + s_ctx->tag + ".iface").c_str(), "").rfind("lora/", 0) == 0;
}

static void lxProfile(uint8_t prof, uint8_t mode, lxst_profile_t& lp)
{
    const ProfileInfo* p = profileInfo(prof);
    if (!p) p = profileInfo(DEFAULT_PROFILE);
    lp = {};
    lp.codec = p->codec;
    lp.mode = p->mode;
    lp.frame_ms = p->frame_ms;
    lp.max_frame_bytes = p->frame_bytes;
    lp.batch_budget = 0;
    lp.batch_base = 0;
    lp.batch_per_frame = 0;
    lp.batch_max_frames = (uint8_t)framesPerPacket(*p);
    uint16_t j, m;
    s_batched = batchedHop();
    receiveBuffer(*p, j, m, s_batched);
    lp.jitter_ms = j;
    lp.max_jitter_ms = m;
    lp.sequenced = 0;
    lp.can_speak = 1;
    lp.tx_mode = mode == MODE_HALF_DUPLEX ? LXST_TX_PTT : LXST_TX_OPEN;
    lp.rx_any = 1;
}

static void lxState()
{
    if (s_lx < 0) return;
    uint8_t m[4] = { LXST_OP_STATE, 1, (uint8_t)(s_muted ? 1 : 0), 0 };
    itsSend(s_lx, m, sizeof m, 0);
}

static void lxOp(uint8_t op, int arg = -1)
{
    if (s_lx < 0) return;
    uint8_t m[2] = { op, (uint8_t)arg };
    itsSend(s_lx, m, arg < 0 ? 1 : 2, 0);
}

/* The call link's first-hop bitrate as hopCheck last read it (0 = unknown),
 * and when hopCheck runs next. */
static uint32_t s_hopBps = 0;
static int64_t  s_hopAt = 0;
#define PH_HOP_CHECK_MS 1000

static void onLxRecv(int handle, size_t)
{
    for (;;) {
        size_t n = recvNext(handle, s_lxBuf, PH_BUF);
        if (n == 0) return;
        if (handle != s_lx || s_lxBuf[0] != LXST_EV_BATCH || n < 2) continue;
        if (s_call->ph != PH_LIVE || s_ctx->link < 0) continue;
        /* Never at a rate the first hop cannot carry (hopCheck is moving the
         * call off it). */
        const ProfileInfo* run = profileInfo(s_call->profile);
        if (run && !fitsHop(*run, s_hopBps, s_call->mode)) continue;
        /* One batch, framesPerPacket() frames at most, is one Link packet. */
        Bytes f[MAX_FRAMES];
        int nf = 0;
        size_t count = s_lxBuf[1], at = 2;
        for (size_t k = 0; k < count && at + 2 <= n; ++k) {
            size_t len = s_lxBuf[at] | (size_t)s_lxBuf[at + 1] << 8;
            at += 2;
            if (at + len > n) break;
            f[nf++] = Bytes{ s_lxBuf + at, len };
            at += len;
            if (nf == MAX_FRAMES) {
                linkSend(s_ctx->link, s_txBuf, encodeFrames(f, nf, s_txBuf, PH_BUF));
                nf = 0;
            }
        }
        if (nf) linkSend(s_ctx->link, s_txBuf, encodeFrames(f, nf, s_txBuf, PH_BUF));
    }
}

static void onLxDisc(int)
{
    s_lx = -1;
    if (s_call->ph == PH_NONE) return;
    warn("lxphone: the voice session closed under the call");
    s_ctx->why = "The voice session closed";
    s_call->sessionOpen = false;
    s_call->hangup(nowMs());
    runActs();
}

static bool lxOpen(uint32_t arg)
{
    lxst_profile_t lp;
    lxProfile((uint8_t)arg, (uint8_t)(arg >> 8), lp);
    int h = itsConnect(LXST_TASK, LXST_PORT_CALL, &lp, sizeof lp, pdMS_TO_TICKS(500), 0, onLxRecv, onLxDisc);
    if (h < 0) return false;
    s_lx = h;
    lxState();
    return true;
}

static void lxSendProfile(uint8_t prof, uint8_t mode)
{
    if (s_lx < 0) return;
    uint8_t m[1 + sizeof(lxst_profile_t)];
    lxst_profile_t lp;
    lxProfile(prof, mode, lp);
    m[0] = LXST_OP_PROFILE;
    std::memcpy(m + 1, &lp, sizeof lp);
    itsSend(s_lx, m, sizeof m, 0);
}

/* Received frames gather in one OP_FRAMES across a receive callback's drain,
 * and go to the session when it ends, when a signal needs acting on, or when
 * the message is full: one session message per wake, not one per packet. */
static constexpr size_t FWD_HDR = 8;
static size_t s_fwdLen = 0;
static int    s_fwdCount = 0;

static void lxFlush()
{
    if (s_fwdCount && s_lx >= 0) {
        s_fwd[7] = (uint8_t)s_fwdCount;
        itsSend(s_lx, s_fwd, s_fwdLen, 0);
    }
    s_fwdLen = 0;
    s_fwdCount = 0;
}

static void lxFrames(const Packet& p)
{
    if (s_lx < 0) return;
    for (int i = 0; i < p.nFrames; ++i) {
        const Bytes& f = p.frames[i];
        if (FWD_HDR + 2 + f.n > PH_BUF) continue;
        if (s_fwdCount == 255 || s_fwdLen + 2 + f.n > PH_BUF) lxFlush();
        if (!s_fwdCount) {
            s_fwd[0] = LXST_OP_FRAMES;
            for (int b = 0; b < 4; ++b) s_fwd[1 + b] = (uint8_t)(PH_SPEAKER >> (8 * b));
            s_fwd[5] = s_fwd[6] = 0;
            s_fwdLen = FWD_HDR;
        }
        s_fwd[s_fwdLen++] = (uint8_t)f.n;
        s_fwd[s_fwdLen++] = (uint8_t)(f.n >> 8);
        std::memcpy(s_fwd + s_fwdLen, f.p, f.n);
        s_fwdLen += f.n;
        s_fwdCount++;
    }
}

/* ─────────────── publishing ─────────────── */

static std::string peerLabel()
{
    if (!s_ctx->peerName.empty()) return s_ctx->peerName;
    const std::string& h = !s_ctx->peerIdent.empty() ? s_ctx->peerIdent : s_ctx->typed;
    return h.substr(0, 8);
}

static const char* endText(Result r)
{
    switch (r) {
    case R_ANSWERED:  return "Call ended";
    case R_MISSED:    return "Missed call";
    case R_DECLINED:  return "Declined";
    case R_REJECTED:  return "Rejected";
    case R_BUSY:      return "Busy";
    case R_NO_ANSWER: return "No answer";
    case R_CANCELLED: return "Cancelled";
    default:          return "Call failed";
    }
}

static int64_t silentMs(int64_t now)
{
    return s_ctx->heardAt > 0 ? now - s_ctx->heardAt : 0;
}

static std::string s_liveStatus;   /* lxst.phone.status as last published while live */

/* A live call's status line: "Connected", or after SILENT_WARN_MS of link
 * silence the warning with the time since the link last heard anything. */
static std::string liveStatus(int64_t now)
{
    int64_t s = silentMs(now);
    if (s < SILENT_WARN_MS) return "Connected";
    return "Connection may be lost (nothing heard for " + durationText(s / 1000) + ")";
}

static void publish()
{
    int64_t now = nowMs();
    const char* state;
    std::string status;
    bool pttRedial = false;
    switch (s_call->ph) {
    case PH_DIAL:
    case PH_AWAIT:      state = "dialling";   status = "Calling " + peerLabel() + ELLIPSIS; break;
    case PH_RINGBACK:   state = "ringback";   status = "Ringing" ELLIPSIS; break;
    case PH_RINGING:    state = "ringing";    status = "Incoming call"; break;
    case PH_CONNECTING: state = "connecting"; status = "Connecting" ELLIPSIS; break;
    case PH_LIVE:       state = "live";       status = s_liveStatus = liveStatus(now); break;
    case PH_BUSYTONE:   state = "ended";      status = endText(s_call->result); break;
    default:
        if (now < s_endedUntil) { state = "ended"; status = s_endStatus; pttRedial = s_ctx->pttRedial; }
        else if (!s_enabled || s_identKey.empty()) {
            state = "off";
            status = !s_identKey.empty() ? "Not taking calls"
                   : s_identWait ? "Waiting for LXMF identities" ELLIPSIS
                   : "That identity is not on this device";
        } else { state = "idle"; status = "Ready"; }
        break;
    }
    bool call = s_call->ph != PH_NONE && s_call->ph != PH_BUSYTONE;
    const ProfileInfo* pi = profileInfo(s_call->profile);
    storageBegin();
    storageSet("lxst.phone.state", state);
    storageSet("lxst.phone.status", status.c_str());
    storageSet("lxst.phone.muted", s_muted ? 1 : 0);
    storageSet("lxst.phone.ptt_redial", pttRedial ? 1 : 0);
    storageSet("lxst.phone.profile", call && pi ? pi->text : "");
    storageSet("lxst.phone.mode", !call ? "" : s_call->mode == MODE_HALF_DUPLEX ? "half duplex" : "full duplex");
    if (s_call->ph != PH_LIVE) storageSet("lxst.phone.timer", "");
    if (s_call->ph != PH_NONE) {
        storageSet("lxst.phone.dir", s_call->dir == DIR_IN ? "in" : "out");
        storageSet("lxst.phone.peer", s_ctx->peerIdent.c_str());
        storageSet("lxst.phone.peer_dest", s_ctx->peerDest.c_str());
        storageSet("lxst.phone.peer_name", s_ctx->peerName.c_str());
    }
    storageEnd();
}

static void publishTimer(int64_t now)
{
    if (s_call->ph != PH_LIVE || s_call->liveAt < 0) return;
    if (now < s_timerAt) return;
    int64_t secs = (now - s_call->liveAt) / 1000;
    std::string st = liveStatus(now);
    storageBegin();
    storageSet("lxst.phone.timer", durationText(secs).c_str());
    if (st != s_liveStatus) {
        s_liveStatus = st;
        storageSet("lxst.phone.status", st.c_str());
    }
    storageEnd();
    s_timerAt = s_call->liveAt + (secs + 1) * 1000;
}

static void publishIds()
{
    uint8_t ih[16], dh[16];
    bool ok = !s_identKey.empty() && rnsdIdentityHash(s_identKey.c_str(), ih) &&
              rnsdDestinationHash(s_identKey.c_str(), "lxst", "telephony", dh);
    storageBegin();
    storageSet("lxst.phone.ident", ok ? hexOf(ih, 16).c_str() : "");
    storageSet("lxst.phone.dest", ok ? hexOf(dh, 16).c_str() : "");
    storageEnd();
}

/* ─────────────── the call log ─────────────── */

static std::vector<int>* s_logScan = nullptr;
static void logLeaf(const char* key, const char*)
{
    static const char P[] = "s.lxst.phone.log.";
    if (!s_logScan || !key || std::strncmp(key, P, sizeof P - 1)) return;
    int n = std::atoi(key + sizeof P - 1);
    if (n >= 0 && std::find(s_logScan->begin(), s_logScan->end(), n) == s_logScan->end()) s_logScan->push_back(n);
}

static void logBoot()
{
    std::vector<int> ns;
    s_logScan = &ns;
    storageForEach("s.lxst.phone.log.", logLeaf);
    s_logScan = nullptr;
    std::sort(ns.begin(), ns.end());
    s_logNext = ns.empty() ? 0 : ns.back() + 1;
    for (int n : ns)
        if (n < s_logNext - PH_LOG_KEEP) storageDeleteTree(("s.lxst.phone.log." + std::to_string(n)).c_str());
}

/** One log record. `peer` is the identity hash, or what was dialled when the
 *  call ended before the identity was known; `lxmf` is that identity's LXMF
 *  address, "" when not known. */
static void logEntry(Dir dir, const std::string& peer, const std::string& lxmf, const std::string& name,
                     uint32_t at, const std::string& dur, Result r)
{
    int n = s_logNext++;
    std::string b = "s.lxst.phone.log." + std::to_string(n) + ".";
    storageBegin();
    storageSet((b + "dir").c_str(), dir == DIR_IN ? "in" : "out");
    storageSet((b + "peer").c_str(), peer.c_str());
    storageSet((b + "lxmf").c_str(), lxmf.c_str());
    storageSet((b + "name").c_str(), name.c_str());
    storageSet((b + "at").c_str(), (int)at);
    storageSet((b + "dur").c_str(), dur.c_str());
    storageSet((b + "result").c_str(), resultName(r));
    storageEnd();
    if (n >= PH_LOG_KEEP) storageDeleteTree(("s.lxst.phone.log." + std::to_string(n - PH_LOG_KEEP)).c_str());
}

static void logCall(Result r, int64_t now)
{
    std::string peer = !s_ctx->peerIdent.empty() ? s_ctx->peerIdent : s_ctx->typed;
    std::string dur = s_call->liveAt >= 0 ? durationText((now - s_call->liveAt) / 1000) : "";
    /* The name as known now: an announce heard during the call counts. */
    uint8_t ih[16];
    std::string lxmf, name = s_ctx->peerName;
    if (hexTo(s_ctx->peerIdent, ih, 16)) {
        lxmf = lxmfDestOf(ih);
        std::string fresh = nameFor(ih);
        if (!fresh.empty()) name = fresh;
    }
    logEntry(s_call->dir, peer, lxmf, name, s_ctx->at, dur, r);
}

/* ─────────────── carrying out the call's actions ─────────────── */

/** Carry out one action; false when it ended the batch it came in. */
static bool doAct(const Act& a, int64_t now)
{
    switch (a.k) {
    case A_SIGNAL: {
        size_t n = encodeSignals(a.s, a.n, s_txBuf, PH_BUF);
        if (!linkSend(s_ctx->link, s_txBuf, n)) warn("lxphone: signals not sent");
        break;
    }
    case A_IDENTIFY:
        if (!rnsdLinkIdentify(s_ctx->tag.c_str(), "")) warn("lxphone: identify not queued");
        break;
    case A_OPEN:
        if (!lxOpen(a.arg)) {
            warn("lxphone: the voice session's call port refused");
            s_ctx->why = "The voice session is in use";
            s_call->openFailed(now);
            return false;
        }
        break;
    case A_PROFILE:
        lxSendProfile((uint8_t)a.arg, (uint8_t)(a.arg >> 8));
        break;
    case A_TONE:
        lxOp(LXST_OP_TONE, (int)a.arg);
        break;
    case A_LIVE:
        lxOp(LXST_OP_LIVE);
        lxState();
        s_timerAt = 0;
        break;
    case A_CLOSE:
        if (s_lx >= 0) itsDisconnect(s_lx);
        s_lx = -1;
        break;
    case A_TEARDOWN:
        dyingAdd(s_ctx->link, s_ctx->ref, s_ctx->inbound, now + a.arg);
        s_ctx->link = -1;
        break;
    case A_END: {
        Result r = (Result)a.arg;
        if (r != R_NONE) logCall(r, now);
        if (s_ctx->link >= 0) itsDisconnect(s_ctx->link);
        s_ctx->link = -1;
        s_endStatus = *s_ctx->why ? s_ctx->why : endText(r);
        s_endedUntil = now + (s_ctx->pttRedial ? PH_ENDED_PTT_MS : PH_ENDED_MS);
        s_muted = false;
        info("lxphone: call %s %s: %s", s_call->dir == DIR_IN ? "from" : "to",
             s_ctx->peerIdent.empty() ? s_ctx->typed.c_str() : s_ctx->peerIdent.c_str(), resultName(r));
        s_ctx->resolving = false;
        break;
    }
    }
    return true;
}

static void runActs()
{
    int64_t now = nowMs();
    Act a[Call::MAX_ACTS];
    for (;;) {
        int n = s_call->take(a, Call::MAX_ACTS);
        if (!n) break;
        for (int i = 0; i < n; ++i)
            if (!doAct(a[i], now)) break;   /* a refused open: the rest presumed it */
    }
    publish();
}

/* ─────────────── link traffic ─────────────── */

/* The call link's first hop: rnsd.links.<tag>.bitrate, and the hourly airtime
 * budget of a LoRa first hop (lora.<n>.airtime_{hour,limit}_ms; limit 0 = no
 * budget). Read by hopCheck. A nonzero lxst.phone.test_hop_bps (RAM, a test
 * hook) stands in for the bitrate. */
static uint32_t s_testHopLogged = 0;

static void hopRead(uint32_t& bps, uint32_t& usedMs, uint32_t& limitMs)
{
    std::string lk = "rnsd.links." + s_ctx->tag + ".";
    int b = storageGetInt((lk + "bitrate").c_str(), 0);
    bps = b > 0 ? (uint32_t)b : 0;
    int t = storageGetInt("lxst.phone.test_hop_bps", 0);
    uint32_t test = t > 0 ? (uint32_t)t : 0;
    if (test != s_testHopLogged) {
        if (test) info("lxphone: lxst.phone.test_hop_bps: first hop taken as %u bit/s (link reports %u)",
                       (unsigned)test, (unsigned)bps);
        else info("lxphone: lxst.phone.test_hop_bps off, first hop %u bit/s as the link reports", (unsigned)bps);
        s_testHopLogged = test;
    }
    if (test) bps = test;
    usedMs = limitMs = 0;
    std::string iface = storageGetStr((lk + "iface").c_str(), "");
    if (iface.rfind("lora/", 0) == 0) {
        int n = atoi(iface.c_str() + 5);
        char k[48];
        snprintf(k, sizeof k, "lora.%d.airtime_hour_ms", n);
        usedMs = (uint32_t)std::max(0, storageGetInt(k, 0));
        snprintf(k, sizeof k, "lora.%d.airtime_limit_ms", n);
        limitMs = (uint32_t)std::max(0, storageGetInt(k, 0));
    }
}

/* The caller's profile, settled before its RINGING reply: the user's fixed
 * profile or `auto`'s pick, fitted to the first hop in the call's mode
 * (fitCall). */
static CallFit profileChoice()
{
    uint32_t bps, used, lim;
    hopRead(bps, used, lim);
    std::string p = storageGetStr("s.lxst.phone.profile", "auto");
    if (const ProfileInfo* pi = profileByKey(p)) return fitCall(pi->id, bps, s_call->mode);
    int rtt = storageGetInt(("rnsd.links." + s_ctx->tag + ".rtt_ms").c_str(), 0);
    return autoProfile(bps, rtt, s_call->mode);
}

/* Keep the call within what its first hop carries (Call::hop): a call keeps
 * its mode and moves to the best profile that fits the hop in it (a
 * full-duplex call needs both directions to fit); setProfile tells the peer.
 * A full-duplex call no profile fits is refused or ended "Route too slow for
 * a call": a caller as soon as its link is up, before it identifies; a
 * callee once the caller's preferences (which follow RINGING) are heard, so
 * a caller preferring half duplex is let in. A push-to-talk call, or one the
 * peer made half duplex, ends so only when nothing fits even one way.
 * Over a hop with an hourly airtime budget a call is not started or answered
 * with less than MIN_TALK_S of talk left in it, and a live call ends
 * "Airtime limit reached" when it runs out. Runs on a change of the link's
 * first hop, after the peer's signals, and each PH_HOP_CHECK_MS. */
static void hopCheck(int64_t now)
{
    s_hopAt = now + PH_HOP_CHECK_MS;
    if (s_call->ph == PH_NONE || s_call->ph == PH_BUSYTONE || s_ctx->tag.empty()) {
        s_hopBps = 0;
        return;
    }
    uint32_t bps, used, lim;
    hopRead(bps, used, lim);
    s_hopBps = bps;
    const ProfileInfo* pi = profileInfo(s_call->profile);
    if (!pi || !bps) return;   /* an unknown hop changes nothing */
    /* Judge the profile a step-down replaced, so a hop that carries it again
     * gets it back. */
    uint8_t want = s_call->hopWant ? s_call->hopWant : pi->id;
    CallFit f;
    Call::HopStep step = s_call->hop(want, bps, f);
    if (step == Call::HOP_WAIT) return;
    if (step != Call::HOP_FITS) {
        if (step == Call::HOP_FULL_SLOW)
            warn("lxphone: first hop %u bit/s carries no profile full duplex (%s needs %u bit/s each way), "
                 "ending the call", (unsigned)bps, pi->text, (unsigned)txBps(*pi));
        else
            warn("lxphone: first hop %u bit/s carries no profile (%s needs %u bit/s one way), ending the call",
                 (unsigned)bps, pi->text, (unsigned)txBps(*pi));
        s_ctx->why = "Route too slow for a call";
        s_ctx->pttRedial = step == Call::HOP_FULL_SLOW && s_call->dir == DIR_OUT;
        s_call->fail(now);
        return;
    }
    if (f.profile != s_call->profile || f.mode != s_call->mode) {
        const ProfileInfo* to = profileInfo(f.profile);
        info("lxphone: first hop %u bit/s: %s %s (%u bit/s each way) -> %s %s", (unsigned)bps,
             pi->text, s_call->mode == MODE_HALF_DUPLEX ? "half duplex" : "full duplex", (unsigned)txBps(*pi),
             to->text, f.mode == MODE_HALF_DUPLEX ? "half duplex" : "full duplex");
        if (!s_call->hopWant && f.profile != s_call->profile) s_call->hopWant = s_call->profile;
        s_call->setProfile(f.profile, f.mode);
        if (s_call->hopWant == f.profile) s_call->hopWant = 0;
        pi = to;
    } else if (s_lx >= 0 && batchedHop() != s_batched) {
        lxSendProfile(s_call->profile, s_call->mode);
    }
    int64_t left = talkSecondsLeft(*pi, bps, used, lim);
    if (left < 0) return;
    bool starting = s_call->ph != PH_LIVE;
    if ((starting && left < MIN_TALK_S) || left == 0) {
        warn("lxphone: hop airtime %u of %u ms this hour, %lld s of talk left at %s, ending the call",
             (unsigned)used, (unsigned)lim, (long long)left, pi->text);
        s_ctx->why = "Airtime limit reached";
        s_call->fail(now);
    }
}

static bool hopKey(const char* key)
{
    if (s_ctx->tag.empty() || !key) return false;
    std::string lk = "rnsd.links." + s_ctx->tag + ".";
    return key == lk + "bitrate" || key == lk + "iface";
}

static void callPacket(const uint8_t* b, size_t n)
{
    if (!decode(b, n, *s_pkt)) return;
    const Packet& p = *s_pkt;
    int64_t now = nowMs();
    if (p.hasFrames && (s_call->ph == PH_LIVE || s_call->ph == PH_CONNECTING)) lxFrames(p);
    if (p.hasSignals && p.nSignals) {
        lxFlush();
        if (s_call->ph == PH_AWAIT) {
            CallFit f = profileChoice();
            if (f.profile) s_call->setProfile(f.profile, f.mode);   /* none fits: hopCheck ends the call */
        }
        s_call->signals(p.signals, p.nSignals, now);
        hopCheck(now);
        runActs();
    }
}

static void onOutRecv(int handle, size_t)
{
    for (;;) {
        size_t n = recvNext(handle, s_linkBuf, PH_BUF);
        if (n == 0) break;
        if (handle == s_ctx->link && !s_ctx->inbound) callPacket(s_linkBuf, n);
    }
    lxFlush();
}

static void onOutDisc(int ref)
{
    for (int i = 0; i < PH_DYING; ++i)
        if (s_dying[i].h >= 0 && !s_dying[i].inbound && s_dying[i].ref == ref) s_dying[i] = Dying{};
    if (s_ctx->link < 0 || s_ctx->inbound || s_ctx->ref != ref) return;
    s_ctx->link = -1;
    s_call->linkClosed(nowMs());
    runActs();
}

static int onInConnect(int handle, const void* data, size_t len)
{
    if (len < sizeof(rnsd_link_incoming_t)) return -1;
    rnsd_link_incoming_t in;
    std::memcpy(&in, data, sizeof in);
    in.tag[sizeof in.tag - 1] = 0;
    /* LXST's telephony destination proves nothing, either way. */
    rnsdLinkExpectProofs(in.tag, false);
    for (int i = 0; i < PH_PENDING; ++i) {
        if (s_pend[i].h >= 0) continue;
        Pending& p = s_pend[i];
        p.h = handle;
        safeStrncpy(p.tag, in.tag, sizeof p.tag);
        p.idBy = nowMs() + IDENTIFY_MS;
        p.fresh = true;
        static const uint8_t zero[16] = {};
        p.rid = std::memcmp(in.remote_identity_hash, zero, 16) ? hexOf(in.remote_identity_hash, 16) : "";
        return handle;
    }
    warn("lxphone: too many calls coming in, one turned away");
    return -1;
}

static void onInRecv(int handle, size_t)
{
    for (;;) {
        size_t n = recvNext(handle, s_linkBuf, PH_BUF);
        if (n == 0) break;
        if (handle == s_ctx->link && s_ctx->inbound) callPacket(s_linkBuf, n);
    }
    lxFlush();
}

static void onInDisc(int ref)
{
    for (int i = 0; i < PH_PENDING; ++i)
        if (s_pend[i].h == ref) s_pend[i] = Pending{};
    for (int i = 0; i < PH_DYING; ++i)
        if (s_dying[i].h == ref && s_dying[i].inbound) s_dying[i] = Dying{};
    if (s_ctx->link != ref || !s_ctx->inbound) return;
    s_ctx->link = -1;
    s_call->linkClosed(nowMs());
    runActs();
}

/** A pending inbound link identified: it becomes the call, or hears BUSY. */
static void pendingIdentified(int i, std::string identHex)
{
    Pending p = s_pend[i];
    s_pend[i] = Pending{};
    int64_t now = nowMs();
    bool allowed = callerAllowed(identHex);
    uint8_t ih[16], td[16];
    hexTo(identHex, ih, 16);
    /* A call up whose link has heard nothing past deadAfterMs is taken as
     * dead: it ends "Connection lost" and this caller is let in. */
    if (allowed && (s_call->ph == PH_LIVE || s_call->ph == PH_CONNECTING)) {
        std::string lk = "rnsd.links." + s_ctx->tag + ".";
        int ka = storageGetInt((lk + "keepalive_s").c_str(), 0);
        int rtt = storageGetInt((lk + "rtt_ms").c_str(), 0);
        int64_t silent = silentMs(now), dead = deadAfterMs(ka, rtt);
        if (silent > dead) {
            info("lxphone: call link %s heard nothing for %lld s (keepalive %d s, rtt %d ms, bound %lld s): "
                 "ending it as lost for the new call",
                 s_ctx->tag.c_str(), (long long)(silent / 1000), ka, rtt, (long long)(dead / 1000));
            s_ctx->why = "Connection lost";
            if (s_ctx->link >= 0) itsDisconnect(s_ctx->link);
            s_ctx->link = -1;
            s_call->linkClosed(now);
            runActs();
        }
    }
    if (s_call->ph != PH_NONE || !allowed) {
        info("lxphone: %s refused (%s)", identHex.c_str(), allowed ? "a call is up" : "not allowed");
        Result r = refusedResult(s_call->ph != PH_NONE, allowed);
        if (r != R_NONE) logEntry(DIR_IN, identHex, lxmfDestOf(ih), nameFor(ih), wallS(), "", r);
        sendSignal(p.h, ST_BUSY);
        dyingAdd(p.h, -1, true, now + SIGNAL_GRACE_MS);
        return;
    }
    *s_ctx = Ctx{};
    s_ctx->link = p.h;
    s_ctx->inbound = true;
    s_ctx->tag = p.tag;
    s_ctx->peerIdent = identHex;
    s_ctx->peerDest = rnsdDestinationHashFromIdentityHash(ih, "lxst", "telephony", td) ? hexOf(td, 16) : "";
    s_ctx->peerName = nameFor(ih);
    s_ctx->at = wallS();
    s_ctx->heardAt = now;
    s_muted = false;
    info("lxphone: incoming call from %s", identHex.c_str());
    s_call->ring(now);
    runActs();
}

static void pendingRun(int64_t now)
{
    for (int i = 0; i < PH_PENDING; ++i) {
        Pending& p = s_pend[i];
        if (p.h < 0) continue;
        if (p.fresh) {
            /* Even with a call up: the caller identifies, hears BUSY then,
             * and goes in the call log as missed. */
            p.fresh = false;
            sendSignal(p.h, ST_AVAILABLE);
            if (!p.rid.empty()) { pendingIdentified(i, p.rid); continue; }
        }
        if (now >= p.idBy) {
            info("lxphone: a caller never identified");
            dyingAdd(p.h, -1, true, now);
            s_pend[i] = Pending{};
        }
    }
}

/* rnsd.links.in.<8hex>.state closed or failed: an inbound link is gone. rnsd
 * closes its ITS connection only after a grace period, so this is the prompt
 * word that the caller hung up. */
/* A call whose link went stale ends as lost, so the UIs say so. */
static void noteLinkLoss(const std::string& tag)
{
    if (s_call->ph != PH_LIVE && s_call->ph != PH_CONNECTING) return;
    if (storageGetStr(("rnsd.links." + tag + ".last_error").c_str(), "") == "timeout")
        s_ctx->why = "Connection lost";
}

/* rnsd.links.<tag>.heard_s of the call's link moved: its peer was heard. */
static void heardNow()
{
    int64_t now = nowMs();
    s_ctx->heardAt = now;
    if (s_call->ph == PH_LIVE && s_liveStatus != liveStatus(now)) publish();
}

static void inLinkGone(const std::string& t)
{
    for (int i = 0; i < PH_PENDING; ++i)
        if (s_pend[i].h >= 0 && t == s_pend[i].tag) {
            itsDisconnect(s_pend[i].h);
            s_pend[i] = Pending{};
        }
    if (s_ctx->link < 0 || !s_ctx->inbound || s_ctx->tag != t) return;
    noteLinkLoss(t);
    itsDisconnect(s_ctx->link);
    s_ctx->link = -1;
    s_call->linkClosed(nowMs());
    runActs();
}

/* rnsd.links.in.<8hex>.remote_identity: an inbound caller identified. */
static void onInLinkKey(const char* key, const char*)
{
    static const char P[] = "rnsd.links.";
    if (!key || std::strncmp(key, P, sizeof P - 1)) return;
    const char* tag = key + sizeof P - 1;
    const char* dot = std::strrchr(tag, '.');
    if (!dot) return;
    std::string t(tag, dot - tag);
    if (!std::strcmp(dot, ".heard_s")) {
        if (s_ctx->inbound && s_ctx->tag == t) heardNow();
        return;
    }
    if (s_ctx->inbound && hopKey(key)) {
        hopCheck(nowMs());
        runActs();
        return;
    }
    if (!std::strcmp(dot, ".state")) {
        std::string v = storageGetStr(key, "");
        if (v == "closed" || v == "failed") inLinkGone(t);
        return;
    }
    if (std::strcmp(dot, ".remote_identity")) return;
    std::string ih = normHash(storageGetStr(key, ""));
    if (ih.empty()) return;
    for (int i = 0; i < PH_PENDING; ++i)
        if (s_pend[i].h >= 0 && t == s_pend[i].tag) {
            if (s_pend[i].fresh) s_pend[i].rid = ih;   /* AVAILABLE goes first */
            else pendingIdentified(i, ih);
            return;
        }
}

/* rnsd.links.ph<N>.state: our outbound link coming up, or failing. */
static void onOutLinkKey(const char* key, const char* val)
{
    if (s_ctx->tag.empty() || s_ctx->inbound || !val) return;
    if (("rnsd.links." + s_ctx->tag + ".heard_s") == key) {
        heardNow();
        return;
    }
    if (hopKey(key)) {
        hopCheck(nowMs());
        runActs();
        return;
    }
    if (("rnsd.links." + s_ctx->tag + ".state") != key) return;
    std::string v = storageGetStr(key, "");
    int64_t now = nowMs();
    if (v == "active") {
        int rtt = storageGetInt(("rnsd.links." + s_ctx->tag + ".rtt_ms").c_str(), 0);
        s_call->linkActive(now, std::max(10000, 5000 + 4 * rtt));
        hopCheck(now);   /* the first hop is published before the link goes active */
        runActs();
    } else if (v == "failed" || v == "closed") {
        if (s_call->ph == PH_DIAL || s_call->ph == PH_AWAIT) s_ctx->why = "Not reachable";
        noteLinkLoss(s_ctx->tag);
        if (s_ctx->link >= 0) itsDisconnect(s_ctx->link);
        s_ctx->link = -1;
        s_call->linkClosed(now);
        runActs();
    }
}

/* ─────────────── dialling ─────────────── */

static void openLink(const uint8_t tel[16], const uint8_t ih[16])
{
    s_ctx->resolving = false;
    s_ctx->peerDest = hexOf(tel, 16);
    s_ctx->peerIdent = hexOf(ih, 16);
    s_ctx->peerName = nameFor(ih);
    s_ctx->tag = PH_TAG_PFX + std::to_string(s_tagNext);
    s_tagNext = (s_tagNext + 1) % PH_TAGS;
    s_ctx->ref = s_refNext;
    s_refNext = (s_refNext + 1) & 0x7f;
    int64_t left = s_call->deadline() - nowMs();
    std::string id = s_identKey.empty() ? PH_NODE_IDENT : s_identKey;
    int h = rnsdLinkOpen(tel, PH_ASPECT, id.c_str(), s_ctx->tag.c_str(), (uint32_t)std::max<int64_t>(1000, left), 0,
                         s_ctx->ref, onOutRecv, onOutDisc);
    if (h < 0) {
        warn("lxphone: no link to %s", s_ctx->peerDest.c_str());
        s_ctx->why = "No link could be opened";
        s_call->linkClosed(nowMs());
        runActs();
        return;
    }
    s_ctx->link = h;
    s_ctx->heardAt = nowMs();
    rnsdLinkExpectProofs(s_ctx->tag.c_str(), false);
    info("lxphone: calling %s (%s)", s_ctx->peerDest.c_str(), s_ctx->peerIdent.c_str());
    publish();
}

/** Turn what was dialled into the callee's lxst.telephony destination: a
 *  destination whose key we hold (lxst.telephony itself or another of the
 *  identity's, LXMF delivery among them), a phone heard announcing that
 *  identity, or an identity whose telephony or LXMF destination is known.
 *  False while nothing is known yet. */
static bool resolve()
{
    uint8_t typed[16], pk[RNSD_PUBKEY_LEN], ih[16], tel[16], chk[RNSD_PUBKEY_LEN];
    hexTo(s_ctx->typed, typed, 16);
    if (rnsdRecallPubkey(typed, pk) || contactPubkey(s_ctx->typed, pk)) {
        if (!rnsdIdentityHashFromPubkey(pk, ih) || !rnsdDestinationHashFromPubkey(pk, "lxst", "telephony", tel))
            return false;
        if (std::memcmp(tel, typed, 16) && !rnsdRecallPubkey(tel, chk)) rnsdSeedPubkey(tel, pk);
        openLink(tel, ih);
        return true;
    }
    for (auto& [d, p] : *s_peers)
        if (p.ident == s_ctx->typed && hexTo(d, tel, 16)) {
            openLink(tel, typed);
            return true;
        }
    if (!rnsdDestinationHashFromIdentityHash(typed, "lxst", "telephony", tel)) return false;
    if (rnsdRecallPubkey(tel, pk)) {
        openLink(tel, typed);
        return true;
    }
    uint8_t lx[16];
    if (rnsdDestinationHashFromIdentityHash(typed, "lxmf", "delivery", lx) &&
        (rnsdRecallPubkey(lx, pk) || contactPubkey(hexOf(lx, 16), pk))) {
        rnsdSeedPubkey(tel, pk);
        openLink(tel, typed);
        return true;
    }
    return false;
}

static void resolveRun(int64_t now)
{
    if (!s_ctx->resolving || s_call->ph != PH_DIAL || now < s_ctx->resolveAt) return;
    s_ctx->resolveAt = now + PH_RESOLVE_MS;
    if (resolve()) return;
    if (now >= s_ctx->askAt) {
        s_ctx->askAt = now + PH_PATH_ASK_MS;
        uint8_t typed[16], tel[16];
        hexTo(s_ctx->typed, typed, 16);
        rnsdRequestPath(typed);
        if (rnsdDestinationHashFromIdentityHash(typed, "lxst", "telephony", tel)) rnsdRequestPath(tel);
    }
}

/* ─────────────── the destination and announces ─────────────── */

static void onDestRecv(int handle, size_t)
{
    while (recvNext(handle, s_linkBuf, PH_BUF)) {}
}

static void onDestDisc(int) { s_dest = -1; s_retryAt = nowMs() + PH_RETRY_MS; }

static void destClose()
{
    if (s_dest >= 0) itsDisconnect(s_dest);
    s_dest = -1;
}

static void announce()
{
    uint8_t f = RNSD_DEST_ANNOUNCE;   /* no app_data */
    if (s_dest < 0 || itsSend(s_dest, &f, 1, 0) == 0) { s_announceWanted = true; return; }
    s_announceWanted = false;
    int64_t jitter = (int64_t)(esp_random() % 600000) - 300000;   /* ±5 min */
    s_announceAt = nowMs() + PH_ANNOUNCE_MS + jitter;
}

static bool destOpen()
{
    int h = rnsdDestOpen(PH_ASPECT, s_identKey.c_str(), 0, 0, onDestRecv, onDestDisc);
    if (h < 0) return false;
    s_dest = h;
    if (!rnsdDestListenLinks(h, LXST_PHONE_LINK_PORT)) warn("lxphone: listen request not queued");
    announce();
    info("lxphone: hosting %s as %s", PH_ASPECT, s_identKey.c_str());
    return true;
}

/** Bring the destination in line with the settings and the identities. The
 *  identity changes only between calls. */
static void reconcile(int64_t now)
{
    s_enabled = storageGetInt("s.lxst.phone.enabled", 1) != 0;
    if (s_call->ph == PH_NONE) {
        std::string want = phoneIdentity(now - s_startAt < PH_IDENT_WAIT_MS, s_identWait);
        if (!s_identWait && want != s_identKey) {
            destClose();
            s_identKey = want;
            publishIds();
        }
    }
    if (s_enabled && !s_identKey.empty() && s_dest < 0 && now >= s_retryAt) {
        if (!destOpen()) s_retryAt = now + PH_RETRY_MS;
    }
    if ((!s_enabled || s_identKey.empty()) && s_dest >= 0) destClose();
    publish();
}

constexpr size_t ANN_HDR = 1 + 16 + 16 + 64 + 32;

static void peerSet(const std::string& dest, const std::string& ident, const std::string& name, int hops)
{
    auto& peers = *s_peers;
    if (!peers.count(dest) && peers.size() >= PH_PEERS_MAX) {
        auto old = std::min_element(peers.begin(), peers.end(),
                                    [](auto& x, auto& y) { return x.second.seen < y.second.seen; });
        storageDeleteTree(("lxst.phone.peers." + old->first).c_str());
        peers.erase(old);
    }
    Peer& p = peers[dest];
    p.ident = ident;
    if (!name.empty()) p.name = name;
    p.seen = nowMs();
    std::string b = "lxst.phone.peers." + dest + ".";
    storageBegin();
    storageSet((b + "ident").c_str(), ident.c_str());
    storageSet((b + "name").c_str(), p.name.c_str());
    storageSet((b + "hops").c_str(), hops);
    storageSet((b + "heard").c_str(), (int)wallS());
    storageEnd();
}

static void onAnnTel(int handle, size_t)
{
    for (;;) {
        size_t n = recvNext(handle, s_linkBuf, PH_BUF);
        if (n == 0) return;
        if (handle != s_annTel || n < ANN_HDR) continue;
        uint8_t ih[16];
        std::memcpy(ih, s_linkBuf + 17, 16);
        peerSet(hexOf(s_linkBuf + 1, 16), hexOf(ih, 16), nameFor(ih), s_linkBuf[0]);
    }
}

/* LXMF announces name the phones of the same identity. */
static void onAnnLx(int handle, size_t)
{
    for (;;) {
        size_t n = recvNext(handle, s_linkBuf, PH_BUF);
        if (n == 0) return;
        if (handle != s_annLx || n < ANN_HDR || s_peers->empty()) continue;
        std::string ident = hexOf(s_linkBuf + 17, 16);
        char nm[64];
        rnsdAnnounceName(s_linkBuf + ANN_HDR, n - ANN_HDR, nm, sizeof nm);
        if (!*nm) continue;
        for (auto& [d, p] : *s_peers)
            if (p.ident == ident && p.name != nm) {
                p.name = nm;
                storageSet(("lxst.phone.peers." + d + ".name").c_str(), nm);
            }
    }
}

static void onAnnDisc(int)
{
    /* either subscription; the service loop reconnects what is gone */
}

static int annConnect(const char* aspect, void (*cb)(int, size_t))
{
    rnsd_announces_connect_t req = {};
    safeStrncpy(req.aspect, aspect, sizeof req.aspect);
    return itsConnect("rnsd", RNSD_PORT_ANNOUNCES, &req, sizeof req, pdMS_TO_TICKS(2000), 0, cb, onAnnDisc);
}

/* ─────────────── commands ─────────────── */

static void cmdError(const char* why) { storageSet("lxst.phone.cmd.error", why); }
static void cmdOk() { storageSet("lxst.phone.cmd.error", ""); }

/* "<hash>" dials full duplex, "<hash> ptt" push to talk ("ptt" is no hex, so
 * never part of a spaced hash). */
static bool splitPtt(std::string& arg)
{
    if (arg.size() < 3 || arg.compare(arg.size() - 3, 3, "ptt")) return false;
    arg = trim(arg.substr(0, arg.size() - 3));
    return true;
}

static void cmdCall(std::string arg)
{
    if (s_call->ph != PH_NONE) { cmdError("A call is already up."); return; }
    bool ptt = splitPtt(arg);
    std::string h = normHash(arg);
    if (h.empty()) { cmdError("That is not an address (32 hex characters)."); return; }
    cmdOk();
    int64_t now = nowMs();
    *s_ctx = Ctx{};
    s_ctx->typed = h;
    s_ctx->at = wallS();
    s_ctx->resolving = true;
    s_muted = false;
    s_call->dial(now, ptt);
    if (ptt) info("lxphone: push-to-talk call to %s", h.c_str());
    publish();
    resolveRun(now);
}

static void onCmd(const char* key, const char*)
{
    if (!key || std::strcmp(key, "lxst.phone.cmd")) return;
    std::string v = trim(storageGetStr(key, ""));
    if (v.empty()) return;
    storageUnset(key);
    std::string verb = v.substr(0, v.find(' '));
    std::string rest = v.find(' ') == std::string::npos ? "" : trim(v.substr(v.find(' ') + 1));
    int64_t now = nowMs();
    if (verb == "call") cmdCall(rest);
    else if (verb == "answer") {
        if (!s_call->answer(now)) { cmdError("No call is ringing."); return; }
        cmdOk();
        runActs();
    } else if (verb == "reject") {
        if (s_call->ph != PH_RINGING) { cmdError("No call is ringing."); return; }
        cmdOk();
        s_call->hangup(now);
        runActs();
    } else if (verb == "hangup") {
        if (!s_call->hangup(now)) { cmdError("There is no call."); return; }
        cmdOk();
        runActs();
    } else if (verb == "mute") {
        if (s_call->ph == PH_NONE) { cmdError("There is no call."); return; }
        if (rest != "1" && rest != "0") { cmdError("mute 1 or mute 0."); return; }
        cmdOk();
        s_muted = rest == "1";
        lxState();
        publish();
    } else if (verb == "announce") {
        if (s_dest < 0) { cmdError("The phone is not taking calls."); return; }
        cmdOk();
        announce();
    } else {
        cmdError("Unknown command.");
    }
}

/* ─────────────── task ─────────────── */

static TaskHandle_t      s_task = nullptr;
static volatile bool     s_stop = false;
static SemaphoreHandle_t s_parkedSem = nullptr;   /* given by the task each time it parks */

static TickType_t pollTimeout(int64_t now)
{
    int64_t next = INT64_MAX;
    auto at = [&](int64_t t) { if (t > 0 && t < next) next = t; };
    at(s_call->deadline());
    if (s_call->ph != PH_NONE && s_call->ph != PH_BUSYTONE) at(std::max(s_hopAt, now + 1));
    for (int i = 0; i < PH_DYING; ++i) if (s_dying[i].h >= 0) at(s_dying[i].at);
    for (int i = 0; i < PH_PENDING; ++i) if (s_pend[i].h >= 0) at(s_pend[i].fresh ? now : s_pend[i].idBy);
    if (s_ctx->resolving) at(s_ctx->resolveAt);
    if (s_call->ph == PH_LIVE) at(s_timerAt ? s_timerAt : now);
    if (s_endedUntil > now) at(s_endedUntil);
    if (s_dest >= 0) at(s_announceWanted ? now + 1000 : s_announceAt);
    else if (s_enabled && !s_identKey.empty()) at(std::max(s_retryAt, now + 100));
    if (s_annTel < 0 || s_annLx < 0) at(now + PH_RETRY_MS);
    at(s_identAt);
    if (next == INT64_MAX) return portMAX_DELAY;
    int64_t ms = next - now;
    if (ms < 1) ms = 1;
    return pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1;
}

static void service()
{
    int64_t now = nowMs();
    pendingRun(now);
    dyingRun(now);
    resolveRun(now);
    if (s_call->ph != PH_NONE) {
        s_call->tick(now);
        if (now >= s_hopAt) hopCheck(now);
        if (s_call->nActs) runActs();
    }
    publishTimer(now);
    if (s_endedUntil && now >= s_endedUntil) {
        s_endedUntil = 0;
        publish();
    }
    if (s_reconcile || now >= s_identAt) {
        s_reconcile = false;
        reconcile(now);
        s_identAt = now + (s_identWait ? PH_IDENT_RECHECK_MS : PH_IDENT_MS);
    } else if (s_dest < 0 && s_enabled && !s_identKey.empty() && now >= s_retryAt) {
        reconcile(now);
    }
    if (s_dest >= 0 && (s_announceWanted || now >= s_announceAt)) announce();
    if (s_annTel < 0 || s_annLx < 0) {
        static int64_t annRetry = 0;
        if (now >= annRetry) {
            annRetry = now + PH_RETRY_MS;
            if (s_annTel < 0) s_annTel = annConnect(PH_ASPECT, onAnnTel);
            if (s_annLx < 0) s_annLx = annConnect("lxmf.delivery", onAnnLx);
        }
    }
}

static void publishIdle()
{
    storageBegin();
    storageSet("lxst.phone.dir", "");
    storageSet("lxst.phone.peer", "");
    storageSet("lxst.phone.peer_dest", "");
    storageSet("lxst.phone.peer_name", "");
    storageSet("lxst.phone.timer", "");
    storageEnd();
    publish();
}

static void shutdown()
{
    if (s_call->ph != PH_NONE) {
        s_call->hangup(nowMs());
        runActs();
    }
    if (s_ctx->link >= 0) itsDisconnect(s_ctx->link);
    s_ctx->link = -1;
    for (int i = 0; i < PH_DYING; ++i) if (s_dying[i].h >= 0) itsDisconnect(s_dying[i].h);
    for (int i = 0; i < PH_DYING; ++i) s_dying[i] = Dying{};
    for (int i = 0; i < PH_PENDING; ++i) if (s_pend[i].h >= 0) itsDisconnect(s_pend[i].h);
    for (int i = 0; i < PH_PENDING; ++i) s_pend[i] = Pending{};
    if (s_lx >= 0) itsDisconnect(s_lx);
    s_lx = -1;
    destClose();
    if (s_annTel >= 0) itsDisconnect(s_annTel);
    if (s_annLx >= 0) itsDisconnect(s_annLx);
    s_annTel = s_annLx = -1;
    s_peers->clear();
    storageDeleteTree("lxst.phone.peers");
    s_identKey.clear();
    storageBegin();
    storageSet("lxst.phone.state", "off");
    storageSet("lxst.phone.status", "Reticulum is stopped");
    storageSet("lxst.phone.dest", "");
    storageSet("lxst.phone.ident", "");
    storageEnd();
}

static void phoneTaskMain(void*)
{
    s_linkBuf = (uint8_t*)heap_caps_malloc(PH_BUF, MALLOC_CAP_SPIRAM);
    s_txBuf = (uint8_t*)heap_caps_malloc(PH_BUF, MALLOC_CAP_SPIRAM);
    s_lxBuf = (uint8_t*)heap_caps_malloc(PH_BUF, MALLOC_CAP_SPIRAM);
    s_fwd = (uint8_t*)heap_caps_malloc(PH_BUF, MALLOC_CAP_SPIRAM);
    s_call = (Call*)heap_caps_malloc(sizeof(Call), MALLOC_CAP_SPIRAM);
    s_ctx = (Ctx*)heap_caps_malloc(sizeof(Ctx), MALLOC_CAP_SPIRAM);
    s_pend = (Pending*)heap_caps_malloc(sizeof(Pending) * PH_PENDING, MALLOC_CAP_SPIRAM);
    s_dying = (Dying*)heap_caps_malloc(sizeof(Dying) * PH_DYING, MALLOC_CAP_SPIRAM);
    s_pkt = (Packet*)heap_caps_malloc(sizeof(Packet), MALLOC_CAP_SPIRAM);
    if (!s_linkBuf || !s_txBuf || !s_lxBuf || !s_fwd || !s_call || !s_ctx || !s_pend || !s_dying || !s_pkt) {
        err("lxphone: no memory");
        killSelf();
    }
    new (s_call) Call();
    new (s_ctx) Ctx();
    for (int i = 0; i < PH_PENDING; ++i) new (&s_pend[i]) Pending();
    for (int i = 0; i < PH_DYING; ++i) new (&s_dying[i]) Dying();
    new (s_pkt) Packet();
    s_peers = new std::map<std::string, Peer>();

    itsServerInit();
    itsClientInit(8);
    /* 64 packets from rnsd queue: over a second of a one-frame-per-packet
     * 20 ms peer, so a busy moment on this task does not overflow it. */
    itsServerPortOpen(LXST_PHONE_LINK_PORT, ITS_PACKET, PH_PENDING + PH_DYING + 1, 8192, 8192, 64, PH_BUF);
    itsServerOnConnect(LXST_PHONE_LINK_PORT, onInConnect);
    itsServerOnRecv(LXST_PHONE_LINK_PORT, onInRecv);
    itsServerOnDisconnect(LXST_PHONE_LINK_PORT, onInDisc);

    storageSubscribeChanges("lxst.phone.cmd", onCmd);
    storageSubscribeChanges("rnsd.links.in.", onInLinkKey);
    storageSubscribeChanges("rnsd.links." PH_TAG_PFX, onOutLinkKey);
    storageSubscribeChanges("s.lxst.phone.enabled", ON_CHANGE { (void)key; (void)val; s_reconcile = true; });
    storageSubscribeChanges("s.lxst.phone.identity", ON_CHANGE { (void)key; (void)val; s_reconcile = true; });
    logBoot();
    info("task up");

    for (;;) {
        s_reconcile = true;
        s_identAt = 0;
        s_retryAt = 0;
        s_startAt = nowMs();
        publishIdle();
        while (!s_stop) {
            service();
            itsPoll(pollTimeout(nowMs()));
        }
        shutdown();
        info("stopped");
        xSemaphoreGive(s_parkedSem);
        while (s_stop) itsPoll(portMAX_DELAY);
    }
}

static void phoneStart()
{
    s_stop = false;
    if (!s_parkedSem) s_parkedSem = xSemaphoreCreateBinary();
    if (!s_task) s_task = spawnTask(phoneTaskMain, TAG, 8192, nullptr, 1, 1, STACK_PSRAM);
    else xTaskNotifyGive(s_task);
}

/* rnsStop runs the stop hooks in reverse order and each returns only once its
 * component is down, so the phone's links are torn down while rnsd and the
 * interfaces still carry them. The hook waits on the task's park signal, at
 * most 3 s. */
static void phoneStop()
{
    if (!s_task || s_stop) return;
    xSemaphoreTake(s_parkedSem, 0);           /* a park signal left by a stop that timed out */
    s_stop = true;
    xTaskNotifyGive(s_task);
    if (xSemaphoreTake(s_parkedSem, pdMS_TO_TICKS(3000)) != pdTRUE) warn("[%s] stop timed out", TAG);
}

/* ─────────────── CLI ─────────────── */

/* The CLI runs on the console task: it reads storage and writes the command
 * key, never the task's own state. */

static std::vector<std::pair<std::string, std::string>>* s_cliAcc = nullptr;
static void cliLeaf(const char* key, const char* val)
{
    if (s_cliAcc && key) s_cliAcc->emplace_back(key, val ? val : "");
}

static std::map<std::string, std::map<std::string, std::string>> cliRecords(const char* prefix)
{
    std::vector<std::pair<std::string, std::string>> leaves;
    s_cliAcc = &leaves;
    storageForEach(prefix, cliLeaf);
    s_cliAcc = nullptr;
    std::map<std::string, std::map<std::string, std::string>> out;
    size_t pl = std::strlen(prefix);
    for (auto& [k, v] : leaves) {
        if (k.size() <= pl) continue;
        std::string tail = k.substr(pl);
        size_t dot = tail.find('.');
        if (dot == std::string::npos) continue;
        out[tail.substr(0, dot)][tail.substr(dot + 1)] = v;
    }
    return out;
}

static void cliCmd(const std::string& v)
{
    storageSet("lxst.phone.cmd", v.c_str());
    cliPrintf("sent\n");
}

static void cliPhone(const char* args)
{
    if (args && !std::strcmp(args, "help")) {
        cliPrintf("%-*s LXST phone: call, answer, hang up\n", CLI_HELP_COL, "phone [...]");
        return;
    }
    if (args && cliWantsHelp(args)) {
        cliPrintf("phone                    state, call, our address\n");
        cliPrintf("phone call <hash> [ptt]  phone, identity or LXMF address; ptt: push to talk\n");
        cliPrintf("phone answer | reject | hangup\n");
        cliPrintf("phone mute 1|0\n");
        cliPrintf("phone log [n] | peers | announce\n");
        return;
    }
    std::string a = args ? trim(args) : "";
    std::string verb = a.substr(0, a.find(' '));
    std::string rest = a.find(' ') == std::string::npos ? "" : trim(a.substr(a.find(' ') + 1));

    if (verb.empty() || verb == "status") {
        cliPrintf("%s: %s\n", storageGetStr("lxst.phone.state", "off").c_str(),
                  storageGetStr("lxst.phone.status", "").c_str());
        std::string peer = storageGetStr("lxst.phone.peer", "");
        std::string st = storageGetStr("lxst.phone.state", "");
        if (!peer.empty() && st != "idle" && st != "off")
            cliPrintf("%s %s \"%s\"  %s, %s  %s%s\n", storageGetStr("lxst.phone.dir", "").c_str(), peer.c_str(),
                      storageGetStr("lxst.phone.peer_name", "").c_str(),
                      storageGetStr("lxst.phone.profile", "").c_str(), storageGetStr("lxst.phone.mode", "").c_str(),
                      storageGetStr("lxst.phone.timer", "").c_str(),
                      storageGetInt("lxst.phone.muted", 0) ? " muted" : "");
        cliPrintf("dest %s  ident %s\n", storageGetStr("lxst.phone.dest", "").c_str(),
                  storageGetStr("lxst.phone.ident", "").c_str());
        return;
    }
    if (verb == "call") {
        bool ptt = splitPtt(rest);
        if (normHash(rest).empty()) { cliPrintf("usage: phone call <32 hex> [ptt]\n"); return; }
        cliCmd("call " + normHash(rest) + (ptt ? " ptt" : ""));
        return;
    }
    if (verb == "answer" || verb == "reject" || verb == "hangup" || verb == "announce") { cliCmd(verb); return; }
    if (verb == "mute") {
        if (rest != "1" && rest != "0") { cliPrintf("usage: phone mute 1|0\n"); return; }
        cliCmd("mute " + rest);
        return;
    }
    if (verb == "log") {
        int want = rest.empty() ? 10 : std::max(1, std::atoi(rest.c_str()));
        auto recs = cliRecords("s.lxst.phone.log.");
        std::vector<int> ns;
        for (auto& [n, f] : recs) ns.push_back(std::atoi(n.c_str()));
        std::sort(ns.begin(), ns.end());
        for (size_t i = ns.size() > (size_t)want ? ns.size() - want : 0; i < ns.size(); ++i) {
            auto& f = recs[std::to_string(ns[i])];
            time_t t = (time_t)std::atol(f["at"].c_str());
            char when[20] = "-";
            if (t) strftime(when, sizeof when, "%m-%d %H:%M", localtime(&t));
            cliPrintf("%s %-3s %.8s %-14s %-9s %s\n", when, f["dir"].c_str(), f["peer"].c_str(),
                      f["name"].substr(0, 14).c_str(), f["result"].c_str(), f["dur"].c_str());
        }
        return;
    }
    if (verb == "peers") {
        for (auto& [d, f] : cliRecords("lxst.phone.peers."))
            cliPrintf("%s %-16s %s hops\n", d.c_str(), f["name"].empty() ? "-" : f["name"].substr(0, 16).c_str(),
                      f["hops"].c_str());
        return;
    }
    cliPrintf("unknown subcommand. try `phone -h`\n");
}

void LxstPhoneService::onInit()
{
    storageBegin();
    storageDefault("s.lxst.phone.enabled", 1);
    storageDefault("s.lxst.phone.identity", "");
    storageDefault("s.lxst.phone.allow", "all");
    storageDefault("s.lxst.phone.blocked", "");
    storageDefault("s.lxst.phone.profile", "auto");
    storageEnd();
    cliRegisterCmd("phone", cliPhone);
    rnsServiceRegister(TAG, phoneStart, phoneStop);
}
