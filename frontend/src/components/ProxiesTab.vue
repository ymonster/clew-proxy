<script setup lang="ts">
import { ref, computed, onMounted, useId } from 'vue'
import { Badge } from '@/components/ui/badge'
import { Card, CardContent } from '@/components/ui/card'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Button } from '@/components/ui/button'
import {
  getProxyGroups,
  getAutoRules,
  createProxyGroup,
  updateProxyGroup,
  deleteProxyGroup,
  migrateProxyGroup,
  testProxyGroup,
  getSubscriptionCore,
  updateSubscriptionCore,
  refreshSubscriptionCore,
  startSubscriptionCore,
  stopSubscriptionCore,
} from '@/api/client'
import type { ProxyGroup, AutoRule, GroupInUseError, ProxyTestResult, SubscriptionCoreStatus } from '@/api/types'
import { useStatusBus } from '@/composables/useStatusBus'
import { Plus, Search, X, Pencil, Trash2, Globe, RefreshCw, Loader2, ChevronDown, Cable } from 'lucide-vue-next'

const { pushError } = useStatusBus()

// Embedded subscription / mihomo core
const subStatus = ref<SubscriptionCoreStatus | null>(null)
const subUrl = ref('')
const subCorePath = ref('')
const subPort = ref('17890')
const subBusy = ref(false)

async function fetchSubscription() {
  try {
    const s = await getSubscriptionCore()
    subStatus.value = s
    subUrl.value = s.url
    subCorePath.value = s.core_path
    subPort.value = String(s.socks_port || 17890)
  } catch (e) {
    console.error('[ProxiesTab] fetchSubscription failed:', e)
  }
}

async function onToggleSubscription() {
  if (subBusy.value || !subStatus.value) return
  subBusy.value = true
  try {
    const next = !subStatus.value.enabled
    subStatus.value = await updateSubscriptionCore({
      enabled: next,
      url: subUrl.value.trim(),
      socks_port: Number.parseInt(subPort.value, 10) || 17890,
      core_path: subCorePath.value.trim(),
    })
    await fetchData()
  } catch (e) {
    pushError(e, 'Update subscription core failed')
  } finally {
    subBusy.value = false
  }
}

async function onSaveSubscriptionSettings() {
  if (subBusy.value) return
  subBusy.value = true
  try {
    subStatus.value = await updateSubscriptionCore({
      url: subUrl.value.trim(),
      socks_port: Number.parseInt(subPort.value, 10) || 17890,
      core_path: subCorePath.value.trim(),
      enabled: subStatus.value?.enabled ?? false,
    })
    await fetchData()
  } catch (e) {
    pushError(e, 'Save subscription settings failed')
  } finally {
    subBusy.value = false
  }
}

async function onRefreshSubscription() {
  if (subBusy.value) return
  subBusy.value = true
  try {
    // Persist URL/port first so refresh uses the latest values.
    await updateSubscriptionCore({
      url: subUrl.value.trim(),
      socks_port: Number.parseInt(subPort.value, 10) || 17890,
      core_path: subCorePath.value.trim(),
    })
    subStatus.value = await refreshSubscriptionCore()
    await fetchData()
  } catch (e) {
    pushError(e, 'Refresh subscription failed')
    await fetchSubscription()
  } finally {
    subBusy.value = false
  }
}

async function onStartSubscription() {
  if (subBusy.value) return
  subBusy.value = true
  try {
    subStatus.value = await startSubscriptionCore()
    await fetchData()
  } catch (e) {
    pushError(e, 'Start core failed')
  } finally {
    subBusy.value = false
  }
}

async function onStopSubscription() {
  if (subBusy.value) return
  subBusy.value = true
  try {
    subStatus.value = await stopSubscriptionCore()
    await fetchData()
  } catch (e) {
    pushError(e, 'Stop core failed')
  } finally {
    subBusy.value = false
  }
}


const groups = ref<ProxyGroup[]>([])
const autoRules = ref<AutoRule[]>([])
const searchQuery = ref('')

// Test results per group
const testResults = ref(new Map<number, { loading: boolean; result?: ProxyTestResult }>())

// Editor dialog
const editOpen = ref(false)
const editingGroup = ref<ProxyGroup | null>(null)
const formName = ref('')
const formHost = ref('127.0.0.1')
const formPort = ref('7890')
const formTestUrl = ref('https://www.google.com')

// a11y: unique ids for label/field association
const nameId = useId()
const hostId = useId()
const portId = useId()
const testUrlId = useId()
const migrateTargetLabelId = useId()

// Delete / migration dialog
const deleteOpen = ref(false)
const deletingGroup = ref<ProxyGroup | null>(null)
const deleteConflict = ref<GroupInUseError | null>(null)
const migrateTargetId = ref('0')

// Mock expansion
const expandedGroupId = ref<number | null>(null)

const filteredGroups = computed(() => {
  const q = searchQuery.value.toLowerCase().trim()
  if (!q) return groups.value
  return groups.value.filter(g => g.name.toLowerCase().includes(q) || g.host.includes(q))
})

function rulesCountForGroup(id: number): number {
  return autoRules.value.filter(r => r.proxy_group_id === id).length
}

function rulesForGroup(id: number): AutoRule[] {
  return autoRules.value.filter(r => r.proxy_group_id === id)
}

async function fetchData() {
  try {
    const [g, r] = await Promise.all([getProxyGroups(), getAutoRules()])
    groups.value = g
    autoRules.value = r
  } catch (e) {
    console.error('[ProxiesTab] fetchData failed:', e)
  }
}

// Test
async function onTest(id: number) {
  testResults.value.set(id, { loading: true })
  testResults.value = new Map(testResults.value) // trigger reactivity
  try {
    const result = await testProxyGroup(id)
    testResults.value.set(id, { loading: false, result })
  } catch {
    testResults.value.set(id, { loading: false, result: { error: 'request failed' } })
  }
  testResults.value = new Map(testResults.value)
}

// Editor
function openCreate() {
  editingGroup.value = null
  formName.value = ''
  formHost.value = '127.0.0.1'
  formPort.value = '7890'
  formTestUrl.value = 'https://www.google.com'
  editOpen.value = true
}

function openEdit(group: ProxyGroup) {
  editingGroup.value = group
  formName.value = group.name
  formHost.value = group.host
  formPort.value = String(group.port)
  formTestUrl.value = group.test_url
  editOpen.value = true
}

const saving = ref(false)

async function onSaveGroup() {
  if (saving.value) return
  saving.value = true
  try {
    const data = {
      name: formName.value,
      host: formHost.value,
      port: Number.parseInt(formPort.value, 10) || 7890,
      type: 'socks5' as const,
      test_url: formTestUrl.value,
    }
    if (editingGroup.value) {
      await updateProxyGroup(editingGroup.value.id, data)
    } else {
      await createProxyGroup(data as Omit<ProxyGroup, 'id'>)
    }
    editOpen.value = false
    await fetchData()
  } catch (e) {
    pushError(e, 'Save proxy group failed')
  } finally {
    saving.value = false
  }
}

// Delete
async function onDelete(group: ProxyGroup) {
  if (saving.value) return
  saving.value = true
  try {
    const result = await deleteProxyGroup(group.id)
    if ('error' in result && result.error === 'group_in_use') {
      deletingGroup.value = group
      deleteConflict.value = result as GroupInUseError
      migrateTargetId.value = '0'
      deleteOpen.value = true
    } else {
      await fetchData()
    }
  } catch (e) {
    pushError(e, 'Delete proxy group failed')
  } finally {
    saving.value = false
  }
}

async function onMigrateAndDelete() {
  if (!deletingGroup.value || saving.value) return
  saving.value = true
  try {
    await migrateProxyGroup(deletingGroup.value.id, Number.parseInt(migrateTargetId.value, 10))
    deleteOpen.value = false
    await fetchData()
  } catch (e) {
    pushError(e, 'Migrate proxy group failed')
  } finally {
    saving.value = false
  }
}

function toggleExpand(id: number) {
  expandedGroupId.value = expandedGroupId.value === id ? null : id
}

onMounted(async () => {
  await Promise.all([fetchData(), fetchSubscription()])
})
</script>

<template>
  <div class="flex-1 overflow-auto thin-scrollbar flex flex-col">
    <!-- Toolbar -->
    <div class="shrink-0 border-b border-slate-200 dark:border-slate-800 bg-slate-50/50 dark:bg-[#18181b]/50">
      <div class="h-12 flex items-center gap-3 px-4">
        <!-- Search -->
        <div class="relative flex-1 max-w-[240px]">
          <Search class="absolute left-2.5 top-1/2 -translate-y-1/2 w-3.5 h-3.5 text-slate-400 pointer-events-none" />
          <input
            v-model="searchQuery"
            placeholder="Search groups..."
            class="w-full pl-8 pr-7 py-1.5 text-xs rounded-md border border-slate-200 dark:border-slate-700 bg-white dark:bg-[#101922] text-slate-800 dark:text-white placeholder:text-slate-400 outline-none focus:ring-1 focus:ring-blue-500 focus:border-blue-500 transition-colors"
          />
          <button
            v-if="searchQuery"
            @click="searchQuery = ''"
            class="absolute right-2 top-1/2 -translate-y-1/2 text-slate-400 hover:text-slate-600 dark:hover:text-slate-300"
          >
            <X class="w-3.5 h-3.5" />
          </button>
        </div>

        <span class="ml-auto text-xs text-slate-500 dark:text-slate-400 font-medium">
          {{ groups.length }} group{{ groups.length !== 1 ? 's' : '' }}
        </span>

        <button
          @click="openCreate"
          class="shrink-0 flex items-center gap-1.5 px-3 py-1.5 text-xs font-semibold text-white bg-blue-600 hover:bg-blue-700 rounded-md transition-colors shadow-sm shadow-blue-500/20 ml-2"
        >
          <Plus class="w-3.5 h-3.5" /> New Group
        </button>
      </div>
    </div>

    <!-- Group cards -->
    <div class="flex-1 overflow-auto thin-scrollbar p-4 space-y-3">
      <!-- Subscription / Embedded core -->
      <Card class="border border-blue-200 dark:border-blue-900/50 bg-white dark:bg-[#18181b] shadow-sm">
        <CardContent class="p-4 space-y-3">
          <div class="flex items-center gap-2">
            <Cable class="w-4 h-4 text-blue-600 dark:text-blue-400" />
            <span class="text-base font-bold text-slate-800 dark:text-slate-100">Subscription / Embedded core</span>
            <Badge
              variant="outline"
              class="h-5 px-2 text-[10px] font-bold uppercase"
              :class="subStatus?.running
                ? 'text-emerald-600 border-emerald-300 dark:text-emerald-400 dark:border-emerald-700'
                : subStatus?.last_error
                  ? 'text-red-600 border-red-300 dark:text-red-400 dark:border-red-700'
                  : 'text-slate-500 border-slate-300 dark:text-slate-400 dark:border-slate-600'"
            >
              {{ subStatus?.running ? 'running' : (subStatus?.last_error ? 'error' : 'stopped') }}
            </Badge>
            <button
              class="relative shrink-0 ml-auto inline-flex h-5 w-9 items-center rounded-full transition-colors focus:outline-none focus:ring-2 focus:ring-blue-500"
              :class="subStatus?.enabled ? 'bg-blue-600' : 'bg-slate-300 dark:bg-slate-600'"
              :disabled="subBusy"
              title="Enable embedded core"
              @click="onToggleSubscription"
            >
              <span
                class="inline-block h-4 w-4 transform rounded-full bg-white shadow transition-transform"
                :class="subStatus?.enabled ? 'translate-x-[18px]' : 'translate-x-0.5'"
              />
            </button>
          </div>

          <p class="text-xs text-slate-500 dark:text-slate-400 leading-relaxed">
            Place <code class="font-mono text-[11px] px-1 py-0.5 rounded bg-slate-100 dark:bg-slate-800">mihomo.exe</code>
            (or <code class="font-mono text-[11px] px-1 py-0.5 rounded bg-slate-100 dark:bg-slate-800">clash-meta.exe</code>)
            next to <code class="font-mono text-[11px] px-1 py-0.5 rounded bg-slate-100 dark:bg-slate-800">clew.exe</code>,
            or set a custom path below. Clew starts the core as a child process listening on localhost SOCKS only
            (no TUN / no system-wide hijack). Rules still decide which processes use the
            <span class="font-medium text-slate-700 dark:text-slate-300">subscription</span> proxy group.
          </p>

          <div class="space-y-1">
            <Label class="text-slate-700 dark:text-slate-200 text-xs">Subscription URL</Label>
            <Input
              v-model="subUrl"
              placeholder="https://example.com/clash.yaml"
              class="font-mono text-sm"
              :disabled="subBusy"
            />
          </div>

          <div class="flex gap-2">
            <div class="w-28 space-y-1">
              <Label class="text-slate-700 dark:text-slate-200 text-xs">SOCKS port</Label>
              <Input v-model="subPort" placeholder="17890" class="font-mono text-sm" :disabled="subBusy" />
            </div>
            <div class="flex-1 space-y-1">
              <Label class="text-slate-700 dark:text-slate-200 text-xs">Core path (optional)</Label>
              <Input
                v-model="subCorePath"
                placeholder="auto: mihomo.exe beside clew.exe"
                class="font-mono text-sm"
                :disabled="subBusy"
              />
            </div>
          </div>

          <div class="flex flex-wrap items-center gap-2 pt-1">
            <Button
              size="sm"
              variant="outline"
              :disabled="subBusy"
              class="h-8 text-xs"
              @click="onSaveSubscriptionSettings"
            >
              Save
            </Button>
            <Button
              size="sm"
              :disabled="subBusy || !subUrl.trim()"
              class="h-8 text-xs bg-blue-600 hover:bg-blue-700 text-white"
              @click="onRefreshSubscription"
            >
              <RefreshCw class="w-3.5 h-3.5 mr-1" :class="subBusy ? 'animate-spin' : ''" />
              Refresh
            </Button>
            <Button
              v-if="!subStatus?.running"
              size="sm"
              variant="outline"
              :disabled="subBusy"
              class="h-8 text-xs"
              @click="onStartSubscription"
            >
              Start
            </Button>
            <Button
              v-else
              size="sm"
              variant="outline"
              :disabled="subBusy"
              class="h-8 text-xs text-red-600 border-red-300 hover:bg-red-50 dark:hover:bg-red-900/20"
              @click="onStopSubscription"
            >
              Stop
            </Button>
            <span class="ml-auto text-xs font-mono text-slate-500 dark:text-slate-400">
              socks5://{{ subStatus?.socks_host || '127.0.0.1' }}:{{ subStatus?.socks_port || subPort }}
              <template v-if="subStatus?.pid"> · pid {{ subStatus.pid }}</template>
            </span>
          </div>

          <div v-if="subStatus?.last_update || subStatus?.last_error || subStatus?.resolved_core_path"
               class="text-[11px] space-y-0.5 text-slate-500 dark:text-slate-400 border-t border-slate-100 dark:border-slate-800 pt-2">
            <p v-if="subStatus.resolved_core_path" class="font-mono truncate" :title="subStatus.resolved_core_path">
              core: {{ subStatus.resolved_core_path }}
            </p>
            <p v-if="subStatus.last_update">last update: {{ subStatus.last_update }}</p>
            <p v-if="subStatus.last_error" class="text-red-500 dark:text-red-400">{{ subStatus.last_error }}</p>
          </div>
        </CardContent>
      </Card>

      <Card
        v-for="group in filteredGroups"
        :key="group.id"
        class="border border-slate-200 dark:border-slate-800 bg-white dark:bg-[#18181b] shadow-sm"
      >
        <CardContent class="p-4 space-y-2">
          <!-- Row 1: name + badge + actions + rule count -->
          <div class="flex items-center gap-2">
            <span class="text-base font-bold text-slate-800 dark:text-slate-100">{{ group.name }}</span>
            <Badge variant="outline" class="h-5 px-2 text-[10px] font-bold uppercase text-blue-600 border-blue-300 dark:text-blue-400 dark:border-blue-700">SOCKS5</Badge>
            <button
              @click="openEdit(group)"
              class="p-1 rounded text-slate-400 hover:text-blue-600 dark:hover:text-blue-400 hover:bg-slate-100 dark:hover:bg-slate-800 transition-colors"
            >
              <Pencil class="w-3.5 h-3.5" />
            </button>
            <button
              v-if="group.id !== 0"
              :disabled="saving"
              @click="onDelete(group)"
              class="p-1 rounded text-slate-400 hover:text-red-500 hover:bg-red-50 dark:hover:bg-red-900/20 transition-colors disabled:opacity-40 disabled:cursor-not-allowed disabled:hover:text-slate-400 disabled:hover:bg-transparent"
            >
              <Trash2 class="w-3.5 h-3.5" />
            </button>
            <span class="ml-auto text-xs text-slate-400 dark:text-slate-500 font-medium">
              {{ rulesCountForGroup(group.id) }} rule{{ rulesCountForGroup(group.id) !== 1 ? 's' : '' }}
            </span>
          </div>

          <!-- Row 2: address -->
          <div class="text-sm font-mono text-slate-500 dark:text-slate-400">
            socks5://{{ group.host }}:{{ group.port }}
          </div>

          <!-- Row 3: test URL + test button + result -->
          <div class="flex items-center gap-2 pt-2 border-t border-slate-100 dark:border-slate-800/60">
            <Globe class="w-4 h-4 text-slate-400 shrink-0" />
            <span class="text-sm text-slate-500 dark:text-slate-400 flex-1 truncate">{{ group.test_url }}</span>

            <!-- Test button / result -->
            <template v-if="testResults.get(group.id)?.loading">
              <Loader2 class="w-4 h-4 text-blue-500 animate-spin shrink-0" />
            </template>
            <template v-else-if="testResults.get(group.id)?.result">
              <button
                @click="onTest(group.id)"
                class="shrink-0 text-sm font-bold px-2 py-0.5 rounded transition-colors"
                :class="testResults.get(group.id)!.result!.latency_ms != null
                  ? 'text-emerald-600 dark:text-emerald-400 bg-emerald-50 dark:bg-emerald-900/20 hover:bg-emerald-100 dark:hover:bg-emerald-900/40'
                  : 'text-red-500 dark:text-red-400 bg-red-50 dark:bg-red-900/20 hover:bg-red-100 dark:hover:bg-red-900/40'"
              >
                <template v-if="testResults.get(group.id)!.result!.latency_ms != null">
                  {{ testResults.get(group.id)!.result!.latency_ms }}ms
                </template>
                <template v-else>TIMEOUT</template>
              </button>
            </template>
            <template v-else>
              <button
                @click="onTest(group.id)"
                class="p-1 rounded text-slate-400 hover:text-blue-600 dark:hover:text-blue-400 hover:bg-slate-100 dark:hover:bg-slate-800 transition-colors shrink-0"
                title="Test connectivity"
              >
                <RefreshCw class="w-4 h-4" />
              </button>
            </template>
          </div>

          <!-- Mock expansion (demo only) -->
          <div v-if="rulesCountForGroup(group.id) > 0" class="pt-1">
            <button
              @click="toggleExpand(group.id)"
              class="flex items-center gap-1 text-xs text-slate-400 hover:text-slate-600 dark:hover:text-slate-300 transition-colors"
            >
              <ChevronDown class="w-3 h-3 transition-transform" :class="expandedGroupId === group.id ? 'rotate-180' : ''" />
              {{ expandedGroupId === group.id ? 'Hide' : 'Show' }} rules
            </button>
            <div v-if="expandedGroupId === group.id" class="mt-2 space-y-1 pl-4 border-l-2 border-slate-200 dark:border-slate-700">
              <div
                v-for="rule in rulesForGroup(group.id)"
                :key="rule.id"
                class="flex items-center gap-2 text-xs text-slate-500 dark:text-slate-400"
              >
                <span class="font-medium text-slate-700 dark:text-slate-300">{{ rule.name }}</span>
                <span class="font-mono text-slate-400">{{ rule.process_name }}</span>
                <Badge v-if="!rule.enabled" variant="outline" class="h-4 px-1 py-0 text-[9px]">off</Badge>
              </div>
            </div>
          </div>
        </CardContent>
      </Card>

      <!-- Empty state -->
      <div v-if="filteredGroups.length === 0" class="py-16 flex flex-col items-center gap-3 text-slate-400">
        <p class="text-sm">No proxy groups found.</p>
      </div>
    </div>

    <!-- Group Editor Dialog -->
    <Dialog :open="editOpen" @update:open="editOpen = $event">
      <DialogContent class="sm:max-w-[420px]"
        @interact-outside="(e: Event) => e.preventDefault()"
        @escape-key-down="(e: Event) => e.preventDefault()"
      >
        <DialogHeader>
          <DialogTitle>{{ editingGroup ? 'Edit Group' : 'New Proxy Group' }}</DialogTitle>
          <DialogDescription>Configure a SOCKS5 proxy group.</DialogDescription>
        </DialogHeader>
        <div class="space-y-4 py-2">
          <div class="space-y-1">
            <Label :for="nameId" class="text-slate-700 dark:text-slate-200">Name</Label>
            <Input :id="nameId" v-model="formName" placeholder="my-proxy" />
          </div>
          <div class="flex gap-2">
            <div class="flex-1 space-y-1">
              <Label :for="hostId" class="text-slate-700 dark:text-slate-200">Host</Label>
              <Input :id="hostId" v-model="formHost" placeholder="127.0.0.1" class="font-mono" />
            </div>
            <div class="w-24 space-y-1">
              <Label :for="portId" class="text-slate-700 dark:text-slate-200">Port</Label>
              <Input :id="portId" v-model="formPort" placeholder="7890" class="font-mono" />
            </div>
          </div>
          <div class="space-y-1">
            <Label :for="testUrlId" class="text-slate-700 dark:text-slate-200">Test URL</Label>
            <Input :id="testUrlId" v-model="formTestUrl" placeholder="https://www.google.com" class="font-mono text-sm" />
          </div>
        </div>
        <DialogFooter class="gap-2 sm:gap-0">
          <button
            @click="editOpen = false"
            class="inline-flex items-center justify-center rounded-md text-sm font-medium h-9 px-4 border border-slate-200 dark:border-slate-700 text-slate-700 dark:text-slate-300 hover:bg-slate-100 dark:hover:bg-slate-800 transition-colors"
          >
            Cancel
          </button>
          <Button :disabled="saving" @click="onSaveGroup" class="bg-blue-600 hover:bg-blue-700 text-white">
            {{ saving ? 'Saving...' : (editingGroup ? 'Save' : 'Create') }}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>

    <!-- Delete / Migration Dialog -->
    <Dialog :open="deleteOpen" @update:open="deleteOpen = $event">
      <DialogContent class="sm:max-w-[480px]"
        @interact-outside="(e: Event) => e.preventDefault()"
        @escape-key-down="(e: Event) => e.preventDefault()"
      >
        <DialogHeader>
          <DialogTitle>Cannot Delete "{{ deletingGroup?.name }}"</DialogTitle>
          <DialogDescription>
            This proxy group is still in use. Migrate references to another group before deleting.
          </DialogDescription>
        </DialogHeader>
        <div v-if="deleteConflict" class="space-y-3 py-2">
          <!-- Referencing auto rules -->
          <div v-if="deleteConflict.auto_rules.length > 0">
            <p class="text-sm font-medium text-slate-700 dark:text-slate-200 mb-1">Auto Rules ({{ deleteConflict.auto_rules.length }})</p>
            <div class="space-y-1 pl-3 border-l-2 border-slate-200 dark:border-slate-700">
              <div v-for="r in deleteConflict.auto_rules" :key="r.id" class="text-xs text-slate-500 dark:text-slate-400">
                {{ r.name }}
              </div>
            </div>
          </div>
          <!-- Manual hijacks -->
          <div v-if="deleteConflict.manual_hijack_count > 0">
            <p class="text-sm font-medium text-slate-700 dark:text-slate-200">
              Manual Hijacks: {{ deleteConflict.manual_hijack_count }} process{{ deleteConflict.manual_hijack_count > 1 ? 'es' : '' }}
            </p>
          </div>
          <!-- Target group selector -->
          <div class="space-y-1 pt-2 border-t border-slate-200 dark:border-slate-800">
            <Label :for="migrateTargetLabelId" class="text-slate-700 dark:text-slate-200">Migrate to</Label>
            <Select v-model="migrateTargetId">
              <SelectTrigger :id="migrateTargetLabelId">
                <SelectValue placeholder="Select target group" />
              </SelectTrigger>
              <SelectContent>
                <SelectItem
                  v-for="g in groups.filter(g => g.id !== deletingGroup?.id)"
                  :key="g.id"
                  :value="String(g.id)"
                >
                  {{ g.name }} ({{ g.host }}:{{ g.port }})
                </SelectItem>
              </SelectContent>
            </Select>
          </div>
        </div>
        <DialogFooter class="gap-2 sm:gap-0">
          <button
            @click="deleteOpen = false"
            class="inline-flex items-center justify-center rounded-md text-sm font-medium h-9 px-4 border border-slate-200 dark:border-slate-700 text-slate-700 dark:text-slate-300 hover:bg-slate-100 dark:hover:bg-slate-800 transition-colors"
          >
            Cancel
          </button>
          <Button :disabled="saving" @click="onMigrateAndDelete" class="bg-red-600 hover:bg-red-700 text-white">
            {{ saving ? 'Processing...' : 'Migrate & Delete' }}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  </div>
</template>
