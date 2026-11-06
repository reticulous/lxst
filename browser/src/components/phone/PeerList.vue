<!-- A list of people to call — peers heard announcing a phone, or LXMF
     contacts — each with its name (else its hash), an optional note, a
     Call button and a push-to-talk call button. -->
<template>
  <div class="pl">
    <div v-if="!rows.length" class="none">{{ empty }}</div>
    <div v-for="r in rows" :key="r.hash" class="row">
      <div class="mid">
        <div class="nm" :class="{ anon: !r.name }">{{ r.name || groupHash(r.hash) }}</div>
        <div v-if="r.note || r.name" class="sub">
          <span v-if="r.name" class="hs">{{ groupHash(r.hash) }}</span><template v-if="r.note && r.name"> · </template>{{ r.note }}
        </div>
      </div>
      <button class="call" :disabled="disabled" title="Call" @click="emit('call', r.hash)">
        <q-icon :name="matCall" size="16px" /><span>Call</span>
      </button>
      <button class="call ptt" :disabled="disabled" title="Push-to-talk call (half duplex)"
              @click="emit('ptt', r.hash)">
        <q-icon :name="matMic" size="16px" />
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { matCall, matMic } from '@quasar/extras/material-icons'
import { groupHash } from '../../lib/phoneText'
import type { PeerRow } from '../../modules/phone'

defineProps<{ rows: PeerRow[]; empty: string; disabled: boolean }>()
const emit = defineEmits<{ call: [hash: string]; ptt: [hash: string] }>()
</script>

<style scoped>
.pl { display: flex; flex-direction: column; }
.none { color: #9a9a9a; padding: 10px 4px; }
.row { display: flex; align-items: center; gap: 10px; padding: 6px 8px; border-radius: 6px; }
.row:hover { background: #2a2a2a; }
.mid { flex: 1; min-width: 0; }
.nm { font-weight: 700; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.nm.anon { font-weight: 400; font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: calc(11.5px * var(--rfs, 1)); }
.sub { color: #9a9a9a; font-size: calc(11.5px * var(--rfs, 1)); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hs { font-family: "DejaVu Sans Mono", Menlo, monospace; }
.call { flex: none; display: inline-flex; align-items: center; gap: 4px; background: #2f8a4a; border: none; color: #fff;
        border-radius: 6px; padding: 4px 10px; cursor: pointer; font-weight: 700; font-size: calc(12px * var(--rfs, 1)); }
.call:hover:not(:disabled) { background: #37a057; }
.call.ptt { margin-left: -6px; padding: 4px 7px; background: #2a2a2a; border: 1px solid #444; color: #cfcfcf; }
.call.ptt:hover:not(:disabled) { background: #333; }
.call:disabled { background: #333; color: #777; cursor: default; }
</style>
