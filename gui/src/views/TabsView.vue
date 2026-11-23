<script setup lang="ts">
import { IconRefresh, IconSearch } from '@tabler/icons-vue'
import {
  Alert,
  Button,
  Dialog,
  Form,
  FormItem,
  Input,
  InputNumber,
  RadioGroup,
  Space,
  Table,
  Tag,
  type PrimaryTableCol,
} from 'tdesign-vue-next'
import 'tdesign-vue-next/es/alert/style/index.css'
import 'tdesign-vue-next/es/dialog/style/index.css'
import 'tdesign-vue-next/es/form/style/index.css'
import 'tdesign-vue-next/es/input-number/style/index.css'
import 'tdesign-vue-next/es/radio/style/index.css'
import 'tdesign-vue-next/es/space/style/index.css'
import 'tdesign-vue-next/es/table/style/index.css'
import 'tdesign-vue-next/es/tag/style/index.css'
import { computed, onMounted, ref } from 'vue'
import { toast } from 'vue-sonner'
import SettingSwitch from '../components/SettingSwitch.vue'
import {
  hasSettingsHost,
  requestSettings,
  type ExplorerTab,
  type TabList,
  type TabMutation,
} from '../settingsHost'

const columns: PrimaryTableCol[] = [
  { colKey: 'title', title: '标签与目录' },
  { colKey: 'window_label', title: '窗口', width: 90 },
  { colKey: 'policy', title: '标题策略', width: 150 },
  { colKey: 'behavior', title: '标签状态', width: 154 },
  { colKey: 'operations', title: '操作', width: 140 },
]
const titleModes = [
  { label: '自动匹配规则', value: 'inherit' },
  { label: '指定层级', value: 'depth' },
  { label: '完整路径', value: 'all' },
]
const behaviors = [
  {
    field: 'pinned',
    action: 'tabs.pin',
    label: '固定标签',
    description: '按「目录入口」设置的固定位置排列。',
  },
  {
    field: 'protected',
    action: 'tabs.protect',
    label: '独立关闭保护',
    description: '为此标签启用关闭保护。',
  },
  {
    field: 'locked',
    action: 'tabs.lock',
    label: '锁定目录',
    description: '保护当前目录，避免导航替换此标签。',
  },
] as const

const connected = hasSettingsHost()
const snapshot = ref<TabList>()
const loadError = ref('')
const busy = ref(false)
const search = ref('')
const editingId = ref('')
const mode = ref('inherit')
const depth = ref<number | string>(2)

const rows = computed(() => {
  const query = search.value.trim().toLowerCase()
  const windows = new Map<number, number>()
  return (snapshot.value?.tabs ?? [])
    .map((tab) => {
      if (!windows.has(tab.window)) windows.set(tab.window, windows.size + 1)
      return { ...tab, window_label: `窗口 ${windows.get(tab.window)}` }
    })
    .filter((tab) =>
      [tab.path, tab.original_title, tab.preview_title, tab.applied_title].some((value) =>
        value?.toLowerCase().includes(query),
      ),
    )
})
const edited = computed(() => snapshot.value?.tabs.find((tab) => tab.tab_id === editingId.value))
const canEdit = computed(
  () => !!snapshot.value?.scan_complete && !snapshot.value.config_error && !loadError.value,
)
const override = computed(() =>
  mode.value === 'inherit' ? null : mode.value === 'all' ? 'all' : depth.value,
)
const canSave = computed(() => {
  if (
    !edited.value?.path ||
    !edited.value.observed ||
    !canEdit.value ||
    override.value === edited.value.override
  )
    return false
  return (
    mode.value !== 'depth' ||
    (typeof depth.value === 'number' &&
      Number.isInteger(depth.value) &&
      depth.value >= 1 &&
      depth.value <= 32)
  )
})

function tabTitle(tab: ExplorerTab) {
  return (
    (tab.title_applied ? tab.applied_title : tab.original_title) ||
    tab.preview_title ||
    tab.path ||
    '未命名标签'
  )
}

async function readTabs() {
  try {
    const { code, result } = await requestSettings({ action: 'tabs.settings.get' })
    if (code !== 0 || !result.tab_list) throw new Error(result.message || '无法读取标签列表。')
    snapshot.value = result.tab_list
    loadError.value = ''
    if (editingId.value && !result.tab_list.tabs.some((tab) => tab.tab_id === editingId.value))
      editingId.value = ''
  } catch (error) {
    loadError.value = error instanceof Error ? error.message : '无法读取标签列表。'
  }
}

async function load(startAgent = false) {
  if (!connected || busy.value) return
  busy.value = true
  try {
    if (startAgent) {
      const { code, result } = await requestSettings({ action: 'agent.start' })
      if (code !== 0) throw new Error(result.message || '无法启动代理。')
    }
    await readTabs()
  } catch (error) {
    toast.error('启动代理失败。', {
      description: error instanceof Error ? error.message : undefined,
    })
  } finally {
    busy.value = false
  }
}

function edit(tab: ExplorerTab) {
  mode.value = tab.override === null ? 'inherit' : tab.override === 'all' ? 'all' : 'depth'
  depth.value =
    typeof tab.override === 'number' ? tab.override : typeof tab.depth === 'number' ? tab.depth : 2
  editingId.value = tab.tab_id
}

async function apply(request: TabMutation) {
  if (busy.value || loadError.value) return
  busy.value = true
  try {
    const { code, result } = await requestSettings(request)
    if (
      request.action === 'tabs.locate' &&
      result.selection_confirmed &&
      result.foreground_activated
    ) {
      toast.success('已定位标签。')
    } else if (result.policy_saved) {
      if (result.title_applied) toast.success('标题策略已应用。')
      else toast.info('标题策略已更新，实际标题待确认。', { description: result.apply_error })
    } else if (result.state_confirmed && code === 0) {
      toast.success('标签状态已更新。')
    } else if (result.state_confirmed || result.selection_confirmed) {
      toast.warning('操作已执行，部分状态待确认。', { description: result.message })
    } else {
      toast.error('标签操作失败。', { description: result.message })
    }
  } catch (error) {
    toast.error('标签操作失败。', {
      description: error instanceof Error ? error.message : undefined,
    })
  } finally {
    if (request.action !== 'tabs.locate') await readTabs()
    busy.value = false
  }
}

function saveTitle() {
  const tab = edited.value
  if (!tab || !canSave.value) return
  const tab_id = tab.tab_id
  return apply(
    override.value === null
      ? { action: 'tabs.reset', tab_id }
      : { action: 'tabs.set', tab_id, depth: String(override.value) },
  )
}

onMounted(() => load())
</script>

<template>
  <main :aria-busy="busy">
    <div class="content">
      <header class="page-top">
        <div>
          <h1>标签管理</h1>
          <p>查看和管理当前打开的资源管理器标签。</p>
        </div>
        <Space>
          <Button variant="outline" :disabled="!connected || busy" @click="load()">
            <template #icon><IconRefresh :size="17" aria-hidden="true" /></template>
            刷新标签
          </Button>
          <Button
            v-if="snapshot && !snapshot.running"
            theme="primary"
            :disabled="busy"
            @click="load(true)"
          >
            启动代理
          </Button>
        </Space>
      </header>

      <Alert v-if="loadError" theme="error" class="notice" :message="loadError" />
      <Alert
        v-else-if="snapshot?.config_error"
        theme="error"
        class="notice"
        :message="snapshot.config_error"
      />
      <Alert
        v-else-if="snapshot?.running && !snapshot.scan_complete"
        theme="warning"
        class="notice"
        message="标签扫描尚未完成，请刷新后再修改标签。"
      />

      <section class="card tab-list" aria-label="资源管理器标签">
        <div class="toolbar">
          <span v-if="snapshot?.running" class="muted">
            {{ snapshot.tabs.length }} 个标签 ·
            {{ snapshot.mode === 'customize' ? '定制模式' : '观察模式' }}
          </span>
          <span v-else-if="snapshot" class="muted">代理未运行</span>
          <Input v-model="search" class="search" placeholder="搜索标签标题或路径" clearable>
            <template #prefix-icon><IconSearch :size="17" aria-hidden="true" /></template>
          </Input>
        </div>
        <Table
          row-key="tab_id"
          :columns="columns"
          :data="rows"
          :min-width="680"
          :loading="connected && !snapshot && busy"
        >
          <template #empty>
            <span v-if="!connected">请在司南桌面窗口查看标签。</span>
            <span v-else-if="snapshot?.running">{{
              search.trim() ? '没有匹配的标签。' : '暂无打开的资源管理器标签。'
            }}</span>
            <span v-else-if="snapshot">启动代理后即可读取标签。</span>
            <span v-else-if="loadError">请刷新标签重试。</span>
          </template>
          <template #title="{ row }">
            <strong class="path">{{ tabTitle(row) }}</strong>
            <span v-if="row.path" class="path muted">{{ row.path }}</span>
            <span
              v-if="
                row.title_applied === false &&
                row.preview_title &&
                row.preview_title !== tabTitle(row)
              "
              class="path muted"
            >
              预览 · {{ row.preview_title }}
            </span>
          </template>
          <template #policy="{ row }">
            {{
              row.action === 'native'
                ? '系统名称'
                : row.depth === 'all'
                  ? '完整路径'
                  : row.depth + ' 级'
            }}
            <span class="muted">{{
              row.source === 'tab' ? '独立设置' : row.source === 'rule' ? '目录规则' : '全局默认'
            }}</span>
          </template>
          <template #behavior="{ row }">
            <div class="tags">
              <Tag v-if="row.behavior.pinned" theme="primary" variant="light" size="small"
                >固定</Tag
              >
              <Tag v-if="row.behavior.protected" theme="warning" variant="light" size="small"
                >保护</Tag
              >
              <Tag v-if="row.behavior.locked" theme="primary" variant="light" size="small"
                >锁定</Tag
              >
              <span
                v-if="!row.behavior.pinned && !row.behavior.protected && !row.behavior.locked"
                class="muted"
                >普通</span
              >
            </div>
          </template>
          <template #operations="{ row }">
            <Space :size="2">
              <Button
                size="small"
                variant="text"
                theme="primary"
                :disabled="busy || !!loadError || !row.observed || !row.native_guid"
                @click="apply({ action: 'tabs.locate', tab_id: row.tab_id })"
                >定位</Button
              >
              <Button
                size="small"
                variant="text"
                :disabled="busy || !row.observed"
                @click="edit(row)"
                >管理</Button
              >
            </Space>
          </template>
        </Table>
      </section>
    </div>

    <Dialog
      :visible="Boolean(edited)"
      header="管理标签"
      width="min(600px, 92vw)"
      :footer="false"
      :close-on-overlay-click="false"
      @close="!busy && (editingId = '')"
    >
      <div v-if="edited">
        <p class="path">
          <strong>{{ tabTitle(edited) }}</strong>
        </p>
        <p v-if="edited.path" class="path muted">{{ edited.path }}</p>
        <Alert
          v-if="edited.apply_error || edited.behavior.error"
          theme="warning"
          class="notice dialog-notice"
          :message="edited.apply_error || edited.behavior.error"
        />

        <h2>标题策略</h2>
        <Form layout="vertical">
          <FormItem>
            <RadioGroup
              v-model="mode"
              :options="titleModes"
              :disabled="busy || !canEdit || !edited.path"
              aria-label="标题策略"
            />
          </FormItem>
          <FormItem v-if="mode === 'depth'" label="路径层级">
            <InputNumber
              v-model="depth"
              :min="1"
              :max="32"
              :disabled="busy || !canEdit || !edited.path"
            />
          </FormItem>
        </Form>
        <p v-if="!edited.path" class="muted">此标签使用资源管理器的系统名称。</p>
        <Button v-else theme="primary" :disabled="!canSave || busy" @click="saveTitle"
          >应用标题策略</Button
        >

        <h2>标签保护</h2>
        <p v-if="snapshot?.mode !== 'customize'" class="muted">
          在「常规」页启用定制后可修改保护状态。
        </p>
        <p v-else-if="!edited.native_guid" class="muted">当前标签缺少可验证的原生身份。</p>
        <SettingSwitch
          v-for="behavior in behaviors"
          :key="behavior.field"
          :label="behavior.label"
          :description="behavior.description"
          :checked="edited.behavior[behavior.field]"
          :disabled="
            !canEdit || !edited.observed || !edited.native_guid || snapshot?.mode !== 'customize'
          "
          :busy="busy"
          @change="
            (enabled) =>
              edited && apply({ action: behavior.action, tab_id: edited.tab_id, enabled })
          "
        />
      </div>
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
h2 {
  margin: 20px 0 10px;
  font-size: 16px;
  font-weight: 650;
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
.page-top p {
  margin-top: 5px;
  color: var(--muted);
}
.notice {
  margin-bottom: 16px;
}
.dialog-notice {
  margin-top: 12px;
}
.tab-list {
  overflow: hidden;
}
.toolbar {
  padding: 16px 20px;
  border-bottom: 1px solid var(--line);
}
.search {
  width: min(100%, 280px);
}
.path {
  display: block;
  overflow-wrap: anywhere;
}
.muted {
  display: block;
  color: var(--muted);
  font-size: 12px;
}
.tags {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
}
</style>
