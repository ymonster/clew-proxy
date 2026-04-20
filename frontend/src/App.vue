<script setup lang="ts">
declare global {
  interface Window {
    chrome?: { webview?: { postMessage(msg: string): void } }
  }
}

import { ref, computed, onMounted, onUnmounted, shallowRef, defineAsyncComponent } from 'vue'
import { Tabs, TabsContent } from '@/components/ui/tabs'
import { TooltipProvider } from '@/components/ui/tooltip'
import ProcessTree from '@/components/ProcessTree.vue'
import ProcessContextHeader from '@/components/ProcessContextHeader.vue'
import NetworkActivities from '@/components/NetworkActivities.vue'
import AutoRules from '@/components/AutoRules.vue'
import ProxiesTab from '@/components/ProxiesTab.vue'
const ConfigEditor = defineAsyncComponent(() => import('@/components/ConfigEditor.vue'))
import RuleEditorDialog from '@/components/RuleEditorDialog.vue'
import { getStats, getProxyStatus, startProxy, stopProxy, getProcessDetail, hijackProcess, unhijackProcess, createAutoRule, updateAutoRule, deleteAutoRule } from '@/api/client'
import { useSSE } from '@/api/sse'
import { useTheme } from '@/composables/useTheme'
import { useStatusBus } from '@/composables/useStatusBus'
import type { Stats, ProxyStatus, ProcessInfo, AutoRule } from '@/api/types'
import {
  Sun,
  Moon,
  Activity,
  ArrowDown,
  ArrowUp,
  Wifi,
  Globe,
  Server,
  Minus,
  Square,
  Copy,
  X,
} from 'lucide-vue-next'

const { isDark, toggleTheme } = useTheme()
const { current: statusMessage, pushError } = useStatusBus()

// Window control (WebView2 postMessage)
const isMaximized = ref(document.documentElement.classList.contains('maximized'))
function windowCmd(cmd: string) {
  window.chrome?.webview?.postMessage(cmd)
}
// Watch for maximize class changes (set by C++ via execute_script)
const observer = new MutationObserver(() => {
  isMaximized.value = document.documentElement.classList.contains('maximized')
})
onMounted(() => observer.observe(document.documentElement, { attributes: true, attributeFilter: ['class'] }))
onUnmounted(() => observer.disconnect())

const selectedPid = ref<number | undefined>(undefined)
const selectedProcess = shallowRef<ProcessInfo | null>(null)

const stats = ref<Stats>({
  active_connections: 0,
  total_bytes_sent: 0,
  total_bytes_received: 0,
  hijacked_pids: 0,
  auto_rules_count: 0,
})

const proxyStatus = ref<ProxyStatus>({
  running: false,
  enabled: false,
  active_connections: 0,
})

const activeTab = ref('network')
const loading = ref(false)

// === Rule Editor Dialog (global, shared by ProcessContextHeader + AutoRules) ===
const autoRulesRef = ref<InstanceType<typeof AutoRules> | null>(null)
const ruleDialogOpen = ref(false)
const ruleDialogEditing = ref<AutoRule | null>(null)
const ruleDialogPrefillName = ref('')
const ruleDialogPrefillDir = ref('')

function openRuleDialog(rule?: AutoRule | null, prefillName = '', prefillDir = '') {
  ruleDialogEditing.value = rule ?? null
  ruleDialogPrefillName.value = prefillName
  ruleDialogPrefillDir.value = prefillDir
  ruleDialogOpen.value = true
}

async function refreshRules() {
  await fetchStatus()
  autoRulesRef.value?.fetchRules()
}

const ruleSaving = ref(false)

async function onRuleSave(payload: Omit<AutoRule, 'id' | 'matched_count' | 'excluded_count' | 'matched_pids'>) {
  ruleSaving.value = true
  try {
    if (ruleDialogEditing.value) {
      await updateAutoRule(ruleDialogEditing.value.id, payload)
    } else {
      await createAutoRule(payload)
    }
    ruleDialogOpen.value = false
    await refreshRules()
  } catch (e) {
    pushError(e, 'Save rule failed')
  } finally {
    ruleSaving.value = false
  }
}

// Delete from RuleEditorDialog
async function onRuleDeleteFromDialog(id: string) {
  try {
    await deleteAutoRule(id)
    ruleDialogOpen.value = false
    await refreshRules()
  } catch (e) {
    pushError(e, 'Delete rule failed')
  }
}

const processTreeRef = ref<InstanceType<typeof ProcessTree> | null>(null)

const sse = useSSE()

sse.on('proxy_status', (data) => {
  proxyStatus.value = data
})

sse.on('process_update', () => {
  processTreeRef.value?.fetchProcesses()
  refreshSelectedProcess()
})

sse.on('process_exit', () => {
  processTreeRef.value?.fetchProcesses()
  refreshSelectedProcess()
})

// Tab config — match mockup icons
const tabs = [
  { key: 'network',  label: 'Network Activities', icon: Wifi },
  { key: 'rules',    label: 'Rules',    icon: Activity },
  { key: 'proxies',  label: 'Proxies',  icon: Server },
  { key: 'settings', label: 'Settings', icon: Globe },
] as const

async function fetchStatus() {
  try {
    // /api/stats has no SSE event — always poll.
    stats.value = await getStats()
    // /api/proxy/status is pushed via SSE proxy_status event when SSE is up.
    // Only hit the endpoint as a fallback when SSE is disconnected.
    if (!sse.connected.value) {
      proxyStatus.value = await getProxyStatus()
    }
  } catch {
    // Backend not available yet
  }
}

async function toggleProxy() {
  loading.value = true
  try {
    if (proxyStatus.value.running) {
      await stopProxy()
    } else {
      await startProxy()
    }
    await fetchStatus()
  } catch (e) {
    pushError(e, 'Proxy engine toggle failed')
  } finally {
    loading.value = false
  }
}

async function onSelectProcess(pid: number | undefined) {
  selectedPid.value = pid
  if (pid != null) {
    try {
      const detail = await getProcessDetail(pid)
      if (selectedPid.value === pid) selectedProcess.value = detail
    } catch {
      if (selectedPid.value === pid) selectedProcess.value = null
    }
  } else {
    selectedProcess.value = null
  }
}

// === Hack/Unhack from ProcessContextHeader ===

function hasHijackedDescendant(node: ProcessInfo): boolean {
  if (node.children) {
    for (const child of node.children) {
      if (child.hijacked || hasHijackedDescendant(child)) return true
    }
  }
  return false
}

const headerHasHijackedDescendants = computed(() => {
  const p = selectedProcess.value
  if (!p) return false
  return hasHijackedDescendant(p)
})

const headerHasChildren = computed(() => {
  return !!selectedProcess.value?.children?.length
})

async function headerHack(pid: number) {
  try {
    await hijackProcess(pid)  // default tree=true
    await refreshSelectedProcess()
  } catch (e) {
    pushError(e, 'Hijack failed')
  }
}

async function headerUnhack(pid: number) {
  try {
    await unhijackProcess(pid)  // default tree=false (single)
    await refreshSelectedProcess()
  } catch (e) {
    pushError(e, 'Unhijack failed')
  }
}

async function headerUnhackTree(node: ProcessInfo) {
  try {
    await unhijackProcess(node.pid, true)  // tree=true (cascade)
    await refreshSelectedProcess()
  } catch (e) {
    pushError(e, 'Unhijack tree failed')
  }
}

async function refreshSelectedProcess() {
  const pid = selectedPid.value
  if (pid != null) {
    try {
      const detail = await getProcessDetail(pid)
      if (selectedPid.value === pid) selectedProcess.value = detail
    } catch {
      if (selectedPid.value === pid) selectedProcess.value = null
    }
  }
}

function onCreateRuleFromProcess() {
  if (!selectedProcess.value) return
  const proc = selectedProcess.value
  // Extract working dir from cmdline
  const cmd = proc.cmdline || ''
  let exePath = ''
  if (cmd.startsWith('"')) {
    const end = cmd.indexOf('"', 1)
    if (end > 0) exePath = cmd.slice(1, end)
  } else {
    const space = cmd.indexOf(' ')
    exePath = space > 0 ? cmd.slice(0, space) : cmd
  }
  let workDir = ''
  const lastSep = Math.max(exePath.lastIndexOf('/'), exePath.lastIndexOf('\\'))
  if (lastSep > 0) workDir = exePath.slice(0, lastSep + 1)

  openRuleDialog(null, proc.name, workDir)
}

function formatRate(bytes: number): string {
  if (bytes < 1024) return `${bytes} B/s`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB/s`
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB/s`
}

let statusTimer: ReturnType<typeof setInterval> | null = null

onMounted(() => {
  fetchStatus()
  statusTimer = setInterval(fetchStatus, 3000)
})

onUnmounted(() => {
  if (statusTimer) clearInterval(statusTimer)
})
</script>

<template>
  <TooltipProvider :delay-duration="300">
    <div class="h-screen w-full flex flex-col bg-slate-50 dark:bg-[#09090b] text-slate-800 dark:text-slate-200 overflow-hidden transition-colors duration-200">

      <!-- ==================== TITLE BAR (h-10, draggable) ==================== -->
      <header class="titlebar h-10 bg-white dark:bg-[#18181b] border-b border-slate-200 dark:border-slate-800 flex items-center justify-between shrink-0 transition-colors select-none">
        <div class="flex items-center gap-6 pl-4" style="-webkit-app-region: no-drag">
          <div class="flex items-center gap-2 font-bold text-slate-800 dark:text-slate-100">
            <!-- Clew mark: stylized C with thread + needle -->
            <svg viewBox="0 0 256 256" class="w-4 h-4" fill="none" stroke="currentColor" stroke-width="18" stroke-linecap="round" stroke-linejoin="round" aria-label="Clew">
              <path d="M 185 79 A 69 69 0 1 0 186 181" />
              <path d="M 185 79 L 228 56" />
              <circle cx="232" cy="54" r="12" fill="currentColor" stroke="none" />
            </svg>
            <span class="text-sm">Clew <span class="text-slate-400 dark:text-slate-500 font-normal text-xs ml-0.5">v0.7.0</span></span>
          </div>

          <div class="flex items-center gap-3 text-xs font-medium text-slate-600 dark:text-slate-400">
            <div
              class="flex items-center gap-1.5 px-2 py-0.5 rounded border transition-colors"
              :class="proxyStatus.running
                ? 'bg-emerald-50 dark:bg-emerald-500/10 text-emerald-700 dark:text-emerald-400 border-emerald-100 dark:border-emerald-500/20'
                : 'bg-slate-100 dark:bg-slate-800 text-slate-500 dark:text-slate-400 border-slate-200 dark:border-slate-700'"
            >
              <div
                class="w-1.5 h-1.5 rounded-full"
                :class="proxyStatus.running ? 'bg-emerald-500 dark:bg-emerald-400 animate-pulse' : 'bg-slate-400'"
              />
              {{ proxyStatus.running ? 'Running' : 'Stopped' }}
            </div>
            <div class="flex items-center gap-1">
              <Activity class="w-3 h-3 text-slate-400 dark:text-slate-500" />
              {{ stats.active_connections }}
            </div>
            <div class="flex items-center gap-1 text-amber-600 dark:text-amber-500">
              <ArrowDown class="w-3 h-3" />
              {{ formatRate(stats.total_bytes_received) }}
            </div>
            <div class="flex items-center gap-1 text-blue-600 dark:text-blue-400">
              <ArrowUp class="w-3 h-3" />
              {{ formatRate(stats.total_bytes_sent) }}
            </div>
          </div>
        </div>

        <!-- Window controls (no-drag) -->
        <div class="flex items-center h-full" style="-webkit-app-region: no-drag">
          <button
            @click="toggleTheme"
            class="h-full px-3 text-slate-500 dark:text-slate-400 hover:bg-slate-100 dark:hover:bg-slate-800 transition-colors"
            title="Toggle theme"
          >
            <Sun v-if="!isDark" class="w-3.5 h-3.5" />
            <Moon v-else class="w-3.5 h-3.5" />
          </button>
          <button
            @click="windowCmd('minimize')"
            class="h-full px-3 text-slate-500 dark:text-slate-400 hover:bg-slate-100 dark:hover:bg-slate-800 transition-colors"
            title="Minimize"
          >
            <Minus class="w-3.5 h-3.5" />
          </button>
          <button
            @click="windowCmd('maximize')"
            class="h-full px-3 text-slate-500 dark:text-slate-400 hover:bg-slate-100 dark:hover:bg-slate-800 transition-colors"
            title="Maximize"
          >
            <Copy v-if="isMaximized" class="w-3 h-3" />
            <Square v-else class="w-3 h-3" />
          </button>
          <button
            @click="windowCmd('close')"
            class="h-full px-3 text-slate-500 dark:text-slate-400 hover:bg-red-500 hover:text-white transition-colors"
            title="Close"
          >
            <X class="w-3.5 h-3.5" />
          </button>
        </div>
      </header>

      <!-- ==================== MAIN BODY ==================== -->
      <div class="flex flex-1 overflow-hidden relative">

        <!-- ==================== LEFT SIDEBAR ==================== -->
        <aside class="relative flex flex-col border-r border-slate-200 dark:border-slate-800 shrink-0 bg-slate-50 dark:bg-[#18181b]/50 transition-colors" style="width: 300px; min-width: 200px;">
          <ProcessTree ref="processTreeRef" class="flex-1 min-h-0" @select="onSelectProcess" />
        </aside>

        <!-- ==================== RIGHT MAIN AREA ==================== -->
        <main class="flex-1 flex flex-col bg-white dark:bg-[#09090b] min-w-0 transition-colors overflow-hidden">
          <Tabs v-model="activeTab" class="flex flex-col flex-1 min-h-0">

            <!-- Tab navigation (h-12) — match mockup exactly -->
            <nav class="h-12 flex px-4 border-b border-slate-200 dark:border-slate-800 bg-slate-50/50 dark:bg-[#18181b]/50 transition-colors shrink-0">
              <button
                v-for="tab in tabs"
                :key="tab.key"
                @click="activeTab = tab.key"
                class="flex items-center gap-1.5 px-4 h-full text-sm font-medium border-b-2 transition-colors"
                :class="activeTab === tab.key
                  ? 'border-blue-600 text-blue-600 dark:border-blue-500 dark:text-blue-400'
                  : 'border-transparent text-slate-500 hover:text-slate-700 dark:text-slate-400 dark:hover:text-slate-200 hover:border-slate-300 dark:hover:border-slate-700'"
              >
                <component :is="tab.icon" class="w-4 h-4" />
                {{ tab.label }}
                <!-- Badge: active rules count on Rules tab -->
                <span
                  v-if="tab.key === 'rules' && stats.auto_rules_count > 0"
                  class="px-1.5 py-0.5 rounded-full text-xs font-bold transition-colors"
                  :class="activeTab === 'rules'
                    ? 'bg-blue-100 text-blue-700 dark:bg-blue-500/10 dark:text-blue-400'
                    : 'bg-slate-100 text-slate-600 dark:bg-slate-800 dark:text-slate-400'"
                >{{ stats.auto_rules_count }}</span>
              </button>
            </nav>

            <!-- Process Context Header — only on Network tab -->
            <ProcessContextHeader
              v-if="activeTab === 'network'"
              :process="selectedProcess"
              :has-hijacked-descendants="headerHasHijackedDescendants"
              :has-children="headerHasChildren"
              @create-rule="onCreateRuleFromProcess"
              @hack="headerHack"
              @unhack="headerUnhack"
              @unhack-tree="headerUnhackTree"
            />

            <TabsContent value="network" class="flex-1 m-0 min-h-0 data-[state=active]:flex data-[state=active]:flex-col overflow-hidden">
              <NetworkActivities :selectedPid="selectedPid" />
            </TabsContent>

            <TabsContent value="rules" class="flex-1 m-0 min-h-0 data-[state=active]:flex data-[state=active]:flex-col overflow-hidden">
              <AutoRules ref="autoRulesRef" @open-add="openRuleDialog()" @open-edit="openRuleDialog($event)" @rules-changed="fetchStatus" />
            </TabsContent>

            <TabsContent value="proxies" class="flex-1 m-0 min-h-0 data-[state=active]:flex data-[state=active]:flex-col overflow-hidden">
              <ProxiesTab />
            </TabsContent>

            <TabsContent value="settings" class="flex-1 m-0 p-4 min-h-0 data-[state=active]:flex data-[state=active]:flex-col">
              <Suspense>
                <ConfigEditor />
                <template #fallback>
                  <div class="flex-1 flex items-center justify-center text-sm text-slate-400">Loading editor...</div>
                </template>
              </Suspense>
            </TabsContent>
          </Tabs>
        </main>
      </div>

      <!-- ==================== FOOTER (h-8) ==================== -->
      <footer class="h-8 bg-slate-50 dark:bg-[#18181b] border-t border-slate-200 dark:border-slate-800 flex items-center justify-between px-4 text-xs text-slate-500 dark:text-slate-400 font-medium shrink-0 transition-colors select-none">
        <div class="flex items-center gap-6">
          <button
            class="flex items-center gap-1.5 hover:text-slate-700 dark:hover:text-slate-200 transition-colors"
            :disabled="loading"
            @click="toggleProxy"
          >
            <span>Proxy Engine:</span>
            <span
              class="font-semibold flex items-center gap-1"
              :class="proxyStatus.running ? 'text-emerald-600 dark:text-emerald-400' : 'text-slate-500'"
            >
              <div class="w-1.5 h-1.5 rounded-full" :class="proxyStatus.running ? 'bg-emerald-500 dark:bg-emerald-400' : 'bg-slate-400'" />
              {{ proxyStatus.running ? 'Active' : 'Stopped' }}
            </span>
          </button>
          <div class="flex items-center gap-1.5">
            <span>Listen Port:</span>
            <span class="text-slate-700 dark:text-slate-300 font-mono tracking-tight">18080</span>
          </div>
        </div>
        <div
          v-if="statusMessage"
          class="flex-1 min-w-0 mx-6 text-center truncate font-medium"
          :class="{
            'text-red-600 dark:text-red-400': statusMessage.kind === 'error',
            'text-emerald-600 dark:text-emerald-400': statusMessage.kind === 'success',
            'text-slate-700 dark:text-slate-300': statusMessage.kind === 'info',
          }"
          :title="statusMessage.text"
        >
          {{ statusMessage.text }}
        </div>
        <div class="flex items-center gap-6">
          <div v-if="sse.connected.value" class="flex items-center gap-1.5">
            <div class="w-1.5 h-1.5 rounded-full bg-emerald-500 dark:bg-emerald-400" />
            <span>SSE Connected</span>
          </div>
          <div v-else class="flex items-center gap-1.5 text-red-400">
            <div class="w-1.5 h-1.5 rounded-full bg-red-500" />
            <span>SSE Disconnected</span>
          </div>
          <div class="flex items-center gap-1.5">
            <div class="w-1.5 h-1.5 rounded-full bg-emerald-500 dark:bg-emerald-400" />
            <span>Ready</span>
          </div>
        </div>
      </footer>
    </div>

    <!-- ==================== GLOBAL RULE EDITOR DIALOG ==================== -->
    <RuleEditorDialog
      :open="ruleDialogOpen"
      :rule="ruleDialogEditing"
      :saving="ruleSaving"
      :prefill-process-name="ruleDialogPrefillName"
      :prefill-work-dir="ruleDialogPrefillDir"
      @update:open="ruleDialogOpen = $event"
      @save="onRuleSave"
      @delete="onRuleDeleteFromDialog"
    />
  </TooltipProvider>
</template>
