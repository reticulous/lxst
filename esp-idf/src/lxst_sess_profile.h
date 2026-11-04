/**
 * lxst_sess_profile.h — what the voice session derives from an lxst_profile_t
 * beyond the frame helpers of lxst_frame.h: the Opus encoder preset of an LXST
 * Opus profile, the release tail, the receive hold, profile validity and the
 * profile's display name. Host-portable.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include "lxst_frame.h"

struct lxst_profile_t;

namespace lxsess {

/** The largest frame the session handles, codec byte included (partyline's
 *  own cap is 400). */
constexpr int MAX_FRAME_BYTES = 512;

/** Receive buffer limits: the most a profile's jitter_ms and max_jitter_ms
 *  may say, the depth when max_jitter_ms is 0, and the most frame memory one
 *  speaker's buffer takes. */
constexpr int    MAX_JITTER_MS = 30000;
constexpr int    DEFAULT_MAX_JITTER_MS = 3000;
constexpr size_t SPEAKER_JITTER_BYTES = 128 * 1024;

/** The longest frame the profile's codec makes, codec byte included: a Codec2
 *  frame's fixed length, else the profile's cap; MAX_FRAME_BYTES with rx_any.
 *  For a valid profile. */
int frameBytesMax(const lxst_profile_t& p);

/** One speaker's receive buffer: slots of frameBytesMax(p) bytes; maxMs from
 *  max_jitter_ms (DEFAULT_MAX_JITTER_MS when 0) and startMs, both cut so the
 *  slots fit SPEAKER_JITTER_BYTES. */
struct JitterSize { int frameBytes, startMs, maxMs; };
JitterSize jitterSize(const lxst_profile_t& p, int startMs);

/** Encoder settings for an LXST Opus profile byte. */
struct OpusPreset {
    int  bitrate;     /* bits per second: the profile's ceiling          */
    bool voip;        /* VOICE_* profiles: voip application, else audio  */
    bool narrow;      /* VOICE_LOW and AUDIO_MIN: narrowband             */
};
bool opusPreset(uint8_t mode, OpusPreset* out);

/** Keep transmitting this long after the talk key is released. */
inline int tailMs(int frameMs) { return frameMs + 100 > 250 ? frameMs + 100 : 250; }

/** A drained speaker that has heard nothing for this long is released. */
inline int holdMs(int jitterMs) { return jitterMs > 1000 ? jitterMs : 1000; }

/** The session's sample rate and playout block from the engine settings
 *  s.audio.rate and s.audio.block_ms, clamped as the engine clamps them
 *  (8000..48000 Hz, 10..40 ms) and so that one block fits maxBlock samples. */
inline void sessionFormat(int rate, int blockMs, int maxBlock, int* outRate, int* outBlockMs)
{
    if (rate < 8000) rate = 8000;
    if (rate > 48000) rate = 48000;
    if (blockMs < 10) blockMs = 10;
    if (blockMs > 40) blockMs = 40;
    if ((long long)rate * blockMs / 1000 > maxBlock) blockMs = maxBlock * 1000 / rate;
    *outRate = rate;
    *outBlockMs = blockMs;
}

/** The codec and mode bytes are known, the frame duration is one the codec
 *  makes (whole sub-frames for Codec2), the byte cap leaves room for a frame and
 *  stays within MAX_FRAME_BYTES, the batch frame cap is 1..LXST_MAX_BATCH,
 *  max_jitter_ms is at most MAX_JITTER_MS and jitter_ms at most the depth. */
bool profileValid(const lxst_profile_t& p);

/** A received frame the session will decode. rx_any: any well-formed LXST
 *  frame of 2 … MAX_FRAME_BYTES (Opus of at most 120 ms; Codec2 a known mode
 *  and whole sub-frames). Otherwise the profile's frame rules, and a Codec2
 *  frame in the profile's mode. */
bool rxFrameOk(const lxst_profile_t& p, const uint8_t* f, size_t len);

/** Transmit keying. */
enum TxSel : uint8_t { TXSEL_PTT, TXSEL_VOX, TXSEL_OPEN };
/** A profile's tx_mode over the s.lxst.tx setting: LXST_TX_OPEN and
 *  LXST_TX_PTT override it, LXST_TX_SETTING (and anything unknown) follows it. */
TxSel txSelect(uint8_t txMode, TxSel setting);

/** "Codec2 3200, 200 ms" / "Opus voice 16 kbps, 20 ms". */
void profileName(const lxst_profile_t& p, char* out, size_t n);

} // namespace lxsess
