# 0.8.4：全局输入卡顿修复

## 原因与修改

旧版 `ExplorerShortcuts` 构造时就在代理主线程安装 `WH_KEYBOARD_LL`，即使仅观察、没有快捷键也会安装。低级键盘钩子必须通过安装线程的消息循环执行；该线程同时等待 IPC（空闲轮询约 100 ms）、扫描 Shell COM、读取配置和同步 Explorer，导致其他软件的按键也要等它处理。命中快捷键时，回调还直接调用 `OpenProcess`／`ReadProcessMemory`。

新版采用以下处理：

- 独立消息线程管理 Explorer 快捷键钩子；不执行扫描、IPC、配置读取或目标进程内存读取。
- 回调只筛选快捷键、确认前台窗口、向最多 16 项的单生产者／单消费者队列投递请求。普通按键快速放行；队列满时放行该次快捷键，不等待。
- 独立工作线程读取选中标签，验证窗口、进程创建时间及前台状态。超过 250 ms、配置变更、停用或焦点变化的未完成读取被丢弃。代理执行前仍验证原生 GUID、进程生命周期和当前活动标签。
- 观察模式、没有可用 Explorer 目标或没有可用绑定时不安装 Explorer 键盘钩子。状态提供 `shortcuts.hook_active`、`input_thread_id`、`input_dispatch`、`dropped`。
- TUI 快捷键录入钩子默认不安装；F8／F9 快捷键输入框聚焦时启用，离开后移除；已吞掉的按键会先处理对应释放。原有 Ctrl+C／Ctrl+Q 录入保护保留。
- 目录全局快捷键继续使用 `RegisterHotKey`，保持原行为。

## 替换正在使用的程序

只关闭 TUI 不会停止旧代理。先用任务管理器的“详细信息／命令行”确认旧程序均已退出，或通过启动它的原始 EXE 和数据目录执行 `agent stop`。新建目录启动新版不能修复另一份仍运行的旧代理。本次不自动结束用户进程或重启 Explorer。

如果已经像本次反馈那样结束所有旧 TabTail 进程，直接测试新版即可：

```powershell
Set-Location 'D:\100_Projects\110_Daily\test2'
$tt = (Resolve-Path '.\build\package-v0.8.4\bin\tabtail.exe').Path
$data = Join-Path $PWD 'build\manual-v084-data'
& $tt --version
& $tt --data-dir $data tui --lang zh-CN
```

此目录使用独立配置。先保持观察模式，在记事本、浏览器输入框、聊天软件中持续输入中文和英文，检查输入法候选、连按和删除是否流畅；切换到后台再测。随后在 F8／F9 快捷键框录入组合键，Tab 离开、Esc 取消，确认其他软件输入仍流畅。

退出 TUI 后检查后台状态：

```powershell
$state = & $tt --data-dir $data agent status --json | ConvertFrom-Json
$state.shortcuts | Format-List
# 观察模式预期：hook_active 为 False；input_dispatch 描述独立线程
```

再使用正常工作配置与已适配的 Explorer 验收启用定制后的输入和快捷键。若 Explorer 仍驻留旧路径的 DLL，遵循现有模块冲突提示处理；该问题与进程中的键盘监听修复分开，不要求为了观察模式测试重启 Explorer。

若要保留原程序路径、原配置以及 Explorer 已加载的兼容 DLL，可先关闭该路径下的所有 TUI，再使用已有更新脚本，仅替换 EXE（不覆盖已加载 DLL）。本次修改没有变更原生 DLL 协议：

```powershell
# 示例：此前使用的确实是 0.8.3 和 manual-v083-data 时
.\tools\update-agent.ps1 `
  -CurrentExecutable '.\build\package-v0.8.3\bin\tabtail.exe' `
  -NewExecutable '.\build\package-v0.8.4\bin\tabtail.exe' `
  -DataDirectory '.\build\manual-v083-data' `
  -ExecutableOnly
```

脚本会备份原 EXE、停止对应代理、在原路径更新并恢复之前的观察／定制模式；临时标签覆盖会随代理重启清除。参数应使用实际旧路径与数据目录，不要套用与当前运行程序无关的路径。

## 自动回归

```powershell
cmake --build build/msvc-lifecycle --config Release --parallel 6
ctest --test-dir build/msvc-lifecycle -C Release --output-on-failure
```

`shortcut_input` 测试通过生产监听线程和相同按键决策路径验证：主线程忙、目标读取停滞时仍响应；工作线程隔离；非目标应用／普通文本放行；长按不重复；队列满时放行；停用清理；无目标、无绑定和观察模式卸载；TUI 录入钩子按需启用与卸载。测试不向用户软件注入按键，也不操作 Explorer。`agent` 集成测试另检查真实观察代理不安装键盘钩子。

自动测试验证线程隔离与行为，不代表已经测量了各软件或中文输入法的端到端延迟；实际手感需按以上步骤复测。
