<script setup lang="ts">
import type { SplitterPanelEmits, SplitterPanelProps } from "reka-ui"
import { SplitterPanel, useForwardPropsEmits } from "reka-ui"

const props = defineProps<SplitterPanelProps>()
const emits = defineEmits<SplitterPanelEmits>()

// `class` (and any other extra attrs) fall through to SplitterPanel's root div
// via Vue's default attribute inheritance — no explicit handling needed.
const forwarded = useForwardPropsEmits(props, emits)
</script>

<template>
  <SplitterPanel data-slot="resizable-panel" v-bind="forwarded">
    <slot />
  </SplitterPanel>
</template>
