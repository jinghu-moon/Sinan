# TabTail 0.8.0 测试步骤

0.8.1 修复版请先执行 [升级与输入复测](testing-v0.8.1.md)，以下功能命令继续适用。

## 准备

先通过原先启动代理的可执行文件路径和数据目录停止旧代理；不要用新版路径向旧版代理发送停止命令。旧版 DLL 已加载到 Explorer 时，保存文件操作进度后再手动重启 Explorer，避免多个版本同时处理内部接口。以下使用独立测试数据目录，不改 PATH、不启用登录启动。

```powershell
Set-Location 'D:\100_Projects\110_Daily\test2'
$tt = (Resolve-Path '.\build\package-v0.8.0\bin\tabtail.exe').Path
$data = Join-Path $PWD 'build\manual-v08-data'
$folder = Join-Path $PWD 'build\manual-v08-work'
New-Item -ItemType Directory -Force "$folder\src\agent\config" | Out-Null
& $tt --version
& $tt --data-dir $data entries add work "$folder" --name '开发' --shortcut 'Ctrl+Alt+1' --descendants
& $tt --data-dir $data entries list
& $tt --data-dir $data agent start
& $tt --data-dir $data start
& $tt --data-dir $data tui --lang zh-CN
```

## 目录快捷键及别名

1. 在记事本等其他应用中按 Ctrl+Alt+1，应打开测试目录。再按一次应定位已有标签；按住不连续创建标签。
2. 在 Explorer 前台按快捷键：已打开则定位；先关闭不受保护的测试目录标签，再在其他目录的 Explorer 中按快捷键，应在该窗口新建标签并选中。
3. 根目录标签应显示“开发”。进入 `src\agent\config`，默认层级 2 显示“开发 · agent\config”；层级改为 3 后显示“开发 · src\agent\config”。离开测试目录后恢复普通路径标题，返回重新显示别名。
4. F9 编辑入口，关闭对子目录生效，验证仅根目录显示别名；切换“仅别名”和“常规路径”再应用。F7 切换中英文，列表和编辑器文案应同步。

```powershell
& $tt --data-dir $data config set depth 3
& $tt --data-dir $data rules test "$folder\src\agent\config"
& $tt --data-dir $data entries open work
& $tt --data-dir $data agent status --json
& $tt --data-dir $data tabs list --json
```

状态中的 `directory_shortcuts.errors` 应为空。用其他软件占用同一按键后，检查注册错误；释放占用后至少等 5 秒应恢复。设置同程序重复快捷键应直接拒绝保存：

```powershell
& $tt --data-dir $data shortcuts set toggle_pin 'Ctrl+Alt+1'
# 预期非零退出；不要覆盖 work 入口的快捷键。
```

## 左置、右置、取消固定

在同一测试窗口打开至少 4 个不同目录标签。F2 选中两个标签并固定，或者用下列命令取得当前运行期 ID：

```powershell
$list = & $tt --data-dir $data tabs list --json | ConvertFrom-Json
$list.tabs | Select-Object tab_id,path | Format-Table -AutoSize
$id = ($list.tabs | Where-Object path -eq $folder | Select-Object -First 1).tab_id
if (-not $id) { throw '未识别测试标签，请重新获取列表。' }
& $tt --data-dir $data pinned position left
& $tt --data-dir $data tabs pin $id on
& $tt --data-dir $data pinned position right
& $tt --data-dir $data tabs pin $id off
& $tt --data-dir $data pinned position keep
```

左置／右置后实际标签应移动，当前选中的标签保持选中，其他标签的相对顺序保持。取消固定后应进入固定组旁边的普通区域；仅锁定目录的标签不随固定组移动。固定组内拖动调换顺序，等至少 2 秒应保留组内顺序；跨组拖动则会回到所属区域。跨窗口拖动、关闭重开、多 DPI 下还需人工验证。

检查 `tabs pin` 返回的 `docking.order_confirmed`，以及状态中的 `features.docking`。配置保存成功不能替代实际排序确认；失败时查看 `error`，特别是系统版本、旧 DLL 或视觉树尚未可用。

## 清理

```powershell
& $tt --data-dir $data pinned position keep
& $tt --data-dir $data entries remove work
& $tt --data-dir $data stop
& $tt --data-dir $data agent stop
```

仅清理本次测试目录和测试窗口，勿批量关闭日常工作标签。

## 开发验证

```powershell
cmake --build build/msvc-lifecycle --config Release --parallel 6
ctest --test-dir build/msvc-lifecycle -C Release --output-on-failure
```

`entries` 覆盖别名、路径边界、规范化、重复项、真实 OS 热键占用／释放和稳定排序。`entries_cli` 覆盖增删改、别名预览和位置配置；`tui` 渲染真实双语组件、72×24 导航与编辑保存。

原生探针使用全新目录复制测试二进制及 DLL，把 `tabtail_feature_probe.exe` 改名为 `tabtail.exe` 后执行 `docking`。它创建自己的 `/separate` Explorer 窗口，仅操作以该目录下 `TabTail-acceptance-` 开头的测试目录；每次更新 DLL 必须更换目录。`docking --alias` 额外测试真实标题钩子与导航，要求没有其他正在接管该测试窗口标题的代理。失败后用同一探针的 `cleanup` 清理其测试窗口，不能结束用户的 Explorer／代理。
