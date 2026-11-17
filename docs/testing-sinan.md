# Sinan 0.12：架构重构验收

当前命令为 `sinan`，不再使用 `tt`／`tabtail`。测试以 Windows Terminal、Windows 11 x64 为目标；普通和管理员终端均可按同样步骤检查。历史设计文档中的旧名称不代表兼容入口。

## 1. 构建和自动回归

```powershell
Set-Location 'D:\100_Projects\110_Daily\test2'
cmake --preset windows-msvc
cmake --build --preset release --parallel 6
ctest --preset release
cmake --install build/sinan --config Release --prefix build/package-sinan
python tests/integration/package_test.py build/package-sinan ON
python tests/integration/architecture_test.py build/package-sinan/bin/sinan.exe
```

完整构建有 23 组 CTest。测试使用独立临时目录，观察已有 Explorer COM 列表；不会启用用户标签定制、注入、导航、重启 Explorer，或更改真实产品 PATH。登录测试只登记并清理自己的随机数据目录启动项。

独立 CLI／Agent 构建：

```powershell
cmake -S . -B build/sinan-headless -A x64 -DSINAN_BUILD_TUI=OFF
cmake --build build/sinan-headless --config Release --parallel 6
ctest --test-dir build/sinan-headless -C Release --output-on-failure
cmake --install build/sinan-headless --config Release --prefix build/package-sinan-headless
python tests/integration/package_test.py build/package-sinan-headless OFF
```

无 TUI 构建有 22 组测试，规则测试仍验证规则和代理，只将界面断言改为“明确报告未包含 TUI，配置不变”。此构建不能生成 FTXUI 工程。`dependencies` 检查生成工程依赖和实际 PE imports，防止 UI 进入 Agent、Agent 实现进入应用层或 DbgHelp／WinHTTP 进入注入 DLL。

## 2. 隔离配置与动作搜索

```powershell
$sn = (Resolve-Path './build/package-sinan/bin/sinan.exe').Path
$data = Join-Path $PWD 'build/sinan-manual-data'
& $sn --version
& $sn --help
& $sn --data-dir $data actions list --query '固定'
& $sn --data-dir $data actions list --query 'GUDING'
& $sn --data-dir $data preview 'D:\项目\源码\模块' --depth 2
& $sn --data-dir $data actions run preview --parameters '{"path":"D:\\项目\\源码\\模块","depth":"1"}'
& $sn --data-dir $data config set depth 3
& $sn --data-dir $data config show --json
```

预期：版本为 Sinan 0.12.0；搜索包含 `tabs.pin`；两个预览分别显示 `源码\模块` 和 `模块`。查询和预览不创建状态；第一次配置编辑写入 `$data\sinan.toml`。缺少目标的 `actions run tabs.pin` 返回退出码 2，不启动代理。

## 3. 独立 Agent 与身份

```powershell
& $sn --data-dir $data agent start --json
& $sn --data-dir $data agent start --json
& $sn --data-dir $data agent status --json
& $sn --data-dir $data tabs list --json
& $sn --data-dir $data agent stop --json
```

预期：两个 start 返回相同 PID；`executable` 指向相邻 `sinan-agent.exe`；模式为 `observe`，`shortcuts.hook_active` 为 false。停止后状态为 `running: false`。CLI 的 `agent run` 返回 2；调试独立宿主可直接运行同目录 `sinan-agent.exe --data-dir $data`，并通过原目录 CLI 停止。

自动 `architecture` 测试额外验证：复制到其他目录的真实 CLI 不能连接当前 Agent；同目录但未列入消费端角色的 EXE 也被服务端拒绝；原 CLI 随后仍能查询。IPC 测试会发送旧产品、缺少产品和错误版本报文，再发送正确报文，确认拒绝及恢复。

## 4. TUI 与真实 Explorer 回归

若旧 TabTail 定制仍在运行，先通过原版本的原路径停止其定制和代理。Sinan 不接管旧协议与租约。以下是用户主动启用真实定制的测试，自动回归不代替它。

```powershell
& $sn --data-dir $data tui --lang zh-CN
& $sn --data-dir $data tui --lang en
& $sn --data-dir $data start
& $sn --data-dir $data tabs list --json
```

逐项检查：

1. 设置全局 3 级、目录规则 2 级、单标签 1 级，确认优先级；相同目录的两个标签独立修改，已有标签也能列出。
2. 修改层级时预览立即变化；保存不覆盖其他未编辑字段；切换语言、72×24 窗口、按钮、弹窗、快捷键输入保持正常。
3. 目录入口的别名和全局快捷键生效；前台 Explorer 快捷键仅作用于可验证的活动标签；其他应用输入不产生卡顿。
4. 固定／取消固定、独立保护／解除保护、锁定／解除锁定分别验证；检查实际标签图标，任务视图图标保持正常；固定左右排列保留活动标签。
5. 固定、保护及锁定的真实鼠标／键盘路由、系统关闭确认框与通知；字体、加粗、文字颜色、标签背景、宽度三模式和恢复；浅深主题与不同 DPI。
6. 工作目录保存和恢复；自动登录恢复默认关闭，只有用户显式开启才登记。

需要对单标签测试时，从 `tabs list` 复制当前完整 ID：

```powershell
$id = '<当前 tab_id>'
& $sn --data-dir $data tabs set $id --depth 1
& $sn --data-dir $data tabs pin $id on
& $sn --data-dir $data tabs pin $id off
& $sn --data-dir $data tabs reset $id
```

退出测试：

```powershell
& $sn --data-dir $data stop
& $sn --data-dir $data agent stop
```

未经过真实系统／UI 矩阵验证的能力仍按实验能力报告；出现不支持时保留具体适配错误，不将“配置已保存”当成“已应用”。

## 5. 安装与 PATH（手工选择执行）

```powershell
& $sn install
# 完全关闭并重新打开 Windows Terminal
sinan --version
sinan path status
sinan tui
```

默认目录为 `%LOCALAPPDATA%\Sinan`。安装复制全部八个运行文件；`--data-dir` 不能重定向安装或真实 PATH 修改。卸载命令为 `sinan uninstall`，默认保留配置。自动构建验证使用 CMake 安装目录，不自动执行用户安装。
