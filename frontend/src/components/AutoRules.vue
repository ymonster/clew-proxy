<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { Badge } from '@/components/ui/badge'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import {
  getAutoRules,
  getProxyGroups,
  updateAutoRule,
  deleteAutoRule,
} from '@/api/client'
import type { AutoRule, ProxyGroup } from '@/api/types'
import { useStatusBus } from '@/composables/useStatusBus'
import { Plus, Search, X, Pencil, Trash2, Power, PowerOff } from 'lucide-vue-next'

const { pushError } = useStatusBus()

const emit = defineEmits<{
  'open-add': []
  'open-edit': [rule: AutoRule]
  'rules-changed': []
}>()

const rules = ref<AutoRule[]>([])
const proxyGroups = ref<ProxyGroup[]>([])
const groupMap = computed(() => {
  const map = new Map<number, ProxyGroup>()
  for (const g of proxyGroups.value) map.set(g.id, g)
  return map
})
const searchQuery = ref('')
const statusFilter = ref<'all' | 'active' | 'disabled'>('all')
const selectedIds = ref(new Set<string>())
const operating = ref(false)

// Delete confirmation
const deleteDialogOpen = ref(false)
const pendingDeleteIds = ref<string[]>([])
const pendingDeleteLabel = ref('')

function confirmDelete(ids: string[], label: string) {
  pendingDeleteIds.value = ids
  pendingDeleteLabel.value = label
  deleteDialogOpen.value = true
}

function onDeleteConfirmed() {
  const ids = new Set(pendingDeleteIds.value)
  deleteDialogOpen.value = false
  selectedIds.value = new Set()

  // Optimistic: remove from local list immediately
  rules.value = rules.value.filter(r => !ids.has(r.id))

  // Background: API delete + re-fetch (recovers on failure)
  Promise.all([...ids].map(id => deleteAutoRule(id)))
    .then(() => fetchRules())
    .then(() => emit('rules-changed'))
    .catch(e => {
      console.error('[AutoRules] delete failed, re-fetching:', e)
      fetchRules()
    })
}

async function fetchRules() {
  try {
    const [r, g] = await Promise.all([getAutoRules(), getProxyGroups()])
    rules.value = r
    proxyGroups.value = g
  } catch (e) {
    console.error('[AutoRules] fetchRules failed:', e)
  }
}

const filteredRules = computed(() => {
  let result = rules.value
  if (statusFilter.value === 'active') result = result.filter(r => r.enabled)
  if (statusFilter.value === 'disabled') result = result.filter(r => !r.enabled)
  const q = searchQuery.value.toLowerCase().trim()
  if (q) result = result.filter(r => r.name.toLowerCase().includes(q) || r.process_name.toLowerCase().includes(q))
  return result
})

const allCount = computed(() => rules.value.length)
const activeCount = computed(() => rules.value.filter(r => r.enabled).length)
const disabledCount = computed(() => rules.value.filter(r => !r.enabled).length)

// Selection
const hasSelection = computed(() => selectedIds.value.size > 0)
const allSelected = computed(() => filteredRules.value.length > 0 && filteredRules.value.every(r => selectedIds.value.has(r.id)))
const someSelected = computed(() => selectedIds.value.size > 0 && !allSelected.value)

function toggleSelectAll() {
  if (allSelected.value) {
    selectedIds.value.clear()
  } else {
    filteredRules.value.forEach(r => selectedIds.value.add(r.id))
  }
}

function toggleSelect(id: string) {
  if (selectedIds.value.has(id)) selectedIds.value.delete(id)
  else selectedIds.value.add(id)
}

function cancelSelection() {
  selectedIds.value = new Set()
}

// Batch enable/disable
async function batchEnable() {
  if (operating.value) return
  operating.value = true
  try {
    await Promise.all(
      rules.value.filter(r => selectedIds.value.has(r.id) && !r.enabled)
        .map(r => updateAutoRule(r.id, { enabled: true }))
    )
    selectedIds.value = new Set()
    await fetchRules()
    emit('rules-changed')
  } catch (e) {
    pushError(e, 'Enable rules failed')
  } finally {
    operating.value = false
  }
}

async function batchDisable() {
  if (operating.value) return
  operating.value = true
  try {
    await Promise.all(
      rules.value.filter(r => selectedIds.value.has(r.id) && r.enabled)
        .map(r => updateAutoRule(r.id, { enabled: false }))
    )
    selectedIds.value = new Set()
    await fetchRules()
    emit('rules-changed')
  } catch (e) {
    pushError(e, 'Disable rules failed')
  } finally {
    operating.value = false
  }
}

function batchDelete() {
  const count = selectedIds.value.size
  confirmDelete([...selectedIds.value], `${count} selected rule${count > 1 ? 's' : ''}`)
}

function deleteRule(id: string) {
  const rule = rules.value.find(r => r.id === id)
  confirmDelete([id], rule?.name || id)
}

async function toggleRule(id: string) {
  const rule = rules.value.find(r => r.id === id)
  if (rule) {
    try {
      await updateAutoRule(id, { enabled: !rule.enabled })
      await fetchRules()
      emit('rules-changed')
    } catch (e) {
      pushError(e, 'Toggle rule failed')
    }
  }
}

onMounted(() => {
  fetchRules()
})

defineExpose({ fetchRules })
</script>

<template>
  <div class="flex-1 overflow-auto flex flex-col">
    <!-- ============ TOOLBAR ============ -->
    <div class="shrink-0 border-b border-slate-200 dark:border-slate-800 bg-slate-50/50 dark:bg-[#18181b]/50">

      <!-- Normal toolbar (no selection) -->
      <div v-if="!hasSelection" class="h-12 flex items-center gap-3 px-4">
        <!-- Select all checkbox -->
        <div
          class="w-4 h-4 rounded border cursor-pointer flex items-center justify-center shrink-0 transition-colors"
          :class="allSelected
            ? 'bg-blue-600 border-blue-600'
            : someSelected
              ? 'bg-blue-600/20 border-blue-500'
              : 'border-slate-300 dark:border-slate-600 hover:border-blue-400'"
          @click="toggleSelectAll"
        >
          <svg v-if="allSelected" class="w-3 h-3 text-white" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="3"><path stroke-linecap="round" stroke-linejoin="round" d="M5 13l4 4L19 7" /></svg>
          <svg v-else-if="someSelected" class="w-3 h-3 text-blue-400" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="3"><path stroke-linecap="round" d="M5 12h14" /></svg>
        </div>

        <!-- Search -->
        <div class="relative flex-1 max-w-[240px]">
          <Search class="absolute left-2.5 top-1/2 -translate-y-1/2 w-3.5 h-3.5 text-slate-400 pointer-events-none" />
          <input
            v-model="searchQuery"
            placeholder="Search rules..."
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

        <!-- Filter pills -->
        <div class="flex items-center gap-1 ml-auto">
          <button
            v-for="f in (['all', 'active', 'disabled'] as const)"
            :key="f"
            @click="statusFilter = f"
            class="px-2.5 py-1 text-[11px] font-semibold rounded-md transition-colors capitalize"
            :class="statusFilter === f
              ? 'bg-blue-600 text-white shadow-sm shadow-blue-500/20'
              : 'text-slate-500 dark:text-slate-400 hover:bg-slate-200 dark:hover:bg-slate-800'"
          >
            {{ f === 'all' ? `All (${allCount})` : f === 'active' ? `Active (${activeCount})` : `Disabled (${disabledCount})` }}
          </button>
        </div>

        <!-- Add Rule -->
        <button
          @click="emit('open-add')"
          class="shrink-0 flex items-center gap-1.5 px-3 py-1.5 text-xs font-semibold text-white bg-blue-600 hover:bg-blue-700 rounded-md transition-colors shadow-sm shadow-blue-500/20 ml-2"
        >
          <Plus class="w-3.5 h-3.5" /> Add Rule
        </button>
      </div>

      <!-- Batch action toolbar (selection active) -->
      <div v-else class="h-12 flex items-center gap-3 px-4">
        <div
          class="w-4 h-4 rounded border cursor-pointer flex items-center justify-center shrink-0 transition-colors"
          :class="allSelected ? 'bg-blue-600 border-blue-600' : 'bg-blue-600/20 border-blue-500'"
          @click="toggleSelectAll"
        >
          <svg v-if="allSelected" class="w-3 h-3 text-white" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="3"><path stroke-linecap="round" stroke-linejoin="round" d="M5 13l4 4L19 7" /></svg>
          <svg v-else class="w-3 h-3 text-blue-400" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="3"><path stroke-linecap="round" d="M5 12h14" /></svg>
        </div>

        <span class="text-xs font-semibold text-blue-600 dark:text-blue-400">
          {{ selectedIds.size }} selected
        </span>

        <div class="flex items-center gap-1.5 ml-2">
          <button
            @click="batchEnable"
            :disabled="operating"
            class="flex items-center gap-1 px-2.5 py-1 text-[11px] font-semibold rounded-md bg-emerald-50 dark:bg-emerald-900/30 text-emerald-700 dark:text-emerald-400 hover:bg-emerald-100 dark:hover:bg-emerald-900/50 transition-colors disabled:opacity-50"
          >
            <Power class="w-3 h-3" /> Enable
          </button>
          <button
            @click="batchDisable"
            :disabled="operating"
            class="flex items-center gap-1 px-2.5 py-1 text-[11px] font-semibold rounded-md bg-slate-100 dark:bg-slate-800 text-slate-600 dark:text-slate-400 hover:bg-slate-200 dark:hover:bg-slate-700 transition-colors disabled:opacity-50"
          >
            <PowerOff class="w-3 h-3" /> Disable
          </button>
          <button
            @click="batchDelete"
            :disabled="operating"
            class="flex items-center gap-1 px-2.5 py-1 text-[11px] font-semibold rounded-md bg-red-50 dark:bg-red-900/20 text-red-600 dark:text-red-400 hover:bg-red-100 dark:hover:bg-red-900/40 transition-colors disabled:opacity-50"
          >
            <Trash2 class="w-3 h-3" /> Delete
          </button>
        </div>

        <button
          @click="cancelSelection"
          class="ml-auto text-xs text-slate-500 dark:text-slate-400 hover:text-slate-700 dark:hover:text-slate-200 transition-colors"
        >
          Cancel
        </button>
      </div>
    </div>

    <!-- ============ RULE LIST ============ -->
    <div class="flex-1 overflow-auto divide-y divide-slate-100 dark:divide-slate-800/40">
      <div
        v-for="rule in filteredRules"
        :key="rule.id"
        class="flex items-center gap-3 px-4 py-3 group transition-colors"
        :class="selectedIds.has(rule.id) ? 'bg-blue-50/60 dark:bg-blue-900/10' : 'hover:bg-slate-50 dark:hover:bg-[#18181b]/60'"
      >
        <!-- Checkbox -->
        <div
          class="w-4 h-4 rounded border cursor-pointer flex items-center justify-center shrink-0 transition-colors"
          :class="selectedIds.has(rule.id)
            ? 'bg-blue-600 border-blue-600'
            : 'border-slate-300 dark:border-slate-600 hover:border-blue-400'"
          @click.stop="toggleSelect(rule.id)"
        >
          <svg v-if="selectedIds.has(rule.id)" class="w-3 h-3 text-white" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="3"><path stroke-linecap="round" stroke-linejoin="round" d="M5 13l4 4L19 7" /></svg>
        </div>

        <!-- Toggle switch -->
        <div
          class="w-8 h-4 rounded-full p-[2px] cursor-pointer relative transition-colors shrink-0"
          :class="rule.enabled ? 'bg-blue-600' : 'bg-slate-300 dark:bg-slate-700'"
          @click="toggleRule(rule.id)"
        >
          <div class="w-3 h-3 bg-white rounded-full transition-transform absolute" :class="rule.enabled ? 'translate-x-4' : 'translate-x-0'" />
        </div>

        <!-- Info -->
        <div class="flex-1 min-w-0">
          <div class="flex items-center gap-2">
            <span class="text-sm font-medium text-slate-800 dark:text-slate-200 truncate">{{ rule.name }}</span>
            <span
              class="shrink-0 text-[10px] font-bold uppercase tracking-wider px-1.5 py-0.5 rounded"
              :class="rule.enabled ? 'bg-emerald-500/10 text-emerald-600 dark:text-emerald-400' : 'bg-slate-200 dark:bg-slate-800 text-slate-500'"
            >{{ rule.enabled ? 'Active' : 'Off' }}</span>
            <Badge variant="outline" class="h-4 px-1.5 py-0 text-[9px]">tree</Badge>
            <Badge v-if="rule.protocol === 'udp'" variant="outline" class="h-4 px-1.5 py-0 text-[9px] text-emerald-600 border-emerald-300 dark:text-emerald-400 dark:border-emerald-700">udp</Badge>
            <Badge v-if="rule.protocol === 'both'" variant="outline" class="h-4 px-1.5 py-0 text-[9px] text-violet-600 border-violet-300 dark:text-violet-400 dark:border-violet-700">tcp+udp</Badge>
          </div>
          <div class="flex items-center gap-3 mt-0.5">
            <span class="text-xs font-mono text-slate-500 dark:text-slate-400 truncate">{{ rule.process_name }}</span>
            <span class="text-slate-300 dark:text-slate-700">&middot;</span>
            <Badge variant="outline" class="h-5 px-2 text-[10px] font-bold shrink-0">
              {{ groupMap.get(rule.proxy_group_id)?.name ?? 'default' }}
            </Badge>
          </div>
        </div>

        <!-- Action buttons (hover) -->
        <div class="shrink-0 flex items-center gap-1 opacity-0 group-hover:opacity-100 transition-opacity">
          <button
            @click="emit('open-edit', rule)"
            class="p-1.5 rounded text-slate-400 hover:text-blue-600 dark:hover:text-blue-400 hover:bg-slate-200 dark:hover:bg-slate-800 transition-colors"
            title="Edit rule"
          >
            <Pencil class="w-3.5 h-3.5" />
          </button>
          <button
            @click="deleteRule(rule.id)"
            class="p-1.5 rounded text-slate-400 hover:text-red-500 hover:bg-red-50 dark:hover:bg-red-900/20 transition-colors"
            title="Delete rule"
          >
            <Trash2 class="w-3.5 h-3.5" />
          </button>
        </div>
      </div>

      <!-- Empty state -->
      <div v-if="filteredRules.length === 0" class="py-16 flex flex-col items-center gap-3 text-slate-400">
        <svg class="w-10 h-10 opacity-30" xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke="currentColor"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5" d="M9 5H7a2 2 0 00-2 2v12a2 2 0 002 2h10a2 2 0 002-2V7a2 2 0 00-2-2h-2M9 5a2 2 0 002 2h2a2 2 0 002-2M9 5a2 2 0 012-2h2a2 2 0 012 2"/></svg>
        <p v-if="searchQuery || statusFilter !== 'all'" class="text-sm">No matching rules found.</p>
        <p v-else class="text-sm">No rules yet. Click <strong>Add Rule</strong> to create one.</p>
      </div>
    </div>

    <!-- Delete confirmation dialog -->
    <Dialog :open="deleteDialogOpen" @update:open="deleteDialogOpen = $event">
      <DialogContent class="sm:max-w-[400px]">
        <DialogHeader>
          <DialogTitle>Delete Rule</DialogTitle>
          <DialogDescription>
            Are you sure you want to delete <strong>{{ pendingDeleteLabel }}</strong>? This action cannot be undone.
          </DialogDescription>
        </DialogHeader>
        <DialogFooter class="gap-2 sm:gap-0">
          <button
            @click="deleteDialogOpen = false"
            class="inline-flex items-center justify-center rounded-md text-sm font-medium h-9 px-4 border border-slate-200 dark:border-slate-700 text-slate-700 dark:text-slate-300 hover:bg-slate-100 dark:hover:bg-slate-800 transition-colors"
          >
            Cancel
          </button>
          <button
            @click="onDeleteConfirmed"
            class="inline-flex items-center justify-center rounded-md text-sm font-medium h-9 px-4 bg-red-600 hover:bg-red-700 text-white transition-colors"
          >
            Delete
          </button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  </div>
</template>
