import { ref, readonly } from 'vue'

export type StatusKind = 'error' | 'success' | 'info'

export interface StatusMessage {
  kind: StatusKind
  text: string
  id: number
}

const AUTO_CLEAR_MS = 5000

const current = ref<StatusMessage | null>(null)
let nextId = 1
let clearTimer: ReturnType<typeof setTimeout> | null = null

function push(kind: StatusKind, text: string) {
  if (clearTimer) {
    clearTimeout(clearTimer)
    clearTimer = null
  }
  current.value = { kind, text, id: nextId++ }
  clearTimer = setTimeout(() => {
    current.value = null
    clearTimer = null
  }, AUTO_CLEAR_MS)
}

function formatError(e: unknown): string {
  if (e instanceof Error) return e.message
  if (typeof e === 'string') return e
  try {
    return JSON.stringify(e)
  } catch {
    return String(e)
  }
}

export function pushError(e: unknown, prefix?: string) {
  const msg = formatError(e)
  push('error', prefix ? `${prefix}: ${msg}` : msg)
}

export function pushSuccess(text: string) {
  push('success', text)
}

export function pushInfo(text: string) {
  push('info', text)
}

export function clearStatus() {
  if (clearTimer) {
    clearTimeout(clearTimer)
    clearTimer = null
  }
  current.value = null
}

export function useStatusBus() {
  return {
    current: readonly(current),
    pushError,
    pushSuccess,
    pushInfo,
    clearStatus,
  }
}
