import { ref, readonly } from 'vue'
import type { ProcessInfo, AutoRule } from './types'

export interface SSEEvents {
  process_update: ProcessInfo
  auto_rule_changed: AutoRule
  auto_rule_matched: { rule_id: string; pid: number; process_name: string }
}

export type SSEEventType = keyof SSEEvents
export type SSEHandler<T extends SSEEventType = SSEEventType> = (data: SSEEvents[T]) => void

// Module-scoped singleton: one EventSource shared by the whole app, so components
// can subscribe to events and observe `connected` without opening duplicate streams.
const connected = ref(false)
const handlers = new Map<SSEEventType, Set<SSEHandler>>()
let eventSource: EventSource | null = null
let reconnectTimer: ReturnType<typeof setTimeout> | null = null
let connectInFlight: Promise<void> | null = null

async function connect(): Promise<void> {
  if (connectInFlight) return connectInFlight
  connectInFlight = (async () => {
    if (eventSource) eventSource.close()

    eventSource = new EventSource('/api/events')

    eventSource.onopen = () => {
      connected.value = true
    }

    eventSource.onerror = () => {
      connected.value = false
      eventSource?.close()
      eventSource = null

      if (reconnectTimer) clearTimeout(reconnectTimer)
      reconnectTimer = setTimeout(() => { void connect() }, 3000)
    }

    const eventTypes: SSEEventType[] = [
      'process_update',
      'auto_rule_changed',
      'auto_rule_matched',
    ]

    for (const eventType of eventTypes) {
      eventSource.addEventListener(eventType, (event: MessageEvent) => {
        const eventHandlers = handlers.get(eventType)
        if (!eventHandlers) return
        try {
          const data = JSON.parse(event.data)
          for (const handler of eventHandlers) {
            handler(data)
          }
        } catch {
          // Ignore parse errors
        }
      })
    }
  })()
  try {
    await connectInFlight
  } finally {
    connectInFlight = null
  }
}

function ensureConnected() {
  if (!eventSource && !connectInFlight) void connect()
}

function sseOn<T extends SSEEventType>(event: T, handler: SSEHandler<T>) {
  if (!handlers.has(event)) handlers.set(event, new Set())
  handlers.get(event)!.add(handler as SSEHandler)
}

function sseOff<T extends SSEEventType>(event: T, handler: SSEHandler<T>) {
  handlers.get(event)?.delete(handler as SSEHandler)
}

export function useSSE() {
  ensureConnected()
  return {
    connected: readonly(connected),
    on: sseOn,
    off: sseOff,
    connect,
  }
}
