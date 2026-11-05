# lxst — internals

The [README](README.md) covers what the session does and how it is driven.
This file covers how it works inside and what must stay true when it changes.
`include/lxst.h` is the ITS (inter-task streaming) contract; `src/lxst_session.cpp` is the task.
The phone (§9) has its own task; `include/lxst_phone.h` is its storage
contract.

## 1. The task

Everything runs on the lxst task. It is driven by ITS callbacks, storage
subscriptions and two `esp_timer`s whose callbacks only bump a counter or set
a flag and notify the task:

- the **playout clock**, periodic at the session's block (`s.audio.block_ms`),
  running while any speaker exists, a call tone sounds, or the test tone is
  feeding the transmitter;
- the **release tail**, one-shot after the talk key is released.

The browser endpoint adds a third, a 50 ms one-shot that retries what its
channel refused (§5).

The task loop is one `itsPoll`, then `service()`: open or close the session if
a callback asked for it, retry an endpoint that did not open and a held-key
subscription that failed (every 5 s), end a spurt whose tail fired, run the
playout ticks that accumulated (at most four at once; the rest count as
`ticks_skipped`), let the endpoint bring back a dropped connection, send the
browser's pending control messages, and publish stats at most once a second
while something is happening. The poll's timeout is the earliest of those
deadlines; with nothing pending it waits forever. Nothing in the task waits on
anything else, apart from `itsConnect` to the audio engine (at most 500 ms,
never from the playout tick; §5), which is the platform's connect primitive.

Every receive callback drains its port completely. A callback is one
notification for however many packets are queued, so reading one per call
leaves the rest until the next packet arrives and overflows the port under
load.

## 2. Session lifecycle

The session has two consumers, the room on port 1 and a call on port 3, and
the one that has it is the *active* consumer (§7). `onSessConnect` checks the
profile and refuses a second room while one is connected; it only records the
request. The task then opens the session for whichever consumer is active:

1. The format is resolved once, from `s.audio.rate` and `s.audio.block_ms`,
   clamped as the engine clamps them (8000 … 48000 Hz, 10 … 40 ms) and so that
   one block fits 1920 samples, and holds for the whole session. Everything
   below runs at that rate; the codecs resample internally where they need to
   (Codec2's 8 kHz).
2. The owner's endpoint opens (§6).
3. The held keys are subscribed and the talk key is claimed; every device
   talk source starts released and `lxst.cmd.ptt` is reset to `0`.
4. The profile is applied: the batcher is sized, the encoder is built,
   `lxst.profile` is published, and the transmit state is evaluated.

`OP_PROFILE` applies a profile the same way mid-session: the spurt in progress
is dropped, every speaker is released with an `EV_SPEAKING 0`, and the encoder
is rebuilt. When the active consumer disconnects and no other takes over, the
spurt and the speakers are dropped without events, the endpoint closes
(unless a call tone still sounds), the encoder is freed and the talk-key
claim is released.

A profile is valid when its codec and mode bytes are known, its frame
duration is one the codec makes (whole Codec2 sub-frames; 10 … 120 ms in the
Opus steps), its byte cap holds a frame and is at most 512, its batch frame
cap is 1 … 64, its `max_jitter_ms` is at most 30000, and its `jitter_ms` is
within the buffer (`max_jitter_ms`, or 3000 when that is 0).

## 3. Transmit

**Gating.** The transmitter may run when the session is live, has an encoder,
the member may speak and is not muted. It is keyed by the owner's talk key in
`ptt`, by the voice gate in `vox`, and always in `open`. The mode is the
active profile's `tx_mode` when it names one (`LXST_TX_OPEN`, `LXST_TX_PTT`:
a call's full and half duplex), else `s.lxst.tx` (`LXST_TX_SETTING`, what
rooms send); `txSelect` in `lxst_sess_profile.h` is the rule, and
`lxst.tx_mode` publishes the result. The state machine:

```
IDLE ── gate ok and keyed ───────────────▶ ON          (EV_SPEAKING self 1)
ON   ── key up, ptt ─────────────────────▶ TAIL        (tail timer started)
ON   ── key up (vox, open) or gate lost ─▶ IDLE        (flush, EV_TALK_END)
TAIL ── keyed again ─────────────────────▶ ON
TAIL ── tail fired or gate lost ─────────▶ IDLE        (flush, EV_TALK_END)
```

The tail is max(250 ms, frame_ms + 100 ms). The voice gate compares each
microphone block's root-mean-square level with `s.lxst.vox_db` and stays open 400 ms past
the last block above it. The same per-block level, computed once, sets
`lxstMicLevel()` while the state is not IDLE; `spurtEnd` zeroes it. A change of the effective mode or of endpoint owner
ends the spurt in progress.

**The microphone.** The session wants the microphone when it may transmit at
all and either the mode is not `ptt` or a spurt is running. In `ptt` the
device's capture connection is opened at key-down and closed at the end of
the tail, so the microphone, and the engine's power lock with it, is held
only while transmitting. There is no pre-roll: nothing was captured before the
key went down, so there is nothing to replay. In `vox` and `open` the
microphone stays open for the session, since the gate needs it.

**Encoding.** Microphone PCM (16-bit linear samples) fills a unit buffer; a unit is one codec call, a
Codec2 sub-frame or a whole Opus frame. A Codec2 frame is built as
`[0x02][mode][sub-frame …]` until it holds frame_ms of sub-frames. A sub-frame
that encodes short discards the partial frame, because a frame with a short
sub-frame is invalid on the wire. An Opus frame is `[0x01]` and one Opus packet
encoded with its maximum size set to the profile's cap less the codec byte,
so it always fits. Opus runs at complexity 2, at the profile's bit-rate
ceiling, mono, with Opus's voice application for the `VOICE_*` profiles and
its audio application otherwise, and narrowband for `VOICE_LOW` and `AUDIO_MIN`.

Codec2 modes, as LXST frames them:

| Mode | Byte | Sub-frame | Bytes per sub-frame |
|---|---|---|---|
| 700C | 0x00 | 40 ms | 4 |
| 1200 | 0x01 | 40 ms | 6 |
| 1300 | 0x02 | 40 ms | 7 |
| 1400 | 0x03 | 40 ms | 7 |
| 1600 | 0x04 | 40 ms | 8 |
| 2400 | 0x05 | 20 ms | 6 |
| 3200 | 0x06 | 20 ms | 8 |

When the encoder is built for a Codec2 profile, the codec's own frame size is
checked against this table and a mismatch leaves the session without an
encoder.

**Batching.** Encoded frames go straight into an `EV_BATCH` message. Before a
frame joins a non-empty batch, the batch is sent if `base + Σ(len + per_frame)`
would pass `budget` with that frame in it (a budget of 0 is no byte limit); a
frame always fits an empty batch. After it joins, the batch is sent once it
holds `max_frames` frames. The end of a spurt sends the partial batch, then
`EV_TALK_END`. The rule counts bytes and frames, never time, so the session
needs no batching timer. Frames are not numbered here; the consumer numbers
them as it sends. A batch the consumer's port refuses is counted in
`tx_dropped` and lost.

**Test tone.** `lxst.test_tone` replaces the microphone with a sine peaking
about 12 dB below full scale, generated one block per playout tick; the clock runs while the
tone is feeding a transmitter that may run.

## 4. Receive

**Speakers.** Up to eight speakers at once, each with its own jitter buffer,
decoder and one decoded unit of PCM, all allocated when its first frame
arrives. A frame from a ninth is dropped. A deaf session drops incoming frames
and releases every speaker when it becomes deaf.

**The jitter buffer** (`lxst_jitter.h`) holds encoded frames, one slot per
frame time, and decodes a frame only when it is due to play. An encoded frame
is tens of bytes where its decoded PCM is kilobytes, and a push-to-talk room
buffers seconds per speaker, which is why it holds frames.

- *Size.* A slot is as large as the longest frame the codec makes: a Codec2
  frame's fixed length, 2 + sub-frames × sub-frame bytes, or the profile's
  byte cap for Opus. One speaker's slots take at most 128 KB; the cap and the
  start depth below are cut to what fits (`jitterSize` in
  `lxst_sess_profile.h`).

- *Ordering.* Sequenced frames are placed by their 16-bit sequence number,
  wrap-aware. A frame up to 64 ahead of the next expected one leaves empty
  slots for the missing ones, which play as concealment unless the frame
  arrives before its slot plays. A frame up to 1024 behind the playhead is
  late and dropped. A jump beyond either window is the sender restarting its
  count and is appended. Unsequenced frames play in arrival order.
- *Depth.* A speaker starts playing once `jitter_ms` of slots are buffered,
  or at once with what it has when the talker has ended or nothing has
  arrived for the hold time. Partyline sets `jitter_ms` to the room's
  `ptt_jitter_ms` in a push-to-talk room and its cap `max_jitter_ms` to 30 s;
  in other rooms it sets neither, and the session uses `s.lxst.jitter_ms` and
  a 3 s cap. Past the cap the oldest slot is dropped. The phone sets both
  from the call profile (`receiveBuffer`); over a LoRa first hop SUPE
  delivers a peer's frames in batches of up to `BATCH_FRAMES` (12) after
  holding them up to `BATCH_WAIT_MS` (3 s), so there the start depth is
  `BATCH_WAIT_MS` in whole frames and the cap that plus a batch. A smaller
  buffer evicts most of each batch and its 1 s hold splits every spurt at
  the gaps between batches (`dropped` counts those evictions: frames that
  arrived and were thrown away, not frames lost on air).
- *Release.* The hold time is max(1 s, the start depth). A drained buffer is
  released when the talker sent end-of-talk or nothing has arrived for the
  hold time; otherwise it is an underrun and the buffer fills to its start
  depth again. A frame that extends the sequence after end-of-talk is the
  next spurt and clears the mark; a back-fill, late or duplicate frame does
  not, and late or duplicate frames do not count as arrivals.

**Playout.** On each clock tick every speaker adds one block to a 32-bit
accumulator: it plays out its decoded unit, decodes the next unit of the
current frame, or pops the next frame from its buffer. A frame that fails the
profile's frame check, or a Codec2 frame whose mode byte is not the
profile's, plays as concealment, as does an empty slot.

**Any codec (`rx_any`).** A call's peer may send any codec and Codec2 mode,
whatever profile was agreed, so a call's profile sets `rx_any`. Then a frame
need only be a well-formed LXST frame of at most 512 bytes (an Opus frame of
at most 120 ms, one decoded unit; a Codec2 frame in a known mode with whole
sub-frames), frames play in arrival order whatever `sequenced` says, the
speaker's slots are sized for 512-byte frames, and the speaker's decoder
follows each frame: it is rebuilt when the codec or Codec2 mode changes. The
Opus decoder is mono, so a stereo stream is downmixed by Opus itself. Rooms
set `rx_any` 0 and keep the profile's frame rules. The accumulator is
clipped to 16 bits and sent to the endpoint as one block. Nothing ever sends
more than one block per tick, because the engine's per-client ring is short
and drops overflow. A speaker becomes audible (`EV_SPEAKING 1`) when its first
frame is loaded and silent when it is released.

Mixing here, not in the audio engine, keeps the engine's client limit out of
the room size.

## 5. Endpoints

The session sees its microphone and speaker only through `LxstEndpoint`
(`lxst_endpoint.h`): open at a rate and block, capture on or off, play one
block, and a sink that receives microphone PCM.

**The device** is two ITS client connections to the audio engine's stream
port, both `pcm16`. The playback connection, `"pcm16,playback"`, stays open for
the session and does not count towards wanting the microphone. The capture
connection, `"pcm16"`, exists only while the session wants the microphone.
The playback connection is made when the endpoint opens; one that failed or
dropped, and the capture connection, are made by the task's service loop, at
most every 5 s after a failure. Playing a block never connects, so a missing
engine costs the playout tick nothing. While the microphone is wanted and its connection is
down, the task wakes once a second to look again.

**The browser** is the ITS packet port 2, which the WebRTC (Web Real-Time Communication) router opens for the
DataChannel `lxst:2`, one channel message to one packet. One browser is in use
at a time, but the port takes two connections, so that a channel being
replaced can be told: a later connect becomes the current one, and the
earlier is sent `OWNER` 0 and closed once that has left, or after 1 s. A
connect that finds both taken closes the retired one at once; failing that,
it drops the current one (sent `OWNER` 0) for the newer, and if the newer
never completes the channel is reported closed. A message is classified by
`lxst_dc.h`: exactly
two bytes starting `0x80` is the talk key; otherwise it must be longer than
`av_hdr_t`, of audio type and in a codec the endpoint decodes (`pcm16`,
`ulaw16k`, `adpcm`); anything else is dropped. Control bytes are `0x80` and
above and `av_hdr`'s type byte is 0 or 1, so the two never collide. Microphone
blocks are decoded and re-chunked to the session's block, and reach the
session only while it uses this endpoint and wants the microphone. The mix
goes out one block per message, encoded in the channel's codec and stamped
with the wall clock. The endpoint opens only at 16 kHz, because nothing
resamples between the session and the browser.

`OWNER` and `MIC` are sent from one place on every change and on every
connect, owner first, after every ITS poll. A control message the channel's
window refuses, and a replaced channel still waiting to close, are looked at
again from a 50 ms one-shot timer. The inbound window holds a few
microphone blocks; the outbound one holds about a quarter of a second, and a
slow channel drops mix blocks rather than growing latency.

## 6. Ownership

`lxst_owner.h` is the whole rule, host-tested on its own. Four talk sources:
the board's talk key, its Space key, `lxst.cmd.ptt`, and the browser's push-to-talk message.
The browser takes the endpoint when its channel opens or its key goes down
(only while the channel is open). The device takes it on the down edge of any
device source, and when the channel closes. Key releases never move it.
Taking it releases the other side's held keys, so a key held across a change
of owner keys nothing until it is pressed again, and the browser's key is
released whenever its channel opens or closes. The session is keyed when the
owner's key is held. A change of owner ends the spurt with its end-of-talk,
closes the old endpoint (so an unused device engine powers down) and opens
the owner's.

`lxst.cmd.claim` = `device` runs the device's take without a key going down,
and is reset to "". A user interface on the device writes it before it acts for the
user, so the device's microphone and speaker take what follows.

## 7. The call port and arbitration

`lxst_arb.h` is the whole rule for who has the session, host-tested on its
own:

```
room connects       accepted unless a room is connected; held at once while a call is live
call connects       accepted unless a call is connected; pending
OP_LIVE             the call has the session; a connected room is held
call disconnects    a held room resumes
room disconnects    forgotten; a held room leaves nothing to resume
```

Each consumer keeps its own last profile and `OP_STATE` values (`Consumer`
in the session), whether or not it has the session. A message from a
consumer that does not have it updates only that record; its frames and
talk-end marks are dropped. So a pending call's `OP_PROFILE` and `OP_STATE`
are stored, and a held room's mute and deafen changes wait for its resume.

**Pending.** A call connects when it starts ringing, either way, and a
pending call touches nothing: the room keeps transmitting, receiving and
playing. Only `OP_TONE` acts at once.

**Live.** `OP_LIVE` drops the room's spurt without its end-of-talk, releases
its speakers with `EV_SPEAKING 0`, and gives the session to the call: its own
profile, then its state. With no room the session opens for the call. The
room is sent `EV_HELD 1` and is held; `lxst.state` becomes `call` and
`lxst.held` 1.

**Resume.** The call port's disconnect stops the tone. If the call was live,
its spurt and speakers are dropped (there is no one to send them to), and a
connected room gets the session back with its own last profile and state and
`EV_HELD 0`; with no room the session closes. A room that connects while a
call is live is accepted and held at once.

**`EV_HELD`** is never sent from a connect callback. It is queued as one
pending value (`heldOut`), sent from the task loop and from the room's
receive callback, and a send the room's port refuses is tried again: the
task's poll wakes at least once a second while one is waiting. A newer value
replaces an unsent one, so the room always ends on the latest.

Disconnects are deferred to the task loop (`settleClose`), and a connect
callback settles any pending disconnect first, so a consumer that leaves and
another that arrives in the same poll are taken in order.

## 8. Call tones

`lxst_tones.h` generates them, host-tested on its own. The tone is mixed
into the playout accumulator after the speakers, so it rides over any room
audio and through the same clip, on whichever endpoint owns the session:

| Tone | Sound | Cadence |
|---|---|---|
| ring | 400 Hz + 450 Hz | 0.4 s on, 0.2 s off, 0.4 s on, 2 s off |
| ring-back | 382 Hz | 2 s on, 5 s off |
| busy | 382 Hz | 0.25 s on, 0.25 s off |

The peak is about −12 dBFS (decibels relative to full scale; the ring's two
components share it). Every burst is ramped in and out over 5 ms, so the
cadence does not click, and setting a tone, even the same one, restarts its
cadence at the start of a burst. The tone's position counts samples, so the
cadence holds at any rate.

A tone needs the owner's playback even when no consumer has the session (a
call ringing with no room, the busy tone after a failed call). The endpoint
is opened when the first of a live session and a tone needs it and closed
when neither does (`audioUpdate`), and the playout clock runs while a tone
sounds, gaps included, so the block stream to the engine stays even.

## 9. The phone

The phone is the lxphone task, `src/lxst_phone.cpp`, in three parts:

- `lxst_phone_call.{h,cpp}`, the call as a state machine for either side.
  It is pure: the task feeds it events with the time and carries out the
  actions it queues (send signals, identify, connect the session's call port,
  change its profile, set a tone, go live, close the port, tear the link down
  after a delay, end with a result). It holds no handles and does no I/O, so
  the host test drives both sides through every path.
- `lxst_phone_wire.{h,cpp}`, the packets, the signals and the profile table,
  on lxst's msgpack codec. Encoding writes into a caller buffer and decoding
  points into the input, so neither allocates. It also holds the rules that
  are pure arithmetic: frames per packet, the `auto` profile, the receive
  buffering, and what a profile costs the first hop (`txBps`, `fitsHop`,
  `fitProfile`, `fitCall`, `talkSecondsLeft`; a full-duplex call counts both
  directions), which `lxst_phone.cpp`'s `hopCheck` applies on every
  first-hop change, after the peer's signals and each second of a call,
  through `Call::setProfile` (profile and mode together, the peer told in one
  packet). `Call::hopHalf` marks a call half duplex only for its hop: only
  such a call goes back to full duplex when the hop allows, and a peer's own
  mode preference clears it.
- `lxst_phone.cpp`, the I/O: the hosted destination, the Links, the session's
  call port, storage, announces and the CLI. Everything runs on the task:
  ITS callbacks and storage subscriptions dispatch through its one `itsPoll`,
  whose timeout is the next deadline.

**Phases.** The caller goes dial (resolve the destination, path, link) →
await (link up, waiting for RINGING) → ring-back → connecting (CONNECTING
heard, ESTABLISHED sent, waiting for the callee's) → live. The callee goes
ringing → live. A caller that hears BUSY or REJECTED before the call is live
plays the busy tone for 4 s with the link already gone, then ends. The
session's call port is connected when the call starts ringing on either side
(the caller on RINGING), so the ring-back and ring play; `OP_LIVE` follows on
CONNECTING at the caller and on answering at the callee.

**The session profile.** A call's `lxst_profile_t` is built from the profile
table: codec, mode byte, frame time and frame cap from the profile; no byte
budget and `batch_max_frames` = ceil(60 / frame_ms), so every `EV_BATCH` is
exactly one Link packet; unsequenced; `rx_any` 1; `tx_mode` `LXST_TX_OPEN` in
full duplex and `LXST_TX_PTT` in half; start depth frame_ms × max(1, buffer −
2) but at least 60 ms, and a cap of frame_ms × (buffer + 1) + 250 ms. A
preference from the peer that changes the profile or mode sends a new
`OP_PROFILE`. The call never runs at `0x70`: `runProfile` turns it into
`0x80`, for the caller's own choice at RINGING and for every preference heard,
and a preference of `0x70` is answered with PREFERRED_PROFILE `0x80` (at once,
or right after ESTABLISHED for a callee still ringing). `rx_any` plays the
peer's 10 ms frames until it follows. Received frames go to the session as speaker 1; the frames of
every packet drained in one receive callback are gathered into one
`OP_FRAMES`, flushed at the end of the drain, when a signal is to be acted on,
or when the message is full.

**Inbound links.** rnsd hands each Link to `lxst.telephony` to the task's
port 1. Up to four may be pending identification at once. Each hears
AVAILABLE at once, even while a call is up, so a caller identifies and is
then told BUSY rather than left to time out; a caller that does not identify
within 30 s is dropped. The identity comes from
`rnsd.links.<tag>.remote_identity` (or with the connect, when rnsd already
knows it), and is acted on only after AVAILABLE has gone. Then the caller is
checked: blocked first, then `allow`, then whether a call is up. One that
passes becomes the call and rings; any other hears BUSY.

The phone proves nothing, either way: `rnsdLinkExpectProofs(tag, false)` is
set on every inbound link and on the outbound one, because an LXST
telephony destination proves nothing, so a proof counted against it would
only ever time out.

**Tearing down after a signal.** BUSY and REJECTED are followed by a
teardown, and a packet queued on the Link's ITS handle just before
`itsDisconnect` is dropped with the handle. So those teardowns wait 1 s
(`SIGNAL_GRACE_MS`) in a small list of dying links, which the task's
deadlines run.

**Hang-up.** rnsd closes an inbound link's ITS connection only after a grace
period, so the task watches `rnsd.links.<tag>.state` too: `closed` or
`failed` ends the call (or drops a pending link) at once, within about
100 ms of the link closing. An outbound link's state key does the same, and
`active` on it starts the wait for RINGING, max(10 s, 5 s + 4 × round trip).
A live call has no deadline of its own: it ends when its Link closes or
goes stale (rnsd's keepalive and stale handling), never on silence, since a
muted or microphone-less peer sends nothing.

**A silent Link.** RNS lets only a Link's initiator send keepalives; the
responder answers them and otherwise only watches, and an initiator ignores
an inbound keepalive request. So the callee cannot probe a caller that died,
and its Link goes stale only after two keepalive intervals, and the interval
scales with the round trip up to 360 s. rnsd publishes
`rnsd.links.<tag>.heard_s` (moved by any packet heard, keepalives included)
and `keepalive_s`/`rtt_ms`; the task stamps its own clock each time
`heard_s` moves (`Ctx::heardAt`, set at the call's start too). From that:
- after `SILENT_WARN_MS` (90 s) of silence the live status line becomes
  "Connection may be lost (nothing heard for m:ss)", republished with the
  timer each second and reset by the next packet heard;
- an identified, allowed caller arriving while a live or connecting call's
  Link has been silent past `deadAfterMs(keepalive_s, rtt_ms)` (one
  interval + max(15 s, 4 RTT), at least 90 s, 360 s for an unknown
  interval) ends that call "Connection lost" and rings. A live peer is heard
  at least once per interval: a Python RNS initiator's watchdog wakes at most
  every 5 s and sends after one interval without sending, and a responder
  answers ours. One lost keepalive can therefore let a new caller pre-empt a
  muted call; the Link itself would go stale at the next one.

**Identity.** The identity is chosen as partyline chooses (the only LXMF
identity, the user's choice when there are several, the node's own when
there is none) and re-checked every minute and on every change of
`s.lxst.phone.identity` or `s.lxst.phone.enabled`, but only between calls.
For the first minute after the task starts, an LXMF identity whose key is
stored but which lxmf has not published yet makes the choice wait, checked
every 2 s, so the phone does not come up on the node's own identity and move
a moment later.

**Dialling.** A typed hash is resolved every 500 ms until it resolves: a
destination whose public key the node holds (the telephony destination
itself, or another of the identity's, LXMF delivery among them, or an LXMF
contact's key), a phone heard announcing that identity, or an identity whose
telephony or LXMF delivery destination's key is known. The telephony
destination's key is seeded from the identity's when it was not itself
heard. Meanwhile a path is requested for both the typed hash and its
telephony destination every 15 s. The link's deadline is what is left of the
call's 70 s.

**Announces.** The destination is announced with no app_data when it opens
and every 3 h ± 5 min after; an announce that cannot be queued is tried
again a second later. The task also listens to `lxst.telephony` announces
(the peer list, the newest 32) and `lxmf.delivery` announces (to name the
phones of the same identity).

## 10. Rules worth keeping

- File-local types live in anonymous namespaces. A global `struct Session` in
  one straddle and another of the same name elsewhere are one type to the
  linker under the one-definition rule, and with different layouts one
  corrupts the other and the image crashes.
- Drain the ITS port on every receive callback (§1).
- Timer callbacks only flag and notify; all session state belongs to the
  task.
- The session never sends more than one block per tick to the engine, and
  the playout tick never connects anything.
- Every buffer comes from PSRAM (pseudo-static RAM), with no internal-RAM
  fallback; size a speaker's slots by the codec's real largest frame, not
  the 512-byte ceiling, except under `rx_any`, where any frame may come.
- The jitter buffer holds encoded frames; decode only what is about to play.
- Frame rules (`lxst_frame.cpp`) follow the server's own checks byte for
  byte, Opus durations read from the TOC (table of contents) byte included;
  the host test compares them with partyline's, so change both together.
- An ITS disconnect callback receives the connection's ref (the one that
  side chose at connect: a client's `ref` argument, or what a server's
  connect callback returned), not its handle. The session's ports and the
  phone's link port return the handle as the ref, so the two match there; the
  phone's outbound link chooses its own ref and is matched on it.
- A pending call never touches the room; only `OP_LIVE` takes the session.
- `EV_HELD` is queued and retried, never dropped and never sent from a
  connect callback (§7).
- A signal that precedes a teardown (BUSY, REJECTED) gets 1 s to leave the
  ITS ring before the Link's handle closes, or it goes with the handle.
- The call state machine stays pure (no handles, no I/O, time passed in), so
  the host test covers both sides; I/O belongs in `lxst_phone.cpp`.
- The phone acts on `rnsd.links.<tag>.state` for a hang-up, not on the ITS
  disconnect, which rnsd sends only after a grace period.
