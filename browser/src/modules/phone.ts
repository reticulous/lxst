/**
 * phone — the Phone window (SPA side) over the lxst telephony task.
 *
 * The node is the phone; this window is a view over its storage and a writer
 * of its one command key (the contract is lxst_phone.h):
 *
 *   lxst.phone.{state,dir,peer,peer_dest,peer_name,status,timer,muted,profile,mode,ptt_redial}
 *   lxst.phone.{dest,ident}              our number
 *   lxst.phone.peers.<dest>.{ident,name,hops,heard}
 *   s.lxst.phone.log.<n>.{dir,peer,name,at,dur,result}
 *   lxst.phone.cmd ("call <hash>", "call <hash> ptt", answer, reject, hangup, "mute 1|0"), .error
 *   s.lxmf.id.<n>.contacts.<peer>.*, lxmf.id.<n>.dest_hash   LXMF contacts
 *
 * The firmware publishes finished strings; this module passes them through
 * and formats only the call log's time.
 *
 * Audio is always this browser's, never the device's: Answer and Call start
 * the page's one audio surface (lxst's lib/lxstAudio.ts, shared with the
 * Partyline window) on the click, which opens "lxst:2" and takes the
 * endpoint, and then write the command. A call that is not ringing here over
 * that channel rings locally (lib/ringer.ts).
 */
import { ref, computed, watch, effectScope, type ComputedRef } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { registerApp } from 'spangap-browser/lib/apps'
import { registerWindowMount } from 'spangap-browser/lib/windowMounts'
import { registerTopbarIcon } from 'spangap-browser/lib/topbarIcons'
import PhoneWindow from '../panels/PhoneWindow.vue'
import IncomingCall from '../components/phone/IncomingCall.vue'
import PhoneIndicator from '../components/phone/PhoneIndicator.vue'
import {
  audioActive, audioOwner, audioOnDevice, channelState, ctxState, micState, micLevel, talkKeyHeldBy,
  startAudio, resumeAudio, stopAudio, setTalkKey,
} from '../lib/lxstAudio'
import { startRing, stopRing } from '../lib/ringer'

/* This window's name on the page's one audio surface. */
const AUDIO_USER = 'phone'

/* Visibility ref for the Phone floating window. */
export const phoneVisible = ref(false)

/** Focus nonce — bumped to raise the window even when already open. */
export const phoneFocus = ref(0)

/* Dock action: only ever show + raise, never hide. */
export function showPhone() { phoneVisible.value = true; phoneFocus.value++ }

export type PhoneState = 'off' | 'idle' | 'dialling' | 'ringback' | 'ringing' | 'connecting' | 'live' | 'ended' | ''

export interface PhoneCall {
  state: PhoneState
  dir: string
  /** Remote identity hash, '' while dialling an unknown identity. */
  peer: string
  peerDest: string
  peerName: string
  status: string
  timer: string
  muted: boolean
  profile: string
  mode: string
  /** Ended: the route carried only half duplex for this outgoing call, so a
   *  push-to-talk call to the same peer would fit. */
  pttRedial: boolean
}

export interface PhoneSelf { dest: string; ident: string }

/** A peer heard announcing lxst.telephony. */
export interface PhonePeer { dest: string; ident: string; name: string; hops: number; heard: number }

/** An LXMF contact; `hash` is its LXMF delivery destination. */
export interface PhoneContact { hash: string; name: string }

/** One row of a list to call from: the hash `call` takes, a name ('' =
 *  none) and a note under it. */
export interface PeerRow { hash: string; name: string; note: string }

export interface PhoneLogEntry {
  n: number
  dir: string
  peer: string
  name: string
  /** Unix seconds, 0 = the clock was unset. */
  at: number
  dur: string
  result: string
}

export interface PhoneAudio {
  /** One line on where the call's audio is, '' = nothing to say. */
  text: string
  /** The line is a fault: nothing is transmitted from here. */
  warn: boolean
  /** Offer to bring the call's audio (back) to this browser. */
  offerHere: boolean
  /** Microphone level 0…1 while sending. */
  level: number
}

export interface UsePhone {
  call: ComputedRef<PhoneCall>
  self: ComputedRef<PhoneSelf>
  /** Newest heard first. */
  peers: ComputedRef<PhonePeer[]>
  /** By name. */
  contacts: ComputedRef<PhoneContact[]>
  /** Newest first. */
  log: ComputedRef<PhoneLogEntry[]>
  /** A call is up: dialling, ringing either way, connecting or live. */
  inCall: ComputedRef<boolean>
  halfDuplex: ComputedRef<boolean>
  /** The last refusal the phone wrote for a command, '' = none. */
  cmdError: ComputedRef<string>
  audio: ComputedRef<PhoneAudio>
  /** The talk key as this window holds it. */
  pttHeld: ComputedRef<boolean>

  /** Call from the click: starts audio here, then dials, as push to talk
   *  (half duplex throughout) when ptt. False for a hash that is not one. */
  dial: (hash: string, ptt?: boolean) => boolean
  /** Call from the click: starts audio here, then answers. */
  answer: () => void
  reject: () => void
  hangup: () => void
  setMuted: (v: boolean) => void
  /** Bring the call's audio here from the device. Call from the click. */
  takeAudio: () => void
  /** Resume audio output here. Call from a gesture. */
  resume: () => void
  /** The half-duplex talk key; a press starts audio here. Call from the gesture. */
  setPtt: (v: boolean) => void
  /** Ask the device for the LXMF contacts not yet in the mirror. */
  loadContacts: () => void
}

type Patch = Record<string, any>

function nest(path: string, val: any): Patch {
  const parts = path.split('.')
  const root: Patch = {}
  let cur = root
  for (let i = 0; i < parts.length - 1; i++) cur = (cur[parts[i]!] = {})
  cur[parts[parts.length - 1]!] = val
  return root
}

function num(v: unknown, d = 0): number { const n = Number(v); return Number.isFinite(n) ? n : d }
function str(v: unknown): string { return v == null ? '' : String(v) }

const HASH_RE = /^[0-9a-f]{32}$/

/** A typed hash as the phone takes it: whitespace ignored, a 64-hex value
 *  taken as its leading 32; '' when it is not a hash. */
export function normaliseHash(s: string): string {
  let x = s.replace(/\s+/g, '').toLowerCase()
  if (/^[0-9a-f]{64}$/.test(x)) x = x.slice(0, 32)
  return HASH_RE.test(x) ? x : ''
}

/* Storage subtrees whose keys are numbers may arrive as arrays; walk both. */
function entries(tree: unknown): [string, any][] {
  if (!tree || typeof tree !== 'object') return []
  return Object.entries(tree as Record<string, any>).filter(([, v]) => v && typeof v === 'object')
}

const ACTIVE: PhoneState[] = ['dialling', 'ringback', 'ringing', 'connecting', 'live']

let _inst: UsePhone | null = null

/** The Phone state layer. One instance for the page, in its own effect scope
 *  so no component's unmount stops it. */
export function usePhone(): UsePhone {
  if (_inst) return _inst
  _inst = effectScope(true).run(build)!
  return _inst
}

function build(): UsePhone {
  const device = useDeviceStore()
  /* Records the node deletes: a reconnect's dump replaces these. */
  for (const t of ['lxst.phone.peers', 's.lxst.phone.log']) device.snapshotTree(t)

  const call = computed<PhoneCall>(() => ({
    state: str(device.get('lxst.phone.state')) as PhoneState,
    dir: str(device.get('lxst.phone.dir')),
    peer: str(device.get('lxst.phone.peer')).toLowerCase(),
    peerDest: str(device.get('lxst.phone.peer_dest')).toLowerCase(),
    peerName: str(device.get('lxst.phone.peer_name')),
    status: str(device.get('lxst.phone.status')),
    timer: str(device.get('lxst.phone.timer')),
    muted: num(device.get('lxst.phone.muted')) === 1,
    profile: str(device.get('lxst.phone.profile')),
    mode: str(device.get('lxst.phone.mode')),
    pttRedial: num(device.get('lxst.phone.ptt_redial')) === 1,
  }))

  const self = computed<PhoneSelf>(() => ({
    dest: str(device.get('lxst.phone.dest')).toLowerCase(),
    ident: str(device.get('lxst.phone.ident')).toLowerCase(),
  }))

  const peers = computed<PhonePeer[]>(() =>
    entries(device.get('lxst.phone.peers'))
      .filter(([k]) => HASH_RE.test(k.toLowerCase()))
      .map(([k, p]) => ({
        dest: k.toLowerCase(), ident: str(p.ident).toLowerCase(), name: str(p.name),
        hops: num(p.hops, -1), heard: num(p.heard),
      }))
      .sort((a, b) => b.heard - a.heard))

  /* The LXMF identities this device holds (groups are not callable). */
  const lxmfSlots = computed<string[]>(() => {
    const out: string[] = []
    for (const [slot, v] of Object.entries<any>(device.get('lxmf.id') ?? {})) {
      if (!HASH_RE.test(str(v?.dest_hash).toLowerCase())) continue
      if (str(device.get(`s.lxmf.id.${slot}.kind`)) === 'group') continue
      out.push(slot)
    }
    return out
  })

  /* A contact is a peer with a conversation (or one kept without), as the
   * Messages window lists them; groups are left out. */
  const contacts = computed<PhoneContact[]>(() => {
    const out = new Map<string, PhoneContact>()
    for (const slot of lxmfSlots.value) {
      for (const [k, c] of entries(device.get(`s.lxmf.id.${slot}.contacts`))) {
        const hash = k.toLowerCase()
        if (!HASH_RE.test(hash) || out.has(hash)) continue
        if (num(c.count) <= 0 && num(c.kept) !== 1) continue
        if (num(c.group) === 1) continue
        out.set(hash, { hash, name: str(c.nick) || str(c.display_name) })
      }
    }
    return [...out.values()].sort((a, b) =>
      (a.name || '~' + a.hash).localeCompare(b.name || '~' + b.hash, undefined, { sensitivity: 'base' }))
  })

  const loadContacts = () => {
    if (!device.synced) return
    for (const slot of lxmfSlots.value) {
      const path = `s.lxmf.id.${slot}.contacts`
      if (device.get(path) === undefined) device.sendCommand({ fetch: path })
    }
  }
  /* Contacts ride no dump: asked for while the window is open, once per
   * stream and identity. */
  watch([phoneVisible, () => device.syncEpoch, lxmfSlots], ([v]) => { if (v) loadContacts() }, { immediate: true })

  const log = computed<PhoneLogEntry[]>(() =>
    entries(device.get('s.lxst.phone.log')).map(([k, r]) => ({
      n: num(k), dir: str(r.dir), peer: str(r.peer).toLowerCase(), name: str(r.name),
      at: num(r.at), dur: str(r.dur), result: str(r.result),
    })).sort((a, b) => b.n - a.n))

  const inCall = computed(() => ACTIVE.includes(call.value.state))
  const halfDuplex = computed(() => call.value.state === 'live' && call.value.mode === 'half duplex')

  const cmdError = computed(() => {
    const v = device.get('lxst.phone.cmd')
    return v && typeof v === 'object' ? str(v.error) : ''
  })

  const cmd = (val: string) => device.sendJson(nest('lxst.phone.cmd', val))

  /* The audio here is held for a call: `pending` from the click until the
   * call is up, `call` while it is, so the idle a call ends on lets it go and
   * the idle a click starts from does not. */
  let hold: '' | 'pending' | 'call' = ''
  const holdAudio = () => {
    startAudio(AUDIO_USER)
    hold = inCall.value ? 'call' : 'pending'
  }
  const releaseAudio = () => {
    if (!hold) return
    hold = ''
    stopAudio(AUDIO_USER)
  }
  watch(() => call.value.state, (st) => {
    if (ACTIVE.includes(st)) { if (hold) hold = 'call' }
    else if (st !== 'ended' && hold === 'call') releaseAudio()
  })
  /* A refused call never comes up. */
  watch(cmdError, (e) => { if (e && hold === 'pending' && !inCall.value) releaseAudio() })

  const dial = (hash: string, ptt = false) => {
    const h = normaliseHash(hash)
    if (!h) return false
    holdAudio()
    cmd(ptt ? `call ${h} ptt` : `call ${h}`)
    return true
  }
  const answer = () => { holdAudio(); cmd('answer') }
  const reject = () => cmd('reject')
  const hangup = () => cmd('hangup')
  const setMuted = (v: boolean) => cmd(v ? 'mute 1' : 'mute 0')
  const takeAudio = () => holdAudio()
  const resume = () => resumeAudio()

  const pttHeld = talkKeyHeldBy(AUDIO_USER)
  const setPtt = (v: boolean) => {
    if (pttHeld.value === v) return
    if (v && !halfDuplex.value) return
    if (v) hold = hold || 'call'
    setTalkKey(v, AUDIO_USER)
  }
  watch(halfDuplex, (h) => { if (!h) setPtt(false) })

  const audio = computed<PhoneAudio>(() => {
    const base = { level: micLevel.value, warn: false, offerHere: false }
    if (!inCall.value) return { ...base, text: '' }
    if (!audioActive.value || audioOnDevice.value || audioOwner.value === false)
      return { ...base, text: 'Call audio is on the device', offerHere: true }
    if (channelState.value === 'connecting') return { ...base, text: 'Connecting audio…' }
    if (channelState.value !== 'open')
      return { ...base, text: 'Audio channel down — nothing is sent from this browser', warn: true }
    if (ctxState.value !== 'running')
      return { ...base, text: 'Browser audio is paused — not playing or sending', warn: true, offerHere: true }
    switch (micState.value) {
      case 'denied': return { ...base, text: 'Microphone blocked — nothing is sent from this browser', warn: true }
      case 'none': return { ...base, text: 'No microphone — nothing is sent from this browser', warn: true }
      case 'error': return { ...base, text: 'Microphone unavailable — nothing is sent from this browser', warn: true }
      case 'asking': return { ...base, text: 'Waiting for microphone permission…' }
      default: return { ...base, text: 'Audio here (browser)' }
    }
  })

  /* An incoming call rings here unless the session already rings it into
   * this browser over the open channel. */
  const ringHere = computed(() =>
    call.value.state === 'ringing'
    && !(channelState.value === 'open' && audioOwner.value !== false && ctxState.value === 'running'))
  watch(ringHere, (r) => { if (r) startRing(); else stopRing() }, { immediate: true })

  /* An incoming call brings the window forward. */
  watch(() => call.value.state, (st, prev) => { if (st === 'ringing' && prev !== 'ringing') showPhone() })

  return {
    call, self, peers, contacts, log, inCall, halfDuplex, cmdError, audio, pttHeld,
    dial, answer, reject, hangup, setMuted, takeAudio, resume, setPtt, loadContacts,
  }
}

export function registerPhone() {
  const ph = usePhone()

  /* The talk key is released whenever the page stops being in front of the
   * person holding it. */
  const release = () => ph.setPtt(false)
  window.addEventListener('blur', release)
  window.addEventListener('pagehide', release)
  document.addEventListener('visibilitychange', () => { if (document.visibilityState !== 'visible') release() })

  /* Dock app — foregrounds the Phone window. */
  registerApp({ id: 'phone', label: 'Phone', icon: 'phone', placement: 8,
                open: showPhone, isOpen: () => phoneVisible.value })

  registerWindowMount({ id: 'phone', title: 'Phone', component: PhoneWindow,
                        visible: phoneVisible, focusToken: phoneFocus })

  /* An incoming call, over whatever window is in front. */
  registerWindowMount({ id: 'phone-incoming', component: IncomingCall })

  /* A call in progress, in the header. */
  registerTopbarIcon({ id: 'lxst-phone', component: PhoneIndicator })
}
