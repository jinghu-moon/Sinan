<script setup lang="ts">
import { IconArrowDown, IconArrowUp, IconPlus, IconRefresh } from '@tabler/icons-vue'
import {
  Button,
  Checkbox,
  Dialog,
  Form,
  FormItem,
  Input,
  InputNumber,
  Radio,
  RadioGroup,
  Space,
  Table,
  Tag,
  Textarea,
  type PrimaryTableCol,
} from 'tdesign-vue-next'
import 'tdesign-vue-next/es/dialog/style/index.css'
import 'tdesign-vue-next/es/form/style/index.css'
import 'tdesign-vue-next/es/input-number/style/index.css'
import 'tdesign-vue-next/es/radio/style/index.css'
import 'tdesign-vue-next/es/space/style/index.css'
import 'tdesign-vue-next/es/table/style/index.css'
import 'tdesign-vue-next/es/tag/style/index.css'
import 'tdesign-vue-next/es/textarea/style/index.css'
import { computed, onMounted, ref } from 'vue'
import { toast } from 'vue-sonner'
import {
  hasSettingsHost,
  requestSettings,
  type DirectoryRule,
  type RuleList,
  type RuleMatch,
  type RuleMutation,
} from '../settingsHost'

const columns: PrimaryTableCol[] = [
  { colKey: 'position', title: '优先级', width: 84 },
  { colKey: 'name', title: '规则', width: 150 },
  { colKey: 'patterns', title: '目录模式' },
  { colKey: 'action', title: '标题显示', width: 116 },
  { colKey: 'enabled', title: '状态', width: 76 },
  { colKey: 'operations', title: '操作', width: 226 },
]

const connected = hasSettingsHost()
const snapshot = ref<RuleList>()
const busy = ref(false)
const editor = ref<DirectoryRule>()
const editRevision = ref('')
const replacing = ref(false)
const patternText = ref('')
const depthValue = ref<number | string>(2)
const fullPath = ref(false)
const deleting = ref<DirectoryRule>()
const testPath = ref('')
const tested = ref<RuleMatch>()
const patterns = computed(() => patternText.value.split(/\r?\n/).filter((line) => line.trim()))
const depthValid = computed(
  () =>
    typeof depthValue.value === 'number' &&
    Number.isInteger(depthValue.value) &&
    depthValue.value >= 1 &&
    depthValue.value <= 32,
)
const canSave = computed(
  () =>
    editor.value?.name.trim() &&
    patterns.value.length > 0 &&
    (editor.value.action === 'native' || fullPath.value || depthValid.value),
)

function showRules(result: Partial<RuleList>) {
  if (!result.rules || !result.rules_revision) throw new Error('未收到规则列表。')
  snapshot.value = { rules: result.rules, rules_revision: result.rules_revision }
  tested.value = undefined
}

async function load() {
  if (!connected || busy.value) return
  busy.value = true
  try {
    const { code, result } = await requestSettings({ action: 'rules.get' })
    if (code !== 0) throw new Error(result.message || '无法读取目录规则。')
    showRules(result)
  } catch (error) {
    toast.error('读取目录规则失败。', {
      description: error instanceof Error ? error.message : undefined,
    })
  } finally {
    busy.value = false
  }
}

function edit(rule?: DirectoryRule) {
  if (!snapshot.value || busy.value) return
  replacing.value = Boolean(rule)
  editRevision.value = snapshot.value.rules_revision
  patternText.value = rule?.patterns.join('\n') ?? ''
  depthValue.value = typeof rule?.depth === 'number' ? rule.depth : 2
  fullPath.value = rule?.depth === 'all'
  editor.value = rule
    ? { ...rule }
    : {
        id: crypto.randomUUID(),
        name: '',
        enabled: true,
        match: 'glob',
        patterns: [],
        action: 'depth',
        case_sensitive: false,
      }
}

async function mutate(request: RuleMutation, success: string) {
  if (busy.value) return false
  busy.value = true
  try {
    const { code, result } = await requestSettings(request)
    if (!result.saved) throw new Error(result.message || '无法保存规则配置。')
    showRules(result)
    if (code === 0) {
      toast.success(success)
    } else {
      toast.warning('规则已保存，应用状态待确认。', { description: result.reload_error })
    }
    return true
  } catch (error) {
    toast.error('操作失败。', { description: error instanceof Error ? error.message : undefined })
    return false
  } finally {
    busy.value = false
  }
}

async function saveRule() {
  if (!editor.value || !canSave.value) return
  const rule = { ...editor.value, name: editor.value.name.trim(), patterns: patterns.value }
  const depth = fullPath.value ? 'all' : Number(depthValue.value)
  if (rule.action === 'depth') rule.depth = depth
  else delete rule.depth
  if (
    await mutate(
      {
        action: 'rule.put',
        rule_id: rule.id,
        rules_revision: editRevision.value,
        rule,
        replace: replacing.value,
      },
      '规则已保存。',
    )
  )
    editor.value = undefined
}

function toggle(rule: DirectoryRule) {
  if (!snapshot.value) return
  return mutate(
    {
      action: 'rule.toggle',
      rule_id: rule.id,
      rules_revision: snapshot.value.rules_revision,
    },
    rule.enabled ? '规则已停用。' : '规则已启用。',
  )
}

function move(rule: DirectoryRule, position: number) {
  if (!snapshot.value) return
  return mutate(
    {
      action: 'rule.move',
      rule_id: rule.id,
      rules_revision: snapshot.value.rules_revision,
      position,
    },
    '规则顺序已更新。',
  )
}

async function deleteRule() {
  if (!deleting.value || !snapshot.value) return
  if (
    await mutate(
      {
        action: 'rule.delete',
        rule_id: deleting.value.id,
        rules_revision: snapshot.value.rules_revision,
      },
      '规则已删除。',
    )
  )
    deleting.value = undefined
}

async function testRule() {
  const path = testPath.value.trim()
  if (!path || !snapshot.value || busy.value) return
  tested.value = undefined
  busy.value = true
  try {
    const { code, result } = await requestSettings({ action: 'test', path })
    if (code !== 0 || !result.source || result.preview_title === undefined)
      throw new Error(result.message || '无法计算匹配结果。')
    if (path === testPath.value.trim()) {
      tested.value = {
        source: result.source,
        rule_id: result.rule_id ?? '',
        matched_pattern: result.matched_pattern ?? '',
        preview_title: result.preview_title,
      }
    }
  } catch (error) {
    toast.error('测试路径失败。', {
      description: error instanceof Error ? error.message : undefined,
    })
  } finally {
    busy.value = false
  }
}

const matchLabel = computed(() => {
  if (!tested.value) return ''
  if (tested.value.source === 'global') return '全局默认'
  return (
    snapshot.value?.rules.find((rule) => rule.id === tested.value?.rule_id)?.name ||
    tested.value.rule_id
  )
})

onMounted(load)
</script>

<template>
  <main :aria-busy="busy">
    <div class="content">
      <header class="page-top">
        <div>
          <h1>目录规则</h1>
          <p>为指定目录设置标签标题。规则从上到下匹配，第一条命中即生效。</p>
        </div>
        <Space>
          <Button variant="outline" :disabled="!connected || busy" @click="load">
            <template #icon><IconRefresh :size="17" aria-hidden="true" /></template>
            重新载入
          </Button>
          <Button theme="primary" :disabled="!snapshot || busy" @click="edit()">
            <template #icon><IconPlus :size="17" aria-hidden="true" /></template>
            新增规则
          </Button>
        </Space>
      </header>

      <section class="card rules-table" aria-label="目录规则列表">
        <Table
          row-key="id"
          :columns="columns"
          :data="snapshot?.rules ?? []"
          :loading="connected && !snapshot && busy"
          :min-width="740"
        >
          <template #empty>
            <p v-if="!connected">请在司南桌面窗口中管理目录规则。</p>
            <p v-else-if="!snapshot && !busy">读取失败，请重新载入。</p>
            <p v-else>暂无规则。新增规则后，可以为指定目录单独设置标题。</p>
          </template>
          <template #position="{ rowIndex }">{{ rowIndex + 1 }}</template>
          <template #name="{ row }">
            <strong class="rule-name">{{ row.name || row.id }}</strong>
            <span class="muted">{{ row.match === 'exact' ? '精确匹配' : '通配符匹配' }}</span>
          </template>
          <template #patterns="{ row }">
            <code class="long-text">{{ row.patterns[0] }}</code>
            <span v-if="row.patterns.length > 1" class="muted"
              >共 {{ row.patterns.length }} 个模式</span
            >
          </template>
          <template #action="{ row }">
            {{
              row.action === 'native'
                ? '系统名称'
                : row.depth === 'all'
                  ? '完整路径'
                  : row.depth + ' 级'
            }}
          </template>
          <template #enabled="{ row }">
            <Tag :theme="row.enabled ? 'success' : 'default'" variant="light" size="small">
              {{ row.enabled ? '已启用' : '已停用' }}
            </Tag>
          </template>
          <template #operations="{ row, rowIndex }">
            <Space :size="2" align="center">
              <Button
                size="small"
                variant="text"
                theme="primary"
                :disabled="busy"
                @click="edit(row)"
                >编辑</Button
              >
              <Button size="small" variant="text" :disabled="busy" @click="toggle(row)">
                {{ row.enabled ? '停用' : '启用' }}
              </Button>
              <Button
                size="small"
                variant="text"
                :disabled="busy || rowIndex === 0"
                aria-label="上移"
                title="上移"
                @click="move(row, rowIndex)"
              >
                <template #icon><IconArrowUp :size="16" aria-hidden="true" /></template>
              </Button>
              <Button
                size="small"
                variant="text"
                :disabled="busy || rowIndex + 1 >= (snapshot?.rules.length ?? 0)"
                aria-label="下移"
                title="下移"
                @click="move(row, rowIndex + 2)"
              >
                <template #icon><IconArrowDown :size="16" aria-hidden="true" /></template>
              </Button>
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

      <section class="card path-test" aria-labelledby="path-test-heading">
        <h2 id="path-test-heading">测试路径</h2>
        <div class="test-controls">
          <Input
            v-model="testPath"
            placeholder="输入绝对路径，例如 D:\项目\司南"
            :disabled="!snapshot"
            @change="tested = undefined"
            @enter="testRule"
          />
          <Button :disabled="!snapshot || !testPath.trim() || busy" @click="testRule"
            >测试匹配</Button
          >
        </div>
        <dl v-if="tested" class="match-result" aria-live="polite">
          <dt>规则来源</dt>
          <dd>{{ matchLabel }}</dd>
          <template v-if="tested.matched_pattern">
            <dt>命中模式</dt>
            <dd>
              <code>{{ tested.matched_pattern }}</code>
            </dd>
          </template>
          <dt>标题预览</dt>
          <dd>{{ tested.preview_title ?? '系统名称' }}</dd>
        </dl>
      </section>
    </div>

    <Dialog
      :visible="Boolean(editor)"
      :header="replacing ? '编辑规则' : '新增规则'"
      :width="'min(640px, 92vw)'"
      :close-on-overlay-click="false"
      :confirm-on-enter="false"
      :confirm-btn="{ content: '保存规则', disabled: !canSave }"
      :confirm-loading="busy"
      cancel-btn="取消"
      @close="!busy && (editor = undefined)"
      @confirm="saveRule"
    >
      <Form v-if="editor" layout="vertical" :disabled="busy">
        <FormItem label="规则名称">
          <Input v-model="editor.name" :maxlength="256" placeholder="例如 司南项目" />
        </FormItem>
        <FormItem label="匹配方式">
          <RadioGroup v-model="editor.match">
            <Radio value="glob">通配符</Radio>
            <Radio value="exact">精确路径</Radio>
          </RadioGroup>
        </FormItem>
        <FormItem label="目录模式">
          <Textarea
            v-model="patternText"
            :autosize="{ minRows: 3, maxRows: 5 }"
            placeholder="每行填写一个绝对路径或模式"
          />
          <template #help>
            {{ editor.match === 'glob' ? '* 匹配当前层，** 可跨目录层级。' : '仅匹配所填目录。' }}
          </template>
        </FormItem>
        <FormItem label="标题显示">
          <RadioGroup v-model="editor.action">
            <Radio value="depth">按路径显示</Radio>
            <Radio value="native">使用系统名称</Radio>
          </RadioGroup>
        </FormItem>
        <FormItem v-if="editor.action === 'depth'" label="路径层级">
          <Space align="center">
            <InputNumber
              v-model="depthValue"
              :min="1"
              :max="32"
              :step="1"
              :disabled="fullPath || busy"
              theme="normal"
            />
            <Checkbox v-model="fullPath">完整路径</Checkbox>
          </Space>
        </FormItem>
        <FormItem label="选项">
          <Space>
            <Checkbox v-model="editor.enabled">启用规则</Checkbox>
            <Checkbox v-model="editor.case_sensitive">区分大小写</Checkbox>
          </Space>
        </FormItem>
      </Form>
    </Dialog>

    <Dialog
      :visible="Boolean(deleting)"
      header="删除目录规则"
      theme="danger"
      confirm-btn="删除规则"
      cancel-btn="取消"
      :confirm-loading="busy"
      @close="!busy && (deleting = undefined)"
      @confirm="deleteRule"
    >
      删除「{{ deleting?.name || deleting?.id }}」？
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

.page-top {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 12px;
  margin-bottom: 26px;
}

.page-top p {
  margin-top: 5px;
  color: var(--muted);
}

.rules-table {
  overflow: hidden;
}

.rules-table :deep(.t-table) {
  border-radius: inherit;
}

.rule-name,
.long-text,
.match-result dd {
  overflow-wrap: anywhere;
}

.muted {
  display: block;
  margin-top: 3px;
  color: var(--muted);
  font-size: 12px;
}

.path-test {
  margin-top: 16px;
  padding: 22px 24px;
}

.test-controls {
  display: flex;
  gap: 8px;
  margin-top: 14px;
}

.match-result {
  display: grid;
  grid-template-columns: max-content minmax(0, 1fr);
  gap: 6px 16px;
  margin: 16px 0 0;
}

.match-result dt {
  color: var(--muted);
}

.match-result dd {
  margin: 0;
}
</style>
