<script setup lang="ts">
import { Button, Form, FormItem, Input, InputNumber, Select } from 'tdesign-vue-next'
import { computed, ref, watch } from 'vue'
import type { FolderSizeSettings } from '../settingsHost'
import SettingSwitch from './SettingSwitch.vue'

const props = defineProps<{
  settings: FolderSizeSettings
  busy: boolean
  disabled: boolean
  showFormat: boolean
  status?: string
  issue?: boolean
}>()
const emit = defineEmits<{ save: [settings: FolderSizeSettings] }>()
const draft = ref({ ...props.settings })
watch(() => props.settings, (settings) => {
  draft.value = { ...settings }
})

const modes = [
  { label: '智能单位', value: 'auto' },
  { label: '固定单位', value: 'fixed' },
  { label: '自定义单位', value: 'custom' },
]
const systems = [
  { label: 'IEC（1024 进制）', value: 'iec' },
  { label: 'SI（1000 进制）', value: 'si' },
]
const unitLabels = [
  'B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB', 'EiB', 'kB', 'MB', 'GB', 'TB', 'PB', 'EB',
]
const units = unitLabels.map((label) => ({ label, value: label }))
const valid = computed(() => {
  const settings = draft.value
  const label = settings.custom_unit_label.trim()
  return Number.isInteger(settings.decimal_places) && settings.decimal_places >= 0 &&
    settings.decimal_places <= 6 && [...label].length > 0 && [...label].length <= 12 &&
    /^[0-9]{1,19}$/.test(settings.custom_unit_bytes) &&
    BigInt(settings.custom_unit_bytes) >= 1n && BigInt(settings.custom_unit_bytes) <= 10n ** 18n
})
const changed = computed(() => JSON.stringify(draft.value) !== JSON.stringify(props.settings))
const preview = computed(() => {
  if (!valid.value) return undefined
  const settings = draft.value
  let unit = settings.fixed_unit
  if (settings.unit_mode === 'auto') unit = settings.unit_system === 'iec' ? 'MiB' : 'MB'
  const custom = settings.unit_mode === 'custom'
  const unitIndex = unitLabels.indexOf(unit)
  const divisor = custom ? BigInt(settings.custom_unit_bytes) :
    unit === 'B' ? 1n : unitIndex < 7 ? 1024n ** BigInt(unitIndex) : 1000n ** BigInt(unitIndex - 6)
  const places = !custom && unit === 'B' ? 0 : settings.decimal_places
  const scale = 10n ** BigInt(places)
  const rounded = (1572864n * scale * 2n + divisor) / (divisor * 2n)
  const value = rounded || 1n
  const number = new Intl.NumberFormat(undefined, {
    maximumFractionDigits: places, minimumFractionDigits: places, useGrouping: false,
  }).format(Number(value) / Number(scale))
  return `${rounded ? '' : '<'}${number} ${custom ? settings.custom_unit_label.trim() : unit}`
})
</script>

<template>
  <SettingSwitch
    label="显示文件夹大小"
    description="用 Everything 索引补充资源管理器详细信息视图中的「大小」列。"
    :checked="settings.enabled"
    :disabled="disabled"
    :busy="busy"
    :status="status"
    :issue="issue"
    @change="(enabled) => emit('save', { ...settings, enabled })"
  />
  <Form v-if="settings.enabled || showFormat" class="size-form" layout="vertical" :disabled="disabled || busy"
    @submit="({ e }) => { e?.preventDefault(); if (valid && changed) emit('save', draft) }">
    <div class="size-fields">
      <FormItem label="单位模式">
        <Select v-model="draft.unit_mode" :options="modes" />
      </FormItem>
      <FormItem v-if="draft.unit_mode === 'auto'" label="单位制">
        <Select v-model="draft.unit_system" :options="systems" />
      </FormItem>
      <FormItem v-else-if="draft.unit_mode === 'fixed'" label="固定单位">
        <Select v-model="draft.fixed_unit" :options="units" />
      </FormItem>
      <template v-else>
        <FormItem label="单位名称">
          <Input v-model="draft.custom_unit_label" :maxlength="24" />
        </FormItem>
        <FormItem label="每单位字节数">
          <Input v-model="draft.custom_unit_bytes" :maxlength="19" />
        </FormItem>
      </template>
      <FormItem label="小数位数">
        <InputNumber v-model="draft.decimal_places" :min="0" :max="6" :decimal-places="0" />
      </FormItem>
    </div>
    <div class="size-actions">
      <span v-if="preview">1,572,864 字节 → {{ preview }}</span>
      <span v-else>请输入有效的单位名称、字节数和小数位数。</span>
      <Button type="submit" :disabled="!valid || !changed || disabled || busy">保存显示设置</Button>
    </div>
  </Form>
</template>

<style scoped>
.size-form {
  padding: 12px 0 6px;
}

.size-fields {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 12px;
}

.size-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  color: var(--muted);
}
</style>
