<script setup lang="ts">
import { Button, Switch } from 'tdesign-vue-next'
import 'tdesign-vue-next/es/switch/style/index.css'
import { computed, onMounted, ref } from 'vue'
import { toast } from 'vue-sonner'
import { hasSettingsHost, requestSettings, type CommandBarSettings } from '../settingsHost'

const defaults = (): CommandBarSettings => ({
  enabled: false, terminal: true, hidden: [], icon_only: [], hidden_separators: [],
})
const baseline = ref(defaults())
const draft = ref(defaults())
const loaded = ref(false)
const busy = ref(false)
const status = ref('')
const errors = ref<string[]>([])
const connected = hasSettingsHost()
const changed = computed(() => JSON.stringify(draft.value) !== JSON.stringify(baseline.value))
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
    baseline.value = structuredClone(result.command_bar)
    draft.value = structuredClone(result.command_bar)
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
onMounted(() => update())
</script>

<template>
  <section id="command-bar-settings" class="command-settings">
    <header class="command-header">
      <div>
        <h2>资源管理器命令栏</h2>
        <p v-if="status" role="status">{{ status }}</p>
      </div>
      <div class="command-actions">
        <Button variant="outline" :disabled="!connected || busy" @click="update()">刷新</Button>
        <Button variant="outline" :disabled="!loaded || busy" @click="draft = defaults()">恢复默认</Button>
        <Button theme="primary" :disabled="!loaded || !changed || busy" :loading="busy" @click="update(true)">保存</Button>
      </div>
    </header>
    <p v-for="error in errors" :key="error" class="command-error">{{ error }}</p>
    <div class="command-options">
      <label>启用命令栏定制 <Switch v-model="draft.enabled" aria-label="启用命令栏定制" :disabled="!loaded || busy" /></label>
      <label>在此打开终端 <Switch v-model="draft.terminal" aria-label="在此打开终端" :disabled="!loaded || busy" /></label>
    </div>
    <div class="command-preview" aria-label="命令栏预览">
      <span v-for="item in groups[0]?.items" v-show="!draft.enabled || !draft.hidden.includes(item)" :key="item">
        {{ item === 'more' ? '…' : names[item] }}
      </span>
      <span v-if="draft.enabled && draft.terminal" class="terminal-preview">在此打开终端</span>
    </div>
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
.command-options { display: grid; grid-template-columns: repeat(auto-fit, minmax(165px, 1fr)); gap: 8px; margin-top: 10px; }
.command-options label { display: flex; gap: 8px; align-items: center; justify-content: space-between; padding: 8px 10px; background: var(--surface-soft, #f3f6f5); border-radius: 6px; }
.command-preview { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; margin-top: 14px; padding: 10px; border: 1px solid var(--border); border-radius: 6px; }
.command-preview span { padding: 3px 6px; }
.terminal-preview { margin-left: auto; }
.command-error { color: var(--warning); overflow-wrap: anywhere; }
</style>
