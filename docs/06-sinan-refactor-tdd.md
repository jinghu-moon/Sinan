# 06：司南 Sinan 开发期架构重构与 TDD 执行文档

状态：P0–P5 全部完成，公开推送和 GitHub 两路 CI 已验证；记录见文末。日期：2026-11-18。

## 1. 目标、事实与取舍

项目已正式更名为司南 Sinan；仓库 https://github.com/jinghu-moon/Sinan，公开，MIT。初始提交及 `baseline-tabtail-v0.11.0` 标签保存 TabTail 0.11.0 基线。构建、参考仓库、个人工具状态、运行配置及原始用户截图不进入版本库。

不可省略的基本事实：

1. CLI、TUI、未来 GUI 都是消费端，输入与展示不同，业务行为必须相同。
2. 观察 Explorer、持有标签状态、续订租约与处理快捷键需要持续运行；命令与面板按需使用。
3. 标题、保护及 XAML 操作必须在经过身份验证的 Explorer 线程执行；GUI 不直接持有注入能力。
4. 配置、预览、规则与动作搜索可以本地完成，不应为查询启动代理或 PowerShell。
5. 原生接口的系统兼容验证与产品更名是两件事；本轮保留既有符号／布局检查。
6. 不需要微服务、插件总线、数据库、额外 UI 框架或新的仓库。使用同仓 CMake 静态库和少量必要进程。

开发期允许破坏性变化：名称、命令、namespace、配置默认位置、进程和 IPC 可以直接更改；不保留 tt/tabtail 别名、自动迁移器或旧协议。旧数据目录只作为用户手工备份，不自动读取。

本轮交付范围：重命名、代码／链接边界、独立 Agent、统一应用入口、可搜索动作目录、可选 TUI、构建／分发／CI、完整回归。GUI 的视觉原型因用户要求先确认架构而暂停，本轮为其提供可消费的动作 API，具体绘制与视觉验收另行进行。PowerShell 自定义执行器也属于后续功能，不伪造已实现状态。

## 2. 依赖与编译产物

逻辑依赖：

```text
CLI / TUI / future GUI -> application -> client -> IPC -> Agent
                              |                       |
                         local services          Windows adapters
                              |                       |
                              +------ domain ---------+
```

目标职责：

| CMake 目标 | 内容 | 禁止依赖 |
| --- | --- | --- |
| sinan_domain | 标题格式、路径规则、目录入口、标签身份／策略 | CLI11、FTXUI、GUI、代理运行循环 |
| sinan_windows | 配置、系统包装、IPC、符号缓存、原生适配控制器 | 任意消费端、Agent 运行循环、DbgHelp／WinHTTP |
| sinan_client | 启停代理、状态与请求客户端 | Agent 实现、UI 框架 |
| sinan_application | 统一动作／查询、配置编辑、安装编排、动作目录 | CLI11、FTXUI、Agent 实现 |
| sinan_agent_runtime | 扫描、状态、保护、快捷键、通知、工作目录 | application、CLI、TUI、GUI |
| sinan_tui | 终端界面及仅在输入框聚焦时启用的快捷键录入 | Agent 实现 |
| sinan | CLI 与可选 TUI 宿主 | Agent 运行循环 |
| sinan_agent | 持续运行的无终端宿主 | CLI11、FTXUI、application |
| sinan_symbols | 独立 PDB 工作进程 | 消费端 UI |

终端快捷键录入从 `agent/interaction` 拆出为 UI 输入组件；后台快捷键仍属于 Agent。GUI 框架将只进入未来面板目标。域层保留现有 Windows 路径语义，暂不制造跨平台适配接口。

分发：`sinan.exe`（CLI/TUI）、`sinan-agent.exe`、`sinan-login.exe`、`sinan-symbols.exe`，以及 `sinan-hook.dll`、`sinan-layout.dll`、`sinan-features.dll`、`sinan-style.dll`。基础能力静态链接。`SINAN_BUILD_TUI=OFF` 时不下载或链接 FTXUI，CLI 和 Agent 必须可独立构建／使用。

## 3. 应用契约

沿用现有简单 JSON 数据模型，统一由 `application_action(root, request)` 返回 `{code,result}`。CLI 负责参数语法与输出；TUI 负责草稿、焦点和布局；业务校验、配置读写、运行期 ID 提取与代理请求归应用层。相同动作从不同入口调用必须得到相同结果和错误语义。

补齐应用层的 config.get/set、preview、appearance.patch、features.get、entries.put/remove、shortcuts.set、pinned.position、restore.automatic、style.load、tabs.list 与符号操作。CLI 不再直接创建 ConfigStore、调用 require_agent_result 或执行系统操作。界面中的本地草稿和标题预览可以使用纯计算函数。

`actions.list` 返回现有动作元数据（稳定 ID、双语名称、关键词、是否需要目标标签、是否修改状态）；支持本地搜索。`actions.execute` 仅分派已登记的现有动作，明确目标参数，拒绝未知动作。此 API 是 GUI 接入点，展示动作不代表已经执行。

## 4. 进程与身份

Agent 从独立 EXE 启动，登录宿主也启动 Agent。CLI 不包含 agent run 命令；需要诊断时直接运行 `sinan-agent.exe --data-dir ...`。Agent 使用自己的 COM 初始化和错误处理。

IPC 保留用户 SID、会话及规范路径验证，升级为角色约束：客户端必须连接相邻的 sinan-agent.exe；Agent 只接受同一分发目录内列明的消费端（sinan.exe／未来 sinan-panel.exe）。同一进程内的单元测试 loopback 可以验证传输，无需放开任意 EXE。加入明确的 Sinan 产品／协议版本 envelope，拒绝缺失或错误版本，不能静默接受旧 TabTail。

Explorer 桥接只接受相邻 sinan-agent.exe，开发用只读探针仅允许明确列出的 sinan.exe 路径；保留用户、会话、进程创建时间、实时模块与窗口身份校验。不能仅检查文件名，也不能通过取消校验解决多 EXE 问题。

## 5. TDD 阶段与失败先行

### P0：基线、Git 与公开仓库

- [x] 运行重构前 CTest：20/20，通过，日志保存在被忽略的 build/research/sinan-baseline-ctest.log。
- [x] 确认 MIT 与第三方许可证，排除参考项目及本地产物，扫描待提交文件的凭据模式和体积。
- [x] 初始提交、基线标签、公开仓库及首次推送。
- [x] 提交本文后才开始修改实现（文档提交 c0dfda2）。

### P1：结构／身份测试（Red）

先添加能够在旧结构中失败的测试：

- 产物必须为 Sinan，必须有独立 Agent，CLI 不再接受 agent run。
- 单独 Agent 的状态报告必须给出可核验的宿主路径；连续启动保持同一 PID，停止等待正确进程退出。
- 复制另一分发目录的 CLI 连接当前代理应拒绝；原分发 CLI 正常连接；不能按任意同目录文件名放行。
- IPC 错误产品／协议 envelope 被拒绝，后续正常请求仍可完成。
- 源码依赖与 CMake 源文件所有权不允许消费端链接 Agent 实现、Agent 链接 UI、DbgHelp／WinHTTP 进入注入 DLL。
- TUI 关闭选项使 FTXUI 完全不参与配置／构建。

记录至少一个真实失败结果，之后才实施 P2/P3。

### P2：名称与进程边界（Green）

- 全部活动源码、测试、工具使用 sinan namespace／Sinan 品牌／sinan.toml／新默认数据目录；历史设计文档保留其原版本身份。
- 从 runtime.cpp 移出代理客户端；新增最小 Agent 入口。
- 修复 IPC、桥接、安装／卸载清单及登录启动链路；仅新分发之间互通。
- 提取 TUI 快捷键录入，避免消费端链接后台输入监听实现。
- 按目标职责拆分 CMake；运行 P1 测试使其转绿。

### P3：统一应用能力与动作目录（Red -> Green）

先新增应用层测试，再实现：

- 预览不创建配置、不启动代理；中文、UNC、根目录及无效层级保持正确。
- 应用层配置编辑与 CLI 的结果一致；不相关配置修改不覆盖外观草稿；已有冲突检查继续生效。
- 未知动作、非法标签 ID、非法快捷键或重复目录 ID 被明确拒绝且不产生部分写入。
- 目录／快捷键／固定位置／外观文件导入统一经过应用层校验，大小限制继续生效。
- 动作 ID 唯一；双语名称与关键词可搜索；空查询可列出动作；未知动作执行失败；需要标签的动作不能省略目标。
- CLI 不再实现业务读写，TUI 和未来 GUI 可调用相同接口。

### P4：分发、文档、独立构建（Red -> Green）

- 安装、卸载、登录 helper 清单包括 Agent，TUI 可选不破坏 CLI。
- 使用干净输出目录完成新构建、安装；包中不包含旧 EXE／DLL、原始截图或参考源代码。
- 从新安装目录运行 --version、--help、preview、actions list 和隔离观察代理 smoke。
- 在无 FTXUI 的独立构建中运行命令与代理；检查目标依赖图和真实 PE imports。
- Windows GitHub Actions 执行配置、构建、CTest、安装与 artifact 打包。

### P5：完整回归与发布检查

- 原有 20 组测试按产品更名与新宿主调整预期后全部通过；保留原业务断言，不能删测试掩盖回归。
- 新增应用动作、进程角色、协议和结构测试全部通过。
- 复验三个模块的离线符号解析／缓存复用；无用户 Explorer 注入、重启或旧代理替换。
- 凭据／忽略目录／大文件审计；提交和推送实现，确认 GitHub main 与本地一致。
- 更新文末结果、README 和新手测试命令，区分自动验证与仍需真机交互验证的范围。

## 6. 自动回归矩阵

| 范围 | 保留／新增验证 |
| --- | --- |
| 域层 | 规则优先级、通配符、同路径实例隔离、身份代际、格式化与别名 |
| 配置 | TOML 校验、并发修订、无效输入不写入、样式三态与分组冲突 |
| CLI／TUI | 原有功能、双语、四字动作按钮、72x24、录入快捷键不退出 |
| Agent | 默认观察、幂等启动、并发状态请求、停止／重启、旧 runtime ID 拒绝 |
| IPC | 大小／深度／超时恢复、产品版本、跨目录拒绝、新 EXE 角色 |
| Explorer 适配 | 原有符号／布局／功能降级及 close-dialog 测试 |
| 构建／包 | 独立 Agent、可选 TUI、仅符号 helper 导入网络／DbgHelp、干净安装 |

测试均使用独立数据／符号目录。自动测试允许读取已有 Explorer COM 清单，不自动修改用户标签、注册真实产品 PATH、重启 Explorer或接管现有代理。登录注册测试仅操作其独立且可清理的条目。新 GUI 的视觉、DPI／IME 和真实 Explorer 操作矩阵不以本次架构测试代替。

## 7. 执行命令与完成条件

```powershell
cmake -S . -B build/sinan -G "Visual Studio 17 2022" -A x64
cmake --build build/sinan --config Release --parallel 6
ctest --test-dir build/sinan -C Release --output-on-failure
cmake --install build/sinan --config Release --prefix build/package-sinan
cmake -S . -B build/sinan-headless -G "Visual Studio 17 2022" -A x64 -DSINAN_BUILD_TUI=OFF -DBUILD_TESTING=OFF
cmake --build build/sinan-headless --config Release --parallel 6
```

完成条件是新的分层、进程、命名、统一动作入口和独立构建真实可用，并有升级前后测试证据；仅更名 target 或移动文件不算完成。使用初始提交／基线标签比较行为，不维护旧产品兼容层。

## 8. 执行记录

- P0：20/20 基线通过；公开仓库已创建，MIT 已被 GitHub 识别；209 个初始跟踪文件，约 2.2 MB，未发现凭据模式。
- P1 Red：旧版 `tabtail.exe` 运行新增 architecture 测试，失败于“CLI must be the renamed Sinan product”；新增应用测试失败于“Rule not found”。日志：`build/research/sinan-red-process.log`、`sinan-red-application.log`。这些失败先于生产结构修改。
- P2 Green：namespace／品牌／产物／默认目录／配置／IPC 全部更名；独立 Agent、登录链路和完整分发清单已实现。客户端、后台快捷键与终端录入分离。CLI 消费共享应用层，不链接 Agent 实现。跨目录客户端及同目录未知角色拒绝测试均通过。
- P3 Green：统一配置、预览、目录、快捷键、位置、宽度增量编辑和样式导入；动作目录含 14 项，支持双语、关键词和明确参数执行。53 个新增应用断言通过；未知／缺失请求、非法修改、重复入口和超大样式文件不写入配置。原有冲突和并发写入测试继续通过。
- P4 Green：干净 `build/sinan` 完整构建成功；干净 `build/sinan-headless` 最初以 BUILD_TESTING=OFF 构建成功且无 FTXUI，随后开启测试复验。两种安装目录均通过完整八文件清单、许可证、本地命令和独立 Agent smoke；生成工程依赖与真实 PE imports 检查通过。修正了原 install 对被忽略截图／原始证据的依赖。Windows CI 完整／无 TUI 两路的构建、CTest、安装、包 smoke 和 artifact 上传均通过（见下方链接）。
- P5 本地回归：完整 23/23 CTest 通过，约 20 秒；无 TUI 22/22 通过，约 11 秒；保留原 20 组业务测试，只有 headless 不构建 TUI 专项，规则测试按构建选项分别验收界面／无界面诊断。TUI 1747 个组件断言通过，保留双语、四字按钮和窄窗口。三个模块离线解析及缓存复用成功，29 个符号、network_used=false；没有执行用户 Explorer 注入／重启或接管旧代理。
- 证据摘要：[07-sinan-refactor.json](evidence/07-sinan-refactor.json)。详细运行日志及渲染保留在忽略的 build/ 下；[Sinan 测试步骤](testing-sinan.md) 区分自动测试与真实交互验收。
- CI 发现并修复：首轮远端测试发现无效离线 PDB 后临时目录残留；23299a6 增加离线 MSF 头验证，并只对新建的私有 staging 目录执行完整清理。保留原清理断言，增加具体错误校验和残留目录诊断；三个有效 PDB 的清理及无关缓存保留也复验通过。后续 [GitHub CI](https://github.com/jinghu-moon/Sinan/actions/runs/44193211183) 两路全部通过，包括安装、包 smoke 和产物上传。
- 提交／推送：重构实现 39b4577、CI 修复 23299a6 已推送公开 main；保留 baseline-tabtail-v0.11.0。最终验收记录另以仅文档提交保存。225 个文件约 2.24 MB，未发现凭据模式、超过 5 MiB 的文件或被排除目录混入。MIT 与第三方声明齐备。不将新 GUI、脚本执行器或尚未实测的 Explorer 交互记为已验收。
