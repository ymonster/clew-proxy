<script setup lang="ts">
import type { ProcessInfo } from '@/api/types'
import { Badge } from '@/components/ui/badge'
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from '@/components/ui/tooltip'
import { ChevronRight, ChevronDown, Monitor, Zap, ZapOff, Network } from 'lucide-vue-next'
import { computed, ref } from 'vue'

const props = defineProps<{
  node: ProcessInfo
  depth: number
  selectedPid: number | null
  isExpanded: (pid: number) => boolean
  isManuallyHijacked: (node: ProcessInfo) => boolean
  isAutoHijacked: (node: ProcessInfo) => boolean
  hasHijackedDescendant: (node: ProcessInfo) => boolean
  isPidActive: (pid: number) => boolean
}>()

const emit = defineEmits<{
  'toggle-expand': [pid: number]
  select: [pid: number]
  hack: [pid: number]
  unhack: [pid: number]
  'unhack-tree': [node: ProcessInfo]
}>()

const hasChildren = computed(() => !!props.node.children?.length)
const expanded = computed(() => props.isExpanded(props.node.pid))
const isSelected = computed(() => props.selectedPid === props.node.pid)
const manualHijack = computed(() => props.isManuallyHijacked(props.node))
const autoHijack = computed(() => props.isAutoHijacked(props.node))
const isRoot = computed(() => props.depth === 0)
const isActive = computed(() => props.isPidActive(props.node.pid))

// Button state logic
const showHack = computed(() =>
  !props.node.hijacked && !autoHijack.value
)
const showUnhack = computed(() => manualHijack.value)
const showUnhackTree = computed(() =>
  manualHijack.value && hasChildren.value && props.hasHijackedDescendant(props.node)
)

// Match mockup: root=text-sm, child=text-[12px]
const nameClass = computed(() => {
  const base = isRoot.value ? 'text-sm' : 'text-[12px]'
  if (isSelected.value) return `${base} font-semibold text-blue-700 dark:text-blue-400`
  return `${base} font-medium text-slate-700 dark:text-slate-200`
})

const pidClass = computed(() => {
  const base = isRoot.value ? 'text-xs' : 'text-[11px]'
  if (isSelected.value) return `${base} text-blue-500 dark:text-blue-400`
  return `${base} text-slate-400 dark:text-slate-500`
})

const iconClass = computed(() => {
  if (isSelected.value) return 'text-blue-600 dark:text-blue-400'
  return 'text-slate-500 dark:text-slate-400'
})

const iconUrl = computed(() => `/api/icon?name=${encodeURIComponent(props.node.name)}`)
const iconFailed = ref(false)

// Match mockup row class: border-l-2 for selection
// Inactive + selected = gray fill/border; inactive + not selected = opacity-50
const rowClass = computed(() => {
  if (isSelected.value) {
    if (!isActive.value) return 'bg-slate-100 dark:bg-slate-800/50 border-slate-400 dark:border-slate-600'
    return 'bg-blue-50 dark:bg-blue-500/10 border-blue-500'
  }
  return 'border-transparent hover:bg-slate-100 dark:hover:bg-slate-800/50'
})

const rowOpacity = computed(() => {
  if (isSelected.value) return undefined
  return isActive.value ? undefined : '0.5'
})

function onRowClick() {
  emit('select', props.node.pid)
}

function onArrowClick(e: Event) {
  e.stopPropagation()
  if (hasChildren.value) {
    emit('toggle-expand', props.node.pid)
  }
}

function onHack(e: Event) {
  e.stopPropagation()
  emit('hack', props.node.pid)
}

function onUnhack(e: Event) {
  e.stopPropagation()
  emit('unhack', props.node.pid)
}

function onUnhackTree(e: Event) {
  e.stopPropagation()
  emit('unhack-tree', props.node)
}
</script>

<template>
  <div>
    <!-- Row with action buttons -->
    <div
      class="group flex py-1.5 pr-2 rounded cursor-pointer transition-colors border-l-2"
      :class="rowClass"
      :style="rowOpacity ? { opacity: rowOpacity } : undefined"
      @click="onRowClick"
      :title="node.cmdline || node.name"
    >
      <!-- Expand arrow -->
      <div class="w-5 flex items-center justify-center shrink-0" @click="onArrowClick">
        <div v-if="hasChildren" class="p-0.5 rounded text-slate-400 dark:text-slate-500 hover:bg-slate-200 dark:hover:bg-slate-700 transition-colors">
          <ChevronDown v-if="expanded" class="w-3 h-3" />
          <ChevronRight v-else class="w-3 h-3" />
        </div>
      </div>

      <!-- Icon + Hijack dot + Name + PID + actions -->
      <div class="flex-1 min-w-0 flex items-start gap-2">
        <img
          v-if="!iconFailed"
          :src="iconUrl"
          :alt="`${node.name} icon`"
          class="w-4 h-4 mt-[1px] shrink-0"
          loading="lazy"
          @error="iconFailed = true"
        />
        <Monitor v-else class="w-4 h-4 mt-[1px] shrink-0" :class="iconClass" />
        <!-- Hijack indicator dot — left of name, always visible -->
        <span
          v-if="node.hijacked"
          class="size-2 rounded-full bg-emerald-500 shrink-0 mt-[5px]"
          :title="manualHijack ? 'Manual hijack' : 'Auto hijack'"
        />
        <div class="flex-1 min-w-0 flex flex-col">
          <!-- Row 1: Name + PID + action buttons -->
          <div class="flex justify-between items-center leading-tight">
            <span class="truncate font-mono tracking-tight" :class="nameClass">
              {{ node.name }}
            </span>
            <div class="flex items-center gap-1 shrink-0 ml-2">
              <!-- Action buttons -->
              <TooltipProvider :delay-duration="300">
                <!-- Hack button (hover only, not hijacked) — always tree mode -->
                <Tooltip v-if="showHack">
                  <TooltipTrigger as-child>
                    <button
                      class="h-6 w-6 rounded flex items-center justify-center text-slate-400 dark:text-slate-500 hover:text-amber-500 dark:hover:text-amber-400 hover:bg-slate-200 dark:hover:bg-slate-700 transition-all opacity-0 group-hover:opacity-100"
                      @click="onHack"
                    >
                      <Zap class="w-3.5 h-3.5" />
                    </button>
                  </TooltipTrigger>
                  <TooltipContent side="top" :side-offset="4">
                    <p class="text-xs">Hack</p>
                  </TooltipContent>
                </Tooltip>

                <!-- Unhack single (visible on any manually hijacked process) -->
                <Tooltip v-if="showUnhack">
                  <TooltipTrigger as-child>
                    <button
                      class="h-6 w-6 rounded flex items-center justify-center text-emerald-500 dark:text-emerald-400 hover:text-red-500 dark:hover:text-red-400 hover:bg-slate-200 dark:hover:bg-slate-700 transition-all"
                      @click="onUnhack"
                    >
                      <ZapOff class="w-3.5 h-3.5" />
                    </button>
                  </TooltipTrigger>
                  <TooltipContent side="top" :side-offset="4">
                    <p class="text-xs">Unhack</p>
                  </TooltipContent>
                </Tooltip>

                <!-- Unhack tree (visible when has hijacked descendants) -->
                <Tooltip v-if="showUnhackTree">
                  <TooltipTrigger as-child>
                    <button
                      class="h-6 w-6 rounded flex items-center justify-center text-emerald-500 dark:text-emerald-400 hover:text-red-500 dark:hover:text-red-400 hover:bg-slate-200 dark:hover:bg-slate-700 transition-all"
                      @click="onUnhackTree"
                    >
                      <Network class="w-3.5 h-3.5 rotate-180" />
                    </button>
                  </TooltipTrigger>
                  <TooltipContent side="top" :side-offset="4">
                    <p class="text-xs">Unhack Tree</p>
                  </TooltipContent>
                </Tooltip>
              </TooltipProvider>

              <!-- PID -->
              <span class="font-mono" :class="pidClass">{{ node.pid }}</span>
            </div>
          </div>
          <!-- Row 2: MANUAL or AUTO badge (below name) -->
          <div v-if="manualHijack || autoHijack" class="mt-1 flex">
            <Badge
              v-if="manualHijack"
              variant="default"
              class="h-4 px-1.5 py-0 text-[9px] font-bold uppercase tracking-wider bg-emerald-500 hover:bg-emerald-500 text-white border-transparent rounded-[3px]"
            >
              MANUAL
            </Badge>
            <Badge
              v-else
              variant="default"
              class="h-4 px-1.5 py-0 text-[9px] font-bold uppercase tracking-wider bg-blue-600 dark:bg-blue-500 hover:bg-blue-600 text-white border-transparent rounded-[3px]"
            >
              AUTO
            </Badge>
          </div>
        </div>
      </div>
    </div>

    <!-- Recursive children with tree guide lines -->
    <div
      v-if="hasChildren && expanded"
      class="ml-[18px] pl-3 border-l border-slate-200 dark:border-slate-700/80"
    >
      <template v-for="child in node.children" :key="child.pid">
        <div class="relative mt-0.5">
          <!-- Horizontal branch line -->
          <div class="absolute left-0 top-[14px] w-3 h-px bg-slate-200 dark:bg-slate-700/80 -translate-x-full" />

          <ProcessTreeNode
            :node="child"
            :depth="depth + 1"
            :selected-pid="selectedPid"
            :is-expanded="isExpanded"
            :is-manually-hijacked="isManuallyHijacked"
            :is-auto-hijacked="isAutoHijacked"
            :has-hijacked-descendant="hasHijackedDescendant"
            :is-pid-active="isPidActive"
            @toggle-expand="(pid: number) => emit('toggle-expand', pid)"
            @select="(pid: number) => emit('select', pid)"
            @hack="(pid: number) => emit('hack', pid)"
            @unhack="(pid: number) => emit('unhack', pid)"
            @unhack-tree="(n: ProcessInfo) => emit('unhack-tree', n)"
          />
        </div>
      </template>
    </div>
  </div>
</template>
