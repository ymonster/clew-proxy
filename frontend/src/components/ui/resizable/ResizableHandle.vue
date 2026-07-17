<script setup lang="ts">
import type { SplitterResizeHandleEmits, SplitterResizeHandleProps } from "reka-ui"
import type { HTMLAttributes } from "vue"
import { reactiveOmit } from "@vueuse/core"
import { SplitterResizeHandle, useForwardPropsEmits } from "reka-ui"
import { GripVertical } from "lucide-vue-next"
import { cn } from "@/lib/utils"

const props = defineProps<
  SplitterResizeHandleProps & { class?: HTMLAttributes["class"]; withHandle?: boolean }
>()
const emits = defineEmits<SplitterResizeHandleEmits>()

const delegatedProps = reactiveOmit(props, "class", "withHandle")
const forwarded = useForwardPropsEmits(delegatedProps, emits)
</script>

<template>
  <SplitterResizeHandle
    data-slot="resizable-handle"
    v-bind="forwarded"
    :class="cn(
      // 1px visual line; reka-ui's default hitAreaMargins keeps a ~5px grab zone.
      'relative flex w-px shrink-0 items-center justify-center bg-slate-200 dark:bg-slate-800',
      'transition-colors hover:bg-blue-400 dark:hover:bg-blue-500 active:bg-blue-500',
      'focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-blue-500',
      // widen the pointer target visually to either side of the 1px line
      'after:absolute after:inset-y-0 after:left-1/2 after:w-2 after:-translate-x-1/2',
      props.class,
    )"
  >
    <div
      v-if="withHandle"
      class="z-10 flex h-6 w-3 items-center justify-center rounded-sm border border-slate-300 bg-slate-100 dark:border-slate-700 dark:bg-slate-800"
    >
      <GripVertical class="h-3 w-3 text-slate-500 dark:text-slate-400" />
    </div>
    <slot />
  </SplitterResizeHandle>
</template>
