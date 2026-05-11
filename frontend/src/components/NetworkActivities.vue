<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted, shallowRef } from 'vue'
import { AgGridVue } from 'ag-grid-vue3'
import {
  AllCommunityModule,
  ModuleRegistry,
  themeAlpine,
  colorSchemeDark,
  type ColDef,
  type GridApi,
} from 'ag-grid-community'

ModuleRegistry.registerModules([AllCommunityModule])
import { getNetworkConnections } from '@/api/client'
import type { NetworkConnection } from '@/api/types'
import { Input } from '@/components/ui/input'
import { Search, Moon } from 'lucide-vue-next'
import { useTheme } from '@/composables/useTheme'
import { useDocumentVisibility } from '@/composables/useDocumentVisibility'

const props = defineProps<{
  selectedPid?: number
}>()

const { isDark } = useTheme()

const rowData = ref<NetworkConnection[]>([])
const filterText = ref('')
const gridApi = shallowRef<GridApi | null>(null)

// Stable row id: include local_ip so dual-stack sockets (IPv4 + IPv6 on the
// same local_port for the same PID) get distinct rows. Without local_ip the
// two collide and AG Grid reuses one DOM/class state for both, leaving stale
// `dead-process-row` styling on the duplicate.
const rowIdGetter = (params: { data: NetworkConnection }) =>
  `${params.data.protocol}-${params.data.pid}-${params.data.local_ip}-${params.data.local_port}-${params.data.remote_ip}-${params.data.remote_port}`

// Defined as a stable constant outside the template so vue doesn't allocate
// a new object on every render (which would push AG Grid into a re-eval that
// races with row data updates).
const rowClassRules = {
  'dead-process-row': (params: { data?: NetworkConnection }) =>
    !!params.data && !params.data.pid_alive,
}

function stateDotColor(state: string): string {
  if (state === 'ESTABLISHED') return '#22c55e'
  if (state === 'BOUND') return '#a855f7'
  if (state === 'TIME_WAIT' || state === 'CLOSE_WAIT') return '#eab308'
  return '#a1a1aa'
}

const gridTheme = computed(() =>
  isDark.value ? themeAlpine.withPart(colorSchemeDark) : themeAlpine
)

const hasData = computed(() => rowData.value.length > 0)

const selectedProcessName = computed(() => {
  if (rowData.value.length > 0) return rowData.value[0]?.process_name ?? 'this process'
  return 'this process'
})

const columnDefs = computed<ColDef<NetworkConnection>[]>(() => {
  const allProcsView = props.selectedPid == null

  const pidCol: ColDef<NetworkConnection> = {
    headerName: 'PID',
    field: 'pid',
    width: 75,
    cellClass: 'font-mono',
  }

  // Every column except Process has a bounded max content width, so they get
  // fixed widths sized to their longest value. Process is the only flex column
  // — it absorbs all slack when the panel is wide and shrinks to minWidth when
  // narrow, so the bounded columns never get squeezed (which is what forced the
  // "double-click the header to auto-fit" dance before). In the per-PID view
  // there is no Process column, so Remote flexes instead to fill the width.
  const cols: ColDef<NetworkConnection>[] = []

  if (allProcsView) {
    cols.push(
      { headerName: 'Process', field: 'process_name', minWidth: 150, flex: 1 },
      pidCol,
    )
  } else {
    cols.push(pidCol)
  }

  cols.push(
    {
      headerName: 'Proto',
      field: 'protocol',
      width: 72,
      cellRenderer: (params: { value: string }) => {
        const v = params.value
        if (v === 'UDP') {
          return `<span style="display:inline-block;padding:1px 6px;border-radius:9999px;font-size:10px;font-weight:600;background:rgba(168,85,247,0.15);color:#a855f7;">UDP</span>`
        }
        return `<span style="display:inline-block;padding:1px 6px;border-radius:9999px;font-size:10px;font-weight:600;background:rgba(59,130,246,0.15);color:#3b82f6;">TCP</span>`
      },
    },
    {
      headerName: 'Remote',
      valueGetter: (params) => {
        if (!params.data) return ''
        if (!params.data.remote_ip) return ''
        return `${params.data.remote_ip}:${params.data.remote_port}`
      },
      ...(allProcsView ? { width: 210, minWidth: 150 } : { flex: 1, minWidth: 160 }),
      cellClass: 'font-mono',
    },
    {
      headerName: 'State',
      field: 'state',
      width: 140,
      cellRenderer: (params: { value: string }) => {
        const value = params.value
        if (!value) return ''
        const dotColor = stateDotColor(value)
        return `<span style="display:inline-flex;align-items:center;gap:6px;"><span style="display:inline-block;width:6px;height:6px;border-radius:50%;background:${dotColor};"></span>${value}</span>`
      },
    },
    {
      headerName: 'Proxy',
      field: 'proxy_status',
      width: 100,
      cellRenderer: (params: { value: string }) => {
        const value = params.value
        if (value === 'PROXIED') {
          return `<span style="display:inline-block;padding:1px 8px;border-radius:9999px;font-size:11px;font-weight:600;background:rgba(34,197,94,0.15);color:#22c55e;">PROXIED</span>`
        }
        if (value === 'IGNORED') {
          return `<span style="display:inline-block;padding:1px 8px;border-radius:9999px;font-size:11px;font-weight:600;background:rgba(234,179,8,0.15);color:#eab308;">IGNORED</span>`
        }
        return `<span style="display:inline-block;padding:1px 8px;border-radius:9999px;font-size:11px;font-weight:600;background:rgba(161,161,170,0.15);color:#a1a1aa;">DIRECT</span>`
      },
    },
    {
      // The header "Local Port" is the widest thing in this column (the value
      // is at most 5 digits), so the width is sized to fit the label.
      headerName: 'Local Port',
      field: 'local_port',
      width: 118,
      cellClass: 'font-mono',
    },
  )

  return cols
})

const defaultColDef = {
  sortable: true,
  resizable: true,
}

const documentVisible = useDocumentVisibility()
let timer: ReturnType<typeof setInterval> | null = null

// pid 0 (Idle / orphaned TIME_WAIT sockets) and pid 4 (System / kernel SMB
// etc.) are not interesting in the firehose "all processes" feed. Hide them
// there; when a process is explicitly selected in the tree we still show
// whatever it owns, including pid 0/4 if the user really clicked them.
const SYSTEM_PIDS = new Set([0, 4])

async function fetchConnections() {
  try {
    const conns = await getNetworkConnections(props.selectedPid)
    rowData.value = props.selectedPid == null
      ? conns.filter(c => !SYSTEM_PIDS.has(c.pid))
      : conns
  } catch {
    // Backend not available
  }
}

function startPolling() {
  stopPolling()
  fetchConnections()
  timer = setInterval(fetchConnections, 2000)
}

function stopPolling() {
  if (timer !== null) {
    clearInterval(timer)
    timer = null
  }
}

function onGridReady(params: { api: GridApi }) {
  gridApi.value = params.api
}

watch(() => props.selectedPid, () => {
  if (documentVisible.value) fetchConnections()
})

watch(filterText, (text) => {
  if (gridApi.value) {
    gridApi.value.setGridOption('quickFilterText', text)
  }
})

// Suspend the 2s /api/tcp poll while hidden. Restart immediately on
// visible — the table is data-dense so a stale state on restore would
// be visibly wrong.
watch(documentVisible, v => v ? startPolling() : stopPolling())

onMounted(() => {
  if (documentVisible.value) startPolling()
})

onUnmounted(() => {
  stopPolling()
})
</script>

<template>
  <div class="h-full w-full flex flex-col">
    <!-- Filter bar -->
    <div class="shrink-0 pb-3">
      <div class="relative w-72">
        <Search class="absolute left-2 top-1/2 -translate-y-1/2 size-3.5 text-muted-foreground pointer-events-none" />
        <Input
          v-model="filterText"
          placeholder="Filter target address or state..."
          class="h-8 pl-7 text-xs"
        />
      </div>
    </div>

    <!-- AG Grid or Empty State -->
    <template v-if="hasData">
      <AgGridVue
        style="flex: 1 1 0; min-height: 0; width: 100%;"
        :theme="gridTheme"
        :rowData="rowData"
        :columnDefs="columnDefs"
        :defaultColDef="defaultColDef"
        :rowHeight="28"
        :headerHeight="32"
        :suppressCellFocus="true"
        :animateRows="false"
        :getRowId="rowIdGetter"
        :rowClassRules="rowClassRules"
        @grid-ready="onGridReady"
      />
    </template>
    <template v-else>
      <div class="flex-1 flex flex-col items-center justify-center text-center px-8">
        <Moon class="size-12 text-muted-foreground/30 mb-4" />
        <p class="text-sm text-muted-foreground">
          We haven't detected any recent network activity
          <template v-if="selectedPid != null"> from {{ selectedProcessName }}</template>.
        </p>
        <p class="text-xs text-muted-foreground/70 mt-2 max-w-sm">
          Connections will appear here once the process starts making network requests. Make sure the proxy engine is running.
        </p>
      </div>
    </template>
  </div>
</template>

<style>
.dead-process-row {
  opacity: 0.4;
  text-decoration: line-through;
}
</style>
