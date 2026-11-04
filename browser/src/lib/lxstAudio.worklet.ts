/**
 * The two AudioWorklet processors of the browser audio endpoint, as source
 * text. The page loads them from a Blob URL (lxstAudio.ts), so they need no
 * file of their own on the device and pass through the one-chunk build
 * untouched. Plain JavaScript: it runs in the AudioWorkletGlobalScope, where
 * `sampleRate` is the context's rate.
 *
 *   lxst-capture   context rate → `outRate` mono: a windowed-sinc low-pass at
 *                  the input rate, then linear interpolation at the output
 *                  instants. Posts `{ pcm: Int16Array(block), rms }` per
 *                  block while switched on with `{ on: true }`.
 *   lxst-playback  `inRate` mono blocks (`{ pcm: Float32Array }`) into a
 *                  jitter ring: plays once `startMs` is banked, holds at most
 *                  `maxMs` (the oldest is dropped), plays silence on underrun
 *                  and banks again. Linear interpolation up to the context
 *                  rate, then the same low-pass to take the images out.
 *                  `{ flush: true }` empties the ring.
 */
export const LXST_WORKLET_SOURCE = String.raw`
function lowpass(rate, cutoff, taps) {
  const h = new Float32Array(taps)
  const m = (taps - 1) / 2
  const fc = cutoff / rate
  let sum = 0
  for (let i = 0; i < taps; i++) {
    const x = i - m
    const sinc = x === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * x) / (Math.PI * x)
    const w = 0.42 - 0.5 * Math.cos(2 * Math.PI * i / (taps - 1)) + 0.08 * Math.cos(4 * Math.PI * i / (taps - 1))
    h[i] = sinc * w
    sum += h[i]
  }
  for (let i = 0; i < taps; i++) h[i] /= sum
  return h
}

/* About 95 taps at 48 kHz (Blackman: ~2.8 kHz transition), scaled with the rate. */
function tapsFor(rate) { return (Math.round(rate / 48000 * 95) | 1) }

class Fir {
  constructor(rate, cutoff) {
    this.h = lowpass(rate, cutoff, tapsFor(rate))
    this.n = this.h.length
    this.buf = new Float32Array(this.n * 2)
    this.i = 0
  }
  push(x) {
    const n = this.n, buf = this.buf, h = this.h
    buf[this.i] = x
    buf[this.i + n] = x
    this.i = this.i + 1 === n ? 0 : this.i + 1
    let y = 0
    for (let k = 0, j = this.i; k < n; k++, j++) y += h[k] * buf[j]
    return y
  }
}

class Capture extends AudioWorkletProcessor {
  constructor(opts) {
    super()
    const o = opts.processorOptions
    this.block = o.block
    this.step = sampleRate / o.outRate
    this.fir = new Fir(sampleRate, o.outRate * 0.425)
    this.on = false
    this.ph = 1
    this.prev = 0
    this.reset()
    this.port.onmessage = (e) => {
      if (e.data && 'on' in e.data) { this.on = !!e.data.on; if (!this.on) this.reset() }
    }
  }
  reset() { this.out = new Int16Array(this.block); this.n = 0; this.sq = 0 }
  emit(v) {
    if (v > 1) v = 1; else if (v < -1) v = -1
    this.out[this.n++] = v < 0 ? v * 32768 : v * 32767
    this.sq += v * v
    if (this.n === this.block) {
      const pcm = this.out
      this.port.postMessage({ pcm, rms: Math.sqrt(this.sq / this.block) }, [pcm.buffer])
      this.reset()
    }
  }
  process(inputs) {
    const inp = inputs[0]
    if (!this.on || !inp || inp.length === 0) return true
    const len = inp[0].length, chans = inp.length
    for (let i = 0; i < len; i++) {
      let x = 0
      for (let c = 0; c < chans; c++) x += inp[c][i]
      const y = this.fir.push(x / chans)
      while (this.ph <= 1) { this.emit(this.prev + (y - this.prev) * this.ph); this.ph += this.step }
      this.ph -= 1
      this.prev = y
    }
    return true
  }
}

class Playback extends AudioWorkletProcessor {
  constructor(opts) {
    super()
    const o = opts.processorOptions
    this.cap = Math.round(o.inRate * o.maxMs / 1000)
    this.startN = Math.round(o.inRate * o.startMs / 1000)
    this.step = o.inRate / sampleRate
    this.fir = new Fir(sampleRate, o.inRate * 0.45)
    this.ring = new Float32Array(this.cap)
    this.r = 0; this.count = 0
    this.primed = false
    this.ph = 0; this.a = 0; this.b = 0
    this.port.onmessage = (e) => {
      const d = e.data
      if (!d) return
      if (d.flush) { this.r = 0; this.count = 0; this.primed = false }
      if (d.pcm) this.bank(d.pcm)
    }
  }
  bank(pcm) {
    const cap = this.cap
    for (let i = 0; i < pcm.length; i++) {
      if (this.count === cap) { this.r = this.r + 1 === cap ? 0 : this.r + 1; this.count-- }
      let w = this.r + this.count
      if (w >= cap) w -= cap
      this.ring[w] = pcm[i]
      this.count++
    }
  }
  pull() {
    if (!this.primed && this.count >= this.startN) this.primed = true
    if (!this.primed) return 0
    if (this.count === 0) { this.primed = false; return 0 }
    const v = this.ring[this.r]
    this.r = this.r + 1 === this.cap ? 0 : this.r + 1
    this.count--
    return v
  }
  process(_inputs, outputs) {
    const out = outputs[0] && outputs[0][0]
    if (!out) return true
    for (let i = 0; i < out.length; i++) {
      const v = this.a + (this.b - this.a) * this.ph
      this.ph += this.step
      while (this.ph >= 1) { this.ph -= 1; this.a = this.b; this.b = this.pull() }
      out[i] = this.fir.push(v)
    }
    for (let c = 1; c < outputs[0].length; c++) outputs[0][c].set(out)
    return true
  }
}

registerProcessor('lxst-capture', Capture)
registerProcessor('lxst-playback', Playback)
`
