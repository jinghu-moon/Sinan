<script setup lang="ts">
import { IconKeyboard, IconRefresh, IconSearch } from '@tabler/icons-vue'
import {
  Button,
  Dialog,
  Form,
  FormItem,
  Input,
  Table,
  Tag,
  type PrimaryTableCol,
} from 'tdesign-vue-next'
import 'tdesign-vue-next/es/dialog/style/index.css'
import 'tdesign-vue-next/es/form/style/index.css'
import 'tdesign-vue-next/es/table/style/index.css'
import 'tdesign-vue-next/es/tag/style/index.css'
import { computed, onMounted, ref } from 'vue'
import { toast } from 'vue-sonner'
import { hasSettingsHost, requestSettings, type ShortcutSettings } from '../settingsHost'

const shortcuts = [
  { id: 'toggle_pin', label: '切换固定标签' },
  { id: 'toggle_protection', label: '切换标签保护' },
  { id: 'toggle_directory_lock', label: '切换目录锁定' },
  { id: 'open_terminal', label: '在此打开终端' },
  { id: 'copy_directory', label: '复制目录路径' },
  { id: 'copy_selection', label: '复制选中项路径' },
  { id: 'locate_clipboard', label: '从剪贴板定位' },
  { id: 'locate_clipboard_global', label: '从剪贴板定位', global: true },
  { id: 'new_folder', label: '新建文件夹' },
  { id: 'new_file', label: '新建文件' },
  { id: 'reopen_closed_tab', label: '重开刚关闭的标签' },
  { id: 'merge_duplicate_tabs', label: '合并重复标签' },
  { id: 'open_parent_tab', label: '在新标签打开父目录' },
  { id: 'toggle_file_extensions', label: '切换文件扩展名显示' },
  { id: 'transfer_selection', label: '发送到标签' },
  { id: 'extract_archive', label: '解压并定位' },
]
type Shortcut = (typeof shortcuts)[number]

const columns: PrimaryTableCol[] = [
  { colKey: 'label', title: '操作' },
  { colKey: 'scope', title: '生效范围', width: 150 },
  { colKey: 'binding', title: '按键', width: 220 },
  { colKey: 'edit', title: '设置', width: 82 },
]

const connected = hasSettingsHost()
const snapshot = ref<ShortcutSettings>()
const busy = ref(false)
const search = ref('')
const editing = ref<Shortcut>()
const chord = ref('')
const folderName = ref('')
const recording = ref(false)
const captureHint = ref('')
const listener = computed(() => snapshot.value?.shortcut_listener)
const rows = computed(() => {
  const query = search.value.trim().toLowerCase()
  return shortcuts
    .map((shortcut) => ({
      ...shortcut,
      scope: shortcut.global ? '全局' : '资源管理器前台',
      binding: snapshot.value?.shortcut_settings[shortcut.id] ?? '',
      error: listener.value?.errors?.[shortcut.id],
    }))
    .filter((row) =>
      [row.label, row.scope, row.binding].some((value) => value.toLowerCase().includes(query)),
    )
})
const listenerIssue = computed(() => {
  const state = listener.value
  if (state?.error) return state.error
  if (!state?.running) return ''
  const errors = state.errors
  return [errors?.listener, errors?.priority_foreground, errors?.priority_timer, errors?.capture]
    .filter(Boolean)
    .join('；')
})
const canSave = computed(() => {
  const shortcut = editing.value
  if (!shortcut || !snapshot.value) return false
  const existing = snapshot.value.shortcut_settings
  return (
    chord.value.trim() !== existing[shortcut.id] ||
    (shortcut.id === 'new_folder' && folderName.value !== existing.new_folder_name)
  )
})

function updateSettings(result: Partial<ShortcutSettings>) {
  if (!result.shortcut_settings || !result.shortcut_listener) throw new Error('未收到快捷键配置。')
  snapshot.value = {
    shortcut_settings: result.shortcut_settings,
    shortcut_listener: result.shortcut_listener,
  }
}

async function load() {
  if (!connected || busy.value) return
  busy.value = true
  try {
    const { code, result } = await requestSettings({ action: 'shortcuts.settings.get' })
    if (code !== 0) throw new Error(result.message || '无法读取快捷键。')
    updateSettings(result)
  } catch (error) {
    toast.error('读取快捷键失败。', {
      description: error instanceof Error ? error.message : undefined,
    })
  } finally {
    busy.value = false
  }
}

function edit(shortcut: Shortcut) {
  if (!snapshot.value || busy.value) return
  chord.value = snapshot.value.shortcut_settings[shortcut.id] ?? ''
  folderName.value = snapshot.value.shortcut_settings.new_folder_name ?? ''
  recording.value = false
  captureHint.value = ''
  editing.value = shortcut
}

function startRecording() {
  recording.value = true
  captureHint.value = ''
}

function captureChord(event: KeyboardEvent) {
  if (!recording.value || event.key === 'Tab' || event.isComposing) return
  if (event.key === 'Escape') {
    event.preventDefault()
    event.stopPropagation()
    recording.value = false
    return
  }
  if (['Control', 'Alt', 'Shift', 'Meta'].includes(event.key)) return

  event.preventDefault()
  event.stopPropagation()
  const key = event.key.toUpperCase()
  if (
    (!event.ctrlKey && !event.altKey && !event.metaKey) ||
    event.location === 3 ||
    !/^(?:[A-Z0-9]|F(?:[1-9]|1[0-9]|2[0-4]))$/.test(key)
  ) {
    captureHint.value = '请按住 Ctrl、Alt 或 Win，再按字母、数字或 F1–F24。'
    return
  }

  chord.value = [
    event.ctrlKey && 'Ctrl',
    event.altKey && 'Alt',
    event.shiftKey && 'Shift',
    event.metaKey && 'Win',
    key,
  ]
    .filter(Boolean)
    .join('+')
  recording.value = false
  captureHint.value = ''
}

async function save() {
  const shortcut = editing.value
  const existing = snapshot.value?.shortcut_settings
  if (!shortcut || !existing || !canSave.value || busy.value) return
  busy.value = true
  recording.value = false
  try {
    const { code, result } = await requestSettings({
      action: 'shortcuts.set',
      shortcut_id: shortcut.id,
      chord: chord.value.trim(),
      expected_chord: existing[shortcut.id] ?? '',
      ...(shortcut.id === 'new_folder'
        ? { name: folderName.value, expected_name: existing.new_folder_name }
        : {}),
    })
    if (result.conflict) {
      toast.warning('快捷键已在其他窗口更新。', { description: '请重新载入后再修改。' })
      return
    }
    if (!result.saved) throw new Error(result.message || '无法保存快捷键。')

    updateSettings(result)
    editing.value = undefined
    if (code !== 0) {
      toast.warning('快捷键已保存，应用状态待确认。', {
        description:
          result.shortcut_registration?.errors?.[shortcut.id] ||
          result.reload_error ||
          result.message,
      })
    } else {
      toast.success('快捷键设置已保存。')
    }
  } catch (error) {
    toast.error('保存快捷键失败。', {
      description: error instanceof Error ? error.message : undefined,
    })
  } finally {
    busy.value = false
  }
}

onMounted(load)
</script>

<template>
  <main :aria-busy="busy">
    <div class="content">
      <header class="page-top">
        <div>
          <h1>快捷键</h1>
          <p>配置资源管理器操作的按键与查看监听状态。</p>
        </div>
        <Button variant="outline" :disabled="!connected || busy" @click="load">
          <template #icon><IconRefresh :size="17" aria-hidden="true" /></template>
          重新载入
        </Button>
      </header>

      <section class="card shortcut-list" aria-label="快捷键配置">
        <div class="toolbar">
          <div class="listener">
            <Tag v-if="listenerIssue" theme="warning" variant="light">监听异常</Tag>
            <Tag v-else-if="listener?.running" theme="success" variant="light">代理运行中</Tag>
            <Tag v-else-if="snapshot" variant="light">代理未运行</Tag>
            <span v-if="listenerIssue" class="detail">{{ listenerIssue }}</span>
            <span v-else-if="listener?.running" class="detail">
              前台监听{{ listener.hook_active ? '已启用' : '未启用' }} · 全局注册{{
                listener.global_available ? '可用' : '不可用'
              }}
            </span>
          </div>
          <Input v-model="search" class="search" placeholder="搜索操作或快捷键" clearable>
            <template #prefix-icon><IconSearch :size="17" aria-hidden="true" /></template>
          </Input>
        </div>
        <Table
          row-key="id"
          :columns="columns"
          :data="snapshot ? rows : []"
          :loading="connected && !snapshot && busy"
          :min-width="620"
        >
          <template #empty>
            <p v-if="!connected">请在司南桌面窗口中管理快捷键。</p>
            <p v-else-if="!snapshot && !busy">读取失败，请重新载入。</p>
            <p v-else-if="snapshot">没有匹配的快捷键。</p>
          </template>
          <template #label="{ row }">
            <strong>{{ row.label }}</strong>
            <span
              v-if="row.id === 'new_folder' && snapshot?.shortcut_settings.new_folder_name"
              class="detail"
            >
              预设名称 {{ snapshot.shortcut_settings.new_folder_name }}
            </span>
          </template>
          <template #binding="{ row }">
            <kbd v-if="row.binding">{{ row.binding }}</kbd>
            <span v-else class="detail">未设置</span>
            <span v-if="row.binding && row.error" class="row-error">{{ row.error }}</span>
          </template>
          <template #edit="{ row }">
            <Button size="small" variant="text" theme="primary" :disabled="busy" @click="edit(row)"
              >编辑</Button
            >
          </template>
        </Table>
      </section>
    </div>

    <Dialog
      :visible="Boolean(editing)"
      :header="editing ? '编辑快捷键 · ' + editing.label : '编辑快捷键'"
      width="min(540px, 92vw)"
      :close-on-overlay-click="false"
      :confirm-on-enter="false"
      :confirm-btn="{ content: '保存快捷键', disabled: !canSave }"
      :confirm-loading="busy"
      cancel-btn="取消"
      @close="!busy && (editing = undefined)"
      @confirm="save"
    >
      <Form v-if="editing" layout="vertical" :disabled="busy">
        <FormItem label="组合键">
          <div class="chord-controls">
            <Input v-model="chord" placeholder="例如 Ctrl+Alt+1" clearable />
            <Button
              variant="outline"
              :disabled="busy"
              :aria-pressed="recording"
              @click="startRecording"
              @keydown="captureChord"
              @blur="recording = false"
            >
              <template #icon><IconKeyboard :size="17" aria-hidden="true" /></template>
              {{ recording ? '请按组合键' : '录制' }}
            </Button>
          </div>
          <template #help>{{ captureHint || '留空可清除绑定。' }}</template>
        </FormItem>
        <FormItem v-if="editing.id === 'new_folder'" label="预设文件夹名称">
          <Input
            v-model="folderName"
            :maxlength="255"
            placeholder="留空时使用资源管理器的默认名称"
          />
        </FormItem>
      </Form>
    </Dialog>
  </main>
</template>

<style scoped>
h1,
p {
  margin: 0;
}

h1 {
  font-size: 32px;
  font-weight: 650;
  letter-spacing: -0.03em;
}

.page-top,
.toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 12px;
}

.page-top {
  margin-bottom: 26px;
}

.page-top p,
.detail {
  color: var(--muted);
  overflow-wrap: anywhere;
}

.page-top p {
  margin-top: 5px;
}

.shortcut-list {
  overflow: hidden;
}

.toolbar {
  padding: 16px 20px;
  border-bottom: 1px solid var(--line);
}

.listener {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px 10px;
  min-width: 0;
}

.detail,
.row-error {
  display: block;
  font-size: 12px;
}

.row-error {
  margin-top: 3px;
  color: var(--warning);
  overflow-wrap: anywhere;
}

.search {
  width: min(100%, 260px);
}

.chord-controls {
  display: flex;
  width: 100%;
  gap: 8px;
}

.chord-controls button {
  flex: none;
}
</style>
