<script setup lang="ts">
import {
  IconDeviceFloppy,
  IconFolder,
  IconRefresh,
  IconRuler2,
  IconTypography,
} from '@tabler/icons-vue'
import { Button, Select } from 'tdesign-vue-next'
import 'tdesign-vue-next/es/select/style/index.css'
import { computed, onMounted, ref } from 'vue'
import { toast } from 'vue-sonner'
import { hasSettingsHost, requestSettings, type AppearanceSettings } from '../settingsHost'
import CommandBarSettings from './CommandBarSettings.vue'

const defaults: AppearanceSettings = {
  depth: '2',
  separator: '\\',
  tabs: { width_mode: 'system', width: 320, min_width: 120, max_width: 480 },
}
const connected = hasSettingsHost()
const baseline = ref<AppearanceSettings>()
const draft = ref(copy(defaults))
const busy = ref(false)
const depthOptions = [
  ...Array.from({ length: 32 }, (_, index) => ({
    label: `${index + 1} 级`,
    value: String(index + 1),
  })),
  { label: '完整路径', value: 'all' },
]
const separatorOptions = [
  { label: '反斜杠 \\', value: '\\' },
  { label: '斜杠 /', value: ' / ' },
  { label: '间隔号 ·', value: ' · ' },
]
const widthModeOptions = [
  { label: '系统默认', value: 'system' },
  { label: '固定宽度', value: 'fixed' },
  { label: '随内容', value: 'content' },
]
const fieldsDisabled = computed(() => busy.value || (connected && !baseline.value))
const sampleParts = ['项目', '司南', '开发资料', '界面设计']
const samplePath = 'D:\\' + sampleParts.join('\\')
const sampleTitle = computed(() =>
  draft.value.depth === 'all'
    ? samplePath
    : sampleParts.slice(-Number(draft.value.depth)).join(draft.value.separator),
)

function copy(settings: AppearanceSettings): AppearanceSettings {
  return { ...settings, tabs: { ...settings.tabs } }
}

function confirmSelectOption(event: KeyboardEvent) {
  const field = event.currentTarget as HTMLLabelElement
  // TDesign's Input handles Enter separately from the Select's arrow key navigation.
  const highlighted = document.querySelector<HTMLElement>(
    `.${field.id}-menu:not(.t-popup--animation-leave-active) .t-select-option__hover`,
  )
  if (highlighted?.getClientRects().length) highlighted.click()
}

const desired = computed(() => {
  const savedTabs = (baseline.value ?? defaults).tabs
  const { width_mode, width, min_width, max_width } = draft.value.tabs
  const tabs = { ...savedTabs, width_mode }
  if (width_mode === 'fixed') tabs.width = width
  if (width_mode === 'content') {
    tabs.min_width = min_width
    tabs.max_width = max_width
  }
  return { ...draft.value, tabs }
})

const widthError = computed(() => {
  const { width, min_width, max_width } = desired.value.tabs
  if (
    [width, min_width, max_width].some(
      (value) => !Number.isInteger(value) || value < 80 || value > 1000,
    )
  )
    return '宽度必须是 80 至 1000 DIP 内的整数。'
  if (min_width > max_width) return '最小宽度不能大于最大宽度。'
  return ''
})
const dirty = computed(() => JSON.stringify(desired.value) !== JSON.stringify(baseline.value))
const previewSize = computed(() => {
  if (widthError.value) return {}
  const { width_mode, width, min_width, max_width } = desired.value.tabs
  if (width_mode === 'fixed') return { width: width + 'px' }
  if (width_mode === 'content')
    return {
      width: 'max-content',
      maxWidth: `min(100%, ${max_width}px)`,
      minWidth: `min(100%, ${min_width}px)`,
    }
  return {}
})

function showSettings(settings: AppearanceSettings) {
  baseline.value = settings
  draft.value = copy(settings)
}

async function load(notify = false) {
  if (!connected || busy.value) return
  busy.value = true
  try {
    const { code, result } = await requestSettings({ action: 'appearance.settings.get' })
    if (code !== 0 || !result.depth || !result.separator || !result.tabs)
      throw new Error(result.message || '无法读取标签外观。')
    showSettings({ depth: result.depth, separator: result.separator, tabs: result.tabs })
    if (notify) toast.success('已重新载入标签外观。')
  } catch (error) {
    toast.error('读取失败。', { description: error instanceof Error ? error.message : undefined })
  } finally {
    busy.value = false
  }
}

async function save() {
  if (!baseline.value || !dirty.value || widthError.value || busy.value) return
  const settings = copy(desired.value)
  busy.value = true
  try {
    const { code, result } = await requestSettings({
      action: 'appearance.settings.save',
      settings,
      expected: baseline.value,
    })
    if (result.conflict) {
      toast.warning('标签外观已在其他窗口修改。', { description: '请重新载入后再编辑。' })
    } else if (result.saved) {
      showSettings(settings)
      if (code !== 0 || !result.runtime_applied) {
        toast.warning('外观已保存，应用状态待确认。', {
          description: result.reload_error || '可在「常规」页查看代理与标签定制状态。',
        })
      } else {
        toast.success('标签外观已保存。')
      }
    } else {
      toast.error('保存失败。', { description: result.message })
    }
  } catch (error) {
    toast.error('保存失败。', { description: error instanceof Error ? error.message : undefined })
  } finally {
    busy.value = false
  }
}

onMounted(() => load())
</script>

<template>
  <main :aria-busy="busy">
    <div class="content">
      <header class="page-top">
        <div>
          <h1>标签外观</h1>
          <p>调整资源管理器标签的默认标题和宽度。</p>
        </div>
        <div class="actions">
          <Button variant="outline" :disabled="!connected || busy" @click="load(true)">
            <template #icon><IconRefresh :size="17" aria-hidden="true" /></template>
            重新载入
          </Button>
          <Button
            theme="primary"
            type="submit"
            form="appearance-settings"
            :disabled="!baseline || !dirty || !!widthError || busy"
          >
            <template #icon><IconDeviceFloppy :size="17" aria-hidden="true" /></template>
            保存外观
          </Button>
        </div>
      </header>

      <div class="appearance-grid">
        <form id="appearance-settings" @submit.prevent="save">
          <p v-if="!connected" class="page-status">
            可在浏览器中预览。保存设置请使用司南桌面窗口。
          </p>
          <p v-else-if="!baseline && !busy" class="page-status">读取失败，请重新载入。</p>

          <fieldset class="card fields" :disabled="fieldsDisabled">
            <div class="section-heading">
              <span class="section-icon"><IconTypography :size="22" aria-hidden="true" /></span>
              <h2>标题显示</h2>
            </div>
            <label id="title-depth" class="field" @keydown.enter.prevent="confirmSelectOption">
              <span>路径层级</span>
              <Select
                v-model="draft.depth"
                :options="depthOptions"
                :disabled="fieldsDisabled"
                :popup-props="{ overlayClassName: 'title-depth-menu' }"
              />
            </label>
            <label id="title-separator" class="field" @keydown.enter.prevent="confirmSelectOption">
              <span>分隔符</span>
              <Select
                v-model="draft.separator"
                :options="separatorOptions"
                :disabled="fieldsDisabled || draft.depth === 'all'"
                :popup-props="{ overlayClassName: 'title-separator-menu' }"
              />
            </label>
            <p class="hint">单个标签的设置优先，其次是目录规则，最后使用此处的默认值。</p>
          </fieldset>

          <fieldset class="card fields" :disabled="fieldsDisabled">
            <div class="section-heading">
              <span class="section-icon"><IconRuler2 :size="22" aria-hidden="true" /></span>
              <h2>标签宽度</h2>
            </div>
            <label id="width-mode" class="field" @keydown.enter.prevent="confirmSelectOption">
              <span>宽度模式</span>
              <Select
                v-model="draft.tabs.width_mode"
                :options="widthModeOptions"
                :disabled="fieldsDisabled"
                :popup-props="{ overlayClassName: 'width-mode-menu' }"
              />
            </label>
            <div v-if="draft.tabs.width_mode === 'fixed'" class="field">
              <label for="fixed-width">固定宽度（DIP）</label>
              <input
                id="fixed-width"
                v-model.number="draft.tabs.width"
                type="number"
                min="80"
                max="1000"
                step="1"
              />
            </div>
            <template v-else-if="draft.tabs.width_mode === 'content'">
              <div class="field">
                <label for="min-width">最小宽度（DIP）</label>
                <input
                  id="min-width"
                  v-model.number="draft.tabs.min_width"
                  type="number"
                  min="80"
                  max="1000"
                  step="1"
                />
              </div>
              <div class="field">
                <label for="max-width">最大宽度（DIP）</label>
                <input
                  id="max-width"
                  v-model.number="draft.tabs.max_width"
                  type="number"
                  min="80"
                  max="1000"
                  step="1"
                />
              </div>
            </template>
            <p v-if="widthError" class="field-error">{{ widthError }}</p>
          </fieldset>
        </form>

        <section class="card preview" aria-labelledby="preview-heading">
          <div class="preview-heading">
            <h2 id="preview-heading">示例预览</h2>
            <span v-if="baseline && dirty" class="draft-label">未保存</span>
          </div>
          <div class="preview-stage">
            <div class="preview-tab" :style="previewSize">
              <IconFolder :size="18" aria-hidden="true" />
              <span class="preview-title" :title="sampleTitle">{{ sampleTitle }}</span>
            </div>
          </div>
          <p class="sample-path">
            <span>示例路径</span><code>{{ samplePath }}</code>
          </p>
        </section>
      </div>
    </div>
  </main>
  <CommandBarSettings />
</template>

<style scoped>
h1,
h2,
p {
  margin: 0;
}

h1 {
  font-size: 32px;
  font-weight: 650;
  letter-spacing: -0.03em;
}

h2 {
  font-size: 16px;
  font-weight: 650;
}

.page-top,
.actions,
.section-heading,
.preview-heading {
  display: flex;
  align-items: center;
  gap: 12px;
}

.page-top,
.preview-heading {
  justify-content: space-between;
}

.page-top {
  flex-wrap: wrap;
  margin-bottom: 26px;
}

.actions {
  flex-wrap: wrap;
}

.page-top p {
  margin-top: 5px;
  color: var(--muted);
}

.appearance-grid {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  align-items: start;
  gap: 16px;
}

form {
  display: grid;
  gap: 16px;
}

.fields,
.preview {
  min-width: 0;
  margin: 0;
  padding: 22px 24px;
}

.section-icon {
  display: inline-grid;
  width: 39px;
  height: 39px;
  flex: none;
  place-items: center;
  border-radius: 10px;
  background: #e7f1ee;
  color: #176657;
}

.field {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  align-items: center;
  gap: 12px;
  margin-top: 16px;
}

.field > input {
  box-sizing: border-box;
  width: 100%;
  min-height: 34px;
  padding: 4px 9px;
  border: 1px solid #cfddda;
  border-radius: 6px;
  background: #fff;
  color: var(--ink);
  font: inherit;
}

.field > input:focus-visible {
  outline: 2px solid var(--accent);
  outline-offset: 1px;
}

.field > input:disabled {
  color: var(--muted);
  background: #f3f6f5;
}

.hint,
.field-error,
.sample-path {
  margin-top: 14px;
  font-size: 13px;
}

.hint,
.page-status,
.sample-path {
  color: var(--muted);
}

.field-error,
.draft-label {
  color: var(--warning);
}

.draft-label {
  font-size: 12px;
}

.preview-stage {
  min-height: 56px;
  margin-top: 16px;
  padding: 16px 12px 0;
  border-radius: 10px;
  background: #e9f0ee;
}

.preview-tab {
  display: flex;
  align-items: center;
  gap: 8px;
  box-sizing: border-box;
  width: max-content;
  max-width: 100%;
  padding: 10px 12px;
  border: 1px solid #d5e4df;
  border-radius: 8px;
  background: #fff;
}

.preview-tab svg {
  flex: none;
  color: var(--accent);
}

.preview-title {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sample-path {
  display: flex;
  flex-wrap: wrap;
  gap: 6px 12px;
  overflow-wrap: anywhere;
}

@media (max-width: 880px) {
  .appearance-grid {
    grid-template-columns: minmax(0, 1fr);
  }
}

@media (max-width: 420px) {
  .field {
    grid-template-columns: minmax(0, 1fr);
    gap: 5px;
  }
}
</style>
