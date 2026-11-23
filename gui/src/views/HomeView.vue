<script setup lang="ts">
import { IconActivity, IconAppWindow, IconPower, IconRefresh } from '@tabler/icons-vue'
import { Button, CheckboxGroup, RadioGroup } from 'tdesign-vue-next'
import 'tdesign-vue-next/es/checkbox/style/index.css'
import 'tdesign-vue-next/es/radio/style/index.css'
import { computed, onMounted, ref, watch } from 'vue'
import { toast } from 'vue-sonner'
import SettingSwitch from '../components/SettingSwitch.vue'
import FolderSizeSettings from '../components/FolderSizeSettings.vue'
import {
  hasSettingsHost,
  requestSettings,
  type GeneralState,
  type SettingsRequest,
} from '../settingsHost'

const connected = hasSettingsHost()
const state = ref<GeneralState>()
const busy = ref(false)
const agent = computed(() => state.value?.agent)
const blocked = computed(() => !state.value || Boolean(agent.value?.error))
const summaryOptions = [
  { label: '文件数量', value: 'show_files' },
  { label: '文件夹数量', value: 'show_folders' },
  { label: '大小', value: 'show_size' },
] as const
const summaryFields = ref<string[]>([])
const savedSummary = computed(() => summaryOptions.filter(({ value }) => state.value?.selection_summary[value])
  .map(({ value }) => value))
watch(savedSummary, (fields) => { summaryFields.value = [...fields] })
const summaryChanged = computed(() => summaryFields.value.length !== savedSummary.value.length ||
  summaryFields.value.some((field) => !savedSummary.value.includes(field as (typeof summaryOptions)[number]['value'])))

function saveSummary() {
  void execute({ action: 'selection-summary.set', settings: {
    show_files: summaryFields.value.includes('show_files'),
    show_folders: summaryFields.value.includes('show_folders'),
    show_size: summaryFields.value.includes('show_size'),
  } })
}
const canToggleCustomization = computed(
  () => agent.value?.mode === 'customize' || agent.value?.hook_available === true,
)

const runtime = computed(() => {
  if (!connected)
    return { title: '浏览器预览', detail: '请在司南桌面窗口中管理设置。', tone: 'neutral' }
  if (agent.value?.error)
    return { title: '代理连接失败', detail: agent.value.error, tone: 'warning' }
  if (!state.value)
    return busy.value
      ? { title: '等待读取', detail: '正在读取配置和代理状态。', tone: 'neutral' }
      : { title: '读取失败', detail: '请刷新配置和代理状态。', tone: 'warning' }
  if (!agent.value?.running)
    return {
      title: '代理未运行',
      detail: '启动代理后，司南可以监测资源管理器标签。',
      tone: 'neutral',
    }

  const count = agent.value.tab_count ?? 0
  if (agent.value.mode !== 'customize')
    return {
      title: '观察中',
      detail: agent.value.hook_available
        ? `正在观察 ${count} 个资源管理器标签。`
        : `正在观察 ${count} 个标签。当前 Explorer 版本暂不支持标题定制。`,
      tone: 'neutral',
    }

  return agent.value.runtime_applied
    ? { title: '定制已启用', detail: `正在管理 ${count} 个资源管理器标签。`, tone: 'active' }
    : {
        title: '定制待确认',
        detail: '已启用标签定制，等待资源管理器确认应用结果。',
        tone: 'warning',
      }
})

const startupStatus = computed(() => {
  const current = state.value?.startup
  if (!current) return undefined
  if (current.error) return current.error
  if (current.foreign_entry_preserved) return '登录项已被其他命令占用。'
  if (current.workspace_restore && current.login_registered)
    return '登录项已登记，工作区自动恢复也在使用。'
  if (current.login_registered) return 'Windows 登录项已登记。'
  if (state.value?.autostart || current.workspace_restore) return '设置已开启，登录项待登记。'
  return undefined
})

const trayStatus = computed(() => {
  if (agent.value?.tray?.error) return agent.value.tray.error
  if (agent.value?.tray?.registered) return '图标已登记到 Windows 通知区。'
  if (!state.value?.show_tray_icon) return undefined
  return agent.value?.running ? '图标登记待确认。' : '代理运行后显示图标。'
})

const explorerOptions = [
  {
    field: 'double_click_parent',
    action: 'double-click-parent.set',
    label: '双击空白处返回上级',
    description: '在资源管理器文件列表的空白处双击，返回父目录。',
  },
  {
    field: 'external_tabs',
    action: 'external-tabs.set',
    label: '在当前窗口打开外部目录',
    description: '从其他应用打开文件夹时，优先在现有资源管理器窗口中创建标签。',
  },
  {
    field: 'everything_search',
    action: 'everything-search.set',
    label: 'Everything 搜索入口',
    description: '在资源管理器搜索框中使用 Everything 查找文件名。',
  },
] as const

const folderSizeStatus = computed(() => {
  if (!state.value?.folder_sizes.enabled) return undefined
  const feature = agent.value?.folder_sizes
  if (feature?.error) return { text: feature.error, issue: true }
  if (!agent.value?.running) return { text: '等待代理启动。', issue: true }
  if (feature?.pending) return { text: '等待打开本地资源管理器目录。', issue: true }
  if (feature?.index === 'disconnected') return { text: '请启动支持 SDK 的 Everything 1.5。', issue: true }
  if (feature?.index === 'loading') return { text: 'Everything 索引正在加载。', issue: true }
  if (feature?.active) return { text: '已生效。', issue: false }
  return { text: '等待资源管理器确认。', issue: true }
})

function explorerStatus(field: (typeof explorerOptions)[number]['field']) {
  const listener = agent.value?.[field]
  const error = listener?.error || listener?.activation?.error
  if (error) return { text: error, issue: true }
  if (!state.value?.[field]) {
    if (listener?.active) return { text: '等待确认关闭。', issue: true }
    return undefined
  }
  if (field === 'everything_search' && state.value.search_box_hidden)
    return { text: '资源管理器搜索框已隐藏。', issue: true }
  if (listener?.active) return { text: '已生效。', issue: false }
  if (!agent.value?.running) return { text: '等待代理启动。', issue: true }
  if (listener?.pending) return { text: '等待打开资源管理器窗口。', issue: true }
  return { text: '等待确认生效。', issue: true }
}

async function execute(request: SettingsRequest, silent = false) {
  if (!connected || busy.value) return
  busy.value = true
  try {
    const reply = await requestSettings(request)
    if (request.action === 'general.get' && reply.code === 0)
      state.value = reply.result as GeneralState
    if (reply.state) state.value = reply.state

    if (reply.refresh_error) {
      toast.error('刷新状态失败。', { description: reply.refresh_error })
    } else if (reply.result.restart_required) {
      toast.warning('托盘设置已保存，请重启司南代理以应用。')
    } else if (reply.code !== 0) {
      const description = reply.result.error || reply.result.message
      if (reply.result.saved) {
        toast.warning('设置已保存，应用状态待确认。', { description })
      } else if (reply.code === 5 && !description) {
        toast.warning('当前运行状态待确认。')
      } else {
        toast.error('操作失败。', { description })
      }
    } else if (request.action === 'start' && !state.value?.agent.runtime_applied) {
      toast.warning('已启用标签定制，等待资源管理器确认。')
    } else if ('enabled' in request || request.action === 'folder-sizes.set' || request.action === 'selection-summary.set' || request.action === 'everything-search.scope') {
      toast.success('设置已保存。')
    } else if (request.action === 'agent.start') {
      toast.success('代理已启动。')
    } else if (request.action === 'start') {
      toast.success('标签定制已启用。')
    } else if (request.action === 'stop') {
      toast.success('标签定制已停止。')
    } else if (!silent) {
      toast.success('状态已刷新。')
    }
  } catch (error) {
    toast.error('操作失败。', { description: error instanceof Error ? error.message : undefined })
  } finally {
    busy.value = false
  }
}

const refresh = () => execute({ action: 'general.get' })
onMounted(() => execute({ action: 'general.get' }, true))
</script>

<template>
  <main :aria-busy="busy">
    <div class="content">
      <header class="page-top">
        <div>
          <h1>常规</h1>
          <p>管理司南启动方式与资源管理器交互。</p>
        </div>
        <Button variant="outline" :disabled="!connected || busy" @click="refresh">
          <template #icon><IconRefresh :size="17" aria-hidden="true" /></template>
          刷新状态
        </Button>
      </header>

      <section class="runtime card" aria-labelledby="runtime-heading">
        <div class="section-icon runtime-icon">
          <IconActivity :size="23" aria-hidden="true" />
        </div>
        <div class="runtime-copy">
          <h2 id="runtime-heading">运行状态</h2>
          <div class="runtime-title" :data-tone="runtime.tone">
            <span class="status-dot"></span>{{ runtime.title }}
          </div>
          <p>{{ runtime.detail }}</p>
        </div>
        <Button
          v-if="state && !agent?.error && (!agent?.running || canToggleCustomization)"
          :theme="agent?.mode === 'customize' ? 'default' : 'primary'"
          :variant="agent?.mode === 'customize' ? 'outline' : 'base'"
          :disabled="busy"
          @click="
            execute({
              action: !agent?.running
                ? 'agent.start'
                : agent.mode === 'customize'
                  ? 'stop'
                  : 'start',
            })
          "
        >
          {{ !agent?.running ? '启动代理' : agent.mode === 'customize' ? '停止定制' : '启用定制' }}
        </Button>
      </section>

      <div class="cards">
        <section class="card options" aria-labelledby="startup-heading">
          <div class="section-heading">
            <span class="section-icon"><IconPower :size="22" aria-hidden="true" /></span>
            <h2 id="startup-heading">启动与托盘</h2>
          </div>
          <SettingSwitch
            label="登录自启"
            description="登录 Windows 后自动在后台启动司南。"
            :checked="state?.autostart === true"
            :disabled="!state"
            :busy="busy"
            :status="startupStatus"
            :issue="
              Boolean(
                state?.startup.error ||
                state?.startup.foreign_entry_preserved ||
                (state?.autostart && !state.startup.login_registered),
              )
            "
            @change="(enabled) => execute({ action: 'startup.set', enabled })"
          />
          <SettingSwitch
            label="显示托盘图标"
            description="通过 Windows 通知区查看运行状态或管理代理。"
            :checked="state?.show_tray_icon === true"
            :disabled="blocked"
            :busy="busy"
            :status="trayStatus"
            :issue="
              Boolean(agent?.tray?.error || (state?.show_tray_icon && !agent?.tray?.registered))
            "
            @change="(enabled) => execute({ action: 'tray.set', enabled })"
          />
        </section>

        <section class="card options" aria-labelledby="explorer-heading">
          <div class="section-heading">
            <span class="section-icon"><IconAppWindow :size="22" aria-hidden="true" /></span>
            <h2 id="explorer-heading">资源管理器交互</h2>
          </div>
          <SettingSwitch
            v-for="option in explorerOptions"
            :key="option.field"
            :label="option.label"
            :description="option.description"
            :checked="state?.[option.field] === true"
            :disabled="blocked"
            :busy="busy"
            :status="explorerStatus(option.field)?.text"
            :issue="explorerStatus(option.field)?.issue"
            @change="(enabled) => execute({ action: option.action, enabled })"
          />
          <fieldset v-if="state?.everything_search" class="summary-settings" :disabled="blocked || busy">
            <legend>Everything 搜索范围</legend>
            <RadioGroup
              :value="state.everything_search_scope"
              :options="[
                { label: '当前文件夹', value: 'current-folder' },
                { label: '全盘', value: 'all-drives' },
              ]"
              :disabled="blocked || busy"
              @change="(scope) => {
                if (scope === 'current-folder' || scope === 'all-drives')
                  execute({ action: 'everything-search.scope', scope })
              }"
            />
          </fieldset>
          <FolderSizeSettings
            v-if="state"
            :settings="state.folder_sizes"
            :show-format="state.selection_summary.show_size"
            :busy="busy"
            :disabled="blocked"
            :status="folderSizeStatus?.text"
            :issue="folderSizeStatus?.issue"
            @save="(settings) => execute({ action: 'folder-sizes.set', settings })"
          />
          <fieldset v-if="state" class="summary-settings" :disabled="blocked || busy">
            <legend>选中项统计</legend>
            <p>选中文件或文件夹时，资源管理器底部状态栏自动显示统计。大小使用上方的单位和小数位数。取消勾选全部选项可恢复系统显示。</p>
            <div class="summary-fields">
              <CheckboxGroup v-model="summaryFields" :options="[...summaryOptions]" :disabled="blocked || busy" />
              <Button :disabled="!summaryChanged || blocked || busy" @click="saveSummary">保存统计设置</Button>
            </div>
          </fieldset>
        </section>
      </div>

      <footer v-if="state" class="data-location">
        <span>数据目录</span><code>{{ state.data_directory }}</code>
      </footer>
    </div>
  </main>
</template>

<style scoped>
.summary-settings {
  min-width: 0;
  margin: 16px 0 0;
  padding: 0;
  border: 0;
}

.summary-settings p {
  margin: 6px 0 12px;
  color: var(--muted);
}

.summary-fields {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.page-top,
.runtime,
.section-heading {
  display: flex;
  align-items: center;
  gap: 16px;
}

.page-top {
  justify-content: space-between;
  margin-bottom: 26px;
}

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

.page-top p,
.runtime p {
  margin-top: 5px;
  color: var(--muted);
  overflow-wrap: anywhere;
}

.runtime {
  padding: 24px;
  margin-bottom: 16px;
}

.runtime-copy {
  flex: 1;
  min-width: 0;
}

h2 {
  font-size: 16px;
  font-weight: 650;
}

.runtime h2 {
  color: var(--muted);
  font-size: 13px;
}

.runtime-title {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 4px;
  font-size: 20px;
  font-weight: 650;
}

.status-dot {
  background: #657a7b;
  width: 8px;
  height: 8px;
  border-radius: 50%;
}

.runtime-title[data-tone='active'] .status-dot {
  background: var(--accent);
}

.runtime-title[data-tone='warning'] .status-dot {
  background: #b66727;
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

.runtime-icon {
  width: 48px;
  height: 48px;
}

.cards {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 16px;
}

.options {
  padding: 22px 24px 7px;
}

.section-heading {
  gap: 12px;
  padding-bottom: 4px;
}

.data-location {
  display: flex;
  flex-wrap: wrap;
  gap: 7px 10px;
  margin-top: 24px;
  color: var(--muted);
  font-size: 12px;
}

.data-location code {
  overflow-wrap: anywhere;
}

@media (max-width: 880px) {
  .cards {
    grid-template-columns: minmax(0, 1fr);
  }
}

@media (max-width: 540px) {
  .runtime {
    flex-wrap: wrap;
  }
}
</style>
