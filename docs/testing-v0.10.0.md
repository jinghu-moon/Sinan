# 0.10.0：符号兼容测试

首先验证兼容报告与符号准备，这两步不会重启 Explorer、启用标题定制或修改现有配置。命令支持管理员终端，但符号缓存属于运行命令的 Windows 用户。

```powershell
Set-Location 'D:\100_Projects\110_Daily\test2'
$tt = (Resolve-Path '.\build\package-v0.10.0\bin\tabtail.exe').Path
& $tt --version
& $tt symbols status
& $tt symbols prepare
$LASTEXITCODE
& $tt symbols status
```

预期版本为 0.10.0。`prepared: true` 表示匹配 PDB 和标题布局契约通过；它不是 Explorer 中的应用确认。`missing_symbols` 应为空。`symbol_profile.inventory/title` 是符号适配结果，`capabilities` 同时考虑内置回退。当前已内置的系统即使符号准备失败仍可能 `ready: true`，应分别查看，不把回退当作解析成功。

如果微软没有公开该构建的符号或断网，准备返回非零和原因；配置、预览、普通观察继续可用。若报告布局不支持，保留报告即可，不手动改 RVA，也不需要重启 Explorer 来反复试错。

## 缓存复用及离线

```powershell
# 有缓存时无需下载；--offline 明确禁止联网。
& $tt symbols prepare --offline
# 强制联网重新下载和解析。
& $tt symbols prepare --force

# 隔离测试，不影响正式运行期的缓存。
$cache = Join-Path $PWD 'build\manual-v010-symbols'
& $tt symbols status --cache-dir $cache
& $tt symbols prepare --cache-dir $cache
& $tt symbols prepare --offline --cache-dir $cache
```

隔离目录仅用于验证；正式使用请省略 `--cache-dir`。`--data-dir` 仅改变配置和代理目录，不改变符号缓存。

已有离线 PDB 时，保留全部分发文件，通过配套工作程序导入；严格匹配当前系统 ExplorerFrame 的 PDB，不能使用别的 Windows 构建或模块的 PDB。

```powershell
$worker = Join-Path (Split-Path -Parent $tt) 'tabtail-symbols.exe'
& $worker --offline --pdb 'D:\Symbols\ExplorerFrame.pdb'
& $tt symbols status
```

PDB 导入只进行本地解析。日常联网／离线准备建议使用主命令，由主进程施加总时间上限。

## TUI

```powershell
$data = Join-Path $PWD 'build\manual-v010-data'
& $tt --data-dir $data tui --lang zh-CN
```

F6 → 滚动到“07 符号兼容”。“兼容检查”只读取状态；“准备符号”可联网下载；“离线解析”仅使用本地 PDB。没有选中标签也可使用。每项操作独立成按钮，执行中禁用重复提交；结果窗口可滚动，失败也保留具体原因。F7 切换英文，在 72×24 和较大窗口分别检查按钮、卡片与报告。

## 标题真机验收

先让原来路径的旧代理停止控制标签，并按原有升级流程释放旧 DLL；不要让不同路径版本同时控制同一个 Explorer。分发目录需要完整保留 `tabtail-symbols.exe` 和所有 DLL。下面只在你准备测试新版时执行：

```powershell
$data = Join-Path $PWD 'build\manual-v010-data'
& $tt --data-dir $data doctor
& $tt --data-dir $data start
& $tt --data-dir $data tabs list --json
```

打开两个不同目录标签，在 F2 分别设置不同层级，确认标题随导航更新。停止定制后应恢复 Shell 显示名。报告中的 `title_applied` 才是逐标签应用确认。

新构建只通过标题／枚举契约时，固定／锁定／图标／重排等仍可能不可用，错误应指出相应功能尚未验证，不能让未知布局进入内部调用。精确内置构建应按现有 [保护与外观测试](testing-tab-features.md) 回归；本次不宣称其他版本的行为和 XAML 私有布局已经兼容。

## 开发验证

```powershell
cmake --build build/msvc-lifecycle --config Release --parallel 6
ctest --test-dir build/msvc-lifecycle -C Release --output-on-failure
```

自动测试不下载 PDB，不重启 Explorer，不修改真实 PATH，不停用用户代理。联网验证单独在 `build/research` 缓存进行。
