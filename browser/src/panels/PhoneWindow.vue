<!-- The Phone window. A call in progress takes the top: who, the status
     line and its buttons (components/phone/CallView.vue). With no call up,
     the window is the phone at rest: our number to give out, a hash to dial,
     and the lists to call from — recent calls, peers heard announcing a
     phone, LXMF contacts. What it shows comes from modules/phone.ts; this is
     the only place layout is decided. In a half-duplex call, holding Space
     while no text field has focus is the talk key, as the button is. -->
<template>
  <FloatingWindow
    id="phone"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
    :min-size="{ w: 24, h: 30 }"
    @update:visible="v => emit('update:visible', v)"
  >
    <template #titlebar-right>
      <span class="rfs-btn" title="Smaller" @click="zoomOut">-</span>
      <span class="rfs-btn" title="Larger" @click="zoomIn">+</span>
    </template>

    <template #default>
      <div class="ph" :style="{ '--rfs': scale }">
        <CallView v-if="callShown" :call="c" :audio="ph.audio.value" :half-duplex="ph.halfDuplex.value"
                  :held="ph.pttHeld.value" :error="ph.inCall.value ? ph.cmdError.value : ''"
                  @answer="ph.answer" @reject="ph.reject" @hangup="ph.hangup" @mute="ph.setMuted"
                  @ptt="ph.setPtt" @take="ph.takeAudio" @resume="ph.resume" />

        <div v-if="!ph.inCall.value" class="rest">
          <div v-if="c.state === 'off'" class="offline">
            {{ c.status || 'The phone is off: calls are not taken.' }}
          </div>

          <div class="num">
            <div class="k">Your number</div>
            <div v-if="me.dest" class="hrow" title="Copy" @click="copy(me.dest)">
              <span class="hv">{{ groupHash(me.dest) }}</span>
              <q-icon :name="copied === me.dest ? matCheck : matContentCopy" size="14px" class="cp" />
            </div>
            <div v-else class="dim">Not known yet.</div>
            <div v-if="me.ident" class="hrow small" title="Copy" @click="copy(me.ident)">
              <span class="lab">Identity</span>
              <span class="hv">{{ groupHash(me.ident) }}</span>
              <q-icon :name="copied === me.ident ? matCheck : matContentCopy" size="13px" class="cp" />
            </div>
          </div>

          <form class="dial" @submit.prevent="onDial">
            <input v-model="typed" class="in" spellcheck="false" autocomplete="off"
                   placeholder="Phone, identity or LXMF address" />
            <button class="go" type="submit" :disabled="!dialable" title="Call">
              <q-icon :name="matCall" size="16px" /><span>Call</span>
            </button>
          </form>
          <div v-if="ph.cmdError.value" class="err">{{ ph.cmdError.value }}</div>

          <div class="tabs">
            <button v-for="t in TABS" :key="t.id" class="tab" :class="{ on: tab === t.id }" @click="tab = t.id">
              {{ t.label }}<span v-if="t.id === 'peers' && ph.peers.value.length" class="n">{{ ph.peers.value.length }}</span>
            </button>
          </div>
          <div class="list">
            <CallLog v-if="tab === 'recent'" :entries="ph.log.value" :disabled="ph.inCall.value" @call="onCall" />
            <PeerList v-else-if="tab === 'peers'" :rows="peerRows" :disabled="ph.inCall.value"
                      empty="No phone heard announcing yet." @call="onCall" />
            <PeerList v-else :rows="contactRows" :disabled="ph.inCall.value"
                      empty="No LXMF contacts." @call="onCall" />
          </div>
        </div>
      </div>
    </template>
  </FloatingWindow>
</template>

<script lang="ts">
/* Which list is showing, kept across remounts for the session. */
import { ref as _ref } from 'vue'
type Tab = 'recent' | 'peers' | 'contacts'
const tab = _ref<Tab>('recent')
</script>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { matCall, matCheck, matContentCopy } from '@quasar/extras/material-icons'
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import { focusedWindowId } from 'spangap-browser/lib/windows'
import { useWinZoom } from 'rns/lib/winZoom'
import CallView from '../components/phone/CallView.vue'
import CallLog from '../components/phone/CallLog.vue'
import PeerList from '../components/phone/PeerList.vue'
import { usePhone, normaliseHash, type PeerRow } from '../modules/phone'
import { groupHash } from '../lib/phoneText'

const props = defineProps<{ visible: boolean; title: string }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const TABS: { id: Tab; label: string }[] = [
  { id: 'recent', label: 'Recent' },
  { id: 'peers', label: 'Phones heard' },
  { id: 'contacts', label: 'Contacts' },
]

const defaultGeom = { x: 34, y: 8, w: 34, h: 76 }
const ph = usePhone()
const { scale, zoomIn, zoomOut } = useWinZoom('phone')

const c = computed(() => ph.call.value)
const me = computed(() => ph.self.value)
const callShown = computed(() => ph.inCall.value || c.value.state === 'ended')

/* ── Dialling ── */
const typed = ref('')
const dialable = computed(() => !ph.inCall.value && normaliseHash(typed.value) !== '')
function onDial() {
  if (!dialable.value) return
  if (ph.dial(typed.value)) typed.value = ''
}
function onCall(hash: string) {
  if (!ph.inCall.value) ph.dial(hash)
}

const peerRows = computed<PeerRow[]>(() => ph.peers.value.map(p => ({
  hash: p.dest, name: p.name, note: p.hops >= 0 ? `${p.hops} hop${p.hops === 1 ? '' : 's'}` : '',
})))
const contactRows = computed<PeerRow[]>(() => ph.contacts.value.map(x => ({ hash: x.hash, name: x.name, note: '' })))

/* ── Our number ── */
const copied = ref('')
let copiedTimer: ReturnType<typeof setTimeout> | null = null
function copy(h: string) {
  navigator.clipboard?.writeText(h).then(() => {
    copied.value = h
    if (copiedTimer) clearTimeout(copiedTimer)
    copiedTimer = setTimeout(() => { copied.value = '' }, 1500)
  }).catch(() => { /* not allowed here */ })
}

/* ── The talk key, in a half-duplex call ──
 * Space keys the transmitter while this window is in front and no text field
 * has the focus. */
function typing(t: EventTarget | null): boolean {
  const el = t as HTMLElement | null
  if (!el || !el.tagName) return false
  return el.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(el.tagName)
}
function onKeyDown(e: KeyboardEvent) {
  if (e.code !== 'Space' || typing(e.target)) return
  if (!props.visible || !ph.halfDuplex.value || focusedWindowId.value !== 'phone') return
  e.preventDefault()
  if (!e.repeat && !c.value.muted) ph.setPtt(true)
}
function onKeyUp(e: KeyboardEvent) {
  if (e.code === 'Space' && ph.pttHeld.value) ph.setPtt(false)
}
onMounted(() => {
  document.addEventListener('keydown', onKeyDown)
  document.addEventListener('keyup', onKeyUp)
})
onBeforeUnmount(() => {
  document.removeEventListener('keydown', onKeyDown)
  document.removeEventListener('keyup', onKeyUp)
  if (copiedTimer) clearTimeout(copiedTimer)
  ph.setPtt(false)
})

/* Never left keyed: the window hidden or behind another, or muted, lets go. */
watch(() => props.visible && !c.value.muted, (ok) => { if (!ok) ph.setPtt(false) })
watch(focusedWindowId, (id) => { if (id !== 'phone') ph.setPtt(false) })
</script>

<style scoped>
.ph { position: relative; height: 100%; display: flex; flex-direction: column; background: #1d1d1d;
      overflow: hidden; color: #e8e8e8; font-size: calc(13px * var(--rfs, 1)); }
.rfs-btn {
  display: inline-flex; align-items: center; justify-content: center;
  width: 18px; height: 18px; border-radius: 4px; font-size: 14px; font-weight: 700;
  color: rgba(255,255,255,0.5); cursor: pointer; font-family: system-ui; line-height: 1;
  user-select: none;
}
.rfs-btn:hover { color: rgba(255,255,255,0.9); background: rgba(255,255,255,0.1); }
.rest { flex: 1; min-height: 0; display: flex; flex-direction: column; padding: 12px 14px 0; gap: 10px;
        border-top: 1px solid #2c2c2c; }
.offline { padding: 6px 10px; border-radius: 6px; background: #2a2a2a; color: #d6a12a; font-size: calc(12px * var(--rfs, 1)); }
.k { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; letter-spacing: .8px; text-transform: uppercase; margin-bottom: 4px; }
.num { display: flex; flex-direction: column; gap: 2px; }
.hrow { display: inline-flex; align-items: center; gap: 8px; cursor: pointer; padding: 2px 4px; margin-left: -4px;
        border-radius: 5px; align-self: flex-start; max-width: 100%; }
.hrow:hover { background: #2a2a2a; }
.hv { font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: calc(14px * var(--rfs, 1)); white-space: nowrap;
      overflow: hidden; text-overflow: ellipsis; }
.hrow.small .hv { font-size: calc(11.5px * var(--rfs, 1)); color: #9a9a9a; }
.lab { color: #9a9a9a; font-size: calc(11px * var(--rfs, 1)); }
.cp { color: #777; flex: none; }
.dim { color: #9a9a9a; }
.dial { display: flex; gap: 8px; }
.in { flex: 1; min-width: 0; background: #1c1c1c; color: #e8e8e8; border: 1px solid #3a3a3a; border-radius: 6px;
      padding: 6px 8px; font-size: calc(12.5px * var(--rfs, 1)); outline: none;
      font-family: "DejaVu Sans Mono", Menlo, monospace; }
.in:focus { border-color: #2c6bed; }
.go { flex: none; display: inline-flex; align-items: center; gap: 4px; background: #2f8a4a; border: none; color: #fff;
      border-radius: 6px; padding: 4px 12px; cursor: pointer; font-weight: 700; font-size: calc(12.5px * var(--rfs, 1)); }
.go:disabled { background: #333; color: #777; cursor: default; }
.err { color: #e09b6a; font-size: calc(12px * var(--rfs, 1)); }
.tabs { display: flex; gap: 4px; border-bottom: 1px solid #333; }
.tab { background: none; border: none; color: #9a9a9a; padding: 6px 10px; cursor: pointer;
       font-size: calc(12.5px * var(--rfs, 1)); border-bottom: 2px solid transparent; margin-bottom: -1px; }
.tab.on { color: #e8e8e8; border-bottom-color: #2c6bed; }
.n { margin-left: 5px; color: #777; font-size: calc(11px * var(--rfs, 1)); }
.list { flex: 1; min-height: 0; overflow-y: auto; margin: 0 -6px; padding-bottom: 10px; }
</style>
