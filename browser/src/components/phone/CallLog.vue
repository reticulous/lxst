<!-- The call log, newest first: which way, who (name, else the hash), when,
     how long and how it ended. A row calls that identity back. -->
<template>
  <div class="cl">
    <div v-if="!entries.length" class="none">No calls yet.</div>
    <div v-for="e in entries" :key="e.n" class="row" :class="{ bad: bad(e) }"
         :title="disabled ? '' : `Call ${e.name || e.peer} back`" @click="!disabled && e.peer && emit('call', e.peer)">
      <q-icon :name="icon(e)" size="16px" class="ic" :class="e.dir" />
      <div class="mid">
        <div class="nm" :class="{ anon: !e.name }">{{ e.name || groupHash(e.peer) }}</div>
        <div class="sub">{{ [e.result, e.dur].filter(Boolean).join(' · ') }}</div>
      </div>
      <div class="at">{{ callTime(e.at) }}</div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { matCallMade, matCallReceived, matCallMissed, matCallMissedOutgoing } from '@quasar/extras/material-icons'
import type { PhoneLogEntry } from '../../modules/phone'
import { callTime, groupHash } from '../../lib/phoneText'

defineProps<{ entries: PhoneLogEntry[]; disabled: boolean }>()
const emit = defineEmits<{ call: [hash: string] }>()

const bad = (e: PhoneLogEntry) => e.result !== 'answered' && e.result !== ''
function icon(e: PhoneLogEntry) {
  if (e.dir === 'in') return e.result === 'missed' ? matCallMissed : matCallReceived
  return bad(e) ? matCallMissedOutgoing : matCallMade
}
</script>

<style scoped>
.cl { display: flex; flex-direction: column; }
.none { color: #9a9a9a; padding: 10px 4px; }
.row { display: flex; align-items: center; gap: 10px; padding: 6px 8px; border-radius: 6px; cursor: pointer; }
.row:hover { background: #2a2a2a; }
.ic { flex: none; color: #4abf6a; }
.ic.out { color: #6a9be0; }
.row.bad .ic { color: #e07a6a; }
.mid { flex: 1; min-width: 0; }
.nm { font-weight: 700; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.nm.anon { font-weight: 400; font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: calc(11.5px * var(--rfs, 1)); }
.sub { color: #9a9a9a; font-size: calc(11.5px * var(--rfs, 1)); }
.at { flex: none; color: #9a9a9a; font-size: calc(11.5px * var(--rfs, 1)); }
</style>
