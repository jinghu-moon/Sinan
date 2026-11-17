# 司南 Sinan

轻量的 Windows 资源管理器增强工具。优先复用系统能力，通过命令和快捷入口减少文件管理操作；提供 CLI、简体中文／英文 TUI，并为后续 GUI 动作面板提供共享应用 API。项目由 TabTail 更名，目前处于开发阶段。

司南是古代辨明方向的器具。这个名称表达了项目的职责：帮助用户在目录、标签和操作之间迅速找到方向。英文及 CLI 名称均为 `sinan`。

## 功能

- 标签标题按全局默认、目录规则、临时标签覆盖选择显示层级；支持路径别名、正向通配符及预览。
- 标签固定、独立关闭保护、目录锁定、固定标签左置／右置。
- 宽度、图标、字体、加粗、文本颜色和标签背景设置，读取实际结果确认是否生效。
- 目录入口及全局快捷键、Explorer 前台快捷键、系统通知、工作目录保存和恢复。
- Nord 真彩色双语 TUI；动作目录支持中英文及关键词搜索。
- 按能力报告 Explorer 兼容状态；符号解析／下载由独立工作进程完成。

原生能力依赖系统模块与布局验证，**符号存在不等于所有 Windows 构建都兼容**。自动回归覆盖状态与适配契约，真实 Explorer 鼠标／键盘路由、图标视觉、DPI 和主题仍按 [人工测试](docs/testing-sinan.md) 验收。GUI 面板和 PowerShell 自定义执行器尚未实现。

## 架构与产物

```text
CLI / TUI / future GUI → application → client → IPC → Agent
                             ↓                         ↓
                        Windows services ← domain / native adapters
```

- `sinan_domain`：标题、规则、目录入口及策略。
- `sinan_windows`：配置、系统包装、IPC、原生适配和符号缓存。
- `sinan_client`：代理生命周期和请求客户端。
- `sinan_application`：统一业务校验、配置编辑、安装编排、动作目录。
- `sinan_agent_runtime`：持续扫描、标签状态、快捷键、通知、工作目录。
- `sinan_tui`：终端消费层；按需构建，不进入 Agent。

分发包的 `bin/` 包含 `sinan.exe`、`sinan-agent.exe`、`sinan-login.exe`、`sinan-symbols.exe`，以及 `sinan-hook.dll`、`sinan-layout.dll`、`sinan-features.dll`、`sinan-style.dll`。基础库静态链接；只有符号 helper 链接 DbgHelp／WinHTTP。

Agent 是独立的无终端进程。IPC 验证用户、会话、进程映像完整路径和角色；只允许同一分发目录的消费端与 Agent 通信。消费端不能直接发送修改 Explorer 的桥接请求。

详见先于实现提交的 [06：重构 TDD 文档](docs/06-sinan-refactor-tdd.md)。

## 构建与测试

Windows x64、Visual Studio 2022 C++ 桌面开发工具、Windows SDK、CMake 3.25+、Python 3.11+（测试用）。首次配置下载固定版本并校验 SHA-256；TOML 使用仓库内的 toml++ 3.4 单头文件。

```powershell
cmake --preset windows-msvc
cmake --build --preset release --parallel 6
ctest --preset release
cmake --install build/sinan --config Release --prefix build/package-sinan
```

不需要 TUI 时：

```powershell
cmake -S . -B build/sinan-headless -A x64 -DSINAN_BUILD_TUI=OFF
cmake --build build/sinan-headless --config Release --parallel 6
ctest --test-dir build/sinan-headless -C Release --output-on-failure
```

关闭 TUI 后不下载或链接 FTXUI；CLI 与 Agent 保持可用。Windows CI 分别构建两种配置、运行回归并生成安装目录 artifact。

## 使用

Windows Terminal 中从分发目录运行：

```powershell
$sn = (Resolve-Path './build/package-sinan/bin/sinan.exe').Path
& $sn --version
& $sn tui
& $sn preview 'D:\Projects\Sinan\src' --depth 2
& $sn actions list --query '固定'
& $sn actions run preview --parameters '{"path":"D:\\Projects\\Sinan\\src","depth":"1"}'
```

使用独立测试配置，启动仅观察的代理：

```powershell
$data = Join-Path $PWD 'build/sinan-manual-data'
& $sn --data-dir $data agent start --json
& $sn --data-dir $data tabs list --json
& $sn --data-dir $data agent stop --json
```

`agent start` 只观察；`start` 启用真实定制。`agent run` 已移除，直接运行 `sinan-agent.exe --data-dir ...` 可诊断独立宿主。命令速查使用 `sinan --help`，详细验收见 [测试步骤](docs/testing-sinan.md)。

用户安装及 PATH 注册使用 `sinan install`，默认数据目录为 `%LOCALAPPDATA%\Sinan`，配置文件为 `sinan.toml`。新终端使用 `sinan`，不提供旧 `tt`／`tabtail` 别名。不自动读取或迁移 TabTail 目录、代理与配置；重构前基线保存在 `baseline-tabtail-v0.11.0` 标签和 [历史记录](docs/history/tabtail-v0.11.md)。

## 许可证与仓库内容

主项目采用 [MIT](LICENSE)，第三方组件见 [THIRD_PARTY.md](THIRD_PARTY.md)。参考项目源码、构建依赖、运行数据、个人工具状态和原始截图通过 `.gitignore` 排除；GPL 参考代码不作为 MIT 源码发布。
