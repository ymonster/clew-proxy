<script setup lang="ts">
import { ref, watch, onMounted, onUnmounted, computed, useId } from 'vue'
// JSON-only Monaco build: import the core API + JSON language contribution only,
// so Vite can tree-shake away the 80+ unused languages.
import * as monaco from 'monaco-editor/esm/vs/editor/editor.api.js'
import 'monaco-editor/esm/vs/language/json/monaco.contribution'
import EditorWorker from 'monaco-editor/esm/vs/editor/editor.worker?worker'
import JsonWorker from 'monaco-editor/esm/vs/language/json/json.worker?worker'

// Tell Monaco which worker to spawn per language.
// Must be set before any `monaco.editor.create` call.
interface MonacoEnv {
  MonacoEnvironment?: { getWorker: (workerId: string, label: string) => Worker }
}
const globalScope = globalThis as unknown as MonacoEnv
if (!globalScope.MonacoEnvironment) {
  globalScope.MonacoEnvironment = {
    getWorker(_workerId: string, label: string) {
      if (label === 'json') return new JsonWorker()
      return new EditorWorker()
    },
  }
}

import { Button } from '@/components/ui/button'
import { Badge } from '@/components/ui/badge'
import { Label } from '@/components/ui/label'
import { getConfig, updateConfig, getAutostart, setAutostart } from '@/api/client'
import { Save } from 'lucide-vue-next'
import { useTheme } from '@/composables/useTheme'
import { CLEW_VERSION } from '@/version'

const { isDark } = useTheme()

const editorContainer = ref<HTMLDivElement | null>(null)
let editor: monaco.editor.IStandaloneCodeEditor | null = null

const savedContent = ref('')
const currentContent = ref('')
const saving = ref(false)
const saveMessage = ref<{ type: 'success' | 'error'; text: string } | null>(null)
const closeToTray = ref(false)
const editorOpen = ref(false)

// Autostart (Windows Task Scheduler entry; not persisted in clew.json)
const autostartEnabled = ref(false)
const autostartMinimized = ref(false)
const autostartMessage = ref<string | null>(null)

// DNS settings
const dnsEnabled = ref(false)
const dnsMode = ref('forwarder')
const dnsUpstream = ref('')
const dnsSaveMessage = ref<string | null>(null)

// Log level — quill switches at runtime via clew::set_log_level (wired in
// app.cpp's config_store observer chain). Changing this dropdown reloads
// the live process; no restart needed.
const logLevel = ref('info')
const logLevelMessage = ref<string | null>(null)

// a11y: unique ids for label/field association
const dnsModeId = useId()
const dnsUpstreamId = useId()
const logLevelId = useId()

const hasUnsavedChanges = computed(() => {
  return currentContent.value !== savedContent.value
})

async function fetchConfig(options: { pushToEditor?: boolean } = {}) {
  try {
    const config = await getConfig()
    const json = JSON.stringify(config, null, 2)
    const ui = (config as Record<string, unknown>).ui as Record<string, unknown> | undefined
    if (ui?.close_to_tray !== undefined) {
      closeToTray.value = ui.close_to_tray as boolean
    }
    const dns = (config as Record<string, unknown>).dns as Record<string, unknown> | undefined
    if (dns) {
      dnsEnabled.value = !!dns.enabled
      dnsMode.value = (dns.mode as string) || 'forwarder'
      dnsUpstream.value = (dns.upstream_host as string) || ''
    }
    const lvl = (config as Record<string, unknown>).log_level as string | undefined
    if (lvl) logLevel.value = lvl
    // Only overwrite the Monaco editor when explicitly requested (initial open),
    // OR when there are no unsaved local edits. Otherwise preserve user's work.
    const shouldPush = options.pushToEditor === true
      || (editor !== null && !hasUnsavedChanges.value)
    if (shouldPush) {
      savedContent.value = json
      currentContent.value = json
      if (editor) editor.setValue(json)
    }
  } catch {
    // Backend not available
  }
}

async function saveLogLevel() {
  logLevelMessage.value = null
  try {
    const config = await getConfig() as Record<string, unknown>
    config.log_level = logLevel.value
    await updateConfig(config)
    logLevelMessage.value = 'Saved'
    setTimeout(() => { logLevelMessage.value = null }, 2000)
    await fetchConfig()
  } catch {
    logLevelMessage.value = 'Save failed'
  }
}

async function saveDns() {
  dnsSaveMessage.value = null
  try {
    const config = await getConfig() as Record<string, unknown>
    const dns = (config.dns || {}) as Record<string, unknown>
    dns.enabled = dnsEnabled.value
    dns.mode = dnsMode.value
    dns.upstream_host = dnsUpstream.value
    dns.upstream_port = 53
    if (!dns.listen_host) dns.listen_host = '127.0.0.2'
    if (!dns.listen_port) dns.listen_port = 53
    config.dns = dns
    await updateConfig(config)
    dnsSaveMessage.value = 'Saved'
    setTimeout(() => { dnsSaveMessage.value = null }, 2000)
    await fetchConfig()  // refresh local state only; preserves unsaved Monaco edits
  } catch {
    dnsSaveMessage.value = 'Save failed'
  }
}

async function saveConfig() {
  if (!editor) return
  saving.value = true
  saveMessage.value = null
  try {
    const content = editor.getValue()
    const parsed = JSON.parse(content) as Record<string, unknown>
    await updateConfig(parsed)
    savedContent.value = content
    currentContent.value = content
    saveMessage.value = { type: 'success', text: 'Config saved and reloaded' }
    setTimeout(() => {
      saveMessage.value = null
    }, 3000)
  } catch (err) {
    let message: string
    if (err instanceof SyntaxError) message = 'Invalid JSON syntax'
    else if (err instanceof Error) message = err.message
    else message = 'Failed to save config'
    saveMessage.value = { type: 'error', text: message }
  } finally {
    saving.value = false
  }
}

async function fetchAutostart() {
  try {
    const s = await getAutostart()
    autostartEnabled.value = s.enabled
    autostartMinimized.value = s.start_minimized
  } catch {
    // Backend not available; leave defaults (both false).
  }
}

async function applyAutostart(next: { enabled: boolean; start_minimized: boolean }) {
  autostartMessage.value = null
  // Optimistic update so the toggle feels immediate; revert on failure.
  const prev = { enabled: autostartEnabled.value, start_minimized: autostartMinimized.value }
  autostartEnabled.value = next.enabled
  autostartMinimized.value = next.start_minimized
  try {
    const result = await setAutostart(next)
    autostartEnabled.value = result.enabled
    autostartMinimized.value = result.start_minimized
  } catch (err) {
    autostartEnabled.value = prev.enabled
    autostartMinimized.value = prev.start_minimized
    autostartMessage.value = err instanceof Error ? err.message : 'Failed to update autostart'
    setTimeout(() => { autostartMessage.value = null }, 4000)
  }
}

async function toggleAutostart() {
  await applyAutostart({
    enabled: !autostartEnabled.value,
    start_minimized: autostartMinimized.value,
  })
}

async function toggleStartMinimized() {
  if (!autostartEnabled.value) return
  await applyAutostart({
    enabled: true,
    start_minimized: !autostartMinimized.value,
  })
}

async function toggleCloseToTray() {
  try {
    const config = await getConfig() as Record<string, unknown>
    const ui = (config.ui || {}) as Record<string, unknown>
    ui.close_to_tray = !closeToTray.value
    config.ui = ui
    await updateConfig(config)
    closeToTray.value = !closeToTray.value
    // Refresh Monaco editor content
    await fetchConfig()
  } catch {
    // revert on error
  }
}

function initEditor() {
  if (!editorContainer.value || editor) return
  editor = monaco.editor.create(editorContainer.value, {
    value: currentContent.value,
    language: 'json',
    theme: isDark.value ? 'vs-dark' : 'vs',
    automaticLayout: true,
    minimap: { enabled: false },
    scrollBeyondLastLine: false,
    fontSize: 13,
    lineNumbers: 'on',
    tabSize: 2,
    wordWrap: 'on',
    renderLineHighlight: 'line',
    padding: { top: 8, bottom: 8 },
  })

  editor.onDidChangeModelContent(() => {
    if (editor) {
      currentContent.value = editor.getValue()
    }
  })

  editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, () => {
    saveConfig()
  })
}

async function openEditor() {
  editorOpen.value = true
  await fetchConfig({ pushToEditor: true })
  // Wait for DOM to render the container, then init
  setTimeout(() => initEditor(), 0)
}

function closeEditor() {
  if (hasUnsavedChanges.value) {
    if (!confirm('You have unsaved changes. Discard?')) return
  }
  editorOpen.value = false
  if (editor) {
    editor.dispose()
    editor = null
  }
}

onMounted(() => {
  // Only fetch close-to-tray + autostart state, don't init editor
  fetchConfig()
  fetchAutostart()
})

watch(isDark, (dark) => {
  if (editor) {
    monaco.editor.setTheme(dark ? 'vs-dark' : 'vs')
  }
})

onUnmounted(() => {
  if (editor) {
    editor.dispose()
    editor = null
  }
})
</script>

<template>
  <div class="flex flex-col h-full overflow-auto">
    <div class="p-6 flex flex-col gap-6 flex-1 min-h-0">
      <!-- General Section -->
      <div class="max-w-xl">
        <h2 class="text-base font-semibold text-slate-800 dark:text-slate-100 mb-1">General</h2>
        <p class="text-xs text-slate-500 dark:text-slate-400 mb-4">Global configuration for Clew behavior.</p>
        <div class="rounded-lg border border-slate-200 dark:border-slate-800 overflow-hidden divide-y divide-slate-200 dark:divide-slate-800">
          <div class="flex items-center justify-between px-4 py-3 bg-white dark:bg-[#18181b]">
            <div>
              <p class="text-sm font-medium text-slate-700 dark:text-slate-200">Close to System Tray</p>
              <p class="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                When enabled, closing the window hides it to the system tray instead of exiting.
              </p>
            </div>
            <button
              class="relative shrink-0 ml-4 inline-flex h-5 w-9 items-center rounded-full transition-colors focus:outline-none focus:ring-2 focus:ring-blue-500 focus:ring-offset-2 dark:focus:ring-offset-[#18181b]"
              :class="closeToTray ? 'bg-blue-600' : 'bg-slate-300 dark:bg-slate-600'"
              @click="toggleCloseToTray"
            >
              <span
                class="inline-block h-4 w-4 transform rounded-full bg-white shadow transition-transform"
                :class="closeToTray ? 'translate-x-[18px]' : 'translate-x-0.5'"
              />
            </button>
          </div>

          <!-- Autostart on Logon -->
          <div class="px-4 py-3 bg-white dark:bg-[#18181b]">
            <div class="flex items-center justify-between">
              <div>
                <p class="text-sm font-medium text-slate-700 dark:text-slate-200">Start Clew at logon</p>
                <p class="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                  Clew runs as administrator (UAC). Implemented via Windows Task Scheduler
                  (`ClewAutoStart`) to skip the UAC prompt at every login. Re-toggle if you
                  move the clew.exe path.
                </p>
              </div>
              <button
                class="relative shrink-0 ml-4 inline-flex h-5 w-9 items-center rounded-full transition-colors focus:outline-none focus:ring-2 focus:ring-blue-500 focus:ring-offset-2 dark:focus:ring-offset-[#18181b]"
                :class="autostartEnabled ? 'bg-blue-600' : 'bg-slate-300 dark:bg-slate-600'"
                @click="toggleAutostart"
              >
                <span
                  class="inline-block h-4 w-4 transform rounded-full bg-white shadow transition-transform"
                  :class="autostartEnabled ? 'translate-x-[18px]' : 'translate-x-0.5'"
                />
              </button>
            </div>

            <div class="mt-3 pl-4 flex items-center justify-between"
                 :class="autostartEnabled ? '' : 'opacity-50'">
              <div>
                <p class="text-xs font-medium text-slate-700 dark:text-slate-200">Start minimized to tray</p>
                <p class="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5">
                  Clew launches without a visible window — only the tray icon. Only effective when "Start at logon" is on.
                </p>
              </div>
              <button
                :disabled="!autostartEnabled"
                class="relative shrink-0 ml-4 inline-flex h-5 w-9 items-center rounded-full transition-colors focus:outline-none focus:ring-2 focus:ring-blue-500 focus:ring-offset-2 dark:focus:ring-offset-[#18181b] disabled:cursor-not-allowed"
                :class="autostartMinimized ? 'bg-blue-600' : 'bg-slate-300 dark:bg-slate-600'"
                @click="toggleStartMinimized"
              >
                <span
                  class="inline-block h-4 w-4 transform rounded-full bg-white shadow transition-transform"
                  :class="autostartMinimized ? 'translate-x-[18px]' : 'translate-x-0.5'"
                />
              </button>
            </div>
            <p v-if="autostartMessage" class="mt-2 text-xs text-red-500">{{ autostartMessage }}</p>
          </div>

          <!-- DNS Proxy -->
          <div class="px-4 py-3 bg-white dark:bg-[#18181b]">
            <div class="flex items-center justify-between">
              <div>
                <p class="text-sm font-medium text-slate-700 dark:text-slate-200">DNS Proxy</p>
                <p class="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                  Route DNS queries through the proxy. Required when system DNS cannot resolve proxied services.
                </p>
              </div>
              <button
                class="relative shrink-0 ml-4 inline-flex h-5 w-9 items-center rounded-full transition-colors focus:outline-none focus:ring-2 focus:ring-blue-500 focus:ring-offset-2 dark:focus:ring-offset-[#18181b]"
                :class="dnsEnabled ? 'bg-blue-600' : 'bg-slate-300 dark:bg-slate-600'"
                @click="dnsEnabled = !dnsEnabled; saveDns()"
              >
                <span
                  class="inline-block h-4 w-4 transform rounded-full bg-white shadow transition-transform"
                  :class="dnsEnabled ? 'translate-x-[18px]' : 'translate-x-0.5'"
                />
              </button>
            </div>

            <div v-if="dnsEnabled" class="mt-3 pl-0 space-y-3">
              <div class="flex items-center gap-3">
                <Label :for="dnsModeId" class="text-xs text-slate-500 dark:text-slate-400 w-20 shrink-0">Mode</Label>
                <select
                  :id="dnsModeId"
                  v-model="dnsMode"
                  @change="saveDns"
                  class="h-8 px-2 text-sm border border-slate-200 dark:border-slate-700 rounded bg-slate-50 dark:bg-[#101922] text-slate-800 dark:text-slate-200 focus:outline-none focus:ring-1 focus:ring-blue-500"
                >
                  <option value="forwarder">Forwarder (global, requires system DNS → 127.0.0.2)</option>
                </select>
              </div>
              <div class="flex items-center gap-3">
                <Label :for="dnsUpstreamId" class="text-xs text-slate-500 dark:text-slate-400 w-20 shrink-0">Upstream</Label>
                <input
                  :id="dnsUpstreamId"
                  v-model="dnsUpstream"
                  @change="saveDns"
                  type="text"
                  placeholder="DNS server"
                  class="h-8 px-2 text-sm font-mono border border-slate-200 dark:border-slate-700 rounded bg-slate-50 dark:bg-[#101922] text-slate-800 dark:text-slate-200 focus:outline-none focus:ring-1 focus:ring-blue-500 w-40"
                />
                <span class="text-xs text-slate-500 dark:text-slate-400 font-mono">:53</span>
              </div>
              <p v-if="dnsSaveMessage" class="text-xs text-green-500">{{ dnsSaveMessage }}</p>
            </div>
          </div>

          <!-- Log Level -->
          <div class="px-4 py-3 bg-white dark:bg-[#18181b]">
            <div class="flex items-center justify-between">
              <div>
                <p class="text-sm font-medium text-slate-700 dark:text-slate-200">Log Level</p>
                <p class="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                  Runtime log verbosity. Switching takes effect immediately, no restart needed.
                  <code class="font-mono">debug</code> writes per-event diagnostics to <code class="font-mono">clew.log</code>.
                </p>
              </div>
              <div class="flex items-center gap-2 ml-4 shrink-0">
                <Label :for="logLevelId" class="sr-only">Log level</Label>
                <select
                  :id="logLevelId"
                  v-model="logLevel"
                  @change="saveLogLevel"
                  class="h-8 px-2 text-sm border border-slate-200 dark:border-slate-700 rounded bg-slate-50 dark:bg-[#101922] text-slate-800 dark:text-slate-200 focus:outline-none focus:ring-1 focus:ring-blue-500"
                >
                  <option value="debug">debug</option>
                  <option value="info">info</option>
                  <option value="warning">warning</option>
                  <option value="error">error</option>
                </select>
                <span v-if="logLevelMessage" class="text-xs text-green-500">{{ logLevelMessage }}</span>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- Configuration File Section -->
      <div class="flex-1 min-h-0 flex flex-col" :class="{ 'max-w-xl': !editorOpen }">
        <div v-if="!editorOpen" class="rounded-lg border border-slate-200 dark:border-slate-800 overflow-hidden">
          <div class="flex items-center justify-between px-4 py-3 bg-white dark:bg-[#18181b]">
            <div>
              <p class="text-sm font-medium text-slate-700 dark:text-slate-200">Configuration File</p>
              <p class="text-xs text-slate-500 dark:text-slate-400 mt-0.5">Edit raw JSON config for advanced tuning.</p>
            </div>
            <Button size="sm" variant="outline" @click="openEditor">
              Edit
            </Button>
          </div>
        </div>

        <template v-else>
          <div class="flex items-center justify-between mb-3">
            <div class="flex items-center gap-2">
              <h2 class="text-base font-semibold text-slate-800 dark:text-slate-100">Configuration File</h2>
              <Badge
                v-if="hasUnsavedChanges"
                variant="outline"
                class="text-orange-400 border-orange-400/40"
              >
                Unsaved changes
              </Badge>
              <span
                v-if="saveMessage"
                class="text-xs"
                :class="saveMessage.type === 'success' ? 'text-green-500' : 'text-red-500'"
              >
                {{ saveMessage.text }}
              </span>
            </div>
            <div class="flex items-center gap-2">
              <Button
                size="sm"
                :disabled="saving || !hasUnsavedChanges"
                @click="saveConfig"
              >
                <Save class="size-4 mr-1" />
                {{ saving ? 'Saving...' : 'Save & Reload' }}
              </Button>
              <Button size="sm" variant="outline" @click="closeEditor">
                Close
              </Button>
            </div>
          </div>

          <!-- Monaco Editor -->
          <div
            ref="editorContainer"
            class="flex-1 min-h-[300px] rounded-md border border-slate-200 dark:border-slate-800 overflow-hidden"
          />
        </template>
      </div>

      <!-- About Section -->
      <div class="max-w-xl">
        <h2 class="text-base font-semibold text-slate-800 dark:text-slate-100 mb-1">About</h2>
        <div class="rounded-lg border border-slate-200 dark:border-slate-800 overflow-hidden">
          <div class="px-4 py-3 bg-white dark:bg-[#18181b] space-y-1">
            <p class="text-xs text-slate-500 dark:text-slate-400">
              Clew <span class="font-mono text-slate-700 dark:text-slate-300">v{{ CLEW_VERSION }}</span>
            </p>
            <p class="text-xs text-slate-500 dark:text-slate-400">
              Windows 进程级流量代理 ·
              <a href="https://github.com/ymonster/clew-proxy" target="_blank"
                 class="text-blue-600 dark:text-blue-400 hover:underline">github.com/ymonster/clew-proxy</a>
            </p>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
