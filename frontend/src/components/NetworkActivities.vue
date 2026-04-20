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

const props = defineProps<{
  selectedPid?: number
}>()

const { isDark } = useTheme()

const rowData = ref<NetworkConnection[]>([])
const filterText = ref('')
const gridApi = shallowRef<GridApi | null>(null)

const gridTheme = computed(() =>
  isDark.value ? themeAlpine.withPart(colorSchemeDark) : themeAlpine
)

const hasData = computed(() => rowData.value.length > 0)

const selectedProcessName = computed(() => {
  if (rowData.value.length > 0) return rowData.value[0]?.process_name ?? 'this process'
  return 'this process'
})

const columnDefs = computed<ColDef<NetworkConnection>[]>(() => {
  const cols: ColDef<NetworkConnection>[] = []

  // Show process + PID columns when viewing all processes
  if (props.selectedPid == null) {
    cols.push(
      {
        headerName: 'Process',
        field: 'process_name',
        minWidth: 120,
        flex: 1,
      },
      {
        headerName: 'PID',
        field: 'pid',
        width: 70,
        maxWidth: 80,
        cellClass: 'font-mono',
      },
    )
  }

  cols.push(
    {
      headerName: 'Proto',
      field: 'protocol',
      width: 65,
      maxWidth: 70,
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
      minWidth: 160,
      flex: 2,
      cellClass: 'font-mono',
    },
    {
      headerName: 'State',
      field: 'state',
      width: 130,
      maxWidth: 150,
      cellRenderer: (params: { value: string }) => {
        const value = params.value
        if (!value) return ''
        const dotColor = value === 'ESTABLISHED'
          ? '#22c55e'
          : value === 'BOUND'
            ? '#a855f7'
            : value === 'TIME_WAIT' || value === 'CLOSE_WAIT'
              ? '#eab308'
              : '#a1a1aa'
        return `<span style="display:inline-flex;align-items:center;gap:6px;"><span style="display:inline-block;width:6px;height:6px;border-radius:50%;background:${dotColor};"></span>${value}</span>`
      },
    },
    {
      headerName: 'Proxy',
      field: 'proxy_status',
      width: 110,
      maxWidth: 120,
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
      headerName: 'Local Port',
      field: 'local_port',
      width: 90,
      maxWidth: 100,
      cellClass: 'font-mono',
    },
  )

  // When a specific PID is selected, add PID column at front
  if (props.selectedPid != null) {
    cols.unshift({
      headerName: 'PID',
      field: 'pid',
      width: 70,
      maxWidth: 80,
      cellClass: 'font-mono',
    })
  }

  return cols
})

const defaultColDef = {
  sortable: true,
  resizable: true,
}

let timer: ReturnType<typeof setInterval> | null = null

async function fetchConnections() {
  try {
    rowData.value = await getNetworkConnections(props.selectedPid)
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
  fetchConnections()
})

watch(filterText, (text) => {
  if (gridApi.value) {
    gridApi.value.setGridOption('quickFilterText', text)
  }
})

onMounted(() => {
  startPolling()
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
        :getRowId="(params: { data: NetworkConnection }) => `${params.data.protocol}-${params.data.pid}-${params.data.local_port}-${params.data.remote_ip}-${params.data.remote_port}`"
        :getRowClass="(params: { data?: NetworkConnection }) => params.data && !params.data.pid_alive ? 'dead-process-row' : ''"
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
