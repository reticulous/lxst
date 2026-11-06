/**
 * lxst_phone_wire.cpp — LXST telephony packets and call profiles. See
 * lxst_phone_wire.h.
 */
#include "lxst_phone_wire.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace lxphone {

namespace {

struct Rd {
    const uint8_t* p;
    const uint8_t* e;
    bool left(size_t n) const { return (size_t)(e - p) >= n; }
};

uint64_t be(const uint8_t* p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}

/* A w-byte big-endian length or count field. */
bool lenField(Rd& r, int w, uint64_t& n)
{
    if (!r.left((size_t)w)) return false;
    n = be(r.p, w);
    r.p += w;
    return true;
}

bool skip(Rd& r, int depth);

bool skipN(Rd& r, uint64_t n, int depth)
{
    while (n--) if (!skip(r, depth)) return false;
    return true;
}

bool skipBytes(Rd& r, uint64_t n)
{
    if (n > (uint64_t)(r.e - r.p)) return false;
    r.p += n;
    return true;
}

bool skip(Rd& r, int depth)
{
    if (depth > 16 || !r.left(1)) return false;
    uint8_t b = *r.p++;
    uint64_t n = 0;
    if (b <= 0x7f || b >= 0xe0 || b == 0xc0 || b == 0xc2 || b == 0xc3) return true;
    if ((b & 0xf0) == 0x80) return skipN(r, 2ull * (b & 0x0f), depth + 1);
    if ((b & 0xf0) == 0x90) return skipN(r, b & 0x0f, depth + 1);
    if ((b & 0xe0) == 0xa0) return skipBytes(r, b & 0x1f);
    switch (b) {
    case 0xc4: case 0xd9: return lenField(r, 1, n) && skipBytes(r, n);
    case 0xc5: case 0xda: return lenField(r, 2, n) && skipBytes(r, n);
    case 0xc6: case 0xdb: return lenField(r, 4, n) && skipBytes(r, n);
    case 0xc7: return lenField(r, 1, n) && skipBytes(r, n + 1);
    case 0xc8: return lenField(r, 2, n) && skipBytes(r, n + 1);
    case 0xc9: return lenField(r, 4, n) && skipBytes(r, n + 1);
    case 0xca: return skipBytes(r, 4);
    case 0xcb: return skipBytes(r, 8);
    case 0xcc: case 0xd0: return skipBytes(r, 1);
    case 0xcd: case 0xd1: return skipBytes(r, 2);
    case 0xce: case 0xd2: return skipBytes(r, 4);
    case 0xcf: case 0xd3: return skipBytes(r, 8);
    case 0xd4: return skipBytes(r, 2);
    case 0xd5: return skipBytes(r, 3);
    case 0xd6: return skipBytes(r, 5);
    case 0xd7: return skipBytes(r, 9);
    case 0xd8: return skipBytes(r, 17);
    case 0xdc: return lenField(r, 2, n) && skipN(r, n, depth + 1);
    case 0xdd: return lenField(r, 4, n) && skipN(r, n, depth + 1);
    case 0xde: return lenField(r, 2, n) && skipN(r, 2 * n, depth + 1);
    case 0xdf: return lenField(r, 4, n) && skipN(r, 2 * n, depth + 1);
    default: return false;   /* 0xc1 */
    }
}

/* A non-negative integer at r: true with v set. Anything else is skipped and
 * reported through `isUint` false (a malformed value returns false). */
bool readUint(Rd& r, uint64_t& v, bool& isUint)
{
    isUint = false;
    if (!r.left(1)) return false;
    uint8_t b = *r.p;
    int w = b == 0xcc ? 1 : b == 0xcd ? 2 : b == 0xce ? 4 : b == 0xcf ? 8 : 0;
    if (b <= 0x7f) { v = b; r.p++; isUint = true; return true; }
    if (w) {
        if (!r.left(1 + (size_t)w)) return false;
        v = be(r.p + 1, w);
        r.p += 1 + w;
        isUint = true;
        return true;
    }
    /* signed forms holding a non-negative value count too */
    w = b == 0xd0 ? 1 : b == 0xd1 ? 2 : b == 0xd2 ? 4 : b == 0xd3 ? 8 : 0;
    if (w) {
        if (!r.left(1 + (size_t)w)) return false;
        uint64_t raw = be(r.p + 1, w);
        bool neg = raw >> (8 * w - 1) & 1;
        r.p += 1 + w;
        if (!neg) { v = raw; isUint = true; }
        return true;
    }
    return skip(r, 0);
}

/* A bin at r: true with the bytes set. Anything else is skipped. */
bool readBin(Rd& r, Bytes& out, bool& isBin)
{
    isBin = false;
    if (!r.left(1)) return false;
    uint8_t b = *r.p;
    int w = b == 0xc4 ? 1 : b == 0xc5 ? 2 : b == 0xc6 ? 4 : 0;
    if (!w) return skip(r, 0);
    r.p++;
    uint64_t n;
    if (!lenField(r, w, n) || !r.left((size_t)n)) return false;
    out.p = r.p;
    out.n = (size_t)n;
    r.p += n;
    isBin = true;
    return true;
}

/* An array header at r: its count. False (and nothing consumed) when the
 * value is not an array. */
bool arrayHeader(Rd& r, uint64_t& n)
{
    if (!r.left(1)) return false;
    uint8_t b = *r.p;
    if ((b & 0xf0) == 0x90) { n = b & 0x0f; r.p++; return true; }
    if (b == 0xdc && r.left(3)) { n = be(r.p + 1, 2); r.p += 3; return true; }
    if (b == 0xdd && r.left(5)) { n = be(r.p + 1, 4); r.p += 5; return true; }
    return false;
}

bool readSignals(Rd& r, Packet& out)
{
    out.hasSignals = true;
    out.nSignals = 0;
    uint64_t count;
    bool ok;
    uint64_t v;
    if (!arrayHeader(r, count)) {
        if (!readUint(r, v, ok)) return false;
        if (ok && v <= 0xffffffffu) out.signals[out.nSignals++] = (uint32_t)v;
        return true;
    }
    while (count--) {
        if (!readUint(r, v, ok)) return false;
        if (ok && v <= 0xffffffffu && out.nSignals < MAX_SIGNALS) out.signals[out.nSignals++] = (uint32_t)v;
    }
    return true;
}

bool readFrames(Rd& r, Packet& out)
{
    out.hasFrames = true;
    out.nFrames = 0;
    uint64_t count;
    bool ok;
    Bytes f;
    if (!arrayHeader(r, count)) {
        if (!readBin(r, f, ok)) return false;
        if (ok) out.frames[out.nFrames++] = f;
        return true;
    }
    while (count--) {
        if (!readBin(r, f, ok)) return false;
        if (ok && out.nFrames < MAX_FRAMES) out.frames[out.nFrames++] = f;
    }
    return true;
}

size_t putUint(uint32_t v, uint8_t* o)
{
    if (v <= 0x7f) { o[0] = (uint8_t)v; return 1; }
    if (v <= 0xff) { o[0] = 0xcc; o[1] = (uint8_t)v; return 2; }
    if (v <= 0xffff) { o[0] = 0xcd; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)v; return 3; }
    o[0] = 0xce;
    o[1] = (uint8_t)(v >> 24); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 8); o[4] = (uint8_t)v;
    return 5;
}

const ProfileInfo PROFILES[] = {
    { 0x10, "ulbw", "Codec2 700C, 400 ms",    0x02, 0x00, 400, 42,  2 },
    { 0x20, "vlbw", "Codec2 1600, 320 ms",    0x02, 0x04, 320, 66,  2 },
    { 0x30, "lbw",  "Codec2 3200, 200 ms",    0x02, 0x06, 200, 82,  2 },
    { 0x40, "mq",   "Opus medium, 60 ms",     0x01, 0x01, 60,  61,  5 },
    { 0x50, "hq",   "Opus high, 60 ms",       0x01, 0x02, 60,  121, 5 },
    { 0x60, "shq",  "Opus super-high, 60 ms", 0x01, 0x03, 60,  241, 5 },
    { 0x70, "ull",  "Opus medium, 10 ms",     0x01, 0x01, 10,  11,  2 },
    { 0x80, "ll",   "Opus medium, 20 ms",     0x01, 0x01, 20,  21,  3 },
};

}  // namespace

bool decode(const uint8_t* p, size_t n, Packet& out)
{
    out.hasSignals = out.hasFrames = false;
    out.nSignals = out.nFrames = 0;
    Rd r{ p, p + n };
    if (!r.left(1)) return false;
    uint8_t b = *r.p;
    uint64_t entries;
    if ((b & 0xf0) == 0x80) { entries = b & 0x0f; r.p++; }
    else if (b == 0xde && r.left(3)) { entries = be(r.p + 1, 2); r.p += 3; }
    else if (b == 0xdf && r.left(5)) { entries = be(r.p + 1, 4); r.p += 5; }
    else return false;
    while (entries--) {
        uint64_t key;
        bool isUint;
        if (!readUint(r, key, isUint)) return false;
        if (isUint && key == FIELD_SIGNALLING) { if (!readSignals(r, out)) return false; }
        else if (isUint && key == FIELD_FRAMES) { if (!readFrames(r, out)) return false; }
        else if (!skip(r, 0)) return false;
    }
    return r.p == r.e;
}

size_t encodeSignals(const uint16_t* s, int n, uint8_t* out, size_t cap)
{
    if (n < 0 || n > 15 || cap < 3 + 3 * (size_t)n) return 0;
    size_t k = 0;
    out[k++] = 0x81;
    out[k++] = FIELD_SIGNALLING;
    out[k++] = (uint8_t)(0x90 | n);
    for (int i = 0; i < n; ++i) k += putUint(s[i], out + k);
    return k;
}

namespace {

/* A bin at out: its length, 0 when it does not fit in cap. */
size_t putBin(const uint8_t* b, size_t n, uint8_t* out, size_t cap)
{
    size_t hdr = n < 0x100 ? 2 : n < 0x10000 ? 3 : 5;
    if (cap < hdr + n) return 0;
    size_t k = 0;
    if (n < 0x100) { out[k++] = 0xc4; out[k++] = (uint8_t)n; }
    else if (n < 0x10000) { out[k++] = 0xc5; out[k++] = (uint8_t)(n >> 8); out[k++] = (uint8_t)n; }
    else {
        out[k++] = 0xc6;
        out[k++] = (uint8_t)(n >> 24); out[k++] = (uint8_t)(n >> 16);
        out[k++] = (uint8_t)(n >> 8); out[k++] = (uint8_t)n;
    }
    if (n) std::memcpy(out + k, b, n);
    return k + n;
}

}  // namespace

size_t encodeFrames(const Bytes* frames, int n, uint8_t* out, size_t cap)
{
    if (n < 1) return 0;
    if (n == 1) return encodeFrame(frames[0].p, frames[0].n, out, cap);
    size_t hdr = n < 16 ? 1 : n < 0x10000 ? 3 : 5;
    if (cap < 2 + hdr) return 0;
    size_t k = 0;
    out[k++] = 0x81;
    out[k++] = FIELD_FRAMES;
    if (n < 16) out[k++] = (uint8_t)(0x90 | n);
    else if (n < 0x10000) { out[k++] = 0xdc; out[k++] = (uint8_t)(n >> 8); out[k++] = (uint8_t)n; }
    else {
        out[k++] = 0xdd;
        out[k++] = (uint8_t)(n >> 24); out[k++] = (uint8_t)(n >> 16);
        out[k++] = (uint8_t)(n >> 8); out[k++] = (uint8_t)n;
    }
    for (int i = 0; i < n; ++i) {
        size_t w = putBin(frames[i].p, frames[i].n, out + k, cap - k);
        if (!w) return 0;
        k += w;
    }
    return k;
}

size_t encodeFrame(const uint8_t* frame, size_t n, uint8_t* out, size_t cap)
{
    size_t hdr = n < 0x100 ? 2 : n < 0x10000 ? 3 : 5;
    if (cap < 2 + hdr + n) return 0;
    size_t k = 0;
    out[k++] = 0x81;
    out[k++] = FIELD_FRAMES;
    if (n < 0x100) { out[k++] = 0xc4; out[k++] = (uint8_t)n; }
    else if (n < 0x10000) { out[k++] = 0xc5; out[k++] = (uint8_t)(n >> 8); out[k++] = (uint8_t)n; }
    else {
        out[k++] = 0xc6;
        out[k++] = (uint8_t)(n >> 24); out[k++] = (uint8_t)(n >> 16);
        out[k++] = (uint8_t)(n >> 8); out[k++] = (uint8_t)n;
    }
    if (n) std::memcpy(out + k, frame, n);
    return k + n;
}

SigKind sigKind(uint32_t s)
{
    if (s >= PREFERRED_PROFILE) return SIG_PROFILE;
    if (s >= PREFERRED_MODE) return SIG_MODE;
    return SIG_STATUS;
}

uint32_t sigValue(uint32_t s)
{
    switch (sigKind(s)) {
    case SIG_PROFILE: return s - PREFERRED_PROFILE;
    case SIG_MODE:    return s - PREFERRED_MODE;
    default:          return s;
    }
}

const ProfileInfo* profileInfo(uint32_t id)
{
    for (const auto& p : PROFILES) if (p.id == id) return &p;
    return nullptr;
}

const ProfileInfo* profileByKey(const std::string& key)
{
    for (const auto& p : PROFILES) if (key == p.key) return profileInfo(runProfile(p.id));
    return nullptr;
}

const ProfileInfo* profileTable(int& count)
{
    count = (int)(sizeof PROFILES / sizeof PROFILES[0]);
    return PROFILES;
}

int framesPerPacket(const ProfileInfo& p)
{
    if (!p.frame_ms || p.frame_ms >= BATCH_MS) return 1;
    return (BATCH_MS + p.frame_ms - 1) / p.frame_ms;
}

int payloadBytes(const ProfileInfo& p)
{
    int n = framesPerPacket(p);
    int fb = p.frame_bytes;
    auto binHdr = [](int len) { return len < 0x100 ? 2 : 3; };
    if (n == 1) return 2 + binHdr(fb) + fb;               /* {0x01: bin} */
    int arrHdr = n < 16 ? 1 : 3;
    return 2 + arrHdr + n * (binHdr(fb) + fb);            /* {0x01: [bin …]} */
}

int packetBytes(const ProfileInfo& p)
{
    int padded = (payloadBytes(p) / 16 + 1) * 16;
    return LINK_HEADER_BYTES + LINK_TOKEN_BYTES + padded;
}

int packetMs(const ProfileInfo& p)
{
    return p.frame_ms * framesPerPacket(p);
}

uint32_t txBps(const ProfileInfo& p)
{
    int ms = packetMs(p);
    if (ms <= 0) return 0;
    return (uint32_t)(((uint64_t)packetBytes(p) * 8 * 1000 + ms - 1) / ms);
}

bool fitsHop(const ProfileInfo& p, uint32_t hopBps, uint8_t mode)
{
    if (!hopBps) return true;
    return (uint64_t)txBps(p) * modeDirections(mode) * 100 <= (uint64_t)hopBps * HOP_SHARE_PCT;
}

uint8_t fitProfile(uint8_t want, uint32_t hopBps, uint8_t mode)
{
    const ProfileInfo* w = profileInfo(runProfile(want));
    if (w && fitsHop(*w, hopBps, mode)) return w->id;
    const ProfileInfo* best = nullptr;
    for (const auto& p : PROFILES) {
        if (runProfile(p.id) != p.id || !fitsHop(p, hopBps, mode)) continue;
        if (!best || txBps(p) > txBps(*best)) best = &p;
    }
    return best ? best->id : 0;
}

CallFit fitCall(uint8_t want, uint32_t hopBps, uint8_t mode)
{
    CallFit f;
    f.profile = fitProfile(want, hopBps, mode);
    f.mode = f.profile ? mode : 0;
    return f;
}

int64_t talkSecondsLeft(const ProfileInfo& p, uint32_t hopBps,
                        uint32_t usedMs, uint32_t limitMs)
{
    if (!limitMs || !hopBps) return -1;
    uint64_t spent = (uint64_t)usedMs + AIRTIME_RESERVE_MS;
    if (spent >= limitMs) return 0;
    uint32_t bps = txBps(p);
    if (!bps) return -1;
    /* Airtime spent per second of talk is bps / hopBps seconds. */
    return (int64_t)((limitMs - spent) * hopBps / bps / 1000);
}

CallFit autoProfile(uint32_t hopBps, int rtt_ms, uint8_t mode)
{
    uint8_t want = rtt_ms > 4000 ? 0x10 : rtt_ms > 1500 ? 0x30 : DEFAULT_PROFILE;
    return fitCall(want, hopBps, mode);
}

void receiveBuffer(const ProfileInfo& p, uint16_t& jitter_ms, uint16_t& max_jitter_ms, bool batchedHop)
{
    int start = p.buffer > 3 ? p.buffer - 2 : 1;
    int j = p.frame_ms * start;
    if (j < JITTER_FLOOR_MS) j = JITTER_FLOOR_MS;
    int m = p.frame_ms * (p.buffer + 1) + JITTER_MARGIN_MS;
    if (batchedHop && p.frame_ms > 0) {
        int b = (BATCH_WAIT_MS + p.frame_ms - 1) / p.frame_ms * p.frame_ms;
        if (j < b) j = b;
        int bm = j + p.frame_ms * BATCH_FRAMES + JITTER_MARGIN_MS;
        if (m < bm) m = bm;
    }
    if (m < j) m = j;
    jitter_ms = (uint16_t)j;
    max_jitter_ms = (uint16_t)m;
}

std::string durationText(int64_t secs)
{
    if (secs < 0) secs = 0;
    char b[24];
    std::snprintf(b, sizeof b, "%lld:%02d", (long long)(secs / 60), (int)(secs % 60));
    return b;
}

std::string normHash(const std::string& in)
{
    std::string s;
    for (char c : in) {
        if (std::isspace((unsigned char)c)) continue;
        if (!std::isxdigit((unsigned char)c)) return "";
        s += (char)std::tolower((unsigned char)c);
    }
    if (s.size() == 64) s.resize(32);
    return s.size() == 32 ? s : "";
}

bool listHas(const std::string& list, const std::string& hash)
{
    std::string h = normHash(hash);
    if (h.empty()) return false;
    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && std::isspace((unsigned char)list[i])) i++;
        size_t j = i;
        while (j < list.size() && !std::isspace((unsigned char)list[j])) j++;
        if (j > i && normHash(list.substr(i, j - i)) == h) return true;
        i = j;
    }
    return false;
}

}  // namespace lxphone
