# TabTail v0.6 标签宽度

v0.6.1 修复宽度草稿误报 `Configuration changed`：保存时在文件锁内比较编辑开始时的宽度设置，语言、层级、规则或启停等无关修改不再导致失败。宽度本身被别处修改时保留草稿并显示中文 / 英文冲突提示，可使用“重新载入宽度”恢复编辑。界面区分未保存草稿、当前运行状态和已保存但等待布局确认。

已使用 0.6.0 布局模块的开发副本升级 0.6.1 时，先 Ctrl+Q 退出旧 TUI，再执行下面的 EXE 修复升级。此补丁不改变标题 / 布局桥接协议，保留已驻留的两个 DLL，无需重启 Explorer：

```powershell
.\tools\update-agent.ps1 -CurrentExecutable .\build\msvc-nord\Release\tabtail.exe -NewExecutable .\build\msvc-lifecycle\Release\tabtail.exe -DataDirectory .\build\nord-manual-data -ExecutableOnly
.\build\msvc-nord\Release\tabtail.exe --data-dir .\build\nord-manual-data tui --lang zh-CN
```

`-ExecutableOnly` 仅用于已确认协议兼容的 EXE 修复；它保留现有布局模块，且要求该 DLL 已存在。

标签宽度是独立于目录层级规则的全局外观设置，作用于当前用户会话中接入的 Explorer 标签栏。

| 模式 | CLI 值 | 行为 |
| --- | --- | --- |
| 系统默认 | `system` | 保留 / 恢复 Explorer 原有宽度行为 |
| 固定宽度 | `fixed` | 所有标签使用指定宽度，默认 320 DIP |
| 随内容 | `content` | 按最终标题内容测量，默认限制在 120..480 DIP |

所有尺寸必须是 80..1000 范围内的整数，最小宽度不得大于最大宽度。DIP 是缩放前的逻辑尺寸；150% 缩放时，320 DIP 约为 480 个物理像素。宽度包含标签图标、标题、关闭按钮及控件留白。超出最大宽度时仍由 Explorer 截断；标签栏空间不足时沿用控件的横向滚动行为。

```toml
[appearance.tabs]
width_mode = 'content'
width = 320
min_width = 120
max_width = 480
```

这是 schema 2 的可选表。旧配置缺省为 `system`，读取不会修改文件。固定宽度与范围可分别保留，切换模式无需重新填写；`appearance reset` 恢复这组默认值。宽度不参与“标签覆盖 > 目录规则 > 全局层级”的标题策略优先级。

## 升级当前开发副本

先在旧 TUI 中按 **Ctrl+Q** 退出界面，随后在项目根目录执行：

```powershell
.\tools\update-agent.ps1 -CurrentExecutable .\build\msvc-nord\Release\tabtail.exe -NewExecutable .\build\msvc-lifecycle\Release\tabtail.exe -DataDirectory .\build\nord-manual-data
$ttExe = Join-Path $PWD 'build\msvc-nord\Release\tabtail.exe'
$ttData = Join-Path $PWD 'build\nord-manual-data'
function ttv { & $ttExe --data-dir $ttData @args }
ttv --version
ttv tui --lang zh-CN
```

脚本保留原 EXE 路径、原先启用 / 观察模式和原有标题 DLL，并添加独立的 `tabtail-layout.dll`，不重启 Explorer。临时单标签覆盖在代理退出时清除，持久化规则保留。脚本检测到旧 TUI 仍占用 EXE 时会在停止代理之前退出；若已有不同版本的布局 DLL，则拒绝覆盖已驻留模块。

首次从 ZIP 使用时，应保留 `bin` 下 `tabtail.exe`、`tabtail-hook.dll`、`tabtail-layout.dll` 三个配套文件。不要更名 EXE；布局和标题桥接均校验相邻的 `tabtail.exe` 身份。已驻留标题 DLL 还会校验原命令路径。

## TUI 测试

1. 准备短标题、长中文标题和长英文路径的多个标签；程序启动前已存在的标签也应被发现。
2. F4 进入全局设置，在“标签宽度”中切换“系统默认 / 固定宽度 / 随内容”。Tab 移动焦点；箭头选择模式。
3. 固定宽度填 320，点击“应用宽度”。若代理仅观察，先点击“启用标题定制”。第一次布局尚未刷新时可显示“未应用”；稍后应出现“宽度已实测”和各标签实际 DIP 数值。
4. 选择“随内容”，填最小 120、最大 480，应用。短标题应收窄，长标题应展开但不超过 480。修改目录层级或导航到不同长度目录，宽度应随最终标题变化。
5. 把最大值改为 180，应限制较长标签；把最小值设为 280、最大 480，应拉宽短标签。最小值大于最大值、空值、小数、越界值应拒绝保存。
6. 点击“恢复系统宽度”，标题定制仍启用，宽度恢复原来的系统行为。停止定制或正常退出代理也应恢复宽度；仅退出 TUI 不停止代理。
7. 新建、关闭、切换、拖动排序、移至另一窗口；检查新标签也应用设置、关闭按钮可用、没有覆盖新增标签按钮。连续调整窗口大小，标签过多时检查横向滚动。
8. 在 100% / 150% / 200% 缩放显示器之间移动窗口，逻辑宽度应保持，物理像素随缩放变化。FTXUI 界面最低 72×24，两种语言下保存按钮应可通过 Tab 滚动访问。

## CLI 测试

以下使用上面定义的 `ttv`；安装到 PATH 后可替换为 `tabtail`。

```powershell
ttv start
ttv appearance show
ttv appearance set fixed --width 320
ttv appearance status
ttv appearance set content --min 120 --max 480
ttv appearance status
ttv appearance set system
ttv appearance reset
```

首次应用可能返回退出码 5，表示配置已保存但布局尚未确认；等待数秒（一至两次后台扫描）后再次查询 `appearance status`。状态的 `threads[].elements[]` 包含控件的 `ActualWidth`、`Width`、`MinWidth`、`MaxWidth`，单位 DIP。仅有 IPC 确认不算布局确认。

## 实现与验证边界

- 独立布局桥接协议 1、独立 DLL；标题仍使用协议 2，未改变旧驻留标题 Hook。只在启用定制且选择非系统模式后自动接入 XAML；默认 / 观察模式不注入布局模块。
- 通过运行时 XAML diagnostics 发现 `TabView` / `TabViewItem`，在其 UI 线程设置属性。固定和随内容模式都使用控件的 `SizeToContent` 布局与原生滚动；固定模式同时约束 Width、MinWidth、MaxWidth。随内容模式采用原生测量，不估算中英文字符长度。
- 保存原有本地属性值，恢复时按原来源设置或清除，再恢复 TabView 模式。每个 Explorer UI 线程独立维护 10 秒租约；代理异常退出后，UI 计时器尝试恢复原始属性。正常停止立即请求恢复。一次失联不表示已完成恢复。
- XAML 控件采用弱引用；初始树枚举结束后延后交还诊断缓存引用，避免提前释放父对象导致后续子对象句柄无效。当前精确适配版本还处理共享诊断缓存中已注销的回调身份，受 SEH 和运行时类名校验保护。
- 会话、用户、进程创建时间、目标 UI 线程和相邻 EXE 身份均校验；不同代理争用同一 UI 线程返回错误。布局模块受当前精确 ExplorerFrame 适配范围限制。其他 XAML 样式工具同时修改同一属性的兼容性未验收。
- 2026-11-16 在当前两个已打开标签上，通过 XAML 属性与实际布局读回确认：默认 240 / 240 DIP → 固定 320 / 320 → 随内容 216.667 / 250.667 → 恢复 240 / 240；最大 180 时两个标签均为 180，最小 280 时均为 280。发送进程退出且租约超时后也已恢复为 240 / 240。最终原始证据在 `build/research/layout-final-cycle.json` 和相邻的边界、租约验证文件中。
- 上述实测覆盖属性和布局尺寸；拖动、关闭按钮、跨显示器缩放等交互效果仍按上方步骤人工验收。未自动重启 Explorer。
- 已将实际开发副本的原路径代理更新到 0.6.0，保留原标题 DLL；通过真实 `appearance set` / `appearance status` 完成固定、自适应、系统恢复，标题应用和完整扫描均正常。测试后保留 `system`，其余标题 / 目录规则不变。证据为 `build/research/deployed-width-*.json`。
