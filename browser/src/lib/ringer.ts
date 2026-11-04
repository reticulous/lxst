/**
 * ringer — an incoming call's ring, played by this browser itself with Web
 * Audio, for a page whose audio channel is not open (the session rings over
 * the channel when it is).
 *
 * A telephone ring: 400 Hz and 450 Hz together, 0.4 s on, 0.2 s off, 0.4 s
 * on, every 3 s, at about −12 dBFS. Each double burst is scheduled on the
 * AudioContext's clock by a 3 s timer, so nothing waits.
 *
 * Browsers start audio only after the page has had a user gesture. A context
 * that comes up suspended is resumed by the next pointer or key press on the
 * page while the ring lasts.
 */

const PERIOD_MS = 3000
const BURSTS: [number, number][] = [[0, 0.4], [0.6, 1.0]]
const TONES = [400, 450]
/* −12 dBFS for the pair. */
const LEVEL = 0.25 / TONES.length
const RAMP = 0.01

let ctx: AudioContext | null = null
let out: GainNode | null = null
let timer: ReturnType<typeof setInterval> | null = null

function burst() {
  /* A suspended clock stands still: bursts scheduled on it would pile up. */
  if (!ctx || !out || ctx.state !== 'running') return
  const t0 = ctx.currentTime + 0.05
  for (const [on, off] of BURSTS) {
    const g = ctx.createGain()
    g.gain.setValueAtTime(0, t0 + on)
    g.gain.linearRampToValueAtTime(LEVEL, t0 + on + RAMP)
    g.gain.setValueAtTime(LEVEL, t0 + off - RAMP)
    g.gain.linearRampToValueAtTime(0, t0 + off)
    g.connect(out)
    for (const f of TONES) {
      const o = ctx.createOscillator()
      o.frequency.value = f
      o.connect(g)
      o.start(t0 + on)
      o.stop(t0 + off)
      o.onended = () => { try { o.disconnect(); g.disconnect() } catch { /* */ } }
    }
  }
}

function unlock() {
  if (ctx && ctx.state === 'suspended')
    ctx.resume().then(() => {
      /* The ring starts over from the resume, so bursts never overlap. */
      if (!timer) return
      clearInterval(timer)
      burst()
      timer = setInterval(burst, PERIOD_MS)
    }).catch(() => { /* the next gesture retries */ })
}

export function startRing() {
  if (timer) return
  if (!ctx || ctx.state === 'closed') {
    try {
      ctx = new AudioContext()
    } catch (e) {
      console.warn('[lxst] ring: no AudioContext:', e)
      ctx = null
      return
    }
  }
  out = ctx.createGain()
  out.connect(ctx.destination)
  if (ctx.state === 'running') ctx.resume().catch(() => { /* */ })
  unlock()
  document.addEventListener('pointerdown', unlock, true)
  document.addEventListener('keydown', unlock, true)
  burst()
  timer = setInterval(burst, PERIOD_MS)
}

export function stopRing() {
  if (!timer) return
  clearInterval(timer)
  timer = null
  document.removeEventListener('pointerdown', unlock, true)
  document.removeEventListener('keydown', unlock, true)
  if (out) { try { out.disconnect() } catch { /* */ } out = null }
  ctx?.suspend().catch(() => { /* */ })
}
