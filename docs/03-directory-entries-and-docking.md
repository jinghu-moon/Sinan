# 03 · 目录快捷入口、路径别名与固定标签排序

版本：TabTail 0.8.0。Windows 11 x64；配置 schema 4，继续使用 toml++ 单头文件解析。

## 目录入口

同一条记录保存 ID、名称、绝对目录、可选全局快捷键、启用状态、别名是否覆盖子目录和标题显示方式。快捷键可以留空，入口仍可用 CLI／TUI 打开，别名仍可生效。最多 128 条。

```toml
[[directories.entries]]
id = 'work'
name = 'TabTail'
path = 'D:\Projects\TabTail'
shortcut = 'Ctrl+Alt+1'
enabled = true
descendants = false
title = 'alias_depth'

[tab_behavior]
pinned_tabs_protect_close = true
locked_navigation = 'new_tab'
pinned_position = 'keep'
```

全局快捷键通过 RegisterHotKey 注册，使用 MOD_NOREPEAT；代理运行即可生效，不要求 Explorer 在前台。与已有固定／保护／锁定快捷键及其他入口重复时拒绝保存；其他应用占用时保存配置并报告注册失败，代理每隔至少 5 秒重试。修改、删除、禁用入口或退出代理时释放注册。前台 Explorer 操作快捷键仍只在 Explorer 中处理。

按快捷键或执行打开操作时：

1. 检查当前配置、当前用户会话、标签身份、完整扫描和目录可访问性。
2. 目录已打开：定位现有标签并尝试激活窗口，优先前台窗口中的匹配项。
3. 未打开且前台为经过验证的 Explorer：在该窗口新建标签并选中它。
4. 未打开且前台为其他应用：请求打开新的 Explorer 窗口。

目录检查采用现有有界后台检查（800 毫秒），离线／不存在／超时通过错误和通知反馈。全局快捷键保持按下不重复打开；CLI／TUI 的重复打开也会先检查现有目录。新窗口的返回状态为启动请求，不能等同于窗口已显示；现有窗口激活是否成功由 `foreground_activated` 报告。

路径比较采用 Windows 大小写映射、统一斜杠和词法清理，支持盘符路径与 UNC。不会解析目录联接／符号链接为同一物理目录；不同路径指向同一目录时仍视为不同入口。

## 路径别名

`title` 支持 `off`（常规路径）、`alias`（仅别名）、`alias_depth`（别名 + 相对路径层级）。默认仅匹配指定目录，启用 `descendants` 才覆盖子目录。命中根目录时只显示别名；子目录示例：`TabTail · agent\config`。后缀使用当前有效层级和分隔符，`all` 显示完整相对路径。

最具体的目录优先；路径边界严格匹配，`D:\Work` 不匹配 `D:\Work-old`。更具体的 `off` 记录可排除父目录别名。禁用记录不参与匹配。同一路径不能有两条同时启用的记录。

别名作为统一标题格式化的一部分，配置预览、规则测试、TUI、代理和 Explorer 标题钩子共用算法。离开作用域后恢复路径标题，返回后重新显示别名。不重命名文件夹，不修改地址栏。显式“保留系统标题”的目录规则仍保留系统标题。

## 固定标签位置

`keep` 默认不移动；`left`／`right` 在每个窗口内对标签做稳定分组。固定组和普通组分别保留相对顺序。取消固定的标签进入紧邻固定组的普通区域；仅锁定目录的标签不参与固定组，固定且锁定的标签参与。宽度独立配置。

用户在组内拖动的顺序会成为下一轮排序的输入；跨组拖动在下一轮同步时回到所属区域。切换左右位置保留两组内部顺序。停止定制解除排序维护，不强制还原启动前顺序。

实现通过 Explorer 原生按 GUID 移动接口，不模拟鼠标／键盘，不修改标题制造视觉顺序。内部标签成员表不是实际屏幕顺序：规划和确认均读取真正的 `TabView.TabItems`，并与原生 GUID 集合逐项建立对应。移动后恢复原先选中的 GUID，再读取实际顺序；不支持或无法确认时报告失败，不能宣称生效。配置保存成功与排序生效分别报告。

内部适配继续限定 ExplorerFrame `10.0.26100.8457` 的精确 SHA256。排序服务实现还校验 Windows.UI.FileExplorer 的精确 SHA256 与方法入口；XAML 顺序读取沿用 FileExplorerExtensions／Windows.UI.FileExplorer 双模块校验。其他构建保留配置与预览，拒绝内部写入。

Shell 集合可能漏登记后台标签或原生新建标签。定制模式及显式打开目录时，使用已验证的原生窗口清单补齐成员，在 Explorer 的 UI 线程只读取得位置；以 GUID、进程创建时间和窗口身份关联，避免切换 COM 观察身份时丢失设置。普通观察模式不启用此原生补充。原生位置失败、集合竞争仍按不完整处理，不隐藏错误。

## CLI、TUI 与迁移

命令：`entries list`、`entries add ID PATH --name NAME`、`entries remove ID`、`entries open ID`、`pinned show`、`pinned position keep|left|right`。编辑现有入口通过 `entries add ... --replace` 完整替换该记录，省略的可选项恢复默认值。

TUI 新增 F9“目录入口”，支持新建、编辑、删除确认和测试打开；编辑器提供别名实时预览，页面提供固定位置选择。支持简体中文／英文和 72×24，窄屏导航使用短标签。配置保存沿用分组比较与文件锁，外部冲突保留草稿；不会覆盖其他分组。

schema 1–3 读取时补充空入口和 `keep`。首次显式写入 schema 4，保留精确旧字节 `tabtail.pre-schema4.toml`，只读操作不迁移。已有 schema 3 之前的备份逻辑继续保留。旧二进制不能读取 schema 4；回退前须停止新版代理并恢复对应旧配置备份。

测试步骤见 [testing-v0.8.md](testing-v0.8.md)。自动检查不替代真实键盘快捷键、跨窗口拖动、多 DPI 和通知视觉验收。
