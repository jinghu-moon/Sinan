# 0.11.0：三个模块与逐功能兼容验收

0.11.0 将关闭保护、目录锁定、标签选择、新建标签、重排及外观身份映射接入符号契约。内置精确适配继续可用；符号通过表示满足当前适配契约，实际操作和外观还要在目标 Explorer 中确认。

## 1. 先检查兼容性

```powershell
Set-Location 'D:\100_Projects\110_Daily\test2'
$tt = (Resolve-Path '.\build\package-v0.11.0\bin\tabtail.exe').Path
& $tt --version
& $tt symbols status
& $tt symbols prepare
$LASTEXITCODE
$s = & $tt symbols status | ConvertFrom-Json
$s.capabilities | Format-List
$s.symbol_profile | ConvertTo-Json -Depth 10
$s.xaml_modules | ConvertTo-Json -Depth 10
```

版本应为 `0.11.0`。准备会分别解析 `ExplorerFrame.dll`、`FileExplorerExtensions.dll` 和 `Windows.UI.FileExplorer.dll`。完整结果是 `prepared/complete: true`，三个 `modules` 的 `missing_symbols` 为空。状态中的 `symbol_profile` 独立反映符号适配，`capabilities` 同时考虑内置回退。

部分失败返回退出码 5；查看每个模块的 `error`、`failures` 和 `missing_symbols`。已完成的缓存保留，支持的功能继续可用。总限时仍为 90 秒；首次下载较慢时重试会复用已经准备完成的模块。

这些命令不会启用定制、重启 Explorer、停止代理或写入配置。TUI 中 F6 →「07 符号兼容」的三个按钮仍可直接使用，准备动作现在覆盖三个模块；F7 切换英文。

## 2. 离线与缓存升级

```powershell
& $tt symbols prepare --offline
# 独立验证缓存，不被正式运行期使用。
$cache = Join-Path $PWD 'build\manual-v011-symbols'
& $tt symbols prepare --cache-dir $cache
& $tt symbols prepare --offline --cache-dir $cache
```

正式使用省略 `--cache-dir`；运行期仍使用当前用户的 `%LOCALAPPDATA%\TabTail\cache\symbols-v1`。0.10.0 的 frame 缓存会按新版清单重新解析，已有匹配 PDB 可以离线复用。`--data-dir` 不改变符号缓存位置。

需要手动导入本机模块对应的离线 PDB 时：

```powershell
$worker = Join-Path (Split-Path -Parent $tt) 'tabtail-symbols.exe'
& $worker --offline --module frame --pdb 'D:\Symbols\ExplorerFrame.pdb'
& $worker --offline --module view --pdb 'D:\Symbols\FileExplorerExtensions.pdb'
& $worker --offline --module ui --pdb 'D:\Symbols\windows.ui.fileexplorer.pdb'
& $tt symbols prepare --offline
```

三个文件必须分别匹配当前系统模块的 PDB GUID/Age。`--module` 是配套工作程序的导入选项，主 CLI 不需要指定模块。

## 3. 新版功能回归

升级前用启动旧代理的原 EXE 路径和原数据目录停止旧代理。旧 DLL 已驻留 Explorer 时，保存工作后退出相应 Explorer，再打开测试窗口；新旧路径不要同时控制同一窗口。完整保留分发目录，不能只复制 EXE。

```powershell
$data = Join-Path $PWD 'build\manual-v011-data'
$folder = Join-Path $PWD 'build\manual-v011-folders\工作甲'
New-Item -ItemType Directory -Force -Path $folder | Out-Null
explorer.exe $folder
& $tt --data-dir $data start
$list = & $tt --data-dir $data tabs list --json | ConvertFrom-Json
$list.tabs | Select-Object tab_id,path | Format-Table -AutoSize
$id = ($list.tabs | Where-Object path -EQ $folder | Select-Object -First 1).tab_id
if (-not $id) { throw '等待测试标签被识别后，重新获取列表。' }
& $tt --data-dir $data tabs capabilities $id
& $tt --data-dir $data tabs pin $id on
& $tt --data-dir $data tabs visual-probe $id
& $tt --data-dir $data tui --lang zh-CN
```

在测试窗口中继续检查：

| 项目 | 验收行为 |
| --- | --- |
| 标题 | 不同标签可设置不同层级；导航后更新 |
| 固定／独立保护 | 标签关闭按钮、中键、Ctrl+W、右键和批量关闭入口保护原标签；整窗关闭显示系统任务对话框 |
| 目录锁定 | 地址栏、面包屑、侧边栏、前后退、双击子目录保持原目录；目标转发到新标签；刷新和文件操作正常 |
| 选择／新建 | 定位标签命中原实例；目录入口定位已有标签或在指定窗口新建 |
| 固定排序 | 在 F9「目录入口」设置左置／右置，至少三个标签检查真实顺序与选中标签保留；取消固定后按策略恢复分组 |
| 外观 | 图钉／锁出现在标签内部；文字颜色、加粗、字体和背景实际变化；任务栏图标不被替换 |
| 宽度 | 固定宽度、最小／最大范围实际生效，缩放和重开窗口后读回正确 |
| 停止 | 解除保护／锁定／固定及停止定制后，标题和外观恢复 |

详细入口沿用 [保护与外观验收](testing-tab-features.md) 和 [目录入口与重排验收](testing-v0.8.md)，版本准入以本文和 [兼容设计](05-symbol-compatibility.md) 为准。以 `appearance_confirmed`、`protection_confirmed`、实际图标／属性／顺序读回和真实交互为证据，不能只看配置保存或准备成功。

## 4. 本轮验证边界

本轮完成本机三个匹配 PDB 的只读解析、缓存复用与合成布局／独立降级测试，没有自动向用户 Explorer 安装 Hook、重启用户代理或关闭窗口。尚未完成其他 Windows 构建的真机矩阵；不宣称所有版本兼容。ARM64、布局或入口约定改变、缺少微软符号、XAML 属性／接口变化时仍可能局部不可用，报告应给出具体原因。
