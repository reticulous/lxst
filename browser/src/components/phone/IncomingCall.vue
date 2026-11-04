<!-- An incoming call, over whatever is on screen: a card at the top with the
     caller and Answer and Reject, shown while the phone rings and the Phone
     window is not the one in front (it then shows the call itself). Clicking
     the caller opens the Phone window. Answer starts this browser's audio on
     the click, as the window's does. -->
<template>
  <div v-if="shown" class="inc" role="alertdialog" aria-label="Incoming call">
    <div class="who" title="Open Phone" @click="showPhone">
      <q-icon :name="matRingVolume" size="22px" class="ic" />
      <div class="tx">
        <div class="nm" :class="{ anon: !c.peerName }">{{ c.peerName || 'Unknown caller' }}</div>
        <div class="sub">{{ c.status || 'Incoming call' }}<template v-if="hash"> · <span class="hs">{{ groupHash(hash) }}</span></template></div>
      </div>
    </div>
    <button class="b rej" title="Reject" @click="ph.reject()"><q-icon :name="matCallEnd" size="18px" /></button>
    <button class="b ans" title="Answer" @click="onAnswer"><q-icon :name="matCall" size="18px" /></button>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { matCall, matCallEnd, matRingVolume } from '@quasar/extras/material-icons'
import { focusedWindowId } from 'spangap-browser/lib/windows'
import { usePhone, showPhone, phoneVisible } from '../../modules/phone'
import { groupHash } from '../../lib/phoneText'

const ph = usePhone()
const c = computed(() => ph.call.value)
const hash = computed(() => c.value.peer || c.value.peerDest)
const shown = computed(() =>
  c.value.state === 'ringing' && !(phoneVisible.value && focusedWindowId.value === 'phone'))

function onAnswer() {
  ph.answer()
  showPhone()
}
</script>

<style scoped>
/* Above every window, the always-on-top ones included. */
.inc { position: fixed; top: 12px; left: 50%; transform: translateX(-50%); z-index: 3000000;
       display: flex; align-items: center; gap: 10px; width: 420px; max-width: calc(100vw - 16px);
       padding: 10px 12px; border-radius: 12px; background: #2b2b2b; border: 1px solid #4a4a4a;
       box-shadow: 0 18px 50px rgba(0, 0, 0, .6); color: #e8e8e8; font-size: 13px; }
.who { flex: 1; min-width: 0; display: flex; align-items: center; gap: 10px; cursor: pointer; }
.ic { color: #4abf6a; flex: none; animation: pulse 1s ease-in-out infinite alternate; }
@keyframes pulse { from { opacity: .45 } to { opacity: 1 } }
.tx { min-width: 0; }
.nm { font-weight: 700; font-size: 15px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.nm.anon { font-weight: 400; font-style: italic; }
.sub { color: #9a9a9a; font-size: 11.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hs { font-family: "DejaVu Sans Mono", Menlo, monospace; }
.b { flex: none; width: 40px; height: 40px; border-radius: 50%; border: none; color: #fff; cursor: pointer;
     display: inline-flex; align-items: center; justify-content: center; }
.b.ans { background: #2f8a4a; }
.b.ans:hover { background: #37a057; }
.b.rej { background: #b03a37; }
.b.rej:hover { background: #c94643; }
</style>
