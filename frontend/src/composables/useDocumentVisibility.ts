import { ref, onMounted, onUnmounted, type Ref } from 'vue'

// document.visibilityState reflects WebView2 IsVisible after the host
// calls put_IsVisible on minimize / restore / tray hide / tray show
// (webview_app::set_visible). Components use this to suspend periodic
// polls and other expensive work while the window is hidden.
//
// Returns a `visible` Ref<boolean>. Watching it with `{ immediate: true }`
// is safe — the ref is initialised from the current state synchronously.

export function useDocumentVisibility(): Ref<boolean> {
  const visible = ref(typeof document === 'undefined' || document.visibilityState !== 'hidden')

  function update(): void {
    visible.value = document.visibilityState !== 'hidden'
  }

  onMounted(() => {
    document.addEventListener('visibilitychange', update)
    // Re-sync once on mount in case state changed before the listener
    // attached (e.g. mount fired while the host was already hidden).
    update()
  })
  onUnmounted(() => {
    document.removeEventListener('visibilitychange', update)
  })

  return visible
}
