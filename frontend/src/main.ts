import { createApp } from 'vue'
import App from './App.vue'
import './style.css'
import { useTheme } from './composables/useTheme'

const { initTheme } = useTheme()
initTheme()

createApp(App).mount('#app')
