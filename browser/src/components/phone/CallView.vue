<!-- A call in progress: who, the phone's status line, and the buttons for
     where the call is. Ringing: Answer and Reject. Dialling or ringing out:
     Cancel. Live: the timer, profile and mode, Mute and Hang up, and in half
     duplex a hold-to-talk button (held from pointer down until up, leave or
     cancel; Space does the same in the window). Under it, where the call's
     audio is, with "Take audio here" when it is on the device. -->
<template>
  <div class="cv">
    <div class="who">
      <div class="nm" :class="{ anon: !call.peerName }">{{ call.peerName || (call.dir === 'in' ? 'Unknown caller' : 'Unknown') }}</div>
      <div v-if="hash" class="hs" :title="hash">{{ groupHash(hash) }}</div>
    </div>

    <div class="st">{{ call.status }}</div>
    <div v-if="call.state === 'live'" class="tm">{{ call.timer }}</div>
    <div v-if="call.state === 'live' && (call.profile || call.mode)" class="pm">
      {{ [call.profile, call.mode].filter(Boolean).join(' · ') }}
    </div>

    <div class="btns">
      <template v-if="call.state === 'ringing'">
        <button class="rb rej" title="Reject" @click="emit('reject')">
          <q-icon :name="matCallEnd" size="22px" /><span>Reject</span>
        </button>
        <button class="rb ans" title="Answer" @click="emit('answer')">
          <q-icon :name="matCall" size="22px" /><span>Answer</span>
        </button>
      </template>
      <template v-else-if="call.state === 'live'">
        <button class="rb mute" :class="{ on: call.muted }" :title="call.muted ? 'Unmute' : 'Mute'"
                @click="emit('mute', !call.muted)">
          <q-icon :name="call.muted ? matMicOff : matMic" size="22px" /><span>{{ call.muted ? 'Muted' : 'Mute' }}</span>
        </button>
        <button class="rb rej" title="Hang up" @click="emit('hangup')">
          <q-icon :name="matCallEnd" size="22px" /><span>Hang up</span>
        </button>
      </template>
      <template v-else-if="active">
        <button class="rb rej" title="Cancel" @click="emit('hangup')">
          <q-icon :name="matCallEnd" size="22px" /><span>Cancel</span>
        </button>
      </template>
    </div>

    <button v-if="halfDuplex" class="ptt" :class="{ held, dead: held && audio.warn }"
            :disabled="call.muted" :title="call.muted ? 'Muted' : 'Hold to talk (or hold Space)'"
            @pointerdown="down" @pointerup="release" @pointerleave="up" @pointercancel="up"
            @lostpointercapture="up" @contextmenu.prevent>
      <q-icon :name="matMic" size="18px" />
      <span>{{ held ? (audio.warn ? 'Not transmitting' : 'Talking') : 'Hold to talk' }}</span>
      <span v-if="audio.level > 0" class="lvl" :style="{ width: Math.round(audio.level * 100) + '%' }" />
    </button>

    <div v-if="audio.text" class="aud" :class="{ warn: audio.warn }">
      <span>{{ audio.text }}</span>
      <button v-if="audio.offerHere" class="lh" @click="emit('take')">Take audio here</button>
    </div>
    <div v-if="error" class="err">{{ error }}</div>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount } from 'vue'
import { matCall, matCallEnd, matMic, matMicOff } from '@quasar/extras/material-icons'
import type { PhoneCall, PhoneAudio } from '../../modules/phone'
import { groupHash } from '../../lib/phoneText'

const props = defineProps<{
  call: PhoneCall
  audio: PhoneAudio
  halfDuplex: boolean
  /* The talk key as this window holds it. */
  held: boolean
  error: string
}>()
const emit = defineEmits<{
  answer: []
  reject: []
  hangup: []
  mute: [v: boolean]
  ptt: [on: boolean]
  take: []
  resume: []
}>()

const active = computed(() => ['dialling', 'ringback', 'connecting'].includes(props.call.state))
const hash = computed(() => props.call.peer || props.call.peerDest)

function down(e: PointerEvent) {
  e.preventDefault()
  if (e.button !== 0 || props.call.muted) return
  emit('ptt', true)
}
function up() { if (props.held) emit('ptt', false) }
/* On touch devices only the release may start audio output. */
function release() {
  emit('resume')
  up()
}
onBeforeUnmount(up)
</script>

<style scoped>
.cv { display: flex; flex-direction: column; align-items: center; gap: 8px; padding: 28px 16px 20px;
      font-size: calc(13px * var(--rfs, 1)); text-align: center; }
.who { display: flex; flex-direction: column; align-items: center; gap: 3px; max-width: 100%; }
.nm { font-size: calc(20px * var(--rfs, 1)); font-weight: 700; overflow: hidden; text-overflow: ellipsis;
      white-space: nowrap; max-width: 100%; }
.nm.anon { font-weight: 400; font-style: italic; color: #cfcfcf; }
.hs { font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: calc(11.5px * var(--rfs, 1)); color: #9a9a9a; }
.st { color: #b8c8e8; margin-top: 6px; }
.tm { font-size: calc(26px * var(--rfs, 1)); font-variant-numeric: tabular-nums; font-weight: 300; }
.pm { color: #9a9a9a; font-size: calc(11.5px * var(--rfs, 1)); }
.btns { display: flex; gap: 26px; margin: 14px 0 6px; }
.rb { display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 2px;
      width: 72px; height: 72px; border-radius: 50%; border: none; cursor: pointer; color: #fff;
      font-size: calc(11px * var(--rfs, 1)); font-weight: 700; }
.rb.ans { background: #2f8a4a; }
.rb.ans:hover { background: #37a057; }
.rb.rej { background: #b03a37; }
.rb.rej:hover { background: #c94643; }
.rb.mute { background: #2a2a2a; border: 1px solid #444; color: #cfcfcf; }
.rb.mute.on { background: #4a2626; border-color: #8a4545; color: #f0a0a0; }
.ptt { position: relative; overflow: hidden; display: inline-flex; align-items: center; justify-content: center; gap: 6px;
       width: 220px; max-width: 100%; height: 42px; border-radius: 21px; border: 2px solid #2c6bed;
       background: #1d2a44; color: #dfe8ff; font-weight: 700; font-size: calc(13px * var(--rfs, 1)); cursor: pointer;
       user-select: none; -webkit-user-select: none; touch-action: none; }
.ptt.held { background: #2c6bed; color: #fff; }
.ptt.dead { background: #4a2626; border-color: #8a4545; color: #f0a0a0; }
.ptt:disabled { border-color: #444; background: #262626; color: #777; cursor: default; }
.lvl { position: absolute; left: 0; bottom: 0; height: 3px; background: #9fe0b0; transition: width 60ms linear; }
.aud { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; justify-content: center;
       color: #b8c8e8; font-size: calc(12px * var(--rfs, 1)); }
.aud.warn { color: #e09b6a; }
.lh { background: #2c6bed; border: none; color: #fff; border-radius: 5px; padding: 2px 10px; cursor: pointer;
      font-size: calc(11.5px * var(--rfs, 1)); font-weight: 700; }
.err { color: #e09b6a; font-size: calc(12px * var(--rfs, 1)); }
</style>
