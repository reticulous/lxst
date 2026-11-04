/**
 * lxstAudio — this browser as the voice session's microphone and speaker.
 *
 * The lxst task takes its endpoint from the DataChannel "lxst:2" (lxst.h):
 *
 *   browser ── DC "lxst:2" open, protocol "pcm16" ───▶ lxst   endpoint := browser
 *   browser ◀── [0x81 OWNER][1|0] ──────────────────── lxst   who has the endpoint
 *   browser ◀── [0x82 MIC][1|0] ────────────────────── lxst   send the mic / stop
 *   browser ── [av_hdr pcm16][320 samples] …  ───────▶ lxst   while asked
 *   browser ◀── [av_hdr pcm16|ulaw16k][block] … ────── lxst   the mix
 *   browser ── [0x80 PTT][1|0] ──────────────────────▶ lxst   the talk key
 *   browser ── DC close ─────────────────────────────▶ lxst   endpoint := device
 *
 * startAudio() must run inside a user gesture: it creates or resumes the
 * AudioContext, asks for the microphone and opens the channel, which takes the
 * endpoint. While this browser owns the endpoint the channel is kept open —
 * rebuilt on every peer-connection reconnect, reopened with backoff if the
 * device closes it. OWNER 0 (a device talk key took the endpoint) closes the
 * channel and releases the microphone; only the next gesture (startAudio(),
 * a talk-key press) opens it again, so a reconnect never takes the endpoint
 * back. There is no fallback to the device's microphone: with the microphone
 * blocked or the channel down, nothing is transmitted from here.
 *
 * The talk key reaches lxst only while the channel is open and the
 * microphone is granted; it is sent on each change of that, so a channel
 * that opens with the key held is keyed at once.
 *
 * The device takes one browser channel at a time, so this module is the one
 * audio surface for the whole page. Every window that uses it names itself
 * (its `user`): startAudio() adds it, stopAudio() lets it go, and the audio
 * stops only when no user is left. The talk key has one holder at a time; a
 * user's release lets go only of a key it holds.
 */
import { ref, computed, watch, type ComputedRef } from 'vue'
import { getSession } from 'spangap-browser/lib/webrtc-session'
import { LXST_WORKLET_SOURCE } from './lxstAudio.worklet'

const DC_LABEL = 'lxst:2'
const DC_PROTOCOL = 'pcm16'

const DC_PTT = 0x80
const DC_OWNER = 0x81
const DC_MIC = 0x82

/* audio:1 framing: [type u8][codec u8][epoch_ms u64 LE][utc_offset_min i16 LE]. */
const AV_HDR = 12
const AV_TYPE_AUDIO = 1
const CODEC_PCM16 = 0
const CODEC_ULAW16K = 2

/* The session engine's rate and block. */
const RATE = 16000
const BLOCK = 320

const JITTER_START_MS = 60
const JITTER_MAX_MS = 300

/* Mic blocks are dropped rather than queued behind a slow link. */
const DC_TX_HIGH_WATER = 16 * 1024

export type MicState = 'idle' | 'asking' | 'ok' | 'denied' | 'none' | 'error'
export type ChannelState = 'closed' | 'connecting' | 'open'

/** The browser is the audio surface (started by a gesture, not yet stopped). */
export const audioActive = ref(false)
export const channelState = ref<ChannelState>('closed')
/** From LXST_DC_OWNER: true = here, false = on the device, null = not said yet. */
export const audioOwner = ref<boolean | null>(null)
/** The device took the endpoint; the channel stays closed until a gesture here. */
export const audioOnDevice = ref(false)
/** The AudioContext's state ('closed' when there is none). */
export const ctxState = ref<string>('closed')
/** From LXST_DC_MIC. */
export const micWanted = ref(false)
export const micState = ref<MicState>('idle')
/** The user holding the talk key, null = none. */
const keyHolder = ref<string | null>(null)
/** The talk key as this page holds it, whoever holds it. */
export const pttHeld = computed(() => keyHolder.value !== null)
/** The talk key as one user holds it. */
export function talkKeyHeldBy(user: string): ComputedRef<boolean> {
  return computed(() => keyHolder.value === user)
}
/* The windows using the audio. */
const users = new Set<string>()
/** Microphone level 0…1 (root mean square, RMS, of the last block sent), 0
 *  when not sending. */
export const micLevel = ref(0)

const ULAW = new Float32Array(256)
for (let i = 0; i < 256; i++) {
  const u = ~i & 0xff
  const t = (((u & 0x0f) << 3) + 0x84) << ((u & 0x70) >> 4)
  ULAW[i] = ((u & 0x80) ? 0x84 - t : t - 0x84) / 32768
}

let ctx: AudioContext | null = null
let workletReady: Promise<void> | null = null
let capture: AudioWorkletNode | null = null
let playback: AudioWorkletNode | null = null
let micStream: MediaStream | null = null
let micSource: MediaStreamAudioSourceNode | null = null

let dc: RTCDataChannel | null = null
let unregister: (() => void) | null = null
let reopenTimer: ReturnType<typeof setTimeout> | null = null
let backoff = 250
let sentKey = false
/* This browser wants the endpoint: set by a gesture, cleared by OWNER 0. */
let wantChannel = false
/* Generation of the getUserMedia request; a stale one's stream is stopped. */
let micReq = 0

/* ── Context and worklets ── */

function ensureContext(): AudioContext | null {
  if (!ctx || ctx.state === 'closed') {
    try {
      ctx = new AudioContext({ latencyHint: 'interactive' })
    } catch (e) {
      console.error('[lxst] AudioContext failed:', e)
      ctx = null
      return null
    }
    const c = ctx
    /* Nodes of a closed context are dead: rebuilt below and in attachMic. */
    capture = null
    playback = null
    if (micSource) { try { micSource.disconnect() } catch { /* */ } micSource = null }
    ctxState.value = c.state
    c.onstatechange = () => { if (ctx === c) ctxState.value = c.state }
    const url = URL.createObjectURL(new Blob([LXST_WORKLET_SOURCE], { type: 'application/javascript' }))
    workletReady = c.audioWorklet.addModule(url).then(() => {
      URL.revokeObjectURL(url)
      if (ctx !== c) return
      playback = new AudioWorkletNode(c, 'lxst-playback', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [1],
        processorOptions: { inRate: RATE, startMs: JITTER_START_MS, maxMs: JITTER_MAX_MS },
      })
      playback.connect(c.destination)
      capture = new AudioWorkletNode(c, 'lxst-capture', {
        numberOfInputs: 1, numberOfOutputs: 0,
        processorOptions: { outRate: RATE, block: BLOCK },
      })
      capture.port.onmessage = (e) => onMicBlock(e.data)
      attachMic()
      syncCapture()
    })
    workletReady.catch((e) => console.error('[lxst] audio worklet failed:', e))
  }
  if (ctx.state !== 'running' && ctx.state !== 'closed') ctx.resume().catch(() => { /* the next gesture retries */ })
  return ctx
}

function askMic() {
  if (micState.value === 'ok' || micState.value === 'asking') return
  if (!navigator.mediaDevices?.getUserMedia) { micState.value = 'error'; return }
  micState.value = 'asking'
  const req = ++micReq
  navigator.mediaDevices.getUserMedia({
    audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true, channelCount: 1 },
  }).then((stream) => {
    if (req !== micReq || !audioActive.value || !wantChannel) {
      stream.getTracks().forEach(t => t.stop())
      if (req === micReq) micState.value = 'idle'
      return
    }
    micStream = stream
    micState.value = 'ok'
    for (const t of stream.getAudioTracks()) t.onended = () => { if (micStream === stream) dropMic('none') }
    attachMic()
  }).catch((e: unknown) => {
    if (req !== micReq) return
    const name = (e as { name?: string })?.name ?? ''
    micState.value = name === 'NotAllowedError' || name === 'SecurityError' ? 'denied'
      : name === 'NotFoundError' || name === 'OverconstrainedError' ? 'none' : 'error'
    console.warn('[lxst] microphone:', name || e)
  })
}

function attachMic() {
  if (micSource && micSource.context !== ctx) {
    try { micSource.disconnect() } catch { /* */ }
    micSource = null
  }
  if (!ctx || !capture || !micStream || micSource) return
  micSource = ctx.createMediaStreamSource(micStream)
  micSource.connect(capture)
}

function dropMic(state: MicState) {
  micReq++
  if (micSource) { try { micSource.disconnect() } catch { /* */ } micSource = null }
  if (micStream) { micStream.getTracks().forEach(t => t.stop()); micStream = null }
  micState.value = state
}

/* The capture worklet runs only while lxst wants the microphone. */
function syncCapture() {
  const on = micWanted.value && micState.value === 'ok' && channelState.value === 'open'
  capture?.port.postMessage({ on })
  if (!on) micLevel.value = 0
}

/* ── Framing ── */

function avHeader(view: DataView, codec: number) {
  const now = Date.now()
  view.setUint8(0, AV_TYPE_AUDIO)
  view.setUint8(1, codec)
  view.setBigUint64(2, BigInt(now), true)
  view.setInt16(10, -new Date(now).getTimezoneOffset(), true)
}

function onMicBlock(d: { pcm?: Int16Array; rms?: number }) {
  if (!d?.pcm) return
  micLevel.value = Math.min(1, (d.rms ?? 0) * 4)
  if (!dc || dc.readyState !== 'open' || !micWanted.value) return
  if (dc.bufferedAmount > DC_TX_HIGH_WATER) return
  const pcm = d.pcm
  const buf = new ArrayBuffer(AV_HDR + pcm.length * 2)
  const view = new DataView(buf)
  avHeader(view, CODEC_PCM16)
  for (let i = 0; i < pcm.length; i++) view.setInt16(AV_HDR + i * 2, pcm[i]!, true)
  try { dc.send(buf) } catch { /* dropped */ }
}

function onSpeakerBlock(b: Uint8Array) {
  if (!playback || b.length <= AV_HDR || b[0] !== AV_TYPE_AUDIO) return
  const codec = b[1]
  let pcm: Float32Array
  if (codec === CODEC_PCM16) {
    const n = (b.length - AV_HDR) >> 1
    const view = new DataView(b.buffer, b.byteOffset + AV_HDR, n * 2)
    pcm = new Float32Array(n)
    for (let i = 0; i < n; i++) pcm[i] = view.getInt16(i * 2, true) / 32768
  } else if (codec === CODEC_ULAW16K) {
    const n = b.length - AV_HDR
    pcm = new Float32Array(n)
    for (let i = 0; i < n; i++) pcm[i] = ULAW[b[AV_HDR + i]!]!
  } else {
    return
  }
  playback.port.postMessage({ pcm }, [pcm.buffer])
}

function flushPlayback() { playback?.port.postMessage({ flush: true }) }

function onControl(op: number, v: number) {
  if (op === DC_OWNER) {
    audioOwner.value = v === 1
    if (v !== 1) yieldToDevice()
  } else if (op === DC_MIC) {
    micWanted.value = v === 1
  }
}

/* ── The channel ── */

/* A release that cannot be sent closes the channel, which releases the key
 * on the device; the channel is then reopened. */
function sendKey(on: boolean) {
  if (!dc || dc.readyState !== 'open') return
  try {
    dc.send(new Uint8Array([DC_PTT, on ? 1 : 0]))
  } catch {
    if (!on) { closeChannel(); scheduleReopen() }
    return
  }
  sentKey = on
}

/* The key lxst should see: held here, and able to transmit from here. */
const keyed = computed(() => pttHeld.value && micState.value === 'ok' && channelState.value === 'open')
watch(keyed, (k) => { if (k !== sentKey) sendKey(k) })
watch([micWanted, micState, channelState], syncCapture)

function closeChannel() {
  if (dc) { dc.onclose = null; dc.onmessage = null; try { dc.close() } catch { /* */ } dc = null }
  channelLost()
}

function channelLost() {
  channelState.value = 'closed'
  audioOwner.value = null
  micWanted.value = false
  sentKey = false
  flushPlayback()
}

/* The device took the endpoint: stop wanting it. The AudioContext stays. */
function yieldToDevice() {
  wantChannel = false
  audioOnDevice.value = true
  if (reopenTimer) { clearTimeout(reopenTimer); reopenTimer = null }
  closeChannel()
  dropMic('idle')
}

function build(pc: RTCPeerConnection) {
  if (reopenTimer) { clearTimeout(reopenTimer); reopenTimer = null }
  closeChannel()
  if (!audioActive.value || !wantChannel) return
  let ch: RTCDataChannel
  try {
    ch = pc.createDataChannel(DC_LABEL, { ordered: true, protocol: DC_PROTOCOL })
  } catch (e) {
    console.error('[lxst] createDataChannel failed:', e)
    return
  }
  dc = ch
  channelState.value = 'connecting'
  ch.binaryType = 'arraybuffer'
  ch.onopen = () => {
    backoff = 250
    channelState.value = 'open'   /* the keyed watch sends a held key */
  }
  ch.onmessage = (ev) => {
    getSession().noteDcActivity()
    if (!(ev.data instanceof ArrayBuffer)) return
    const b = new Uint8Array(ev.data)
    if (b.length === 0) return
    if (b[0]! >= 0x80) { if (b.length >= 2) onControl(b[0]!, b[1]!) }
    else onSpeakerBlock(b)
  }
  ch.onclose = () => {
    if (dc !== ch) return
    dc = null
    channelLost()
    scheduleReopen()
  }
  ch.onerror = () => { /* onclose follows */ }
}

function openChannel() {
  if (reopenTimer) { clearTimeout(reopenTimer); reopenTimer = null }
  const s = getSession()
  if (s.pc && s.state === 'connected') build(s.pc)
}

function scheduleReopen() {
  if (!audioActive.value || !wantChannel) return
  if (reopenTimer) clearTimeout(reopenTimer)
  reopenTimer = setTimeout(() => { reopenTimer = null; openChannel(); backoff = Math.min(backoff * 2, 5000) }, backoff)
}

/* ── API ── */

/** Make this browser the audio surface and take the endpoint (from the
 *  device, if it has it). Call from a user gesture: it starts or resumes
 *  audio output, asks for the microphone (again, if it was refused, missing
 *  or released) and opens the channel if it is not open. Idempotent. */
export function startAudio(user: string) {
  users.add(user)
  ensureContext()
  wantChannel = true
  audioOnDevice.value = false
  if (!audioActive.value) {
    audioActive.value = true
    backoff = 250
    unregister = getSession().registerChannel(build)   /* built now + on each reconnect */
    getSession().connect()
  } else if (!dc) {
    backoff = 250
    openChannel()
  }
  if (micState.value !== 'ok') askMic()
}

/** Resume audio output from a gesture that does not otherwise start audio
 *  (a pointer release: on touch devices the press is not one that may). */
export function resumeAudio() {
  if (audioActive.value) ensureContext()
}

/** This user no longer needs the audio. When it was the last, stop being the
 *  audio surface: the channel closes (lxst goes back to the device) and the
 *  microphone and audio output are released. */
export function stopAudio(user: string) {
  users.delete(user)
  if (keyHolder.value === user) keyHolder.value = null
  if (users.size > 0 || !audioActive.value) return
  audioActive.value = false
  wantChannel = false
  audioOnDevice.value = false
  ctxState.value = 'closed'
  keyHolder.value = null
  if (reopenTimer) { clearTimeout(reopenTimer); reopenTimer = null }
  if (unregister) { unregister(); unregister = null }
  closeChannel()
  dropMic('idle')
  capture = null
  playback = null
  workletReady = null
  if (ctx) { const c = ctx; ctx = null; c.close().catch(() => { /* */ }) }
}

/** The talk key. Pressing it starts audio here for this user and takes the
 *  endpoint (call presses from the gesture's handler); a release lets go only
 *  of a key this user holds. */
export function setTalkKey(down: boolean, user: string) {
  if (down) {
    startAudio(user)
    keyHolder.value = user
  } else if (keyHolder.value === user) {
    keyHolder.value = null
  }
}
