import { ref, readonly } from 'vue'

const STORAGE_KEY = 'clew-theme'

const isDark = ref(true)

function applyTheme(dark: boolean) {
  if (dark) {
    document.documentElement.classList.add('dark')
  } else {
    document.documentElement.classList.remove('dark')
  }
}

function initTheme() {
  const stored = localStorage.getItem(STORAGE_KEY)
  isDark.value = stored ? stored === 'dark' : true
  applyTheme(isDark.value)
}

function toggleTheme() {
  isDark.value = !isDark.value
  localStorage.setItem(STORAGE_KEY, isDark.value ? 'dark' : 'light')
  applyTheme(isDark.value)
}

export function useTheme() {
  return {
    isDark: readonly(isDark),
    toggleTheme,
    initTheme,
  }
}
