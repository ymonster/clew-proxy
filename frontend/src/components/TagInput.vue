<script setup lang="ts">
import { ref } from 'vue'
import { X } from 'lucide-vue-next'

const props = defineProps<{
  modelValue: string[]
  placeholder?: string
}>()

const emit = defineEmits<{
  'update:modelValue': [value: string[]]
}>()

const inputValue = ref('')

function addTag() {
  const val = inputValue.value.trim()
  if (val && !props.modelValue.includes(val)) {
    emit('update:modelValue', [...props.modelValue, val])
  }
  inputValue.value = ''
}

function removeTag(index: number) {
  const newValue = [...props.modelValue]
  newValue.splice(index, 1)
  emit('update:modelValue', newValue)
}

function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Enter' || e.key === ',') {
    e.preventDefault()
    addTag()
  }
  if (e.key === 'Backspace' && !inputValue.value && props.modelValue.length > 0) {
    removeTag(props.modelValue.length - 1)
  }
}
</script>

<template>
  <div class="flex flex-wrap gap-1.5 p-2 rounded-lg border border-slate-200 dark:border-slate-800 bg-slate-50 dark:bg-[#101922] min-h-[42px] focus-within:ring-1 focus-within:ring-blue-500 transition-all cursor-text">
    <span
      v-for="(tag, i) in modelValue"
      :key="i"
      class="font-mono text-[11px] bg-slate-200/50 dark:bg-[#1c242c] text-slate-700 dark:text-slate-300 px-2 py-0.5 border border-slate-300 dark:border-slate-700 rounded cursor-pointer hover:border-red-500 hover:text-red-500 inline-flex items-center gap-1 transition-colors"
      @click="removeTag(i)"
    >
      {{ tag }} <X class="w-3 h-3" />
    </span>
    <input
      v-model="inputValue"
      :placeholder="modelValue.length === 0 ? placeholder : ''"
      class="flex-1 min-w-[120px] bg-transparent text-xs font-mono text-slate-800 dark:text-white outline-none"
      @keydown="onKeydown"
      @blur="addTag"
    />
  </div>
</template>
