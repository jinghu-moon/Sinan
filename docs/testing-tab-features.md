# 01 · TabTail 0.7.2 标签保护与外观验收

0.7.2 将整窗关闭提示改为 Common Controls v6 系统任务对话框，按语言设置显示中文或英文。默认“保留窗口”；Esc 和关闭提示框也保留窗口。任务对话框采用系统控件外观，不承诺完整 WinUI 深色主题。

0.7.1 已修正图标目标：替换原生标签的 XAML IconSource，窗口／任务栏预览图标保持系统行为。0.7.0 的窗口位图读回不能作为标签替换成功证据。新探针证据见 [02-tab-icon-fix.json](evidence/02-tab-icon-fix.json)，用户已提供标签内图钉截图。

适用设计：[01-tab-protection-and-appearance.md](01-tab-protection-and-appearance.md)。实现状态及限制见 [implementation.md](implementation.md)，原生读回证据见 [evidence/01-native-probe.json](evidence/01-native-probe.json)。

## 1. 版本与启动

Windows 11 x64；当前内部接口仅适配 ExplorerFrame `10.0.26100.8457`，SHA-256：

```text
4e28d2076608df5525ec1b7b2ed8dc0e866f43bf2ded6a5e4213ccd1c2284cbe
```

文字及标签背景还对 FileExplorerExtensions 和 Windows.UI.FileExplorer 做精确文件校验，不能用 ExplorerFrame 版本相同来推定外观适配成功。`doctor`、逐标签 `capabilities`、`visual-probe` 和错误字段共同反映结果。不支持的系统保留配置、预览和观察能力。

在 Windows Terminal 中打开 PowerShell，使用同一份 EXE 和同一数据目录完成一次测试：

```powershell
$tt = (Resolve-Path '.\build\msvc-lifecycle\Release\tabtail.exe').Path
$data = Join-Path $PWD 'build\manual-v07-data'
& $tt --data-dir $data doctor
& $tt --data-dir $data tui --lang zh-CN
```

ZIP 包使用解压后的 `bin\tabtail.exe`。运行目录必须同时包含 `tabtail-hook.dll`、`tabtail-layout.dll`、`tabtail-features.dll`、`tabtail-style-0.7.dll` 和 `tabtail-login.exe`。不要只复制 EXE。

已驻留 Explorer 的旧版 DLL 不能热替换。先用启动旧代理的原 EXE 路径正常停止旧代理，再由用户保存工作并退出加载旧模块的 Explorer，随后开启新 Explorer。停止代理和租约到期都会解除限制，但不会卸载已经固定驻留的 DLL。本轮自动验证没有重启或关闭用户工作窗口。

## 2. 准备专用目录和标签

```powershell
$base = Join-Path $PWD 'build\manual-v07-folders'
New-Item -ItemType Directory -Force -Path "$base\工作甲", "$base\工作乙", "$base\目标" | Out-Null
explorer.exe "$base\工作甲"
```

用 Explorer 的新建标签功能另外打开一次“工作甲”，再打开“工作乙”；确认有两个同路径实例。测试关闭、导航、整窗关闭仅针对这些测试标签。正常启用定制后，重新读取运行期 ID：

```powershell
& $tt --data-dir $data start
$list = & $tt --data-dir $data tabs list --json | ConvertFrom-Json
$list.tabs | Select-Object tab_id, path, behavior
$id = '<从上一步复制一个工作甲标签的完整 tab_id>'
& $tt --data-dir $data tabs capabilities $id
```

`tab_id` 包含本次代理运行期；代理重启后必须重新获取。不要把路径、旧 ID 或 HWND 当成当前标签身份。

退出码 `0` 表示该命令已确认完成；`2` 为输入或配置错误；`3/4` 为能力或身份限制；`5` 为未完成或部分完成。查看 `state_confirmed`、`appearance_confirmed`、`partial`、`behavior.actual`、`error`，不能把配置保存或 ACK 当成视觉成功。XAML 首次初始化可能待确认，代理正常同步会重试。

## 3. 阶段 1：固定与保护

```powershell
& $tt --data-dir $data tabs pin $id on
& $tt --data-dir $data tabs protect $id on
& $tt --data-dir $data tabs pin $id off
& $tt --data-dir $data tabs list --json
```

取消固定后独立保护仍为真，有效保护为 `pinned || protected`。另一同路径标签保持自身状态，能单独关闭。分别在专用受保护标签上测试标签关闭按钮、中键、Ctrl+W、右键关闭和系统提供的批量关闭入口；原标签必须仍在，不能只观察图钉。正常关闭前执行：

```powershell
& $tt --data-dir $data tabs protect $id off
```

固定默认替换为圆润实心图钉，独立保护默认保留文件夹图标；两者并非同一个开关。固定不阻止导航。

## 4. 阶段 2：目录锁定

```powershell
& $tt --data-dir $data tabs lock $id on
& $tt --data-dir $data tabs list --json
```

应显示启用时的 `locked_path`、有效目录身份和锁图标。依次测试地址栏、双击子目录、面包屑、侧边栏、前进、后退、上一级与 Shell 导航。离开锁定目录的请求应在导航发生前取消，支持转发的目标只打开一个新标签，原标签不先跳走再回来。刷新、排序、选择文件、切换视图应继续工作。

目录移动、删除或离线后应报告需要重新确认，不循环导航；独立解除固定、保护不解除锁定。固定和锁定同时存在时锁图标优先，提示和 TUI 显示完整状态。解除锁定保留当前目录及已转发的新标签：

```powershell
& $tt --data-dir $data tabs lock $id off
```

## 5. 快捷键与通知

默认三个快捷键均为空，避免未经真实输入验收便占用用户组合键。可配置：

```powershell
& $tt --data-dir $data shortcuts set toggle_pin 'Ctrl+Alt+P'
& $tt --data-dir $data shortcuts set toggle_protection 'Ctrl+Alt+G'
& $tt --data-dir $data shortcuts set toggle_directory_lock 'Ctrl+Alt+L'
& $tt --data-dir $data shortcuts show
```

仅受支持 Explorer 在前台时处理。长按只切换一次；捕获按键后快速换标签，应取消身份已变化的请求，不能误改新标签。测试地址栏、文件列表、菜单、中文输入法及其他软件焦点。已知系统冲突、重复绑定和无法注册的组合键应给出错误，不静默替换。

F8 → 通知，可选 `all / errors / off`。检查成功、部分完成、保护拦截、导航转发的消息；失败不能报成功，重复拦截不能刷屏。通知使用 Win32 系统通知区域气泡，开发副本无需 MSIX 身份；Windows 通知策略可能不展示气泡，此时 TUI 仍显示状态。点击通知应定位原实例；原标签关闭后不得定位到复用 HWND。气泡显示和点击仍需人工验收。

## 6. 阶段 3：文字、标签背景与图标

临时标签样式示例（PowerShell 7；Windows PowerShell 可用 F8 表单避免原生参数引号差异）：

```powershell
$value = '{"text":{"font_family":"Segoe UI","font_color":"#8A240D","bold":true},"light":{"background":{"normal":"#FDE8D2","hover":"#FFE0A0","selected":"#FFC060"}},"dark":{"background":{"normal":"#4A3830","hover":"#604830","selected":"#705030"}},"icon":{"source":"builtin:pin-rounded","color":"#FF5A36"}}'
& $tt --data-dir $data tabs style $id --value $value
& $tt --data-dir $data tabs visual-probe $id
& $tt --data-dir $data tabs style-reset $id
```

F8 表单支持全局、独立保护、固定、锁定、当前标签和目录规则；各字段可输入 `inherit` 移除当前层覆盖，或 `system` 显式恢复系统值。加粗可填 `true / false / inherit / system`；颜色为 `#RRGGBB / inherit / system`；字体为已安装名称或继承／系统。图标来源支持 `system`、`builtin:pin-rounded`、`builtin:lock-rounded`。本版不导入任意外部图片。

优先级逐字段计算：临时 > 锁定 > 固定 > 独立保护 > 首条命中目录规则 > 全局 > 系统。目录样式规则与标题规则独立，支持每行一个正向 glob 或精确路径、增删、启停、排序。临时样式不改变对应目录的其他标签。

验收顺序：

1. 先设置全局文字颜色，再用目录、固定、锁定、临时逐层覆盖，验证 `false` 覆盖 `true`，高层通用文字颜色覆盖低层主题颜色。
2. 浅色／深色主题下分别观察未选中、悬停、选中；选中和未选中标签始终可区分。`visual-probe` 应读到实际 TabContainer 或 SelectedBackgroundPath 填充、标题 TextBlock 字体与颜色，不能仅检查根元素 Background。
3. 导航后解除图标覆盖，确认恢复该目录的原图标。标签图标检查实时 XAML 视觉树中 PathIcon 的几何、颜色及尺寸；恢复后确认原 IconSource 对象。任务栏窗口图标应保持不变，不能用 WM_GETICON 或 Shell 图像列表作为标签替换成功证据。
4. 验证中文回退、字体错误反馈、高对比度系统颜色、100/125/150/200% 缩放、多屏拖动、宽度固定／自适应重新测量。
5. 72×24 中英文切换后完整输入、预览并保存；按 Tab 能到达应用按钮。切换目标、规则、分组或关闭表单保留各自未保存草稿。
6. 在另一终端修改无关配置，原草稿仍可提交；修改同一字段组则报告冲突、保留草稿，由用户选择重新载入。无效颜色、字体、数量及模式拒绝保存。

预览只展示草稿，不替代 Explorer 的主题、DPI 与视觉验收。默认不启用未经人工比对的彩色背景。

## 7. 阶段 4：整窗关闭与工作标签恢复

含有效关闭保护的专用窗口分别测试窗口关闭按钮、Alt+F4、任务栏关闭。提示默认选择“保留窗口”，检查 Enter、Esc 和对话框关闭按钮均不误关窗口；明确选择“关闭整个窗口”后允许整窗退出且不立刻重开。切换 zh-CN／en，确认每次只显示当前语言文案；auto 跟随系统 UI 语言。只有目录锁定、未固定也未启用独立保护时，不显示此关闭确认。系统注销、关机不弹拦截提示，已经打开的确认框收到会话结束请求后取消；强制结束进程不保证保护。

```powershell
& $tt --data-dir $data workspace save
& $tt --data-dir $data workspace show
& $tt --data-dir $data workspace restore
# 批量操作最多 100 个当前运行期 ID
& $tt --data-dir $data tabs batch protect off $id
```

快照仅保存重要工作目录、窗口组、状态与临时样式，不保存运行期 ID、GUID、HWND、滚动位置、选中文件或历史栈。每个保存窗口组内相同目录合并，重要标记取或；首个临时样式保留。恢复复用已有目录，不关闭现有标签；复用标签仅增加保存的真标记，不清除用户当前保护。保存组按当前窗口 HWND 升序循环映射，无窗口时创建一个 Explorer 窗口作为锚点。跨组映射到同一窗口时去重。

默认最多 20 个，可在 F8 设置为 1..100；总处理预算 20 秒，每个网络可用性检查最多等待 800ms、最多 4 个后台检查。无效／离线目录逐项跳过并返回部分完成，超时后重试复用已打开目录。TUI 工具页提供保存、查看、恢复及针对筛选结果的批量固定／保护／锁定，批量变更需确认。

登录自动恢复默认关闭，必须单独启用：

```powershell
& $tt --data-dir $data workspace save
& $tt --data-dir $data workspace automatic on
# 设置成功后检查 startup.login_registered；退出 TUI 不退出代理。
# 正常启用状态与 automatic 同时为真，后续登录才自动恢复。
& $tt --data-dir $data workspace automatic off
```

注册项位于当前用户 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，通过旁置 `tabtail-login.exe` 隐藏启动同目录代理；无需提权。日志 `startup-state.json` 记录精确归属命令，关闭自动恢复／卸载只移除自身未被他人改写的项。启用自动恢复后状态变更自动更新目录快照；主动停止定制会保存 disabled 状态，下次登录不应用定制。

## 8. 停止、异常退出与回退

```powershell
& $tt --data-dir $data stop
& $tt --data-dir $data agent stop
```

正常停止逐项检查关闭／导航保护、原图标、文字与背景恢复；失败返回部分完成，可重试。退出 TUI 不停止代理。异常结束**本测试代理**，等待超过 10 秒：DLL 的独立租约应解除关闭保护、目录锁定并恢复样式。不得为此结束其他数据目录或用户工作代理。

代理生命周期变化后旧 `tab_id` 必须失效。Explorer 退出、标签关闭、观察 token 变化时，状态只能依据同进程创建时间、同原生 GUID 转移；无法确认时不续租。进程已退出可单独报告无需恢复。

旧配置 schema 1/2 只读加载不改文件；首次显式保存升级为 schema 3，同时精确备份到 `tabtail.pre-schema3.toml`。保留旧文件可供人工回退；新配置字段不能直接交给不支持 schema 3 的旧程序。

## 9. 自动与人工覆盖矩阵

```powershell
cmake --build build/msvc-lifecycle --config Release --parallel 6
ctest --test-dir build/msvc-lifecycle -C Release --output-on-failure
```

| 阶段 | 已具备的自动验证 | 必须人工验证的入口 |
| --- | --- | --- |
| 0 | 精确模块校验、原生实例识别、实际文字／填充读回和恢复 | 视觉、多主题、多 DPI、特殊目录 |
| 1 | 状态组合、同路径双实例、RemoveTab／OnClose 原生入口、租约释放 | 关闭按钮、中键、Ctrl+W、菜单、批量关闭、长按 |
| 2 | 导航前取消、新标签转发计数、目录身份检查 | 地址栏、双击、面包屑、前后退、侧边栏、断网 |
| 3 | 样式优先级、system／inherit、迁移、冲突、72×24 双语组件、实际背景和图标读回 | 实际悬停、主题切换、中文回退、拖动、缩放、高对比度 |
| 4 | 快照格式、去重、数量／时间边界、登录项归属与隐藏启动器 | 三种整窗关闭、真实登录／关机、跨窗口恢复体验 |

`startup` 集成测试会临时注册自身隔离目录的 Run 项，验证隐藏启动器只启动观察代理，然后删除自身注册项；不安装 Hook、不导航、不模拟真实登录。原生探针独立于 CTest，使用专门 acceptance 目录和 `/separate` Explorer，仅关闭自身专用标签。原生入口成功不等价于全部鼠标／键盘路由已经验收。
