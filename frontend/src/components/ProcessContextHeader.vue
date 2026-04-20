<script setup lang="ts">
import { ref, computed } from 'vue'
import type { ProcessInfo } from '@/api/types'
import { revealFile } from '@/api/client'
import { Badge } from '@/components/ui/badge'
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from '@/components/ui/tooltip'
import { Plus, Copy, Check, ChevronDown, ChevronUp, FolderOpen, Monitor, Zap, ZapOff } from 'lucide-vue-next'

const props = defineProps<{
  process: ProcessInfo | null
  hasHijackedDescendants?: boolean
  hasChildren?: boolean
}>()

const emit = defineEmits<{
  'create-rule': []
  hack: [pid: number]
  unhack: [pid: number]
  'unhack-tree': [node: ProcessInfo]
}>()

const isManualHijacked = computed(() =>
  node.value?.hijack_source?.startsWith('manual') ?? false
)
const isAutoHijacked = computed(() =>
  node.value?.hijack_source?.startsWith('auto') ?? false
)
const showHack = computed(() =>
  node.value && !node.value.hijacked && !isAutoHijacked.value
)
const showUnhack = computed(() => isManualHijacked.value)
const showUnhackTree = computed(() =>
  isManualHijacked.value && props.hasChildren && props.hasHijackedDescendants
)

// Copy helpers
const copied = ref<'cwd' | 'full' | null>(null)
const cmdExpanded = ref(false)

async function copy(text: string, kind: 'cwd' | 'full') {
  try { await navigator.clipboard.writeText(text) } catch {}
  copied.value = kind
  setTimeout(() => { copied.value = null }, 1500)
}

const node = computed(() => props.process)
const cmdline = computed(() => node.value?.cmdline ?? node.value?.name ?? '')

// Extract dir + exe from cmdline or name
const exeName = computed(() => node.value?.name ?? '')
const cwd = computed(() => {
  // Use image_path from backend (full exe path) to extract directory
  const imgPath = node.value?.image_path
  if (imgPath) {
    const lastSep = Math.max(imgPath.lastIndexOf('/'), imgPath.lastIndexOf('\\'))
    if (lastSep > 0) return imgPath.slice(0, lastSep + 1)
  }
  // Fallback: parse from cmdline
  const cmd = cmdline.value
  let path = ''
  if (cmd.startsWith('"')) {
    const end = cmd.indexOf('"', 1)
    if (end > 0) path = cmd.slice(1, end)
  } else {
    const space = cmd.indexOf(' ')
    path = space > 0 ? cmd.slice(0, space) : cmd
  }
  const lastSep = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'))
  return lastSep > 0 ? path.slice(0, lastSep + 1) : ''
})

// Args after exe
const cmdArgs = computed(() => {
  const cmd = cmdline.value
  const exe = exeName.value
  const patterns = [
    `"${cwd.value}${exe}"`,
    `${cwd.value}${exe}`,
    `"${exe}"`,
    exe,
  ]
  for (const p of patterns) {
    if (cmd.startsWith(p)) {
      return cmd.slice(p.length).trimStart()
    }
  }
  const idx = cmd.indexOf(exe)
  if (idx !== -1) return cmd.slice(idx + exe.length).trimStart()
  return cmd
})

const hasCmdArgs = computed(() => cmdArgs.value.length > 0)
const isCmdLong = computed(() => cmdArgs.value.length > 40 || cmdline.value.length > 60)

// Full exe path for Reveal in Explorer
const imagePath = computed(() => {
  // Prefer backend-provided image_path
  if (node.value?.image_path) return node.value.image_path
  // Fallback: parse from cmdline
  const cmd = cmdline.value
  if (cmd.startsWith('"')) {
    const end = cmd.indexOf('"', 1)
    if (end > 0) return cmd.slice(1, end)
  } else {
    const space = cmd.indexOf(' ')
    return space > 0 ? cmd.slice(0, space) : cmd
  }
  return ''
})

// Locate in explorer via backend API
const locateFlash = ref(false)
async function locateExe() {
  const path = imagePath.value || (cwd.value + exeName.value)
  if (!path) return
  locateFlash.value = true
  try {
    await revealFile(path)
  } catch { /* ignore */ }
  setTimeout(() => { locateFlash.value = false }, 1000)
}
</script>

<template>
  <div
    v-if="node"
    class="shrink-0 border-b border-slate-200 dark:border-slate-800 bg-slate-50/50 dark:bg-[#18181b]/50 px-4 py-3"
  >
    <!-- Top row: icon + name + PID + Create Rule -->
    <div class="flex items-center justify-between gap-4">
      <div class="flex items-center gap-3 min-w-0">
        <div class="w-9 h-9 rounded-lg bg-blue-600/10 dark:bg-blue-500/10 flex items-center justify-center shrink-0">
          <Monitor class="w-5 h-5 text-blue-600 dark:text-blue-400" />
        </div>
        <div class="min-w-0">
          <div class="flex items-center gap-2">
            <p class="text-sm font-semibold font-mono tracking-tight text-slate-800 dark:text-slate-100 truncate">{{ node.name }}</p>
            <Badge
              v-if="node.hijack_source === 'manual'"
              variant="default"
              class="h-4 px-1.5 py-0 text-[9px] font-bold uppercase tracking-wider bg-emerald-500 hover:bg-emerald-500 text-white border-transparent rounded-[3px]"
            >
              MANUAL
            </Badge>
            <Badge
              v-else-if="node.hijack_source === 'auto'"
              variant="default"
              class="h-4 px-1.5 py-0 text-[9px] font-bold uppercase tracking-wider bg-blue-600 dark:bg-blue-500 hover:bg-blue-600 text-white border-transparent rounded-[3px]"
            >
              AUTO
            </Badge>
          </div>
          <p class="text-xs font-mono text-slate-400 dark:text-slate-500">PID <span class="text-slate-600 dark:text-slate-300">{{ node.pid }}</span></p>
        </div>
      </div>
      <div class="flex items-center gap-1 shrink-0">
        <TooltipProvider :delay-duration="300">
          <!-- Hack button — tree mode by default -->
          <Tooltip v-if="showHack">
            <TooltipTrigger as-child>
              <button
                class="w-7 h-7 rounded-md flex items-center justify-center text-slate-400 hover:text-amber-500 dark:hover:text-amber-400 hover:bg-slate-200 dark:hover:bg-slate-800 transition-colors"
                @click="emit('hack', node!.pid)"
              >
                <Zap class="w-4 h-4" />
              </button>
            </TooltipTrigger>
            <TooltipContent side="bottom" :side-offset="4">
              <p class="text-xs">Hack</p>
            </TooltipContent>
          </Tooltip>

          <!-- Unhack single -->
          <Tooltip v-if="showUnhack">
            <TooltipTrigger as-child>
              <button
                class="w-7 h-7 rounded-md flex items-center justify-center text-emerald-500 dark:text-emerald-400 hover:text-red-500 dark:hover:text-red-400 hover:bg-slate-200 dark:hover:bg-slate-800 transition-colors"
                @click="emit('unhack', node!.pid)"
              >
                <ZapOff class="w-4 h-4" />
              </button>
            </TooltipTrigger>
            <TooltipContent side="bottom" :side-offset="4">
              <p class="text-xs">Unhack</p>
            </TooltipContent>
          </Tooltip>

          <!-- Unhack tree -->
          <Tooltip v-if="showUnhackTree">
            <TooltipTrigger as-child>
              <button
                class="w-7 h-7 rounded-md flex items-center justify-center text-emerald-500 dark:text-emerald-400 hover:text-red-500 dark:hover:text-red-400 hover:bg-slate-200 dark:hover:bg-slate-800 transition-colors"
                @click="emit('unhack-tree', node!)"
              >
                <ZapOff class="w-4 h-4" />
              </button>
            </TooltipTrigger>
            <TooltipContent side="bottom" :side-offset="4">
              <p class="text-xs">Unhack Tree</p>
            </TooltipContent>
          </Tooltip>
        </TooltipProvider>

        <button
          @click="$emit('create-rule')"
          class="w-7 h-7 flex items-center justify-center rounded-md text-slate-400 hover:text-blue-600 dark:hover:text-blue-400 hover:bg-slate-200 dark:hover:bg-slate-800 transition-colors"
          title="Create rule from this process"
        >
          <Plus class="w-4 h-4" />
        </button>
      </div>
    </div>

    <!-- Path + Cmdline row -->
    <div v-if="cmdline" class="mt-2.5 group/path flex items-center gap-1 text-xs flex-wrap">
      <!-- Locate button -->
      <button
        @click="locateExe"
        class="shrink-0 p-1 rounded transition-colors"
        :class="locateFlash
          ? 'bg-emerald-100 dark:bg-emerald-900/40 text-emerald-600 dark:text-emerald-400'
          : 'text-slate-400 hover:text-blue-600 dark:hover:text-blue-400 hover:bg-slate-200 dark:hover:bg-slate-800'"
        title="Reveal executable in Explorer"
      >
        <FolderOpen class="w-3.5 h-3.5" />
      </button>

      <!-- Dir badge + exe badge (path chip) -->
      <span v-if="cwd" class="inline-flex items-center shrink-0 font-mono">
        <span class="px-1.5 py-0.5 rounded-l bg-slate-200/80 dark:bg-slate-800 text-slate-500 dark:text-slate-400 text-[11px] max-w-[200px] truncate" :title="cwd">
          {{ cwd }}
        </span>
        <span class="px-1.5 py-0.5 rounded-r bg-slate-300/80 dark:bg-slate-700 text-slate-700 dark:text-slate-200 text-[11px] font-semibold border-l border-slate-400/30 dark:border-slate-600 shrink-0">
          {{ exeName }}
        </span>
      </span>
      <span v-else class="inline-flex items-center shrink-0 font-mono">
        <span class="px-1.5 py-0.5 rounded bg-slate-300/80 dark:bg-slate-700 text-slate-700 dark:text-slate-200 text-[11px] font-semibold">
          {{ exeName }}
        </span>
      </span>

      <!-- Args (collapse when long) -->
      <span
        v-if="hasCmdArgs"
        class="font-mono text-[11px] text-slate-400 dark:text-slate-500 ml-1"
        :class="!cmdExpanded && isCmdLong ? 'max-w-[12rem] truncate' : 'break-all'"
      >{{ cmdArgs }}</span>

      <!-- Expand / Collapse arrow -->
      <button
        v-if="isCmdLong"
        @click="cmdExpanded = !cmdExpanded"
        class="p-1 rounded hover:bg-slate-200 dark:hover:bg-slate-800 text-slate-400 hover:text-slate-600 dark:hover:text-slate-300 transition-colors shrink-0"
        :title="cmdExpanded ? 'Collapse' : 'Expand'"
      >
        <ChevronUp v-if="cmdExpanded" class="w-3.5 h-3.5" />
        <ChevronDown v-else class="w-3.5 h-3.5" />
      </button>

      <!-- Copy buttons — appear on hover -->
      <button
        v-if="cwd"
        @click="copy(cwd, 'cwd')"
        class="opacity-0 group-hover/path:opacity-100 transition-opacity p-1 rounded hover:bg-slate-200 dark:hover:bg-slate-800 text-slate-400 hover:text-slate-600 dark:hover:text-slate-300 shrink-0"
        title="Copy working directory"
      >
        <Check v-if="copied === 'cwd'" class="w-3.5 h-3.5 text-emerald-500" />
        <Copy v-else class="w-3.5 h-3.5" />
      </button>

      <button
        @click="copy(cmdline, 'full')"
        class="opacity-0 group-hover/path:opacity-100 transition-opacity flex items-center gap-0.5 px-1.5 py-0.5 rounded text-[10px] font-mono font-medium hover:bg-slate-200 dark:hover:bg-slate-800 text-slate-400 hover:text-slate-600 dark:hover:text-slate-300 shrink-0"
        title="Copy full command line"
      >
        <Check v-if="copied === 'full'" class="w-3 h-3 text-emerald-500" />
        <template v-else><Copy class="w-3 h-3" /><span>all</span></template>
      </button>
    </div>
  </div>
</template>
