<script setup lang="ts">
import { Button, Dialog, Input, InputNumber, Select, Switch } from 'tdesign-vue-next'
import { IconArrowDown, IconArrowUp, IconDownload, IconUpload } from '@tabler/icons-vue'
import 'tdesign-vue-next/es/switch/style/index.css'
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { onBeforeRouteLeave, useRoute } from 'vue-router'
import { toast } from 'vue-sonner'
import { closeSettings, hasSettingsHost, onSettingsClose, requestSettings, setCommandBarDraft, type CommandBarSettings, type CommandBarEntry, type CommandApplication, type CommandPosition } from '../settingsHost'

const defaults = (): CommandBarSettings => ({
  enabled: false, terminal: true, hidden: [], icon_only: [], hidden_separators: [], templates: {},
})
const baseline = ref(defaults())
const draft = ref(defaults())
const loaded = ref(false)
const busy = ref(false)
const status = ref('')
const errors = ref<string[]>([])
const catalog = ref<{ id: string; label: string }[]>([])
const selected = ref('')
const addAction = ref('')
const dragging = ref('')
const templates = ref<{label: string; filename: string}[]>([])
const argumentsPreview = ref<string[]>([])
const route = useRoute()
const leaveVisible = ref(false)
let leaveResolve: ((leave: boolean) => void) | undefined
let closing = false
const connected = hasSettingsHost()
const changed = computed(() => JSON.stringify(draft.value) !== JSON.stringify(baseline.value))
watch(changed, setCommandBarDraft, { flush: 'sync' })
const unregisterClose = onSettingsClose(() => {
  closing = true
  leaveVisible.value = true
})
onUnmounted(() => {
  unregisterClose()
  setCommandBarDraft(false)
})
const templateOptions = computed(() => draft.value.templates ?? (draft.value.templates = {}))
const entries = computed(() => draft.value.actions ?? [])
const chosen = computed(() => entries.value.find(item => item.id === selected.value))
const configuredApps = computed(() => draft.value.applications ?? {})
const choices = computed(() => [
  ...catalog.value.map(item => ({ value: item.id, label: item.label })),
  ...Object.entries(configuredApps.value).map(([id, item]) => ({ value: 'application.' + id, label: item.name })),
])
const chosenRule = computed(() => {
  if (!chosen.value) return undefined
  return draft.value.conditions?.[chosen.value.action]
})
const chosenApp = computed(() => chosen.value?.action.startsWith('application.')
  ? configuredApps.value[chosen.value.action.slice(12)] : undefined)
const placements = [{ value: 'menu', label: '菜单' }, { value: 'fixed', label: '固定按钮' }]
const positions = [
  { value: 'menu', label: '司南菜单' }, { value: 'beside', label: '与系统「新建」并列' },
  { value: 'replace', label: '替换系统「新建」' }, { value: 'off', label: '关闭模板入口' },
]
const argumentSources = [
  { value: 'directory', label: '当前目录' }, { value: 'smart_folder', label: '选中的文件夹优先' },
  { value: 'first', label: '首个选中项' }, { value: 'all', label: '全部选中项' },
  { value: 'files', label: '仅选中文件' }, { value: 'folders', label: '仅选中文件夹' },
]
const selectionCounts = [
  { value: 'any', label: '任意数量' }, { value: 'none', label: '空选区' },
  { value: 'one', label: '单选' }, { value: 'multiple', label: '多选' },
]
const selectionKinds = [
  { value: 'any', label: '任意类型' }, { value: 'files', label: '仅文件' }, { value: 'folders', label: '仅文件夹' },
]
const referenceLabel = (entry: CommandBarEntry) => choices.value.find(item => item.value === entry.action)?.label
const entryLabel = (entry: CommandBarEntry) => entry.label || referenceLabel(entry) || entry.action
const sides = [{ value: 'left', label: '左栏' }, { value: 'right', label: '右栏' }]
const standardOrder = ['new', 'cut', 'copy', 'paste', 'rename', 'share', 'delete', 'sort', 'view'] as const
const separatorIds = ['after_new', 'after_delete', 'before_more'] as const
const defaultCommandPositions = [
  { id: 'terminal', label: '在此打开终端', order: 0 },
  { id: 'menu', label: '司南菜单', order: 1 },
  { id: 'context', label: 'Shell 上下文菜单', order: 2 },
]
const systemOrder = computed(() => [
  ...(draft.value.system_order ?? []),
  ...standardOrder.filter(item => !draft.value.system_order?.includes(item)),
])
function moveSystem(index: number, step: number) {
  const order = [...systemOrder.value]
  const item = order[index]
  if (!item || !order[index + step]) return
  order.splice(index, 1)
  order.splice(index + step, 0, item)
  draft.value.system_order = order
}
function moveSeparator(id: string, destination: unknown) {
  const positions = { ...draft.value.separators }
  if (typeof destination === 'string' && destination) positions[id] = destination
  else delete positions[id]
  draft.value.separators = positions
}
type LayoutEntry = { id: string; label: string; side: 'left' | 'right'; order: number; entry?: CommandBarEntry }
const fixedLayout = computed<LayoutEntry[]>(() => [
  ...defaultCommandPositions.map(item => {
    const position = draft.value.positions?.[item.id]
    return { ...item, side: position?.side ?? 'right', order: position?.order ?? item.order }
  }),
  ...entries.value.filter(entry => entry.placement === 'fixed').map((entry, index) => ({
    id: entry.id, label: entryLabel(entry), side: entry.side ?? 'right', order: entry.order ?? 2 + index, entry,
  } as LayoutEntry)),
].sort((left, right) => left.order - right.order))
function setPosition(item: LayoutEntry, position: CommandPosition) {
  if (item.entry) Object.assign(item.entry, position)
  else (draft.value.positions ??= {})[item.id] = position
}
function moveFixed(item: LayoutEntry, step: number) {
  const items = fixedLayout.value.filter(entry => entry.side === item.side)
  const index = items.indexOf(item)
  if (index < 0 || !items[index + step]) return
  items.splice(index, 1)
  items.splice(index + step, 0, item)
  items.forEach((entry, order) => setPosition(entry, { side: entry.side, order }))
}
function changeSide(item: LayoutEntry, side: unknown) {
  if (side !== 'left' && side !== 'right') return
  const destination = fixedLayout.value.filter(entry => entry.side === side && entry !== item)
  destination.push(item)
  destination.forEach((entry, order) => setPosition(entry, { side, order }))
}
async function transferConfiguration(importing: boolean) {
  busy.value = true
  try {
    const { code, result } = await requestSettings(importing ? { action: 'command-bar.import', local: draft.value }
      : { action: 'command-bar.export', settings: draft.value })
    if (code) throw new Error(result.message)
    if (importing && result.command_bar) {
      draft.value = { ...result.command_bar, templates: result.command_bar.templates || {} }
      selected.value = ''
      toast.success('布局已导入草稿')
    } else if (result.document) toast.success('命令栏设置已导出')
  } catch (error) {
    toast.error(importing ? '导入失败' : '导出失败', { description: error instanceof Error ? error.message : undefined })
  } finally {
    busy.value = false
  }
}
const missingEntries = computed(() => entries.value.filter(entry => {
  if (entry.action.startsWith('application.'))
    return !configuredApps.value[entry.action.slice(12)]?.target
  return !referenceLabel(entry)
}))
function reassignAction(entry: CommandBarEntry, value: unknown) {
  if (typeof value === 'string' && choices.value.some(item => item.value === value)) entry.action = value
}
function insertEntry(action: string) {
  if (!action) return
  const id = crypto.randomUUID()
  ;(draft.value.actions ??= []).push({ id, action, placement: 'menu' })
  selected.value = id
}
function addApplication() {
  const id = crypto.randomUUID()
  const app: CommandApplication = { name: '应用程序', target: '', source: 'directory', pass_arguments: false }
  ;(draft.value.applications ??= {})[id] = app
  insertEntry('application.' + id)
}
function duplicate(entry: CommandBarEntry) {
  const copy = { ...entry, id: crypto.randomUUID() }
  const index = entries.value.findIndex(item => item.id === entry.id)
  entries.value.splice(index + 1, 0, copy)
  selected.value = copy.id
}
function moveEntry(id: string) {
  const from = entries.value.findIndex(item => item.id === dragging.value)
  const to = entries.value.findIndex(item => item.id === id)
  if (from < 0 || to < 0 || from === to) return
  const entry = entries.value.splice(from, 1)[0]
  if (entry) entries.value.splice(to, 0, entry)
  dragging.value = ''
}
async function previewTemplates() {
  const { result, code } = await requestSettings({ action: 'command-bar.templates.preview', templates: templateOptions.value })
  if (code) toast.error(result.message || '无法读取模板')
  else templates.value = result.templates || []
}
async function previewArguments() {
  if (!chosenApp.value) return
  const directory = 'D:\\项目资料\\当前 文件夹'
  const { result, code } = await requestSettings({
    action: 'command-bar.arguments.preview', source: chosenApp.value.source, directory,
    selection: [{ path: directory + '\\文件 A.txt', folder: false }, { path: directory + '\\子 文件夹', folder: true }],
  })
  if (code) toast.error(result.message || '无法预览参数')
  else argumentsPreview.value = result.arguments || []
}
async function pickPath(kind: 'directory' | 'application' | 'icon', set: (path: string) => void) {
  try {
    const { result, code } = await requestSettings({ action: 'command-bar.pick-path', kind })
    if (code) throw new Error(result.message)
    if (typeof result.path === 'string') set(result.path)
  } catch (error) {
    toast.error(error instanceof Error ? error.message : '无法选择路径')
  }
}
async function finishLeave(choice: 'save' | 'discard' | 'cancel') {
  if (choice === 'save') {
    await update(true)
    if (changed.value) return
  }
  leaveVisible.value = false
  if (closing && choice !== 'cancel') closeSettings()
  closing = false
  leaveResolve?.(choice !== 'cancel')
  leaveResolve = undefined
}
onBeforeRouteLeave(() => {
  if (!changed.value) return true
  leaveVisible.value = true
  return new Promise<boolean>(resolve => { leaveResolve = resolve })
})
const names = {
  new: '新建', cut: '剪切', copy: '复制', paste: '粘贴', rename: '重命名', share: '共享',
  delete: '删除', sort: '排序', view: '查看', more: '更多选项', details: '详细信息',
  extract_all: '解压全部', set_background: '设为桌面背景', rotate_left: '向左旋转', rotate_right: '向右旋转',
  after_new: '新建后', after_delete: '删除后', before_more: '更多选项前',
}
type Group = 'hidden' | 'icon_only' | 'hidden_separators'
const groups: { key: Group; label: string; items: (keyof typeof names)[] }[] = [
  { key: 'hidden', label: '隐藏系统按钮', items: ['new', 'cut', 'copy', 'paste', 'rename', 'share', 'delete', 'sort', 'view', 'more', 'details'] },
  { key: 'hidden', label: '隐藏上下文按钮', items: ['extract_all', 'set_background', 'rotate_left', 'rotate_right'] },
  { key: 'icon_only', label: '仅显示图标', items: ['new', 'sort', 'view', 'details'] },
  { key: 'hidden_separators', label: '隐藏分隔线', items: ['after_new', 'after_delete', 'before_more'] },
]
const set = (key: Group, id: string, checked: boolean) => {
  const items = draft.value[key].filter((item) => item !== id)
  if (checked) items.push(id)
  draft.value[key] = items
}
const descriptions: Record<string, string> = {
  applied: '已应用到资源管理器', disabled: '定制已关闭', waiting: '等待打开资源管理器窗口',
  pending: '正在等待资源管理器应用', unavailable: '当前资源管理器无法应用',
}

async function update(save = false) {
  if (!connected || busy.value) return
  busy.value = true
  try {
    const { code, result } = await requestSettings(save
      ? { action: 'command-bar.save', settings: draft.value, expected: baseline.value }
      : { action: 'command-bar.get' })
    if (!result.command_bar || (code && !result.saved)) throw new Error(result.message || result.error)
    const settings = { ...result.command_bar, templates: result.command_bar.templates || {} }
    baseline.value = structuredClone(settings)
    draft.value = structuredClone(settings)
    if (result.command_bar_catalog) catalog.value = result.command_bar_catalog
    loaded.value = true
    const application = result.command_bar_application
    status.value = application ? descriptions[application.state] || '' : ''
    errors.value = application?.errors || []
    if (save) {
      if (code) toast.warning('设置已保存', { description: status.value })
      else toast.success('命令栏设置已保存', { description: status.value })
    }
  } catch (error) {
    toast.error(save ? '保存命令栏设置失败' : '读取命令栏设置失败', {
      description: error instanceof Error ? error.message : undefined,
    })
  } finally {
    busy.value = false
  }
}
onMounted(async () => {
  await update()
  if (route.query['command-bar']) {
    await nextTick()
    document.getElementById('command-bar-settings')?.scrollIntoView({ block: 'start' })
  }
})
</script>

<template>
  <section id="command-bar-settings" class="command-settings">
    <header class="command-header">
      <div>
        <h2>资源管理器命令栏</h2>
        <p v-if="status" role="status">{{ status }}</p>
      </div>
      <div class="command-actions">
        <Button variant="outline" :disabled="!connected || busy || changed" @click="update()">刷新</Button>
        <Button variant="outline" :disabled="!loaded || busy" @click="transferConfiguration(true)">
          <template #icon><IconUpload :size="16" /></template>导入
        </Button>
        <Button variant="outline" :disabled="!loaded || busy" @click="transferConfiguration(false)">
          <template #icon><IconDownload :size="16" /></template>导出
        </Button>
        <Button variant="outline" :disabled="!loaded || busy" @click="draft = defaults()">恢复默认</Button>
        <Button theme="primary" :disabled="!loaded || !changed || busy" :loading="busy" @click="update(true)">保存</Button>
      </div>
    </header>
    <p v-for="error in errors" :key="error" class="command-error">{{ error }}</p>
    <div class="command-options">
      <label>启用命令栏定制 <Switch v-model="draft.enabled" aria-label="启用命令栏定制" :disabled="!loaded || busy" /></label>
      <label>在此打开终端 <Switch v-model="draft.terminal" aria-label="在此打开终端" :disabled="!loaded || busy" /></label>
      <label>新建文件快捷按钮 <Switch v-model="draft.new_file_button" :disabled="!loaded || busy" /></label>
      <label>最近关闭 <Switch v-model="draft.recently_closed" :disabled="!loaded || busy" /></label>
      <label>Shell 上下文菜单 <Switch v-model="draft.context_menu" :disabled="!loaded || busy" /></label>
      <label>悬停打开菜单 <Switch :value="Boolean(draft.hover_delay)" :disabled="!loaded || busy"
        @change="value => draft.hover_delay = value === true ? 400 : 0" /></label>
    </div>
    <p v-if="draft.context_menu" role="status">使用资源管理器原生菜单，支持 Shift 扩展命令。第三方扩展由 Windows 加载。</p>
    <div v-if="draft.hover_delay" class="command-fields">
      <label>悬停延时（毫秒） <InputNumber v-model="draft.hover_delay" :min="100" :max="2000" :step="50" /></label>
    </div>
    <div class="command-preview" aria-label="命令栏预览">
      <Button v-for="item in groups[0]?.items" :key="item" size="small" variant="text"
        :disabled="!loaded || busy" :theme="draft.hidden.includes(item) ? 'default' : 'primary'"
        :aria-pressed="!draft.hidden.includes(item)" @click="set('hidden', item, !draft.hidden.includes(item))">
        {{ item === 'more' ? '…' : names[item] }}
      </Button>
      <span v-if="draft.enabled && draft.terminal" class="terminal-preview">在此打开终端</span>
    </div>
    <fieldset class="command-group" :disabled="!loaded || busy">
      <legend>系统按钮布局</legend>
      <div class="command-fields">
        <label>按钮间距（像素） <InputNumber :value="draft.spacing ?? 0" :min="0" :max="16"
          @change="value => draft.spacing = Number(value) || 0" /></label>
        <label v-for="item in separatorIds" :key="item">{{ names[item] }}分隔线位置
          <Select :value="draft.separators?.[item] ?? ''" :options="[
            { label: '系统位置', value: '' },
            ...standardOrder.map(id => ({ label: names[id] + '后', value: id })),
          ]" @change="value => moveSeparator(item, value)" />
        </label>
      </div>
      <div class="command-order">
        <div v-for="(item, index) in systemOrder" :key="item" class="command-order-row">
          <span>{{ names[item as keyof typeof names] }}</span>
          <Button size="small" variant="text" :aria-label="'上移 ' + names[item as keyof typeof names]"
            :disabled="index === 0" @click="moveSystem(index, -1)"><IconArrowUp :size="16" /></Button>
          <Button size="small" variant="text" :aria-label="'下移 ' + names[item as keyof typeof names]"
            :disabled="index === systemOrder.length - 1" @click="moveSystem(index, 1)"><IconArrowDown :size="16" /></Button>
        </div>
      </div>
      <Button size="small" variant="outline" :disabled="!draft.system_order?.length"
        @click="draft.system_order = []">恢复系统按钮顺序</Button>
    </fieldset>
    <fieldset class="command-group" :disabled="!loaded || busy">
      <legend>从模板新建</legend>
      <div class="command-fields">
        <label>模板目录
          <Input v-model="templateOptions.directory" placeholder="使用默认目录" />
          <Button size="small" variant="outline" @click="pickPath('directory', path => templateOptions.directory = path)">选择文件夹</Button>
        </label>
        <label>入口位置 <Select v-model="templateOptions.position" :options="positions" placeholder="司南菜单" /></label>
        <label>读取 PowerToys New+ <Switch v-model="templateOptions.new_plus" /></label>
        <label>隐藏扩展名 <Switch :value="templateOptions.hide_extension ?? true" @change="value => templateOptions.hide_extension = value === true" /></label>
        <label>隐藏开头数字 <Switch :value="templateOptions.hide_digits ?? true" @change="value => templateOptions.hide_digits = value === true" /></label>
        <label>替换日期和目录变量 <Switch v-model="templateOptions.variables" /></label>
      </div>
      <Button size="small" variant="outline" @click="previewTemplates">预览模板名称</Button>
      <ul v-if="templates.length"><li v-for="item in templates" :key="item.filename">{{ item.label }} → {{ item.filename }}</li></ul>
    </fieldset>
    <fieldset class="command-group" :disabled="!loaded || busy">
      <legend>动作编排</legend>
      <div class="command-actions">
        <Select v-model="addAction" filterable :options="choices" placeholder="选择动作" />
        <Button variant="outline" :disabled="!addAction" @click="insertEntry(addAction)">添加动作</Button>
        <Button variant="outline" @click="addApplication">添加应用</Button>
      </div>
      <div class="command-preview" aria-label="动作顺序">
        <Button v-for="entry in entries" :key="entry.id" variant="outline" size="small" draggable="true"
          :theme="entry.id === selected ? 'primary' : 'default'" @dragstart="dragging = entry.id"
          @dragover.prevent @drop.prevent="moveEntry(entry.id)" @click="selected = entry.id">
          {{ entryLabel(entry) }}
        </Button>
      </div>
      <p v-if="missingEntries.length" role="status" class="command-error">
        待重新关联的动作：{{ missingEntries.map(entryLabel).join('、') }}
      </p>
      <div class="command-fields">
        <section v-for="side in sides" :key="side.value">
          <h3>{{ side.label }}</h3>
          <div class="command-order">
            <div v-for="(item, index) in fixedLayout.filter(row => row.side === side.value)" :key="item.id" class="command-order-row">
              <span>{{ item.label }}</span>
              <Select :value="item.side" :options="sides" :aria-label="item.label + '所在栏'" @change="value => changeSide(item, value)" />
              <Button size="small" variant="text" :aria-label="'上移 ' + item.label" :disabled="index === 0"
                @click="moveFixed(item, -1)"><IconArrowUp :size="16" /></Button>
              <Button size="small" variant="text" :aria-label="'下移 ' + item.label"
                :disabled="index === fixedLayout.filter(row => row.side === side.value).length - 1"
                @click="moveFixed(item, 1)"><IconArrowDown :size="16" /></Button>
            </div>
          </div>
        </section>
      </div>
      <template v-if="chosen">
        <p v-if="missingEntries.includes(chosen)" role="status">动作来源缺失</p>
        <div class="command-fields">
          <label>动作来源 <Select :value="chosen.action" filterable :options="choices"
            @change="value => chosen && reassignAction(chosen, value)" /></label>
          <label>显示名称 <Input v-model="chosen.label" :placeholder="referenceLabel(chosen)" /></label>
          <label>位置 <Select v-model="chosen.placement" :options="placements" /></label>
          <label v-if="chosen.placement === 'menu'">子菜单 <Input v-model="chosen.group" placeholder="自定义动作" /></label>
          <label v-else>显示动态标签 <Switch v-model="chosen.dynamic" /></label>
          <label>浅色图标 <Input v-model="chosen.icon_light" placeholder="图标文件路径" />
            <Button size="small" variant="outline" @click="pickPath('icon', path => chosen && (chosen.icon_light = path))">选择浅色图标</Button>
          </label>
          <label>深色图标 <Input v-model="chosen.icon_dark" placeholder="图标文件路径" />
            <Button size="small" variant="outline" @click="pickPath('icon', path => chosen && (chosen.icon_dark = path))">选择深色图标</Button>
          </label>
        </div>
        <div v-if="chosenApp" class="command-fields">
          <label>应用名称 <Input v-model="chosenApp.name" /></label>
          <label>应用路径、执行别名或 AppsFolder 标识 <Input v-model="chosenApp.target" />
            <Button size="small" variant="outline" @click="pickPath('application', path => chosenApp && (chosenApp.target = path))">选择应用程序</Button>
          </label>
          <label>传入参数 <Switch v-model="chosenApp.pass_arguments" /></label>
          <label>参数来源 <Select v-model="chosenApp.source" :options="argumentSources" /></label>
          <Button size="small" variant="outline" :disabled="!chosenApp.pass_arguments" @click="previewArguments">预览参数</Button>
          <code v-if="argumentsPreview.length">{{ argumentsPreview.join(' ') }}</code>
        </div>
        <div v-if="chosenRule" class="command-fields">
          <label>选区数量 <Select v-model="chosenRule.count" :options="selectionCounts" /></label>
          <label>选区类型 <Select v-model="chosenRule.kind" :options="selectionKinds" /></label>
          <label>扩展名 <Input :value="chosenRule.extensions.join(', ')" placeholder=".jpg, .png"
            @change="value => chosenRule && (chosenRule.extensions = String(value).split(',').map(item => item.trim()).filter(Boolean))" /></label>
          <label>不满足时隐藏菜单项 <Switch v-model="chosenRule.hide" /></label>
        </div>
        <div class="command-actions">
          <Button size="small" variant="outline" @click="duplicate(chosen)">复制动作</Button>
          <Button v-if="!chosenRule" size="small" variant="outline"
            @click="(draft.conditions ??= {})[chosen.action] = { count: 'any', kind: 'any', extensions: [], hide: false }">添加选区条件</Button>
          <Button v-else size="small" variant="outline" @click="delete draft.conditions?.[chosen.action]">移除选区条件</Button>
          <Button size="small" variant="outline" theme="danger" @click="draft.actions = entries.filter(item => item.id !== selected); selected = ''">移除动作</Button>
        </div>
      </template>
    </fieldset>
    <Dialog :visible="leaveVisible" header="保存命令栏草稿" :footer="false" @close="finishLeave('cancel')">
      <div class="command-actions">
        <Button theme="primary" :loading="busy" @click="finishLeave('save')">保存并离开</Button>
        <Button variant="outline" @click="finishLeave('discard')">放弃修改</Button>
        <Button variant="text" @click="finishLeave('cancel')">继续编辑</Button>
      </div>
    </Dialog>
    <div v-for="group in groups" :key="group.label" class="command-group">
      <h3>{{ group.label }}</h3>
      <div class="command-options">
        <label v-for="item in group.items" :key="item">
          {{ names[item] }}
          <Switch :value="draft[group.key].includes(item)" :aria-label="`${group.label} · ${names[item]}`" :disabled="!loaded || busy"
            @change="(value) => set(group.key, item, value === true)" />
        </label>
      </div>
    </div>
  </section>
</template>

<style scoped>
.command-settings { margin-top: 22px; padding: 20px; border: 1px solid var(--border); border-radius: 12px; }
.command-header, .command-actions { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
.command-header { justify-content: space-between; }
.command-header p { color: var(--muted); margin-top: 5px; }
.command-group { margin-top: 16px; }
.command-fields { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 10px; margin: 10px 0; }
.command-fields label { display: grid; gap: 5px; align-content: start; }
fieldset.command-group { border: 1px solid var(--border); border-radius: 6px; padding: 12px; }
.command-actions :deep(.t-select__wrap) { min-width: 200px; width: auto; }
.command-options { display: grid; grid-template-columns: repeat(auto-fit, minmax(165px, 1fr)); gap: 8px; margin-top: 10px; }
.command-options label { display: flex; gap: 8px; align-items: center; justify-content: space-between; padding: 8px 10px; background: var(--surface-soft, #f3f6f5); border-radius: 6px; }
.command-preview { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; margin-top: 14px; padding: 10px; border: 1px solid var(--border); border-radius: 6px; }
.command-preview span { padding: 3px 6px; }
.command-order { display: flex; flex-wrap: wrap; gap: 8px; margin: 8px 0 12px; }
.command-order-row { display: flex; align-items: center; gap: 4px; padding: 4px 6px; border: 1px solid var(--border); border-radius: 6px; }
.command-order-row :deep(.t-select__wrap) { width: 76px; }
.terminal-preview { margin-left: auto; }
.command-error { color: var(--warning); overflow-wrap: anywhere; }
</style>
