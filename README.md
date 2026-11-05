# lxst — the LXST voice session on spangap

```
consumer task (partyline)                              lxst task, ITS port 1
  ── itsConnect("lxst", 1, lxst_profile_t) ───────────▶   opens the session; refused while one is open
  ── OP_FRAMES   speaker, seq, [len][frame] … ────────▶   received audio, one speaker per message
  ── OP_TALK_END speaker ─────────────────────────────▶   play out what is buffered, then release
  ── OP_STATE    can_speak, muted, deaf ──────────────▶
  ── OP_PROFILE  lxst_profile_t ──────────────────────▶   room changed: every speaker is dropped
  ◀─ EV_BATCH    [len][frame] … ───────────────────────   put these on the Link; the consumer numbers them
  ◀─ EV_TALK_END ───────────────────────────────────────   the spurt's last batch has gone
  ◀─ EV_SPEAKING speaker, on ───────────────────────────   audible or silent; 0xFFFFFFFF is our transmitter
  ── itsDisconnect ───────────────────────────────────▶   session over
  ◀─ EV_HELD     1|0 ───────────────────────────────────   a live call holds the room (1), or gives it back (0)

phone task (lxphone)                                   lxst task, ITS port 3
  ── itsConnect("lxst", 3, lxst_profile_t) ───────────▶   a call, pending: the room keeps the session
  ── OP_TONE     none|ring|ring-back|busy ────────────▶   mixed into the owner's output, any time
  ── OP_LIVE ─────────────────────────────────────────▶   answered: the call takes the session, the room is held
  ── OP_FRAMES / OP_TALK_END / OP_STATE / OP_PROFILE ─▶   as on port 1; until live only profile and state are kept
  ◀─ EV_BATCH / EV_TALK_END / EV_SPEAKING ──────────────   as on port 1, while live
  ── itsDisconnect ───────────────────────────────────▶   call over: the tone stops, a held room resumes

browser                                                lxst task, DataChannel "lxst:2"
  ── open, protocol "pcm16" ──────────────────────────▶   endpoint := browser
  ◀─ [0x81 OWNER][1] ───────────────────────────────────   the browser has the endpoint
  ◀─ [0x82 MIC][1] ─────────────────────────────────────   send the microphone
  ── [av_hdr][pcm16 microphone block] … ──────────────▶   in place of the device microphone
  ◀─ [av_hdr][pcm16 mix block] … ───────────────────────   in place of the device speaker
  ── [0x80 PTT][1|0] ─────────────────────────────────▶   the browser's talk key
  ── close ───────────────────────────────────────────▶   endpoint := device
```

LXST (Lightweight Extensible Signal Transport) is Mark Qvist's real-time audio
layer for Reticulum. This straddle carries what every LXST-borne voice protocol
shares: the LXST frame (a codec byte, then an Opus packet or whole Codec2
sub-frames, `lxst_frame.h`), a small msgpack reader and writer for
integer-keyed maps (`lxst_msgpack.h`), and the voice session itself: capture,
encoding, batching, a jitter buffer per speaker, decoding, mixing and
transmit gating. The Opus and Codec2 coders are spangap/audio's frame codecs;
lxst adds only the LXST framing around them.

The device has one microphone and one speaker, so there is one session, and
the lxst task owns it. A protocol task holding its own Link opens it over ITS
(inter-task streaming, the platform's messaging between tasks) on port 1 with a profile, feeds it
the frames it receives tagged with a speaker id, a sequence number and an
end-of-talk mark, and gets back batches of encoded frames to put on its Link.
Partyline is that consumer, the room. The phone (see [Telephony](#telephony))
shares the session with it on port 3: a call rings over the room, and while
it is live the room is held, then resumes. One room and one call are
connected at a time; a second of either is refused. Every ITS message is an
opcode byte followed by packed little-endian fields; `include/lxst.h` is the
contract.

The profile says everything that differs between rooms: the codec and its mode
byte, the frame duration, the byte cap on one frame (codec byte included), the
batching rule, the receive buffer depths, whether frames carry sequence
numbers, and whether this member may speak. A consumer sends `OP_PROFILE` on
every room change, even to a room of the same profile, so the old room's
speakers are released.

## Talking

**Transmit mode.** `s.lxst.tx` is `ptt` (while a talk key is held), `vox`
(while the microphone is above `s.lxst.vox_db`, with a 400 ms hang) or `open`.
A profile may name its own mode instead (`tx_mode`): rooms follow the
setting, and a call transmits continuously in full duplex and only while a
talk key is held in half duplex. Nothing is transmitted while the consumer says the member may not speak, or
is muted. In `ptt` the transmitter keeps running for max(250 ms, frame
duration + 100 ms) after the key is released, so the last syllable is not
cut, then flushes the partial batch and marks end-of-talk.

**Microphone level.** While a spurt is being sent, `lxstMicLevel()` (in
`lxst.h`) gives the level of the latest microphone block, 0–100: its
root-mean-square level ×4, clipped, the scale the browser's meter uses; it
reads 0 between spurts. It is a plain atomic, never stored, so a UI polls it.
The talk bars of the Phone and Partyline LCD apps draw it as a strip along
their foot while talking, and `lxst` prints it.

**Talk keys.** The board's held talk key (on the T-Deck, its Mic key) keys the
session whenever one is open. The held Space key does too, while an app has
claimed it (the Partyline room screen does when no text field has focus). The
on-device hold button writes `lxst.cmd.ptt` (`1` held, `0` released), and so
does `lxst ptt`. The browser's talk key is the push-to-talk message (`PTT`)
on its channel. A user interface (UI) on the device that is about to act for the user (the
Phone app's Answer and Call) writes `lxst.cmd.claim` = `device` first, which
takes the endpoint for the device as a device talk key's press does, without
keying.

**Whose microphone and speaker.** The session's audio comes from one of two
endpoints: the device's own audio engine, or a browser attached on the
DataChannel `lxst:2`. The most recently active surface owns it. A browser that
opens the channel or presses its talk key takes it; a device talk key takes it
back; the channel closing returns it to the device. Only the owner's
microphone is heard and only the owner plays the mix, and a change of owner
ends the talk spurt in progress. A browser always uses its own microphone and
speaker, never the device's, and its talk key keys only its own microphone.
lxst tells the browser who owns the endpoint with `OWNER`; a browser told it
does not closes its channel and opens it again only when its user acts there.
One browser channel is in use at a time: a newer one replaces it, and the
replaced one is sent `OWNER` 0 before it is closed. A browser whose microphone
is refused sends no microphone blocks, so nothing is transmitted from it.

The browser channel carries 16 kHz mono in `audio:1`'s framing (`av_hdr_t`
then the payload); every microphone block carries its `av_hdr_t`, and a
message that is neither that nor a talk-key message is dropped. The mix goes
out in the codec the channel's protocol names (`pcm16` or `ulaw16k`);
microphone blocks may be `pcm16`, `ulaw16k` or `adpcm`, of any length. The
session's rate is `s.audio.rate`, read when the session opens; at any rate
other than 16000 Hz the browser endpoint does not open.

## Telephony

```
caller                                                callee (lxst.telephony, SINGLE IN, proves nothing)
  ── LINK ─────────────────────────────────────────────▶
  ◀─ {0x00: [0x03 AVAILABLE]} ───────────────────────────
  ── LINKIDENTIFY (caller's identity) ─────────────────▶
  ◀─ {0x00: [0x04 RINGING]} ─────────────────────────────   or [0x00 BUSY] + teardown: busy, blocked, not allowed
  ── {0x00: [0xFF + profile, 0xF0 + mode]} ────────────▶   the caller's preference; the callee adopts it
                                                            ring up to 60 s
  ◀─ {0x00: [0x05 CONNECTING]} ──────────────────────────   answered
  ── {0x00: [0x06 ESTABLISHED]} ───────────────────────▶   the caller's pipelines are open; the callee ignores it
  ◀─ {0x00: [0x06 ESTABLISHED]} ─────────────────────────   both sides talk
  ◀▶ {0x01: frame} · {0x01: [frame, …]}                     each way; the node sends ceil(60 / frame_ms) frames per packet
  ◀▶ {0x00: [0xFF + profile]} · {0x00: [0xF0 + mode]}       mid-call switch; the receiver follows
                                                            (the node answers 0x70 with [0xFF + 0x80])
  ── or ◀─ teardown ───────────────────────────────────────   hang-up (either side, any time)
  ◀─ {0x00: [0x01 REJECTED]} + teardown ─────────────────   declined while ringing
     teardown alone ─────────────────────────────────────   ring timeout, or the caller gave up
```

The node is an LXST phone: it calls and takes calls from any phone that
speaks LXST telephony (Sideband, the LXST reference telephone, a Partyline
server's dial-in). Every packet is one msgpack map sent as a plain Link packet
with no receipt: key `0x00` holds a list of signals (a bare integer is a
one-element list), key `0x01` one LXST frame (`bin`) or a list of them, and
both may be in one map. A frame is the same `[codec byte][payload]` as in a
room. A call has one speaker and no sequence numbers; frames play in arrival
order. Status codes are BUSY `0x00`, REJECTED `0x01`, AVAILABLE `0x03`,
RINGING `0x04`, CONNECTING `0x05` and ESTABLISHED `0x06`; `0xF0 + mode` is
the preferred mode (full duplex `0x01`, half duplex `0x02`) and `0xFF +
profile` the preferred profile.

**The phone.** The lxphone task hosts the `lxst.telephony` destination of the
phone's identity, a single destination that proves nothing, and announces it
with no app_data at start and every 3 hours (±5 minutes). The identity is
`s.lxst.phone.identity`: automatic (the only LXMF identity, else the first,
else the node's own), an LXMF identity by number, or the node's own. Right
after boot it waits up to a minute for lxmf to load its identities, so the
automatic choice does not settle on the node's own identity too early; it
changes only between calls. With `s.lxst.phone.enabled` off the phone is
neither announced nor hosted, and calls out still work.

**Dialling.** `call <hash>` takes 32 hex characters (spaces ignored, a 64-hex
hash cut to its leading 32): the callee's `lxst.telephony` destination, its
identity, or its LXMF delivery destination. Each resolves to the telephony
destination, from a key the node holds, a phone heard announcing that
identity, or the identity's LXMF announce or contact; while nothing is known
the node asks the network for a path every 15 s. The Phone app and window offer the phones
heard announcing `lxst.telephony` and the LXMF contacts. From dialling to
ESTABLISHED a call has 70 s; once the link is up, RINGING must come within
max(10 s, 5 s + 4 × the link's round trip).

**Answering.** An incoming call that is let in rings for 60 s. Answering sends
CONNECTING, gives the call the session and sends ESTABLISHED. Rejecting sends
REJECTED and tears the link down; a ring that times out tears down without
REJECTED. Until a call is answered the callee acts only on the caller's
profile and mode preferences.

**Who may call.** `s.lxst.phone.allow` is `all`, `contacts` (the LXMF contacts
of any identity on the node) or `none`. `s.lxst.phone.blocked` lists identity
hashes that are always refused, whatever `allow` says. Every new link hears
AVAILABLE, even while a call is up, so the caller identifies; a caller that is
blocked, not allowed, or finds a call up then hears BUSY and the link is torn
down. The exception is a call up whose Link has heard nothing at all,
keepalives included, for longer than one keepalive interval plus max(15 s,
4 × round trip), and never less than 90 s: that call is taken as dead, ends
"Connection lost", and the new caller rings as usual. A live peer is heard at
least once per keepalive interval, while the Link's own stale timer waits two
intervals and a grace, up to about 12 minutes on a slow path, and the callee
side of a Link cannot probe its caller.

**Profiles.** The profile names the codec and frame time each side transmits.
The receiver decodes whatever codec and Codec2 mode each frame carries; the
profile bounds only its buffering.

| Profile | Setting | Codec | Frame | Frame bytes | Frames per packet |
|---|---|---|---|---|---|
| `0x10` | `ulbw` | Codec2 700C | 400 ms | 42 | 1 |
| `0x20` | `vlbw` | Codec2 1600 | 320 ms | 66 | 1 |
| `0x30` | `lbw` | Codec2 3200 | 200 ms | 82 | 1 |
| `0x40` | `mq` | Opus voice medium, 8 kbit/s (default) | 60 ms | 61 | 1 |
| `0x50` | `hq` | Opus voice high, 16 kbit/s | 60 ms | 121 | 1 |
| `0x60` | `shq` | Opus voice max, 32 kbit/s, stereo on the wire | 60 ms | 241 | 1 |
| `0x70` | — | Opus voice medium | 10 ms | 11 | — |
| `0x80` | `ll` | Opus voice medium | 20 ms | 21 | 3 |

Frame bytes include the codec byte. A packet carries ceil(60 / frame_ms)
frames, at least 60 ms of audio. The node encodes mono, a valid stream in
every profile, and downmixes stereo it receives. `s.lxst.phone.profile` is
`auto` or one of the settings above (a stored `ull` reads as `ll`), and
applies to calls the node makes; a callee uses the caller's.

The node never runs a call at `0x70`: LXST sends it as one 10 ms frame per
packet, 100 packets a second, more than the node keeps up with. A call that
would be at `0x70` runs at `0x80`, and the node sends PREFERRED_PROFILE
`0x80`, which the peer follows: as callee right after ESTABLISHED when the
caller preferred `0x70`, and at once when the peer switches to `0x70`
mid-call. Until the peer follows, its `0x70` frames still play. `auto` picks once the link is up: Codec2 700C
(`0x10`) when the link's round trip passes 4 s, Codec2 3200 (`0x30`) past
1.5 s, and Opus `0x40` otherwise, then the fastest profile and mode that fit
the first hop.

**Hop capacity and duty cycle.** A call's voice never exceeds what the call
link's first hop carries (`rnsd.links.<tag>.iface` and `.bitrate`). A
profile's transmit rate is one Link packet per packet interval: the msgpack
frames plus Reticulum's 19-byte header and 48-byte token around the payload
padded to 16 bytes. Full duplex puts both ends on the air at once, so a
full-duplex call fits when twice that rate is at most 60 % of the hop's
bitrate; a half-duplex call when the rate once is. At call start and
whenever the hop changes mid-call, a full-duplex call that does not fit (the
user's fixed profile, or the caller's preference at the callee) moves to the
fastest profile that fits full duplex; with none it becomes half duplex
(push to talk) at the fastest profile one direction fits. The peer is sent
PREFERRED_PROFILE and PREFERRED_MODE for what changed in one packet: a caller
in its RINGING reply, a callee right after ESTABLISHED, either side at once
mid-call. An LXST peer told half duplex squelches its transmit until its user
keys. A call half duplex only because of its hop goes back to full duplex
(PREFERRED_MODE `0x01`) once the hop carries that; one the peer made half
duplex stays so. With nothing that fits even one way the call is refused or
ended, "Route too slow for a call", logged `failed`. Audio is held back while
the call does not fit. Over a LoRa first hop with an hourly airtime budget
(`lora.<n>.airtime_limit_ms`, the interface's `s.lora.<n>.airtime_limit`; 0 is
none), our airtime is estimated as rate ÷ hop bitrate and the budget is the
limit less a 15 s reserve: a call is not started or answered with less than
60 s of talk left in it, and a live call ends "Airtime limit reached", logged
`failed`, when it is spent. The budget's use (`lora.<n>.airtime_hour_ms`) is
republished by the interface at least every 10 s while it transmits and the
radio spends at most 1 s of airtime per second, so the reserve keeps the
hour's actual airtime within the limit.

**Receive buffering.** A call's receive buffer follows its profile: playback
starts a frame or two in and the buffer holds a few frames more. Over a LoRa
first hop frames arrive in batches (SUPE hands a peer's frames up together
when an exchange closes, up to 12 at once, and a sender holds a packet up to
3 s for its exchange), so there playback starts 3 s in (whole frames: 3.2 s
for 700C), a talk spurt rides out a gap of that long between batches, and
the buffer holds a whole batch on top. The profile is resent to the session
when the hop changes between LoRa and anything else mid-call.

| Profile | Packet | Every | Transmit |
|---|---|---|---|
| `0x10` Codec2 700C | 115 B | 400 ms | 2300 bit/s |
| `0x20` Codec2 1600 | 147 B | 320 ms | 3675 bit/s |
| `0x30` Codec2 3200 | 163 B | 200 ms | 6520 bit/s |
| `0x40` Opus medium | 147 B | 60 ms | 19600 bit/s |
| `0x50` Opus high | 195 B | 60 ms | 26000 bit/s |
| `0x60` Opus super-high | 323 B | 60 ms | 43067 bit/s |
| `0x80` Opus medium, 20 ms | 147 B | 60 ms | 19600 bit/s |

LoRa at SF7/125 kHz registers 4000 bit/s: no profile fits full duplex
(`0x10` both ways is 4600 > 2400), so a call there is half duplex at `0x10`
(2300 ≤ 2400), and a fresh 360 s hourly budget gives about 10 minutes of its
talk. Full duplex at `0x10` needs a hop of 7667 bit/s.

**Full and half duplex.** The node calls in full duplex unless the first hop
only carries half, and follows a peer's mode. Full duplex transmits whenever the microphone gives audio and the
call is not muted; half duplex only while a talk key is held. A muted or
microphone-less end sends nothing. A call ends when its Link closes or goes
stale, never on silence. Once the Link has heard nothing for 90 s the status
line reads "Connection may be lost (nothing heard for m:ss)", on the LCD and
in the browser, so the user can hang up; anything heard puts it back to
"Connected".

**Tones.** The session plays them on whichever endpoint owns it, mixed over any
room audio: the ring for an incoming call (400 Hz and 450 Hz together, 0.4 s
on, 0.2 s off, 0.4 s on, 2 s off), the ring-back while the callee rings
(382 Hz, 2 s on, 5 s off) and the busy tone (382 Hz, 0.25 s on, 0.25 s off),
which a caller plays for 4 s after BUSY or REJECTED.

**A call and a room.** A ringing call rings over a room, which keeps playing.
Answering holds the room: partyline is told (`EV_HELD`), tells its server it
is muted and deafened, and its audio is dropped. When the call ends the room
resumes with its own profile and state.

**Whose microphone and speaker.** A call uses the session's endpoint like a
room. The browser's Phone window always uses the browser's own microphone and
speaker: Answer and Call start its audio on that click (opening `lxst:2`,
which takes the endpoint) and then write the command. The Phone app on the
device's LCD (liquid-crystal display) writes `lxst.cmd.claim` = `device`
first, so the device's microphone and speaker take the call; it also rises
over whatever is on the screen when a call comes in, where Enter answers and
Backspace rejects. A browser window with no audio channel open rings locally
as well, so an incoming call is never silent there. The browser's
audio client, `browser/src/lib/lxstAudio.ts`, is this straddle's and is
shared by the Phone and Partyline windows.

**Call log.** The 30 most recent calls (direction, peer, its name, start,
duration, outcome) are kept across reboots. A caller turned away because a
call was up is logged as missed; a blocked or not-allowed caller is not
logged.

Persistent (`s.`):

| Key | Meaning |
|---|---|
| `s.lxst.phone.enabled` | 1 (default): announce `lxst.telephony` and take calls. 0: neither; calls out still work. |
| `s.lxst.phone.identity` | "" (default, automatic: the only LXMF identity, else the first, else the node's own) · an LXMF identity number `0`, `1`, … · `node` |
| `s.lxst.phone.allow` | `all` (default) · `contacts` · `none` |
| `s.lxst.phone.blocked` | Space-separated 32-hex identity hashes that always hear BUSY. |
| `s.lxst.phone.profile` | `auto` (default) · `ulbw` · `vlbw` · `lbw` · `mq` · `hq` · `shq` · `ll` (a stored `ull` reads as `ll`) |
| `s.lxst.phone.log.<n>.{dir,peer,name,at,dur,result}` | The call log, newest = highest `n`. `dir` `in` · `out`; `peer` the identity hash; `at` Unix seconds (0 while the clock was unset); `dur` "m:ss" talked, "" if never connected; `result` `answered` · `missed` · `declined` (we rejected) · `rejected` (they did) · `busy` · `no answer` · `failed` · `cancelled`. |

RAM:

| Key | Meaning |
|---|---|
| `lxst.phone.state` | `off` (disabled or no identity) · `idle` · `dialling` · `ringback` (the callee rings) · `ringing` (an incoming call) · `connecting` · `live` · `ended` (3 s after a call, then `idle`) |
| `lxst.phone.{dir,peer,peer_dest,peer_name}` | The current or last call: `in` · `out`, the remote identity hash, its `lxst.telephony` destination, its display name. |
| `lxst.phone.status` | One finished line for the Phone app and window ("Calling Alice…", "Incoming call", "Connected", "Busy" …); while `ended`, why the call ended, such as "Route too slow for a call", "Airtime limit reached" or "Connection lost" (the link went stale, or was silent past its bound when a new call came in). While live, "Connection may be lost (nothing heard for m:ss)" after 90 s with nothing heard on the link. Both call screens show it. |
| `lxst.phone.timer` | "m:ss" since the call went live, each second; "" otherwise. |
| `lxst.phone.muted` | 1 while our microphone is muted for the call. |
| `lxst.phone.profile`, `lxst.phone.mode` | The call's profile ("Opus medium, 60 ms") and `full duplex` · `half duplex`; "" with no call. |
| `lxst.phone.dest`, `lxst.phone.ident` | Our `lxst.telephony` destination and the phone's identity hash. |
| `lxst.phone.peers.<dest>.{ident,name,hops,heard}` | Phones heard announcing, the newest 32; `heard` Unix seconds. |

Commands:

| Key | Value |
|---|---|
| `lxst.phone.cmd` | `call <hash>` · `answer` · `reject` · `hangup` · `mute 1` · `mute 0` · `announce` |
| `lxst.phone.cmd.error` | Written by the phone: why a command was refused; cleared by the next accepted one. |

## Storage

Persistent (`s.`):

| Key | Meaning |
|---|---|
| `s.lxst.tx` | `ptt` (default) · `vox` · `open` |
| `s.lxst.vox_db` | Voice gate for `vox`, in dB relative to full scale (dBFS; −80 … 0, default −45). |
| `s.lxst.jitter_ms` | Audio held back before a speaker plays, in rooms whose profile names no depth of its own (20 … 3000, default 200; cut to what the speaker's buffer holds). Push-to-talk rooms set their own. |

RAM:

| Key | Meaning |
|---|---|
| `lxst.state` | `idle` · `live` (a room has the session) · `call` (a live call has it) |
| `lxst.held` | 1 while a live call holds a connected room. |
| `lxst.profile` | The session's profile as text ("Codec2 3200, 200 ms"); empty when idle. |
| `lxst.tx_mode` | `ptt` · `vox` · `open` — how the session keys transmit: the active profile's own `tx_mode` (a call's `open` or `ptt`), else `s.lxst.tx`; empty when idle. |
| `lxst.talking` | 1 while this device is transmitting. |
| `lxst.endpoint` | `device` · `browser` — who owns the microphone and speaker. |
| `lxst.browser` | 1 while a browser channel is connected. |
| `lxst.stats.{speakers,frames_tx,tx_dropped,frames_rx,concealed,late,dropped,underruns,blocks_dropped,ticks_skipped}` | Session counters, reset when a session opens. `tx_dropped` counts frames the consumer's port refused; `dropped` frames that found no speaker slot, were malformed or were pushed out of a full buffer; `blocks_dropped` mixed blocks the open endpoint refused. |
| `lxst.stats.{enc_us_avg,enc_us_max,dec_us_avg,dec_us_max}` | Codec time per frame, in µs. |
| `lxst.stats.{heap_int,heap_psram}` | Free internal and PSRAM (pseudo-static RAM, the external memory) heap. |
| `lxst.stats.last_tx`, `lxst.stats.last_rx` | One line on the last spurt sent and received: duration, frames, bytes or concealment, level and dominant frequency. |

Commands (written by others, read by lxst):

| Key | Value |
|---|---|
| `lxst.cmd.ptt` | `1` held, `0` released: the on-device talk button. |
| `lxst.cmd.claim` | `device`: give the endpoint to the device without keying; lxst resets it to "". |
| `lxst.test_tone` | A frequency in Hz transmits a sine tone in place of the microphone; 0 stops it. |

## CLI

```
lxst                     session (and whether a room is held), endpoint, profile, transmit mode, microphone level and stats
lxst tone <hz>|off       transmit a test tone instead of the microphone
lxst ptt 1|0             hold / release the device talk key

phone                    state, the call, our lxst.telephony destination and identity
phone call <hash>        call a phone, an identity or an LXMF address
phone answer | reject | hangup
phone mute 1|0           mute our microphone for the call
phone log [n]            the last n calls (10 unless given)
phone peers              phones heard announcing
phone announce           announce the phone now
```

`phone` writes `lxst.phone.cmd` and prints `sent`; a refusal lands on
`lxst.phone.cmd.error`.

## Placement

The lxst task runs on the secondary core (core 1) at priority 2, below the
LCD render task that shares the core, on a 40 KB PSRAM stack sized for an
Opus encode. Every buffer lxst allocates comes from PSRAM only, with no
fallback to internal RAM: jitter buffers and every audio and message buffer;
the speaker table and the frame being encoded are static, also in PSRAM.
Codec state is spangap/audio's, which prefers PSRAM. One speaker's jitter
buffer takes at most 128 KB.

The lxphone task runs on core 1 at priority 1 on an 8 KB PSRAM stack, with
its buffers in PSRAM. It encodes and decodes nothing; it moves packets
between its Link and the session. It runs while Reticulum runs.

## Testing

`esp-idf/test/host/` holds host tests for everything that does not touch the
platform: the msgpack codec against encodings made by RNS's vendored umsgpack
(`gen_vectors.py`), the frame rules against partyline 1.0.5's own
(`gen_frame_vectors.py`), the batcher, the jitter buffer, the signal helpers
and profile rules, the endpoint ownership, the browser channel's message
classification, who has the session (room or call), the call tones, the call
state machine on both sides, and the telephony wire and profiles against
packets packed by RNS's umsgpack and LXST 0.5.4's profile table
(`gen_phone_vectors.py`):

```sh
cd esp-idf/test/host
g++ -std=c++17 -Wall -Wextra -I../../include test_msgpack.cpp ../../src/lxst_msgpack.cpp -o /tmp/test_msgpack && /tmp/test_msgpack
g++ -std=c++17 -Wall -Wextra -I../../include test_frame.cpp ../../src/lxst_frame.cpp -o /tmp/test_frame && /tmp/test_frame
g++ -std=c++17 -Wall -I../../src -I../../include -Istub test_session_batch.cpp ../../src/lxst_batch.cpp -o /tmp/test_session_batch && /tmp/test_session_batch
g++ -std=c++17 -Wall -I../../src -I../../include -Istub test_session_jitter.cpp ../../src/lxst_jitter.cpp -o /tmp/test_session_jitter && /tmp/test_session_jitter
g++ -std=c++17 -Wall -I../../src -I../../include -Istub test_session_dsp.cpp ../../src/lxst_sess_profile.cpp ../../src/lxst_dsp.cpp ../../src/lxst_frame.cpp -o /tmp/test_session_dsp && /tmp/test_session_dsp
g++ -std=c++17 -Wall -I../../src test_owner.cpp -o /tmp/test_owner && /tmp/test_owner
g++ -std=c++17 -Wall -I../../src -I../../include -Istub -I../../../../audio/esp-idf test_dc.cpp -o /tmp/test_dc && /tmp/test_dc
g++ -std=c++17 -Wall -I../../src test_arb.cpp -o /tmp/test_arb && /tmp/test_arb
g++ -std=c++17 -Wall -I../../src -I../../include -Istub test_tones.cpp ../../src/lxst_tones.cpp ../../src/lxst_dsp.cpp -o /tmp/test_tones && /tmp/test_tones
g++ -std=c++17 -Wall -Wextra -I../../src test_phone_call.cpp ../../src/lxst_phone_call.cpp ../../src/lxst_phone_wire.cpp -o /tmp/test_phone_call && /tmp/test_phone_call
g++ -std=c++17 -Wall -Wextra -I../../src test_phone_wire.cpp ../../src/lxst_phone_wire.cpp -o /tmp/test_phone_wire && /tmp/test_phone_wire
```

`test_dc` takes `av_hdr.h` from a spangap/audio checkout beside this one. The
vector generators run under a Python with partyline 1.0.5 installed (which
brings RNS and LXST).

On the device, `lxst tone 440` with a talk key held sends a tone into whatever
room the consumer is in, and `lxst.stats.last_rx` gives the frequency a
received spurt was heard at. For calls, `phone call <hash>` dials a
Sideband or `rnphone` phone (or another node's `phone` address), `phone log`
shows how each call ended, and the `lxst` status line says whether a room is
held.

See [INTERNALS.md](INTERNALS.md) for how the session works inside.
