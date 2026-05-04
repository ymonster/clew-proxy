<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { hijackProcess, unhijackProcess, getTcpConnections, getUdpConnections } from '@/api/client'
import { useNotifications } from '@/api/notify'
import { useDocumentVisibility } from '@/composables/useDocumentVisibility'
import type { ProcessInfo } from '@/api/types'
import { Search, Monitor, X } from 'lucide-vue-next'
import ProcessTreeNode from './ProcessTreeNode.vue'

const notify = useNotifications()
const documentVisible = useDocumentVisibility()

// Process tree is a shared ref maintained by notify.ts: every backend
// `process_update` push replaces it with a fresh snapshot. We bind the
// component's `processes` view to it directly — no fetch, no polling.
const processes = notify.tree

const ACTIVITY_TIMEOUT_MS = 30_000

const emit = defineEmits<{
  select: [pid: number | undefined]
}>()

// --- State ---
const searchQuery = ref('')
const filterTab = ref<'all' | 'proxied' | 'direct'>('all')
// User's explicit expand/collapse preference per pid. `undefined` (absent
// from the map) means "use the default": expanded during search, collapsed
// otherwise. A click always writes here, so collapse works during search.
const expandedPids = ref<Map<number, boolean>>(new Map())
const selectedPid = ref<number | null>(null)
let activityTimer: ReturnType<typeof setInterval> | null = null

// --- Activity tracking ---
const lastActivityByPid = ref(new Map<number, number>())

async function fetchActivity() {
  try {
    // Pull TCP and UDP in parallel — UDP-only processes (Chrome QUIC,
    // Discord voice, games, anything via the UDP path) wouldn't show as
    // active otherwise.
    const [tcp, udp] = await Promise.all([
      getTcpConnections(),
      getUdpConnections(),
    ])
    const now = Date.now()
    const map = new Map<number, number>()
    for (const conn of tcp) map.set(conn.pid, now)
    for (const conn of udp) map.set(conn.pid, now)
    // Retain non-expired entries for PIDs with no current connections
    for (const [pid, ts] of lastActivityByPid.value) {
      if (!map.has(pid) && now - ts < ACTIVITY_TIMEOUT_MS) {
        map.set(pid, ts)
      }
    }
    lastActivityByPid.value = map
  } catch {
    // Backend not available
  }
}

function isPidActive(pid: number): boolean {
  const lastSeen = lastActivityByPid.value.get(pid)
  if (lastSeen == null) return false
  return Date.now() - lastSeen < ACTIVITY_TIMEOUT_MS
}

// Subtree-active = self-active OR any descendant active. Used by
// ProcessTreeNode to drive the chevron and connector dual-channel encoding
// independent from the per-row text dimming (which still tracks self-only).
const subtreeActivePids = computed(() => {
  const set = new Set<number>()
  function walk(node: ProcessInfo): boolean {
    let any = isPidActive(node.pid)
    for (const child of node.children ?? []) {
      if (walk(child)) any = true
    }
    if (any) set.add(node.pid)
    return any
  }
  for (const root of processes.value) walk(root)
  return set
})

function isSubtreeActive(node: ProcessInfo): boolean {
  return subtreeActivePids.value.has(node.pid)
}

function startActivityPolling() {
  if (activityTimer) return
  fetchActivity()
  // /api/tcp is still polled (per-connection metadata is high-frequency
  // and tab-scoped, not part of the tree push payload).
  activityTimer = setInterval(fetchActivity, 10_000)
}

function stopActivityPolling() {
  if (activityTimer) {
    clearInterval(activityTimer)
    activityTimer = null
  }
}

// Suspend the 10s activity poll while hidden; resume on visible.
watch(documentVisible, v => v ? startActivityPolling() : stopActivityPolling())

onMounted(() => {
  if (documentVisible.value) startActivityPolling()
})

onUnmounted(() => {
  stopActivityPolling()
})

// --- Tree helpers ---

function countProcesses(nodes: ProcessInfo[]): number {
  let count = 0
  for (const node of nodes) {
    count += 1
    if (node.children) {
      count += countProcesses(node.children)
    }
  }
  return count
}

const totalProcessCount = computed(() => countProcesses(processes.value))

// --- Search & Filter ---

function matchesSearch(node: ProcessInfo, query: string): boolean {
  const q = query.toLowerCase()
  return (
    node.name.toLowerCase().includes(q) ||
    String(node.pid).includes(q) ||
    (node.cmdline?.toLowerCase().includes(q) ?? false)
  )
}

function matchesFilter(node: ProcessInfo): boolean {
  if (filterTab.value === 'all') return true
  if (filterTab.value === 'proxied') return node.hijacked
  return !node.hijacked
}

function filterTree(nodes: ProcessInfo[]): ProcessInfo[] {
  const query = searchQuery.value.trim()
  if (!query && filterTab.value === 'all') return nodes

  const result: ProcessInfo[] = []
  for (const node of nodes) {
    const selfMatches =
      (!query || matchesSearch(node, query)) && matchesFilter(node)

    if (selfMatches) {
      // Match: keep entire subtree as-is so the user sees the full
      // descendant context (e.g., search "antigravity" → see its child
      // chrome.exe / cmd.exe even though those names don't contain
      // "antigravity").
      result.push(node)
    } else {
      // Non-match: keep only if some descendant matches, preserving the
      // ancestor chain to that match.
      const filteredChildren = node.children ? filterTree(node.children) : []
      if (filteredChildren.length > 0) {
        result.push({
          ...node,
          children: filteredChildren,
        })
      }
    }
  }
  return result
}

// PIDs whose own data (name / pid / cmdline) matches the current query.
// Distinct from the rendered tree, which also keeps ancestors and
// descendants of matches as context. Used to amber-highlight the actual
// hits like Ctrl+F. Empty when no query.
const matchedPids = computed(() => {
  const set = new Set<number>()
  const q = searchQuery.value.trim()
  if (!q) return set
  function walk(node: ProcessInfo) {
    if (matchesSearch(node, q) && matchesFilter(node)) set.add(node.pid)
    node.children?.forEach(walk)
  }
  processes.value.forEach(walk)
  return set
})

function isMatched(pid: number): boolean {
  return matchedPids.value.has(pid)
}

const filteredProcesses = computed(() => filterTree(processes.value))

const isFiltering = computed(() => searchQuery.value.trim() !== '')

// --- Expand / Collapse ---
function isExpanded(pid: number): boolean {
  const userPref = expandedPids.value.get(pid)
  if (userPref !== undefined) return userPref
  // Default: expanded during search (so matched subtrees are visible),
  // collapsed otherwise.
  return isFiltering.value
}

function toggleExpand(pid: number) {
  const newMap = new Map(expandedPids.value)
  newMap.set(pid, !isExpanded(pid))
  expandedPids.value = newMap
}

// --- Selection ---
function selectProcess(pid: number) {
  selectedPid.value = pid
  emit('select', pid)
}

function selectAllProcesses() {
  selectedPid.value = null
  emit('select', undefined)
}

const isAllSelected = computed(() => selectedPid.value === null)

// --- Context menu actions ---
// v0.9.0: Hack and Unhack are always tree-mode. To spare a single child,
// click Unhack on that node. Backend projection pushes a fresh snapshot
// after the strand applies the mutation; notify.ts swaps it into the
// shared `tree` ref and Vue re-renders — no explicit re-fetch needed.
async function hackProcess(pid: number) {
  try { await hijackProcess(pid) } catch {}
}

async function unhackProcess(pid: number) {
  try { await unhijackProcess(pid, true) } catch {}
}

// --- Hijacked status helpers ---
function isManuallyHijacked(node: ProcessInfo): boolean {
  return node.hijack_source === 'manual'
}

function isAutoHijacked(node: ProcessInfo): boolean {
  return node.hijack_source === 'auto'
}
</script>

<template>
  <div class="flex flex-col h-full overflow-hidden">
    <!-- Row 1: Search (h-12) -->
    <div class="h-12 flex items-center px-3 border-b border-slate-200 dark:border-slate-800/80 shrink-0">
      <div class="relative w-full">
        <Search class="absolute left-2.5 top-1/2 -translate-y-1/2 w-4 h-4 text-slate-400 dark:text-slate-500" />
        <input
          v-model="searchQuery"
          type="text"
          placeholder="Search PID or name..."
          class="w-full pl-9 pr-9 py-1.5 text-sm bg-white dark:bg-[#09090b] border border-slate-200 dark:border-slate-700 rounded-md focus:outline-none focus:ring-1 focus:ring-blue-500 focus:border-blue-500 transition-all placeholder:text-slate-400 dark:placeholder:text-slate-600 text-slate-800 dark:text-slate-200"
        />
        <button
          v-if="searchQuery"
          type="button"
          aria-label="Clear search"
          class="absolute right-2 top-1/2 -translate-y-1/2 p-0.5 rounded text-slate-400 dark:text-slate-500 hover:text-slate-700 dark:hover:text-slate-300 hover:bg-slate-100 dark:hover:bg-slate-800 transition-colors"
          @click="searchQuery = ''"
        >
          <X class="w-3.5 h-3.5" />
        </button>
      </div>
    </div>

    <!-- Row 2: Filter pills (h-12) -->
    <div class="h-12 flex items-center px-3 border-b border-slate-200 dark:border-slate-800/80 shrink-0">
      <div class="flex w-full p-0.5 bg-slate-200/50 dark:bg-[#09090b] rounded-md">
        <button
          v-for="f in (['all', 'proxied', 'direct'] as const)"
          :key="f"
          @click="filterTab = f"
          class="flex-1 text-xs font-medium py-1.5 rounded-[4px] transition-all capitalize"
          :class="filterTab === f
            ? 'bg-white dark:bg-slate-800 text-slate-800 dark:text-slate-200 shadow-sm'
            : 'text-slate-500 dark:text-slate-500 hover:text-slate-700 dark:hover:text-slate-300'"
        >{{ f }}</button>
      </div>
    </div>

    <!-- Row 3: All Processes button (h-12) -->
    <div class="h-12 flex items-center px-2 border-b border-slate-200 dark:border-slate-800 shrink-0">
      <button
        @click="selectAllProcesses"
        class="w-full flex items-center gap-2 px-2 py-1.5 text-sm font-medium rounded-md transition-colors"
        :class="isAllSelected
          ? 'bg-blue-50 dark:bg-blue-500/10 text-blue-700 dark:text-blue-400'
          : 'text-slate-600 dark:text-slate-400 hover:bg-slate-200/50 dark:hover:bg-slate-800/50'"
      >
        <Monitor class="w-4 h-4" />
        All Processes
        <span class="ml-auto text-[10px] font-mono text-slate-400 dark:text-slate-500">
          {{ totalProcessCount }}
        </span>
      </button>
    </div>

    <!-- Process Tree -->
    <div class="flex-1 overflow-y-auto select-none thin-scrollbar">
      <div class="pt-2 px-2 pb-4">
        <template v-if="filteredProcesses.length > 0">
          <ProcessTreeNode
            v-for="node in filteredProcesses"
            :key="node.pid"
            :node="node"
            :depth="0"
            :selected-pid="selectedPid"
            :is-expanded="isExpanded"
            :is-manually-hijacked="isManuallyHijacked"
            :is-auto-hijacked="isAutoHijacked"
            :is-pid-active="isPidActive"
            :is-subtree-active="isSubtreeActive"
            :is-matched="isMatched"
            @toggle-expand="toggleExpand"
            @select="selectProcess"
            @hack="hackProcess"
            @unhack="unhackProcess"
          />
        </template>
        <div v-else class="px-3 py-4 text-xs text-slate-400 dark:text-slate-500 text-center">
          No processes found.
        </div>
      </div>
    </div>
  </div>
</template>
