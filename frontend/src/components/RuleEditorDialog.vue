<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogFooter,
} from '@/components/ui/dialog'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import {
  Tooltip,
  TooltipContent,
  TooltipTrigger,
} from '@/components/ui/tooltip'
import TagInput from '@/components/TagInput.vue'
import { getProxyGroups, browseExe as apiBrowseExe } from '@/api/client'
import type { AutoRule, ProxyTarget, ProxyGroup, TrafficFilter, RuleProtocol } from '@/api/types'
import {
  ChevronRight,
  HelpCircle,
  FolderOpen,
  Terminal,
  Lock,
  Check,
  SlidersHorizontal,
  Trash2,
} from 'lucide-vue-next'

const props = defineProps<{
  open: boolean
  rule: AutoRule | null
  prefillProcessName?: string
  prefillWorkDir?: string
  saving?: boolean
}>()

const emit = defineEmits<{
  'update:open': [value: boolean]
  save: [payload: Omit<AutoRule, 'id' | 'matched_count' | 'excluded_count' | 'matched_pids'>]
  delete: [id: string]
}>()

const isEditing = computed(() => props.rule != null)

// Form state
const formName = ref('')
const formProcessName = ref('')
const formWorkDir = ref('')
const formUseWorkDir = ref(false)
const formCmdlinePattern = ref('')
const formHackTree = ref(false)
const formProtocol = ref<RuleProtocol>('tcp')
const proxyGroups = ref<ProxyGroup[]>([])
const formProxyGroupId = ref('0')
const isCustomProxy = ref(false)
const customProxyHost = ref('')
const formExcludeCidrs = ref<string[]>([])
const formIncludePorts = ref<string[]>([])
const advancedOpen = ref(false)

const fileInput = ref<HTMLInputElement | null>(null)

// Reset form when dialog opens
watch(() => props.open, async (open) => {
  if (open) {
    confirmingDelete.value = false
    try { proxyGroups.value = await getProxyGroups() } catch {}
    if (props.rule) {
      formName.value = props.rule.name
      formProcessName.value = props.rule.process_name
      formCmdlinePattern.value = props.rule.cmdline_pattern
      formHackTree.value = props.rule.hack_tree
      formProtocol.value = props.rule.protocol || 'tcp'
      formProxyGroupId.value = String(props.rule.proxy_group_id ?? 0)
      isCustomProxy.value = false
      customProxyHost.value = ''
      formExcludeCidrs.value = [...props.rule.dst_filter.exclude_cidrs]
      formIncludePorts.value = [...props.rule.dst_filter.include_ports]
      formWorkDir.value = props.rule.image_path_pattern || ''
      formUseWorkDir.value = !!props.rule.image_path_pattern
      advancedOpen.value = props.rule.dst_filter.exclude_cidrs.length > 0 || props.rule.dst_filter.include_ports.length > 0
    } else {
      const pName = props.prefillProcessName || ''
      const pDir = props.prefillWorkDir || ''
      formName.value = pName ? `Rule for ${pName}` : ''
      formProcessName.value = pName
      formCmdlinePattern.value = ''
      formHackTree.value = true
      formProtocol.value = 'tcp'
      formProxyGroupId.value = '0'
      isCustomProxy.value = false
      customProxyHost.value = ''
      formExcludeCidrs.value = []
      formIncludePorts.value = []
      formWorkDir.value = pDir
      formUseWorkDir.value = !!pDir
      advancedOpen.value = false
    }
  }
})

async function browseExe() {
  try {
    const data = await apiBrowseExe()
    if (data.cancelled) return
    if (data.name) formProcessName.value = data.name
    if (data.dir) formWorkDir.value = data.dir
    // Auto-enable: only turn ON, never auto-turn-off (spec §10.3)
    formUseWorkDir.value = true
  } catch {
    // Backend not available, fall back to HTML file picker
    fileInput.value?.click()
  }
}

function onExeSelected(event: Event) {
  const target = event.target as HTMLInputElement
  if (target.files && target.files.length > 0) {
    const file = target.files[0]
    if (!file) return
    formProcessName.value = file.name
    // HTML file picker can't provide full path (browser security)
    formWorkDir.value = ''
    formUseWorkDir.value = true
  }
  target.value = ''
}

function onCmdlineInput(event: Event) {
  const val = (event.target as HTMLInputElement).value
  if (val) formUseWorkDir.value = true
}

function toggleHackTree() {
  formHackTree.value = !formHackTree.value
}

const selectedGroup = computed(() =>
  proxyGroups.value.find(g => g.id === parseInt(formProxyGroupId.value))
)
const canSave = computed(() => formName.value.trim() && formProcessName.value.trim())

function onSave() {
  let proxy: ProxyTarget
  if (isCustomProxy.value && customProxyHost.value) {
    const parts = customProxyHost.value.split(':')
    proxy = { type: 'socks5', host: parts[0] || '127.0.0.1', port: parseInt(parts[1] ?? '7890') || 7890 }
  } else {
    const group = selectedGroup.value
    proxy = { type: group?.type ?? 'socks5', host: group?.host ?? '127.0.0.1', port: group?.port ?? 7890 }
  }
  const dst_filter: TrafficFilter = {
    exclude_cidrs: formExcludeCidrs.value,
    include_cidrs: [],
    include_ports: formIncludePorts.value,
    exclude_ports: [],
  }
  emit('save', {
    name: formName.value,
    enabled: props.rule?.enabled ?? false,
    process_name: formProcessName.value,
    cmdline_pattern: formCmdlinePattern.value,
    image_path_pattern: formUseWorkDir.value ? formWorkDir.value : '',
    hack_tree: formHackTree.value,
    protocol: formProtocol.value,
    proxy_group_id: parseInt(formProxyGroupId.value, 10),
    proxy,
    dst_filter,
  })
}

const confirmingDelete = ref(false)

function onDelete() {
  confirmingDelete.value = true
}

function onDeleteConfirmed() {
  confirmingDelete.value = false
  if (props.rule) emit('delete', props.rule.id)
}

function onClose(value: boolean) {
  emit('update:open', value)
}
</script>

<template>
  <Dialog :open="open" @update:open="onClose">
    <DialogContent class="sm:max-w-2xl bg-white dark:bg-[#181f26] border-slate-200 dark:border-slate-800 text-slate-900 dark:text-slate-100 p-0 overflow-hidden shadow-2xl rounded-xl"
      @interact-outside="(e: Event) => e.preventDefault()"
      @escape-key-down="(e: Event) => e.preventDefault()"
    >
      <DialogHeader class="px-6 py-4 border-b border-slate-200 dark:border-slate-800 bg-slate-50 dark:bg-[#1c242c]">
        <DialogTitle class="flex items-center gap-3">
          <div class="bg-blue-600/10 p-2 rounded-lg text-blue-600 dark:text-blue-500">
            <SlidersHorizontal class="w-6 h-6" />
          </div>
          <div>
            <h2 class="text-xl font-bold tracking-tight text-slate-800 dark:text-white">Auto Rule Editor</h2>
            <p class="text-xs font-medium text-slate-500 dark:text-slate-400">Configure network interception rules</p>
          </div>
        </DialogTitle>
      </DialogHeader>

      <div class="p-6 space-y-8 max-h-[75vh] overflow-y-auto thin-scrollbar">

        <!-- Basic Info Section -->
        <div class="space-y-4">
          <div class="flex items-center gap-3">
            <h3 class="text-xs font-bold text-blue-600 dark:text-blue-500 uppercase tracking-wider whitespace-nowrap">Basic Information</h3>
            <div class="h-px flex-1 bg-slate-200 dark:bg-slate-800" />
          </div>
          <div class="flex flex-col gap-2">
            <label class="text-sm font-medium text-slate-700 dark:text-slate-200 flex items-center gap-1.5">
              Rule Name
              <Tooltip><TooltipTrigger as-child><HelpCircle class="w-3.5 h-3.5 text-slate-400 cursor-help" /></TooltipTrigger><TooltipContent side="right"><p class="text-xs max-w-[200px]">Display name for this rule. Used for identification only.</p></TooltipContent></Tooltip>
            </label>
            <Input v-model="formName" class="h-[42px] bg-slate-50 dark:bg-[#101922] border-slate-200 dark:border-slate-800 transition-all focus:ring-1 focus:ring-blue-500" />
          </div>
        </div>

        <!-- Process Matching Section -->
        <div class="space-y-4">
          <div class="flex items-center gap-3">
            <h3 class="text-xs font-bold text-blue-600 dark:text-blue-500 uppercase tracking-wider whitespace-nowrap">Process Matching</h3>
            <div class="h-px flex-1 bg-slate-200 dark:bg-slate-800" />
          </div>

          <div class="space-y-4">
            <!-- Process Name & Browse -->
            <div class="flex flex-col gap-2">
              <div class="flex justify-between items-center">
                <label class="text-sm font-medium text-slate-700 dark:text-slate-200 flex items-center gap-1.5">
                  Process Name
                  <Tooltip><TooltipTrigger as-child><HelpCircle class="w-3.5 h-3.5 text-slate-400 cursor-help" /></TooltipTrigger><TooltipContent side="right"><p class="text-xs max-w-[220px]">Executable name to match. Supports wildcards: * (any sequence), ? (single char). Case-insensitive.</p></TooltipContent></Tooltip>
                </label>
                <span class="text-xs text-slate-500 dark:text-slate-400">Supports wildcards (*)</span>
              </div>
              <div class="flex gap-2">
                <div class="relative flex-1">
                  <svg class="absolute left-3 top-2.5 w-5 h-5 text-slate-400" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg>
                  <Input v-model="formProcessName" class="h-[42px] pl-10 font-mono bg-slate-50 dark:bg-[#101922] border-slate-200 dark:border-slate-800 transition-all focus:ring-1 focus:ring-blue-500" />
                </div>
                <Button variant="outline" @click="browseExe" class="h-[42px] flex items-center gap-1.5 bg-slate-50 dark:bg-[#1c242c] border-slate-200 dark:border-slate-800 hover:text-blue-600 dark:hover:border-blue-500">
                  <FolderOpen class="w-4 h-4" /> Browse
                </Button>
                <input type="file" accept=".exe" class="hidden" ref="fileInput" @change="onExeSelected" />
              </div>

              <!-- Working Dir Row -->
              <div
                class="mt-1 rounded-lg border border-slate-200 dark:border-slate-800 bg-slate-100/60 dark:bg-[#1c242c]/50 px-3 py-2.5 flex items-center gap-3 transition-opacity"
                :class="formUseWorkDir ? '' : 'opacity-40'"
              >
                <Lock class="w-4 h-4 text-slate-400 flex-shrink-0" />
                <input readonly class="flex-1 bg-transparent text-xs font-mono text-slate-500 dark:text-slate-400 outline-none select-all" :value="formWorkDir" placeholder="Select an executable to auto-fill" />
                <div class="flex items-center gap-2 flex-shrink-0 border-l border-slate-200 dark:border-slate-800 pl-3 ml-1">
                  <span class="text-xs font-medium text-slate-500 dark:text-slate-400">Use as filter</span>
                  <div
                    class="w-8 h-4 rounded-full p-[2px] cursor-pointer relative transition-colors"
                    :class="formUseWorkDir ? 'bg-blue-600' : 'bg-slate-300 dark:bg-slate-700'"
                    @click="formUseWorkDir = !formUseWorkDir"
                  >
                    <div class="w-3 h-3 bg-white rounded-full transition-transform absolute" :class="formUseWorkDir ? 'translate-x-4' : 'translate-x-0'" />
                  </div>
                </div>
              </div>
            </div>

            <!-- Command Line Pattern -->
            <div class="flex flex-col gap-2">
              <div class="flex justify-between items-center">
                <label class="text-sm font-medium text-slate-700 dark:text-slate-200 flex items-center gap-1.5">
                  Command-line Pattern
                  <Tooltip><TooltipTrigger as-child><HelpCircle class="w-3.5 h-3.5 text-slate-400 cursor-help" /></TooltipTrigger><TooltipContent side="right"><p class="text-xs max-w-[250px]">Optional. Without wildcards: space-separated keywords, all must appear (order-independent). With * or ?: glob match against full cmdline.</p></TooltipContent></Tooltip>
                </label>
                <span class="text-xs text-slate-500 dark:text-slate-400">Keywords or wildcards</span>
              </div>
              <div class="relative">
                <Terminal class="absolute left-3 top-2.5 w-5 h-5 text-slate-400" />
                <Input
                  v-model="formCmdlinePattern"
                  @input="onCmdlineInput"
                  class="h-[42px] pl-10 font-mono bg-slate-50 dark:bg-[#101922] border-slate-200 dark:border-slate-800 transition-all focus:ring-1 focus:ring-blue-500"
                />
              </div>
            </div>

            <!-- Capture Tree -->
            <div
              class="flex items-start gap-3 p-3 rounded-lg border border-slate-200 dark:border-slate-800 bg-slate-100/60 dark:bg-[#1c242c]/50 hover:bg-slate-200/50 dark:hover:bg-[#1c242c] transition-colors cursor-pointer group"
              @click.stop="toggleHackTree"
            >
              <div class="relative flex items-center mt-0.5">
                <div
                  class="w-4 h-4 rounded border dark:border-slate-600 border-slate-300 flex items-center justify-center transition-colors"
                  :class="formHackTree ? 'bg-blue-600 border-blue-600 dark:bg-blue-600' : 'bg-white dark:bg-[#101922]'"
                >
                  <Check v-if="formHackTree" class="w-3 h-3 text-white" stroke-width="3" />
                </div>
              </div>
              <div class="flex flex-col select-none">
                <span class="text-sm font-medium text-slate-700 dark:text-slate-200 group-hover:text-slate-900 dark:group-hover:text-white transition-colors">Capture Entire Process Tree</span>
                <span class="text-xs text-slate-500 dark:text-slate-400">Automatically apply this rule to all child processes spawned by the target.</span>
              </div>
            </div>

            <!-- Protocol -->
            <div class="flex flex-col gap-2">
              <label class="text-sm font-medium text-slate-700 dark:text-slate-200">Protocol</label>
              <Select v-model="formProtocol">
                <SelectTrigger class="w-full h-9 bg-white dark:bg-[#101922] border-slate-200 dark:border-slate-800">
                  <SelectValue placeholder="Select protocol" />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="tcp">TCP Only</SelectItem>
                  <SelectItem value="udp">UDP Only</SelectItem>
                  <SelectItem value="both">TCP + UDP</SelectItem>
                </SelectContent>
              </Select>
              <p class="text-xs text-slate-500 dark:text-slate-400">Which network protocol to intercept for this rule.</p>
            </div>
          </div>
        </div>

        <!-- Target Proxy Section -->
        <div class="space-y-4">
          <div class="flex items-center gap-3">
            <h3 class="text-xs font-bold text-blue-600 dark:text-blue-500 uppercase tracking-wider whitespace-nowrap">Target Proxy</h3>
            <div class="h-px flex-1 bg-slate-200 dark:bg-slate-800" />
          </div>
          <div class="flex flex-col gap-2">
            <label class="text-sm font-medium text-slate-700 dark:text-slate-200 flex items-center gap-1.5">
              Proxy Group
              <Tooltip><TooltipTrigger as-child><HelpCircle class="w-3.5 h-3.5 text-slate-400 cursor-help" /></TooltipTrigger><TooltipContent side="right"><p class="text-xs max-w-[220px]">SOCKS5 proxy group to route matched traffic through. Configure groups in the Proxies tab.</p></TooltipContent></Tooltip>
            </label>

            <!-- Group selector with styled badges -->
            <Select
              :model-value="isCustomProxy ? 'custom' : formProxyGroupId"
              @update:model-value="(v: unknown) => { const s = String(v ?? ''); if (s === 'custom') { isCustomProxy = true } else { isCustomProxy = false; formProxyGroupId = s } }"
            >
              <SelectTrigger class="h-[42px] bg-slate-50 dark:bg-[#101922] border-slate-200 dark:border-slate-800 focus:ring-1 focus:ring-blue-500">
                <div class="flex items-center gap-2">
                  <template v-if="isCustomProxy">
                    <span class="font-mono text-xs font-bold px-1.5 py-0.5 rounded bg-blue-600 text-white tracking-tight">custom</span>
                    <span class="text-xs text-slate-500 dark:text-slate-400 font-mono">{{ customProxyHost || '—' }}</span>
                  </template>
                  <template v-else-if="selectedGroup">
                    <span class="font-mono text-xs font-bold px-1.5 py-0.5 rounded bg-slate-800 dark:bg-slate-200 text-slate-100 dark:text-slate-800 tracking-tight shrink-0">
                      {{ selectedGroup.name }}
                    </span>
                    <span class="font-mono text-xs px-2 py-0.5 rounded bg-slate-100 dark:bg-slate-800 text-slate-500 dark:text-slate-400 border border-slate-200 dark:border-slate-700">
                      socks5://<span class="text-slate-700 dark:text-slate-300">{{ selectedGroup.host }}:{{ selectedGroup.port }}</span>
                    </span>
                  </template>
                  <span v-else class="text-slate-400 dark:text-slate-500 text-sm">Select proxy group...</span>
                </div>
              </SelectTrigger>
              <SelectContent class="bg-white dark:bg-[#181f26] border-slate-200 dark:border-slate-800">
                <SelectItem v-for="g in proxyGroups" :key="g.id" :value="String(g.id)" class="py-2">
                  <div class="flex items-center gap-2">
                    <span class="font-mono text-xs font-bold px-1.5 py-0.5 rounded bg-slate-800 dark:bg-slate-200 text-slate-100 dark:text-slate-800 tracking-tight shrink-0">
                      {{ g.name }}
                    </span>
                    <span class="font-mono text-xs px-2 py-0.5 rounded bg-slate-100 dark:bg-slate-800 text-slate-500 dark:text-slate-400 border border-slate-200 dark:border-slate-700">
                      socks5://<span class="text-slate-700 dark:text-slate-300">{{ g.host }}:{{ g.port }}</span>
                    </span>
                  </div>
                </SelectItem>
                <SelectItem value="custom" class="py-2">
                  <div class="flex items-center gap-2">
                    <span class="font-mono text-xs font-bold px-1.5 py-0.5 rounded bg-blue-600 text-white tracking-tight">custom</span>
                    <span class="text-xs text-slate-500 dark:text-slate-400">Enter address manually...</span>
                  </div>
                </SelectItem>
              </SelectContent>
            </Select>

            <!-- Custom proxy input -->
            <div v-if="isCustomProxy" class="flex items-stretch rounded-lg border border-slate-200 dark:border-slate-800 overflow-hidden focus-within:ring-1 focus-within:ring-blue-500 focus-within:border-blue-500 transition-all">
              <div class="flex items-center px-3 bg-slate-100 dark:bg-[#1c242c] border-r border-slate-200 dark:border-slate-800 select-none">
                <span class="text-xs font-mono font-semibold text-slate-500 dark:text-slate-400 tracking-wide">socks5://</span>
              </div>
              <input v-model="customProxyHost" class="flex-1 bg-slate-50 dark:bg-[#101922] px-3 py-2.5 text-sm font-mono text-slate-800 dark:text-white outline-none" type="text" spellcheck="false" autocomplete="off" placeholder="127.0.0.1:7890" />
            </div>
          </div>

          <!-- Advanced Accordion -->
          <div class="pt-1">
            <button
              class="flex items-center gap-1.5 text-xs font-medium text-blue-600 hover:text-blue-700 transition-colors group select-none"
              @click="advancedOpen = !advancedOpen"
            >
              <ChevronRight class="w-4 h-4 transition-transform" :class="advancedOpen ? 'rotate-90' : ''" />
              Advanced
            </button>
            <div v-show="advancedOpen" class="mt-4 space-y-5 pl-3 border-l-2 border-slate-200 dark:border-slate-800">
              <!-- Exclude CIDRs -->
              <div class="flex flex-col gap-2">
                <label class="text-sm font-medium text-slate-700 dark:text-slate-200 flex items-center gap-1.5">
                  Exclude CIDRs
                  <Tooltip><TooltipTrigger as-child><HelpCircle class="w-3.5 h-3.5 text-slate-400 cursor-help" /></TooltipTrigger><TooltipContent side="right"><p class="text-xs max-w-[220px]">Traffic to these CIDR ranges will bypass the proxy (go direct). Default excludes are applied globally via config.</p></TooltipContent></Tooltip>
                </label>
                <TagInput
                  :model-value="formExcludeCidrs"
                  @update:model-value="formExcludeCidrs = $event"
                  placeholder="e.g. 192.168.0.0/16 (Enter to add)"
                />
                <p class="text-xs text-slate-500 dark:text-slate-400">Press Enter or comma to add. Click X to remove.</p>
              </div>

              <!-- Include Ports -->
              <div class="flex flex-col gap-2">
                <label class="text-sm font-medium text-slate-700 dark:text-slate-200 flex items-center gap-1.5">
                  Include Ports
                  <Tooltip><TooltipTrigger as-child><HelpCircle class="w-3.5 h-3.5 text-slate-400 cursor-help" /></TooltipTrigger><TooltipContent side="right"><p class="text-xs max-w-[220px]">Only proxy traffic to these ports. Leave empty to proxy all ports.</p></TooltipContent></Tooltip>
                </label>
                <TagInput
                  :model-value="formIncludePorts"
                  @update:model-value="formIncludePorts = $event"
                  placeholder="e.g. 443 (Enter to add)"
                />
              </div>
            </div>
          </div>
        </div>
      </div>

      <DialogFooter class="px-6 py-4 border-t border-slate-200 dark:border-slate-800 bg-slate-50 dark:bg-[#1c242c] flex flex-row items-center justify-between sm:justify-between w-full rounded-b-xl">
        <div v-if="isEditing" class="flex items-center gap-2">
          <template v-if="!confirmingDelete">
            <button
              class="text-sm font-medium text-red-500 hover:text-red-600 dark:hover:text-red-400 transition-colors flex items-center gap-1 opacity-80 hover:opacity-100"
              @click="onDelete"
            >
              <Trash2 class="w-4 h-4" />
              Delete Rule
            </button>
          </template>
          <template v-else>
            <button
              class="text-sm font-medium text-white bg-red-600 hover:bg-red-700 px-3 py-1 rounded-md transition-colors"
              @click="onDeleteConfirmed"
            >
              Confirm Delete
            </button>
            <button
              class="text-sm font-medium text-slate-500 hover:text-slate-700 dark:hover:text-slate-300 transition-colors"
              @click="confirmingDelete = false"
            >
              Cancel
            </button>
          </template>
        </div>
        <div v-else />
        <div class="flex gap-3">
          <Button variant="outline" class="bg-transparent border-slate-200 dark:border-slate-700 text-slate-700 dark:text-slate-300 hover:bg-slate-200/50 dark:hover:bg-slate-800" @click="onClose(false)">Cancel</Button>
          <Button :disabled="!canSave || saving" class="bg-blue-600 hover:bg-blue-700 text-white shadow-lg shadow-blue-500/20" @click="onSave">
            {{ saving ? 'Saving...' : 'Save Rule' }}
          </Button>
        </div>
      </DialogFooter>
    </DialogContent>
  </Dialog>
</template>
