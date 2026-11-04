<!-- The header's call indicator: while a call is up, a phone with the call's
     timer (live) or status line; a click opens the Phone window. Nothing when
     there is no call. -->
<template>
  <span v-if="ph.inCall.value" class="pi" :class="{ ring: c.state === 'ringing', live: c.state === 'live' }"
        :title="[c.peerName, c.status].filter(Boolean).join(' — ') || 'Phone'" @click="showPhone">
    <q-icon :name="matPhone" size="15px" />
    <span class="t">{{ c.state === 'live' ? c.timer : c.status }}</span>
  </span>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { matPhone } from '@quasar/extras/material-icons'
import { usePhone, showPhone } from '../../modules/phone'

const ph = usePhone()
const c = computed(() => ph.call.value)
</script>

<style scoped>
.pi { display: inline-flex; align-items: center; gap: 4px; padding: 1px 8px; border-radius: 10px; cursor: pointer;
      background: rgba(255, 255, 255, .1); color: #e8e8e8; font-size: 12px; white-space: nowrap; max-width: 160px; }
.pi.live { background: #2f8a4a; color: #fff; }
.pi.ring { background: #2f8a4a; color: #fff; animation: pulse 1s ease-in-out infinite alternate; }
@keyframes pulse { from { opacity: .55 } to { opacity: 1 } }
.t { overflow: hidden; text-overflow: ellipsis; font-variant-numeric: tabular-nums; }
</style>
