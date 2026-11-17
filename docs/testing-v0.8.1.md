# TabTail 0.8.1 升级与复测

## 本机问题

只读检查确认本机同时运行两份代理：

- `build/package-v0.7.0-final/bin/tabtail.exe`，数据目录 `build/manual-v07-data`。
- `build/package-v0.8.0/bin/tabtail.exe`，数据目录 `build/manual-v08-data`。

多个 Explorer 进程加载了旧版或测试目录中的 `tabtail-hook.dll`。原桥接协议把配置不兼容、入口已被 Hook 等错误统一显示为 HRESULT `2147942450`（`0x80070032`），因此仅凭这个数字无法判断原因。旧标题钩子即使代理停止仍固定驻留，必须在 Explorer 退出后才释放。

新版在写入前检查其他路径的驻留标题模块，显示其完整路径；标题桥接使用单独 v3 消息和映射名，避免旧版回调处理新版报文，保留真实错误文本。`doctor --json` 增加 `title_adapter_conflicts`。

F9 的快捷键输入原先是文本框，直接按 Ctrl+Q 会执行退出，未处理的 Ctrl+C 会进入 FTXUI 的 SIGINT 路径。新版在 F8／F9 快捷键框聚焦时记录组合键，其他输入和页面保留原有行为；Tab 离开输入框，Esc 取消。允许手工输入 `Ctrl+Alt+1`。录入不等于保存，仍需点击应用。

录入器只在创建 TUI 时所在的终端窗口前台且快捷键框有焦点时接收组合键。它不注册全局热键，不在后台记录输入；保留键会显示验证错误并保留上次输入。组合键与其他应用冲突时，保存结果和注册结果分别显示；1409 表示按键已被占用，无法从此错误查出占用程序。

## 一次性升级

退出正在运行的 TUI。以下命令使用已经确认的两份代理的原路径停止它们，不覆盖旧二进制。**重启 Explorer 前先完成复制／移动任务并保存工作。** 重启命令会要求手动确认；取消则不重启。

```powershell
Set-Location 'D:\100_Projects\110_Daily\test2'

& '.\build\package-v0.7.0-final\bin\tabtail.exe' --data-dir '.\build\manual-v07-data' agent stop
& '.\build\package-v0.8.0\bin\tabtail.exe' --data-dir '.\build\manual-v08-data' agent stop

$tt = (Resolve-Path '.\build\package-v0.8.1\bin\tabtail.exe').Path
$data = Join-Path $PWD 'build\manual-v08-data'
& $tt --version
& $tt --data-dir $data explorer restart --force

# 重启确认完成、桌面恢复后，再执行：
& $tt --data-dir $data start
& $tt --data-dir $data tui --lang zh-CN
```

目录入口和其他配置继续使用 `manual-v08-data`，无需重新创建。不要随后重新启动旧版代理。单纯关闭 TUI 不会停止后台代理。

## 输入复测

1. F9 → 新建或编辑入口，聚焦“全局快捷键”。直接按 Ctrl+Alt+1，应录入规范文本；录入 Ctrl+Q 或 Ctrl+C 也应留在界面，不退出。
2. Tab 离开快捷键框，Esc 取消编辑，再次进入确认录入未自动保存；Ctrl+Q 在非快捷键输入框中仍退出 TUI。
3. 填好 ID、名称与路径再应用。若 Ctrl+Alt+1 仍报 1409，换成未占用组合键，例如 Ctrl+Alt+Shift+9；占用错误应显示在页面，TUI 保持运行。
4. F8 → 快捷键，也复测上述录入、取消和应用过程。重复／保留组合键必须显示错误，不能退出。

```powershell
& $tt --data-dir $data agent status --json
& $tt doctor --json
```

检查 `directory_shortcuts.errors`／`shortcuts.errors` 和 `title_adapter_conflicts`。干净重启后的 Explorer 不应包含旧路径钩子。若仍失败，新版标题错误会附带原因；界面异步响应发生异常时会保留编辑内容，显示错误并写入数据目录 `tui-error.log`。

## 验证范围

自动回归覆盖真实 FTXUI 组件的 Ctrl+C／Ctrl+Q 处理、F8／F9 录入与取消、占用失败响应、异常快照保护；OS 注册测试覆盖真实热键冲突与释放。只读原生探针确认新版能在加载旧模块的 Explorer 上报告旧路径，并用隔离的 v3 协议读取原始标题，无标题写入或导航。

Windows Terminal 实际组合键、全局热键触发和焦点切换仍需按上述步骤真机复测。
