/**
 * lxst_frame.h — the LXST frame: one codec header byte, then the codec's own
 * payload. Host-portable (no ESP-IDF includes), so host tests compile it.
 *
 *   Opus frame    [0x01][one raw Opus packet]
 *   Codec2 frame  [0x02][mode byte][whole Codec2 sub-frames …]
 */
#pragma once

#include <cstdint>
#include <cstddef>

/* Codec header bytes. */
constexpr uint8_t LXST_CODEC_RAW    = 0x00;
constexpr uint8_t LXST_CODEC_OPUS   = 0x01;
constexpr uint8_t LXST_CODEC_CODEC2 = 0x02;

/* Codec2 mode bytes (the byte after LXST_CODEC_CODEC2). 3200 and 2400 carry
 * 20 ms sub-frames; the others carry 40 ms ones. */
constexpr uint8_t LXST_C2_700C = 0x00;
constexpr uint8_t LXST_C2_1200 = 0x01;
constexpr uint8_t LXST_C2_1300 = 0x02;
constexpr uint8_t LXST_C2_1400 = 0x03;
constexpr uint8_t LXST_C2_1600 = 0x04;
constexpr uint8_t LXST_C2_2400 = 0x05;
constexpr uint8_t LXST_C2_3200 = 0x06;

/* LXST Opus profiles (encoder presets). */
constexpr uint8_t LXST_OPUS_VOICE_LOW    = 0x00;
constexpr uint8_t LXST_OPUS_VOICE_MEDIUM = 0x01;
constexpr uint8_t LXST_OPUS_VOICE_HIGH   = 0x02;
constexpr uint8_t LXST_OPUS_VOICE_MAX    = 0x03;
constexpr uint8_t LXST_OPUS_AUDIO_MIN    = 0x04;
constexpr uint8_t LXST_OPUS_AUDIO_LOW    = 0x05;
constexpr uint8_t LXST_OPUS_AUDIO_MEDIUM = 0x06;
constexpr uint8_t LXST_OPUS_AUDIO_HIGH   = 0x07;
constexpr uint8_t LXST_OPUS_AUDIO_MAX    = 0x08;

/* ── Codec2 ── */

/** Length of one Codec2 sub-frame in ms for a mode byte (20 for 3200 and 2400,
 *  40 for the others), or 0 for an unknown mode byte. */
int lxstC2SubframeMs(uint8_t mode);

/** Bytes in one Codec2 sub-frame for a mode byte (3200:8, 2400:6, 1600:8,
 *  1400:7, 1300:7, 1200:6, 700C:4), or 0 for an unknown mode byte. */
int lxstC2SubframeBytes(uint8_t mode);

/* ── Opus ──
 * Durations are in microseconds, because a CELT frame can be 2.5 ms. */

/** Duration of one raw Opus packet read from its TOC (table of contents)
 *  byte: the frame size of its configuration times its frame count (1 for
 *  code 0, 2 for codes 1 and 2, byte 1 & 0x3F for code 3). -1 when the packet
 *  is empty, or is code 3 without a count byte. */
int32_t lxstOpusDurationUs(const uint8_t* packet, size_t len);

/** The bit-rate ceiling LXST gives an Opus profile, in bit/s, or 0 for an
 *  unknown profile. */
uint32_t lxstOpusBitrateCeiling(uint8_t profile);

/** ceil(ceiling / 8 × frame_ms / 1000): the most bytes one Opus packet of
 *  frame_ms may take at that ceiling. */
uint32_t lxstOpusMaxBytes(uint32_t bitrate_ceiling, uint32_t frame_ms);

/* ── frames ── */

/** Duration of an LXST frame (codec byte first) in microseconds, or -1 when it
 *  is not a well-formed Opus or Codec2 frame. A Codec2 frame needs a known mode
 *  byte and a whole number of sub-frames (at least one byte of them). RAW and
 *  unknown codec bytes have no duration. */
int32_t lxstFrameDurationUs(const uint8_t* frame, size_t len);

/** True when a frame fits a profile: its codec byte is `codec`, its length is
 *  2 … max_bytes (codec byte included), and its duration is exactly frame_ms. */
bool lxstFrameValid(const uint8_t* frame, size_t len, uint8_t codec, uint32_t frame_ms, size_t max_bytes);
