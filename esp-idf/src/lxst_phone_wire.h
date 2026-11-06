/**
 * lxst_phone_wire.h — LXST telephony on the wire, and the call profiles.
 *
 * Every Link packet is one msgpack map: key 0x00 a list of signals (a bare
 * integer is a one-element list), key 0x01 one LXST frame (bin) or a list of
 * them. Both may be in one map. Encoding writes into a caller buffer and
 * decoding points into the input, so neither allocates.
 *
 * Plain C++17 with no platform includes, so the host tests build it as-is.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lxphone {

constexpr uint8_t FIELD_SIGNALLING = 0x00;
constexpr uint8_t FIELD_FRAMES     = 0x01;

/* Status codes; ST_CALLING is local state and never sent. */
enum : uint16_t {
    ST_BUSY        = 0x00,
    ST_REJECTED    = 0x01,
    ST_CALLING     = 0x02,
    ST_AVAILABLE   = 0x03,
    ST_RINGING     = 0x04,
    ST_CONNECTING  = 0x05,
    ST_ESTABLISHED = 0x06,
    PREFERRED_MODE    = 0xF0,   /* + mode    */
    PREFERRED_PROFILE = 0xFF,   /* + profile */
};

enum : uint8_t { MODE_FULL_DUPLEX = 0x01, MODE_HALF_DUPLEX = 0x02 };

constexpr uint8_t DEFAULT_PROFILE = 0x40;

constexpr int MAX_SIGNALS = 16;   /* more in one packet are dropped */
constexpr int MAX_FRAMES  = 32;

struct Bytes {
    const uint8_t* p = nullptr;
    size_t         n = 0;
};

struct Packet {
    bool     hasSignals = false;
    int      nSignals = 0;
    uint32_t signals[MAX_SIGNALS];
    bool     hasFrames = false;
    int      nFrames = 0;
    Bytes    frames[MAX_FRAMES];
};

/** Decode one packet. False when it is not a well-formed msgpack map. Keys
 *  other than 0x00 and 0x01 are skipped; signals that are not non-negative
 *  integers, and frames that are not bin, are left out. */
bool decode(const uint8_t* p, size_t n, Packet& out);

/** {0x00: [s…]} into out; returns its length, 0 when it does not fit. */
size_t encodeSignals(const uint16_t* s, int n, uint8_t* out, size_t cap);

/** {0x01: frame} into out; returns its length, 0 when it does not fit. */
size_t encodeFrame(const uint8_t* frame, size_t n, uint8_t* out, size_t cap);

/** {0x01: frame} for one frame, {0x01: [frame, …]} for more, into out;
 *  returns its length, 0 when n < 1 or it does not fit. */
size_t encodeFrames(const Bytes* frames, int n, uint8_t* out, size_t cap);

/** What a signal is: a profile preference, a mode preference or a status. */
enum SigKind : uint8_t { SIG_STATUS, SIG_MODE, SIG_PROFILE };
SigKind sigKind(uint32_t s);
/** The profile or mode a preference carries, or the status code itself. */
uint32_t sigValue(uint32_t s);

/* ── profiles ── */

struct ProfileInfo {
    uint8_t     id;
    const char* key;          /* s.lxst.phone.profile value */
    const char* text;         /* "Opus medium, 60 ms" */
    uint8_t     codec;        /* LXST codec byte: 0x01 Opus, 0x02 Codec2 */
    uint8_t     mode;         /* Codec2 mode byte or Opus profile byte */
    uint16_t    frame_ms;
    uint16_t    frame_bytes;  /* largest frame, codec byte included */
    uint8_t     buffer;       /* receive buffer, frames */
};

/** The profile with this id, or nullptr. */
const ProfileInfo* profileInfo(uint32_t id);
/** The profile stored under this key ("mq", "lbw" …), or nullptr; "ull"
 *  gives the profile the node runs instead, 0x80. */
const ProfileInfo* profileByKey(const std::string& key);

/** The node never runs a call at 0x70 (one 10 ms frame per packet from LXST
 *  is beyond it): the profile a call at `id` runs at, 0x80 for 0x70, else
 *  `id`. 0x70 stays in the table so its frames still decode. */
constexpr uint8_t PROFILE_ULL = 0x70, PROFILE_LL = 0x80;
inline uint8_t runProfile(uint8_t id) { return id == PROFILE_ULL ? PROFILE_LL : id; }
/** Every profile, in id order. */
const ProfileInfo* profileTable(int& count);

/** Frames sent in one packet: ceil(BATCH_MS / frame_ms), so a packet carries
 *  at least BATCH_MS of audio; 1 for frames of BATCH_MS and longer. */
constexpr int BATCH_MS = 60;
int framesPerPacket(const ProfileInfo& p);

/* ── what a profile costs the first hop ── */

/** Reticulum's per-packet cost on a Link: header (2) + link id (16) +
 *  context (1), and the link token's IV (16) and HMAC (32) around a
 *  plaintext PKCS#7-padded to whole 16-byte blocks. */
constexpr int LINK_HEADER_BYTES = 19;
constexpr int LINK_TOKEN_BYTES  = 48;
/** The share of the first hop's bitrate a call's voice may take, both
 *  directions together: headroom for carrier sense and everyone else on the
 *  channel. */
constexpr int HOP_SHARE_PCT = 60;
/** Directions on the air at once in a call mode: 2 in full duplex, where
 *  both ends transmit throughout, 1 in half duplex. */
inline int modeDirections(uint8_t mode) { return mode == MODE_HALF_DUPLEX ? 1 : 2; }
/** A call starts over a hop with an hourly airtime budget only while at least
 *  this much talk time at the profile's rate is left in it. */
constexpr int MIN_TALK_S = 60;
/** Airtime held back below a hop's hourly limit: its published use is up to
 *  10 s old while it transmits and is checked each second, and the radio
 *  spends at most 1 s of airtime per second, so stopping at limit − 15 s keeps
 *  the hour's actual airtime within the limit. */
constexpr uint32_t AIRTIME_RESERVE_MS = 15000;

/** The msgpack payload of one audio packet at full frames. */
int payloadBytes(const ProfileInfo& p);
/** The whole Link packet that carries it, Reticulum overhead included. */
int packetBytes(const ProfileInfo& p);
/** Audio one packet carries: frame_ms × framesPerPacket. */
int packetMs(const ProfileInfo& p);
/** Our transmit rate for the profile, bits/s, overhead included. */
uint32_t txBps(const ProfileInfo& p);
/** A call at the profile in `mode` fits a first hop of hopBps bits/s:
 *  txBps × modeDirections(mode) at most HOP_SHARE_PCT % of it. hopBps 0
 *  (unknown) fits. */
bool fitsHop(const ProfileInfo& p, uint32_t hopBps, uint8_t mode);
/** `want` when it fits hopBps in `mode`, else the profile the node runs with
 *  the highest txBps that does; 0 when none does. */
uint8_t fitProfile(uint8_t want, uint32_t hopBps, uint8_t mode);

/** A profile and mode a call runs at; profile 0 when the hop carries none. */
struct CallFit {
    uint8_t profile = 0;
    uint8_t mode = 0;
};
/** What a call in `mode` wanting profile `want` runs at over a first hop of
 *  hopBps: fitProfile(want, hopBps, mode) in that same mode. A call never
 *  changes mode for its hop. */
CallFit fitCall(uint8_t want, uint32_t hopBps, uint8_t mode);
/** Seconds of talk at the profile's rate left in an hourly airtime budget of
 *  limitMs less AIRTIME_RESERVE_MS, of which usedMs is spent, the hop's
 *  airtime estimated as txBps / hopBps. -1 with no budget (limitMs 0) or an
 *  unknown hopBps. */
int64_t talkSecondsLeft(const ProfileInfo& p, uint32_t hopBps,
                        uint32_t usedMs, uint32_t limitMs);

/** The profile `auto` picks for a link: Codec2 700C past 4 s round trip,
 *  Codec2 3200 past 1.5 s, else Opus medium — then fitCall for the first
 *  hop's hopBps in `mode`. */
CallFit autoProfile(uint32_t hopBps, int rtt_ms, uint8_t mode = MODE_FULL_DUPLEX);

/** Receive buffering for a profile: playback starts at frame_ms ×
 *  max(1, buffer − 2), never under JITTER_FLOOR_MS; the buffer holds
 *  frame_ms × (buffer + 1) plus JITTER_MARGIN_MS.
 *
 *  A batched hop (a LoRa first hop: SUPE hands a peer's frames up together
 *  when its exchange closes, up to BATCH_FRAMES at once, and a sender holds a
 *  packet up to BATCH_WAIT_MS for its exchange) starts playback at
 *  BATCH_WAIT_MS rounded up to whole frames, so a talk spurt rides out the
 *  gap between batches (the speaker's hold follows the start), and holds a
 *  whole batch on top of that. */
constexpr int JITTER_FLOOR_MS  = 60;
constexpr int JITTER_MARGIN_MS = 250;
constexpr int BATCH_WAIT_MS    = 3000;
constexpr int BATCH_FRAMES     = 12;
void receiveBuffer(const ProfileInfo& p, uint16_t& jitter_ms, uint16_t& max_jitter_ms, bool batchedHop = false);

/* ── text ── */

/** "m:ss" for a duration in seconds. */
std::string durationText(int64_t secs);

/** A typed hash: spaces dropped, lowercased, a 64-hex value cut to its
 *  leading 32. "" unless the result is 32 hex characters. */
std::string normHash(const std::string& in);

/** True when the space-separated list holds this 32-hex hash (any case). */
bool listHas(const std::string& list, const std::string& hash);

}  // namespace lxphone
