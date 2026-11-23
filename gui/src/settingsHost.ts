export type EverythingSearchScope = 'current-folder' | 'all-drives'

export type SettingAction =
  | 'startup.set'
  | 'tray.set'
  | 'double-click-parent.set'
  | 'external-tabs.set'
  | 'everything-search.set'

type AgentAction = 'agent.start' | 'start' | 'stop'

export type AppearanceSettings = {
  depth: string
  separator: string
  tabs: {
    width_mode: 'system' | 'fixed' | 'content'
    width: number
    min_width: number
    max_width: number
  }
}

export type DirectoryRule = {
  id: string
  name: string
  enabled: boolean
  match: 'exact' | 'glob'
  patterns: string[]
  action: 'native' | 'depth'
  depth?: number | 'all'
  case_sensitive: boolean
}

export type RuleMutation = { rule_id: string; rules_revision: string } & (
  | { action: 'rule.put'; rule: DirectoryRule; replace: boolean }
  | { action: 'rule.delete' | 'rule.toggle' }
  | { action: 'rule.move'; position: number }
)

export type RuleList = { rules: DirectoryRule[]; rules_revision: string }
export type RuleMatch = {
  source: 'rule' | 'global'
  rule_id: string
  matched_pattern: string
  preview_title: string | null
}

export type DirectoryEntry = {
  id: string
  name: string
  path: string
  shortcut: string
  enabled: boolean
  descendants: boolean
  scope?: 'global' | 'explorer'
  title: 'off' | 'alias' | 'alias_depth'
}

export type PinnedPosition = 'keep' | 'left' | 'right'
export type ShortcutSettings = {
  shortcut_settings: Record<string, string>
  shortcut_listener: {
    running?: boolean
    hook_active?: boolean
    global_available?: boolean
    errors?: Record<string, string>
    error?: string
  }
}
export type EntrySettings = { entries: DirectoryEntry[]; pinned_position: PinnedPosition }
export type EntryMutation =
  | {
      action: 'entries.put'
      entry_id: string
      entry: DirectoryEntry
      expected_entry: DirectoryEntry | null
    }
  | { action: 'entries.remove'; entry_id: string; expected_entry: DirectoryEntry }
  | { action: 'pinned.position'; position: PinnedPosition; expected_position: PinnedPosition }

export type ExplorerTab = {
  tab_id: string
  window: number
  path: string | null
  original_title: string
  preview_title: string
  applied_title: string
  title_applied: boolean
  source: 'global' | 'rule' | 'tab'
  action: 'native' | 'depth'
  depth: number | 'all'
  override: number | 'all' | null
  native_guid: string | null
  observed: boolean
  apply_error: string
  behavior: { pinned: boolean; protected: boolean; locked: boolean; error: string }
}

export type TabList = {
  running: boolean
  mode?: 'observe' | 'customize'
  scan_complete?: boolean
  config_error?: string
  tabs: ExplorerTab[]
}

export type TabMutation = { tab_id: string } & (
  | { action: 'tabs.locate' | 'tabs.reset' }
  | { action: 'tabs.set'; depth: string }
  | { action: 'tabs.pin' | 'tabs.protect' | 'tabs.lock'; enabled: boolean }
)

export type FolderSizeSettings = {
  enabled: boolean
  unit_mode: 'auto' | 'fixed' | 'custom'
  unit_system: 'iec' | 'si'
  fixed_unit: string
  decimal_places: number
  custom_unit_label: string
  custom_unit_bytes: string
  everything_instance: string
}

export type SelectionSummarySettings = {
  show_files: boolean
  show_folders: boolean
  show_size: boolean
}

export type CommandBarSettings = {
  enabled: boolean
  terminal: boolean
  hidden: string[]
  icon_only: string[]
  hidden_separators: string[]
}

export type SettingsRequest =
  | { action: 'command-bar.get' }
  | { action: 'command-bar.save'; settings: CommandBarSettings; expected: CommandBarSettings }
  | { action: 'folder-sizes.set'; settings: FolderSizeSettings }
  | { action: 'selection-summary.set'; settings: SelectionSummarySettings }
  | {
      action:
        | 'general.get'
        | 'appearance.settings.get'
        | 'rules.get'
        | 'entries.settings.get'
        | 'shortcuts.settings.get'
        | 'tabs.settings.get'
        | AgentAction
    }
  | { action: SettingAction; enabled: boolean }
  | { action: 'everything-search.scope'; scope: EverythingSearchScope }
  | { action: 'test'; path: string }
  | RuleMutation
  | EntryMutation
  | TabMutation
  | { action: 'directories.open'; entry_id: string }
  | {
      action: 'shortcuts.set'
      shortcut_id: string
      chord: string
      expected_chord: string
      name?: string
      expected_name?: string
    }
  | {
      action: 'appearance.settings.save'
      settings: AppearanceSettings
      expected: AppearanceSettings
    }

type ListenerState = {
  active?: boolean
  pending?: boolean
  error?: string
  activation?: ListenerState
}

export type GeneralState = {
  data_directory: string
  autostart: boolean
  show_tray_icon: boolean
  double_click_parent: boolean
  external_tabs: boolean
  everything_search: boolean
  everything_search_scope: EverythingSearchScope
  search_box_hidden: boolean
  folder_sizes: FolderSizeSettings
  selection_summary: SelectionSummarySettings
  agent: {
    error?: string
    running?: boolean
    mode?: 'observe' | 'customize'
    runtime_applied?: boolean
    hook_available?: boolean
    tab_count?: number
    tray?: { registered: boolean; error?: string }
    double_click_parent?: ListenerState
    external_tabs?: ListenerState
    everything_search?: ListenerState
    folder_sizes?: ListenerState & { index?: string }
  }
  startup: {
    error?: string
    workspace_restore?: boolean
    login_registered?: boolean
    foreign_entry_preserved?: boolean
  }
}

type Reply = {
  id: number
  code: number
  result: Partial<
    GeneralState & AppearanceSettings & RuleList & RuleMatch & EntrySettings & ShortcutSettings
  > & {
    message?: string
    error?: string
    saved?: boolean
    conflict?: 'appearance' | 'directories' | 'tab_behavior' | 'shortcuts'
    reload_error?: string
    runtime_applied?: boolean
    restart_required?: boolean
    selection_confirmed?: boolean
    foreground_activated?: boolean
    confirmation?: string
    shortcut_registration?: { errors?: Record<string, string> }
    tab_list?: TabList
    policy_saved?: boolean
    title_applied?: boolean
    state_confirmed?: boolean
    apply_error?: string
    command_bar?: CommandBarSettings
    command_bar_application?: { state: string; errors?: string[] }
  }
  state?: GeneralState
  refresh_error?: string
}

type WebView = {
  postMessage(request: SettingsRequest & { id: number }): void
  addEventListener(type: 'message', listener: (event: MessageEvent<Reply>) => void): void
  removeEventListener(type: 'message', listener: (event: MessageEvent<Reply>) => void): void
}

const webview = () => (window as Window & { chrome?: { webview?: WebView } }).chrome?.webview
export const hasSettingsHost = () => Boolean(webview())
let nextId = 0
let pending = Promise.resolve()

export function requestSettings(request: SettingsRequest) {
  const reply = pending.then(() => sendRequest(request))
  pending = reply.then(
    () => undefined,
    () => undefined,
  )
  return reply
}

function sendRequest(request: SettingsRequest): Promise<Reply> {
  const host = webview()
  if (!host) return Promise.reject(new Error('请在司南桌面应用中打开设置。'))

  const id = ++nextId
  return new Promise((resolve, reject) => {
    const cleanup = () => {
      host.removeEventListener('message', onMessage)
      window.clearTimeout(timeout)
    }
    const onMessage = (event: MessageEvent<Reply>) => {
      if (event.data?.id !== id) return
      cleanup()
      resolve(event.data)
    }
    const timeout = window.setTimeout(() => {
      cleanup()
      reject(new Error('操作超时，请刷新确认当前状态。'))
    }, 45000)
    host.addEventListener('message', onMessage)
    try {
      host.postMessage({ ...request, id })
    } catch (error) {
      cleanup()
      reject(error)
    }
  })
}
