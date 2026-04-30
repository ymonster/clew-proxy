import { ref, readonly } from 'vue'
import type { ProcessInfo } from './types'

// Backend->frontend push channel. Replaces the previous SSE EventSource.
// The C++ host (webview_app) uses ICoreWebView2::PostWebMessageAsJson;
// each message arrives in JS as a `{ event, data }` envelope on
// `window.chrome.webview`'s `message` event. The full process tree is
// delivered inside every `process_update` push, so the frontend never
// polls /api/processes anymore.

declare global {
  interface Window {
    chrome?: {
      webview?: {
        postMessage(msg: unknown): void
        addEventListener(type: 'message', listener: (e: MessageEvent) => void): void
      }
    }
  }
}

export interface NotificationEvents {
  process_update: ProcessInfo[]
  auto_rule_changed: { action: string }
}

export type NotificationEvent = keyof NotificationEvents
export type NotificationHandler<T extends NotificationEvent = NotificationEvent>
  = (data: NotificationEvents[T]) => void

interface Envelope<T extends NotificationEvent> {
  event: T
  data: NotificationEvents[T]
}

const connected = ref(false)
const tree = ref<ProcessInfo[]>([])
const handlers = new Map<NotificationEvent, Set<NotificationHandler>>()

function dispatch(envelope: Envelope<NotificationEvent>): void {
  if (envelope.event === 'process_update') {
    tree.value = envelope.data as ProcessInfo[]
  }
  const set = handlers.get(envelope.event)
  if (!set) return
  for (const h of set) {
    try { h(envelope.data) } catch { /* don't let one handler kill others */ }
  }
}

let initialized = false
function init(): void {
  if (initialized) return
  initialized = true

  const wv = window.chrome?.webview
  if (!wv) {
    // Running in a regular browser (e.g. Vite dev server hit directly
    // instead of through WebView2). No push channel available; the UI
    // will only show static state until reload through WebView2.
    console.warn('[notify] not running in WebView2; push channel unavailable')
    return
  }

  wv.addEventListener('message', (event) => {
    const env = (event as MessageEvent).data as Envelope<NotificationEvent> | null
    if (!env || typeof env !== 'object' || typeof env.event !== 'string') return
    dispatch(env)
  })

  // Tell the backend we're ready to receive. webview_app::on_ready_ then
  // triggers process_projection::replay_to_frontend, which posts the
  // current snapshot back to us through the same WM_PUSH_TO_FRONTEND path.
  wv.postMessage({ type: 'ready' })
  connected.value = true
}

export function useNotifications() {
  init()
  return {
    connected: readonly(connected),
    // tree is intentionally NOT wrapped in readonly(): downstream
    // components pass it to recursive helpers that the type system
    // would otherwise reject. The dispatcher in this module is the only
    // writer; components are expected to read-only by convention.
    tree,
    on<T extends NotificationEvent>(event: T, handler: NotificationHandler<T>): void {
      if (!handlers.has(event)) handlers.set(event, new Set())
      handlers.get(event)!.add(handler as NotificationHandler)
    },
    off<T extends NotificationEvent>(event: T, handler: NotificationHandler<T>): void {
      handlers.get(event)?.delete(handler as NotificationHandler)
    },
  }
}

// Test bridge for Playwright + CDP. The Vue refs live in module scope and
// aren't reachable from page.evaluate(); these getters return their current
// values as plain JSON. Always-on because the WebView2 host is in-process —
// no user-facing surface to leak.
;(window as unknown as { __clew_debug: unknown }).__clew_debug = {
  tree: () => tree.value,
  connected: () => connected.value,
}
