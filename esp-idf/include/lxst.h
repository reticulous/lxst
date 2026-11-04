/**
 * lxst — the LXST voice session: capture, encode, batch, per-speaker jitter,
 * decode, mix, transmit gating and the talk keys, for one room and one call.
 *
 * The device has one microphone and one speaker, so there is one session. A
 * consumer (a protocol task holding its own Link) opens it over ITS with a
 * profile, then:
 *
 *   consumer ── itsConnect("lxst", LXST_PORT_SESSION, lxst_profile_t) ──▶ lxst
 *   consumer ── LXST_OP_FRAMES  speaker, seq, frames …              ──▶   received audio
 *   consumer ── LXST_OP_TALK_END speaker                             ──▶   that speaker stopped
 *   consumer ── LXST_OP_STATE   can_speak, muted, deaf               ──▶
 *   consumer ── LXST_OP_PROFILE lxst_profile_t                       ──▶   room changed
 *   consumer ◀── LXST_EV_BATCH   frames …                            ──    put these on the Link
 *   consumer ◀── LXST_EV_TALK_END                                    ──    after the last batch
 *   consumer ◀── LXST_EV_SPEAKING speaker, on                        ──    audible / silent
 *   consumer ── itsDisconnect                                        ──▶   session over
 *
 * A consumer sends LXST_OP_PROFILE on every room change, even to a room of the
 * same profile, so the old room's speakers are released.
 *
 * Every ITS message on the session handle is one message: an opcode byte, then
 * the fields below, little-endian, packed. Frames are LXST frames
 * (lxst_frame.h), codec byte first.
 *
 * Transmit gating (push-to-talk, voice-activated, open) is the session's, set
 * by s.lxst.tx, so every consumer keys the same way.
 *
 * Endpoints. The session's microphone and speaker are one of two endpoints:
 * the device's own audio engine, or a browser attached over the DataChannel
 * "lxst:2" (LXST_PORT_BROWSER):
 *
 *   browser ── DC "lxst:2" open, protocol "pcm16" ─────────────▶ lxst   endpoint := browser
 *   browser ◀── [LXST_DC_MIC][1] ─────────────────────────────── lxst   send the mic
 *   browser ── [av_hdr][pcm16 block] …  (while asked) ─────────▶ lxst   the microphone
 *   browser ◀── [av_hdr][pcm16 mix block] … ──────────────────── lxst   the speaker
 *   browser ── [LXST_DC_PTT][down] ────────────────────────────▶ lxst   talk key
 *   browser ◀── [LXST_DC_OWNER][0|1] ─────────────────────────── lxst   who has the endpoint
 *   browser ── DC close ───────────────────────────────────────▶ lxst   endpoint := device
 *
 * The most recently active surface owns the endpoint: a browser that opens
 * the channel or presses its talk key takes it, and a device talk key (the
 * board's held keys, the on-device hold button's lxst.cmd.ptt) takes it back.
 * Only the owner's microphone is heard and only the owner plays the mix; a
 * change of owner ends the talk spurt in progress. The browser's talk key
 * keys the browser's microphone only, never the device's. A browser told
 * LXST_DC_OWNER 0 closes its channel, and opens it again (taking the endpoint)
 * only when its user acts there, so a reconnect never takes the endpoint back.
 *
 * The talk keys are the board's held keys (lcd_keys.h) when spangap-lcd is
 * staged, the command key lxst.cmd.ptt ("1" held, "0" released) that the
 * on-device hold button writes, and the browser's LXST_DC_PTT. A UI on the
 * device writes lxst.cmd.claim = "device" to take the endpoint for the device
 * as a device talk key's down edge does, without keying; lxst resets it to "".
 *
 * A call (LXST_PORT_CALL) shares the session with the room consumer; see
 * there. lxst.state is idle · live (a room has the session) · call (a call is
 * live); lxst.held is 1 while a room is held.
 */
#pragma once

#include <cstdint>
#include "lxst_frame.h"
#include "service.h"

/** The lxst task's ITS name. */
#define LXST_TASK "lxst"

/** The session port (ITS_PACKET, one connection at a time). The connect
 *  payload is an lxst_profile_t. */
constexpr uint16_t LXST_PORT_SESSION = 1;

/** The browser endpoint port: the DataChannel "lxst:2" (ITS_PACKET, one
 *  browser at a time; a newer channel replaces the older). The connect
 *  payload (the channel's protocol) names the codec of the blocks lxst sends,
 *  as on audio:1 ("pcm16" or "ulaw16k"; anything else = pcm16). Audio blocks
 *  both ways are [av_hdr_t][payload], mono, at LXST_BROWSER_RATE — audio:1's
 *  framing. lxst sends one block of the session's block (s.audio.block_ms,
 *  20 ms unless changed) at a time; microphone blocks of any length are
 *  accepted and re-chunked, in pcm16, ulaw16k or adpcm. Every microphone
 *  block carries its av_hdr_t; a message that is neither that nor a two-byte
 *  LXST_DC_PTT is dropped. The session runs at s.audio.rate; when that is not
 *  LXST_BROWSER_RATE (no resampling happens) the browser endpoint does not
 *  open, and nothing is played to or transmitted from the browser. On every
 *  connect lxst sends LXST_DC_OWNER and then LXST_DC_MIC; the channel is
 *  accepted whether or not a session is live. Control messages start with one
 *  of the LXST_DC_* values below (av_hdr's type byte is 0 or 1, so the two
 *  never collide). */
constexpr uint16_t LXST_PORT_BROWSER = 2;

/** The one sample rate the browser channel carries. */
constexpr int LXST_BROWSER_RATE = 16000;

enum : uint8_t {
    LXST_DC_PTT   = 0x80,   /* browser → lxst [op][down u8]: the browser's talk key     */
    LXST_DC_OWNER = 0x81,   /* lxst → browser [op][owner u8]: 1 = the browser has the
                             * endpoint, 0 = the device has it                          */
    LXST_DC_MIC   = 0x82,   /* lxst → browser [op][want u8]: 1 = send microphone blocks,
                             * 0 = stop                                                 */
};

/** The most frames one message carries, either direction. */
constexpr int LXST_MAX_BATCH = 64;

struct __attribute__((packed)) lxst_profile_t {
    uint8_t  codec;            /* LXST_CODEC_OPUS or LXST_CODEC_CODEC2              */
    uint8_t  mode;             /* Codec2: LXST_C2_*; Opus: LXST_OPUS_* profile       */
    uint16_t frame_ms;         /* duration of one frame on the wire                  */
    uint16_t max_frame_bytes;  /* cap on one frame, codec byte included              */
    /* Transmit batching. A batch is flushed when adding the next frame would take
     * batch_base + Σ(len + batch_per_frame) past batch_budget, or when it holds
     * batch_max_frames frames, and at the end of a talk spurt. batch_max_frames
     * 1 sends every frame on its own. */
    uint16_t batch_budget;
    uint8_t  batch_base;
    uint8_t  batch_per_frame;
    uint8_t  batch_max_frames; /* 1 … LXST_MAX_BATCH                                 */
    /* Receive. A speaker starts playing once jitter_ms of audio is buffered; the
     * buffer holds at most max_jitter_ms. */
    uint16_t jitter_ms;
    uint16_t max_jitter_ms;
    uint8_t  sequenced;        /* 1: frames carry sequence numbers; 0: arrival order */
    uint8_t  can_speak;
    uint8_t  tx_mode;          /* LXST_TX_*                                          */
    /* 1: decode whatever codec and Codec2 mode each frame carries (a speaker's
     * decoder is rebuilt when it changes; Opus stereo is downmixed), accept any
     * well-formed LXST frame up to 512 bytes, arrival order. 0: frames must fit
     * the profile. */
    uint8_t  rx_any;
};

/* lxst_profile_t.tx_mode */
enum : uint8_t {
    LXST_TX_SETTING = 0,       /* follow s.lxst.tx (rooms)                           */
    LXST_TX_OPEN    = 1,       /* transmit continuously (full duplex)                */
    LXST_TX_PTT     = 2,       /* only while a talk key is held (half duplex)        */
};

/** The call port (ITS_PACKET, one connection at a time; a second is refused).
 *  The connect payload is the call's lxst_profile_t. The call starts pending:
 *  OP_PROFILE and OP_STATE only store, OP_FRAMES are dropped and nothing is
 *  transmitted, while a room on LXST_PORT_SESSION keeps the session. OP_TONE
 *  works at any time. OP_LIVE gives the call the session: a room consumer is
 *  held (LXST_EV_HELD 1; its spurt dropped, its speakers released, its frames
 *  dropped, nothing sent for it) until the call disconnects, when the room's
 *  own last profile and state are re-applied and LXST_EV_HELD 0 is sent. A
 *  room that connects while a call is live is held at once. While live, the
 *  call port carries everything port 1 does. Disconnecting ends the tone. */
constexpr uint16_t LXST_PORT_CALL = 3;

/* LXST_OP_TONE tones, mixed into the endpoint owner's output (over any room
 * audio) on the playout clock; the session opens the owner's playback for a
 * tone when nothing else has it open. */
enum : uint8_t {
    LXST_TONE_NONE     = 0,
    LXST_TONE_RING     = 1,    /* incoming call: 400+450 Hz, 0.4 on 0.2 off 0.4 on 2 off */
    LXST_TONE_RINGBACK = 2,    /* the callee is ringing: 382 Hz, 2 s on, 5 s off     */
    LXST_TONE_BUSY     = 3,    /* 382 Hz, 0.25 s on, 0.25 s off                      */
};

/* Consumer → lxst. */
enum : uint8_t {
    /* [op][lxst_profile_t] — reconfigure; drops the partial batch and every
     * speaker's buffer. */
    LXST_OP_PROFILE  = 0x01,
    /* [op][speaker u32][seq u16][count u8] then count × [len u16][frame] —
     * frames from one speaker; frame i is number seq + i (mod 2^16). seq is
     * ignored when the profile is not sequenced. */
    LXST_OP_FRAMES   = 0x02,
    /* [op][speaker u32] — the speaker stopped; play out what is buffered, then
     * release it. */
    LXST_OP_TALK_END = 0x03,
    /* [op][can_speak u8][muted u8][deaf u8] — muted never transmits; deaf
     * plays nothing. */
    LXST_OP_STATE    = 0x04,
    /* Call port only. [op][tone u8] — LXST_TONE_*; NONE stops it. */
    LXST_OP_TONE     = 0x05,
    /* Call port only. [op] — the call is answered: it takes the session. */
    LXST_OP_LIVE     = 0x06,
};

/* lxst → consumer. */
enum : uint8_t {
    /* [op][count u8] then count × [len u16][frame] — one batch to send. The
     * consumer numbers the frames. */
    LXST_EV_BATCH    = 0x81,
    /* [op] — the talk spurt is over; its last batch has been sent. */
    LXST_EV_TALK_END = 0x82,
    /* [op][speaker u32][on u8] — the speaker became audible (1) or went quiet
     * (0). A speaker of 0xFFFFFFFF is this device's own transmitter. */
    LXST_EV_SPEAKING = 0x83,
    /* Session port only. [op][held u8] — 1: a live call holds the room (mute
     * and deafen at the server); 0: the room has the session again. */
    LXST_EV_HELD     = 0x84,
};

/** The speaker id LXST_EV_SPEAKING uses for our own transmitter. */
constexpr uint32_t LXST_SPEAKER_SELF = 0xFFFFFFFFu;

/** Bring-up service: seeds s.lxst.* defaults, registers the CLI, spawns the
 *  lxst task. */
class LxstService : public Service {
public:
    void onInit() override;
};
