<script setup lang="ts">
import { Switch } from 'tdesign-vue-next'
import 'tdesign-vue-next/es/switch/style/index.css'

const props = defineProps<{
  label: string
  description: string
  checked: boolean
  disabled: boolean
  busy: boolean
  status?: string
  issue?: boolean
}>()

const emit = defineEmits<{ change: [enabled: boolean] }>()

function change(enabled: boolean) {
  if (!props.disabled && !props.busy) emit('change', enabled)
}
</script>

<template>
  <div class="setting">
    <div>
      <div class="label">{{ label }}</div>
      <p>{{ description }}</p>
      <p v-if="status" class="feedback" :class="{ issue }">{{ status }}</p>
    </div>
    <Switch
      class="setting-control"
      :value="checked"
      :disabled="disabled"
      :tabindex="disabled ? -1 : 0"
      role="switch"
      :aria-label="label"
      :aria-checked="checked"
      :aria-disabled="disabled || busy"
      @change="(value) => change(value === true)"
      @keydown.space.prevent="change(!checked)"
      @keydown.enter.prevent="change(!checked)"
    />
  </div>
</template>

<style scoped>
.setting {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: start;
  gap: 20px;
  padding: 19px 0;
}

.setting + .setting {
  border-top: 1px solid var(--line);
}

.label {
  font-size: 15px;
  font-weight: 600;
}

p {
  margin: 5px 0 0;
  color: var(--muted);
  font-size: 13px;
  line-height: 1.65;
}

.feedback {
  color: var(--accent);
  overflow-wrap: anywhere;
}

.feedback.issue {
  color: var(--warning);
}

.setting-control {
  margin-top: 1px;
}

.setting-control:focus-visible {
  outline: 2px solid var(--accent);
  outline-offset: 4px;
}
</style>
