<script setup lang="ts">
import { IconFolderOpen, IconPin, IconPlus, IconRefresh } from '@tabler/icons-vue'
import {
  Button,
  Checkbox,
  Dialog,
  Form,
  FormItem,
  Input,
  Radio,
  RadioGroup,
  Space,
  Table,
  Tag,
  type PrimaryTableCol,
} from 'tdesign-vue-next'
import 'tdesign-vue-next/es/dialog/style/index.css'
import 'tdesign-vue-next/es/form/style/index.css'
import 'tdesign-vue-next/es/radio/style/index.css'
import 'tdesign-vue-next/es/space/style/index.css'
import 'tdesign-vue-next/es/table/style/index.css'
import 'tdesign-vue-next/es/tag/style/index.css'
import { computed, onMounted, ref } from 'vue'
import { toast } from 'vue-sonner'
import {
  hasSettingsHost,
  requestSettings,
  type DirectoryEntry,
  type EntryMutation,
  type EntrySettings,
} from '../settingsHost'

const columns: PrimaryTableCol[] = [
  { colKey: 'name', title: '入口', width: 148 },
  { colKey: 'path', title: '目标目录' },
  { colKey: 'shortcut', title: '快捷键', width: 150 },
  { colKey: 'title', title: '标题显示', width: 124 },
  { colKey: 'enabled', title: '状态', width: 76 },
  { colKey: 'operations', title: '操作', width: 190 },
]
const titleLabels = {
  off: '常规路径',
  alias: '仅别名',
  alias_depth: '别名 + 路径层级',
}

const connected = hasSettingsHost()
const snapshot = ref<EntrySettings>()
const busy = ref(false)
const editor = ref<DirectoryEntry>()
const editedEntry = ref<DirectoryEntry | null>(null)
const deleting = ref<DirectoryEntry>()
const canSave = computed(() => Boolean(editor.value?.name.trim() && editor.value.path.trim()))

function updateSettings(result: Partial<EntrySettings>) {
  if (!result.entries || !result.pinned_position) throw new Error('未收到目录入口配置。')
  snapshot.value = { entries: result.entries, pinned_position: result.pinned_position }
}

async function load() {
  if (!connected || busy.value) return
  busy.value = true
  try {
    const { code, result } = await requestSettings({ action: 'entries.settings.get' })
    if (code !== 0) throw new Error(result.message || '无法读取目录入口。')
    updateSettings(result)
  } catch (error) {
    toast.error('读取目录入口失败。', {
      description: error instanceof Error ? error.message : undefined,
    })
  } finally {
    busy.value = false
  }
}

async function mutate(request: EntryMutation, success: string) {
  if (busy.value) return false
  busy.value = true
  try {
    const { code, result } = await requestSettings(request)
    if (result.conflict) {
      toast.warning('目录设置已在其他窗口更新。', { description: '请重新载入后再操作。' })
      return false
    }
    if (!result.saved) throw new Error(result.message || '无法保存目录设置。')
    updateSettings(result)
    if (code !== 0) {
      const errors = Object.values(result.shortcut_registration?.errors ?? {})
      toast.warning('设置已保存，应用状态待确认。', {
        description: errors.join('；') || result.reload_error || result.message,
      })
    } else {
      toast.success(success)
    }
    return true
  } catch (error) {
    toast.error('操作失败。', { description: error instanceof Error ? error.message : undefined })
    return false
  } finally {
    busy.value = false
  }
}

function edit(entry?: DirectoryEntry) {
  if (!snapshot.value || busy.value) return
  editedEntry.value = entry ?? null
  editor.value = entry
    ? { ...entry, scope: entry.scope ?? 'global' }
    : {
        id: crypto.randomUUID(),
        name: '',
        path: '',
        shortcut: '',
        enabled: true,
        descendants: false,
        title: 'alias_depth',
        scope: 'global',
      }
}

async function saveEntry() {
  if (!editor.value || !canSave.value) return
  const entry = {
    ...editor.value,
    name: editor.value.name.trim(),
    path: editor.value.path.trim(),
    shortcut: editor.value.shortcut.trim(),
  }
  if (
    await mutate(
      {
        action: 'entries.put',
        entry_id: entry.id,
        expected_entry: editedEntry.value,
        entry,
      },
      '目录入口已保存。',
    )
  )
    editor.value = undefined
}

function toggle(entry: DirectoryEntry) {
  return mutate(
    {
      action: 'entries.put',
      entry_id: entry.id,
      expected_entry: entry,
      entry: { ...entry, enabled: !entry.enabled, scope: entry.scope ?? 'global' },
    },
    entry.enabled ? '入口已停用。' : '入口已启用。',
  )
}

async function deleteEntry() {
  const entry = deleting.value
  if (!entry) return
  if (
    await mutate(
      {
        action: 'entries.remove',
        entry_id: entry.id,
        expected_entry: entry,
      },
      '目录入口已删除。',
    )
  )
    deleting.value = undefined
}

function changePinnedPosition(position: unknown) {
  const previous = snapshot.value?.pinned_position
  if (
    !previous ||
    previous === position ||
    (position !== 'keep' && position !== 'left' && position !== 'right')
  )
    return
  return mutate(
    {
      action: 'pinned.position',
      position,
      expected_position: previous,
    },
    '固定标签位置已保存。',
  )
}

async function openEntry(entry: DirectoryEntry) {
  if (busy.value) return
  busy.value = true
  try {
    const { code, result } = await requestSettings({
      action: 'directories.open',
      entry_id: entry.id,
    })
    if (result.confirmation === 'launch_requested') {
      toast.info('已请求资源管理器打开目录。')
    } else if (code === 0 && result.selection_confirmed && result.foreground_activated !== false) {
      toast.success('已定位目录标签。')
    } else if (code === 0 || result.selection_confirmed) {
      toast.warning('目录打开状态待确认。', { description: result.message })
    } else {
      toast.error('打开目录失败。', {
        description: result.message || '请在「常规」页检查代理状态。',
      })
    }
  } catch (error) {
    toast.error('打开目录失败。', {
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
          <h1>目录入口</h1>
          <p>为常用目录设置别名和快捷键，并从列表直接打开。</p>
        </div>
        <Space>
          <Button variant="outline" :disabled="!connected || busy" @click="load">
            <template #icon><IconRefresh :size="17" aria-hidden="true" /></template>
            重新载入
          </Button>
          <Button theme="primary" :disabled="!snapshot || busy" @click="edit()">
            <template #icon><IconPlus :size="17" aria-hidden="true" /></template>
            新增入口
          </Button>
        </Space>
      </header>

      <section class="card entries-table" aria-label="目录入口列表">
        <Table
          row-key="id"
          :columns="columns"
          :data="snapshot?.entries ?? []"
          :loading="connected && !snapshot && busy"
          :min-width="760"
        >
          <template #empty>
            <p v-if="!connected">请在司南桌面窗口中管理目录入口。</p>
            <p v-else-if="!snapshot && !busy">读取失败，请重新载入。</p>
            <p v-else>暂无目录入口。</p>
          </template>
          <template #name="{ row }"
            ><strong class="cell-text">{{ row.name }}</strong></template
          >
          <template #path="{ row }"
            ><code class="cell-text">{{ row.path }}</code></template
          >
          <template #shortcut="{ row }">
            <template v-if="row.shortcut">
              <kbd>{{ row.shortcut }}</kbd>
              <span class="cell-detail">{{
                row.scope === 'explorer' ? '资源管理器前台' : '全局'
              }}</span>
            </template>
            <span v-else class="cell-detail">未设置</span>
          </template>
          <template #title="{ row }">
            {{ titleLabels[row.title as DirectoryEntry['title']] }}
            <span v-if="row.descendants" class="cell-detail">含子目录</span>
          </template>
          <template #enabled="{ row }">
            <Tag :theme="row.enabled ? 'success' : 'default'" variant="light" size="small">
              {{ row.enabled ? '已启用' : '已停用' }}
            </Tag>
          </template>
          <template #operations="{ row }">
            <Space :size="2" align="center">
              <Button
                size="small"
                variant="text"
                theme="primary"
                :disabled="busy || !row.enabled"
                @click="openEntry(row)"
              >
                <template #icon><IconFolderOpen :size="16" aria-hidden="true" /></template>
                打开
              </Button>
              <Button size="small" variant="text" :disabled="busy" @click="edit(row)">编辑</Button>
              <Button size="small" variant="text" :disabled="busy" @click="toggle(row)">{{
                row.enabled ? '停用' : '启用'
              }}</Button>
              <Button
                size="small"
                variant="text"
                theme="danger"
                :disabled="busy"
                @click="deleting = row"
                >删除</Button
              >
            </Space>
          </template>
        </Table>
      </section>

      <section class="card pinned" aria-labelledby="pinned-heading">
        <div class="pinned-heading">
          <IconPin :size="22" aria-hidden="true" />
          <div>
            <h2 id="pinned-heading">固定标签位置</h2>
            <p>在每个资源管理器窗口内排列固定标签。</p>
          </div>
        </div>
        <RadioGroup
          :value="snapshot?.pinned_position"
          :disabled="!snapshot || busy"
          aria-label="固定标签位置"
          @change="changePinnedPosition"
        >
          <Radio value="keep">保持原位</Radio>
          <Radio value="left">靠左</Radio>
          <Radio value="right">靠右</Radio>
        </RadioGroup>
      </section>
    </div>

    <Dialog
      :visible="Boolean(editor)"
      :header="editedEntry ? '编辑入口' : '新增入口'"
      width="min(640px, 92vw)"
      :close-on-overlay-click="false"
      :confirm-on-enter="false"
      :confirm-btn="{ content: '保存入口', disabled: !canSave }"
      :confirm-loading="busy"
      cancel-btn="取消"
      @close="!busy && (editor = undefined)"
      @confirm="saveEntry"
    >
      <Form v-if="editor" layout="vertical" :disabled="busy">
        <FormItem label="入口名称">
          <Input v-model="editor.name" :maxlength="128" placeholder="例如 司南项目" />
        </FormItem>
        <FormItem label="目标目录">
          <Input v-model="editor.path" placeholder="输入绝对路径，例如 D:\项目\司南" />
        </FormItem>
        <FormItem label="快捷键（可选）">
          <Input v-model="editor.shortcut" placeholder="例如 Ctrl+Alt+1" />
        </FormItem>
        <FormItem v-if="editor.shortcut.trim()" label="快捷键作用范围">
          <RadioGroup v-model="editor.scope">
            <Radio value="global">全局</Radio>
            <Radio value="explorer">资源管理器前台</Radio>
          </RadioGroup>
        </FormItem>
        <FormItem label="标题显示">
          <RadioGroup v-model="editor.title">
            <Radio value="off">{{ titleLabels.off }}</Radio>
            <Radio value="alias">{{ titleLabels.alias }}</Radio>
            <Radio value="alias_depth">{{ titleLabels.alias_depth }}</Radio>
          </RadioGroup>
        </FormItem>
        <FormItem label="选项">
          <Space wrap>
            <Checkbox v-model="editor.enabled">启用入口</Checkbox>
            <Checkbox v-model="editor.descendants">标题设置应用到子目录</Checkbox>
          </Space>
        </FormItem>
      </Form>
    </Dialog>

    <Dialog
      :visible="Boolean(deleting)"
      header="删除目录入口"
      theme="danger"
      confirm-btn="删除入口"
      cancel-btn="取消"
      :confirm-loading="busy"
      @close="!busy && (deleting = undefined)"
      @confirm="deleteEntry"
    >
      删除「{{ deleting?.name }}」？
    </Dialog>
  </main>
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
.pinned {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 12px 20px;
}

.page-top {
  margin-bottom: 26px;
}

.page-top p,
.pinned p {
  margin-top: 5px;
  color: var(--muted);
}

.entries-table {
  overflow: hidden;
}

.cell-text,
kbd {
  overflow-wrap: anywhere;
}

.cell-detail {
  display: block;
  margin-top: 3px;
  color: var(--muted);
  font-size: 12px;
}

.pinned {
  margin-top: 16px;
  padding: 22px 24px;
}

.pinned-heading {
  display: flex;
  align-items: center;
  gap: 12px;
}

.pinned-heading svg {
  flex: none;
  color: var(--accent);
}
</style>
