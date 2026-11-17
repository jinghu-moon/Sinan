# ExplorerTabUtility 深度调研：兼容性与命令快捷键

调研日期：2026-11-18。对象：本地 `refer/ExplorerTabUtility-master`；工程版本字段为 **2.5.0**。对照对象：TabTail 0.9.0 当前源码。

本报告依据本地源码、README、发布流程静态分析。未把该参考项目作为后台程序运行，未更改 Explorer、快捷键、注册表或安装状态；未完成跨 Windows 版本真机验收。本地目录名 `master` 不代表已经核实上游最新提交。

## 1. 结论

ExplorerTabUtility 没有一套“兼容所有 Explorer 内部版本”的适配引擎。它通过 **Shell COM、窗口消息、窗口事件和模拟输入** 完成操作，从而避开了大量随版本变化的内部地址与对象布局。

其 README 声明的范围是 **Windows 11 22H2，Build 22621 及以上，且具有 File Explorer Tabs 功能**。这不是 Windows 全版本兼容承诺，也不是未来所有累积更新均已验证的证据。

它适合作为 TabTail“命令 + 快捷键”及部分普通标签操作的参考；它没有实现 TabTail 的逐标签标题改写、图标替换、文字／背景／宽度定制、关闭保护、目录锁定和固定标签排序，不能用它的兼容范围推导这些功能也可免适配。

建议 TabTail 采用 **普通命令按能力运行，内部定制保留版本适配** 的结构。不应直接移除现有 SHA-256、模块身份和内存布局检查。

## 2. 证据范围与源码导航

参考项目主体是 C#／WPF，目标框架 `net9.0-windows;net481`，依赖 H.Hooks 1.7.0，并通过类型库引用 Shell32／SHDocVw。双框架解决运行时部署问题，不是 Explorer ABI 适配方案。

| 主题 | 本地源码位置 |
| --- | --- |
| 系统要求 | [README](../../refer/ExplorerTabUtility-master/README.md)，第 16 行 |
| 工程版本、框架与依赖 | [ExplorerTabUtility.csproj](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/ExplorerTabUtility.csproj) |
| 标签枚举、创建、导航、恢复、窗口转标签 | [ExplorerWatcher.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Hooks/ExplorerWatcher.cs) |
| 快捷键范围 | [HotkeyScope.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Models/HotkeyScope.cs) |
| 操作枚举与配置字段 | [HotKeyAction.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Models/HotKeyAction.cs)、[HotKeyProfile.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Models/HotKeyProfile.cs) |
| 键盘识别与异步投递 | [Keyboard.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Hooks/Keyboard.cs) |
| 鼠标组合与双击 | [Mouse.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Hooks/Mouse.cs) |
| 命令调度与模拟按键 | [HookManager.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Managers/HookManager.cs) |
| 前台窗口判定与有界重试 | [Helper.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Helpers/Helper.cs) |
| Shell 目录身份比较 | [ShellPathComparer.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Interop/ShellPathComparer.cs) |
| 编辑中的配置与已保存配置 | [ProfileManager.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/Managers/ProfileManager.cs) |
| 按作用范围筛选命令、录入时暂停原绑定 | [HotKeyProfileControl.xaml.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/UI/Views/HotKeyProfileControl.xaml.cs) |
| 按名称及完整位置搜索 | [TabSearchPopup.xaml.cs](../../refer/ExplorerTabUtility-master/ExplorerTabUtility/UI/Views/TabSearchPopup.xaml.cs) |

本次读取的关键文件 SHA-256，供后续对照本地副本：

```text
Hooks/ExplorerWatcher.cs
FE9518C1D5E1C29BC4D341553E4914A5AA756462D38A3960BC3374E7894A7C86
Hooks/Keyboard.cs
08E13A0AEB0DA7385B19F14DA7672A1BBB7A04E209E03CF5D607F96CCA1F7A1E
ExplorerTabUtility.csproj
B8D86FD4351549E3A50901D05F05F02693BBCEB8254049EE5D9CD2B88D8C777F
```

## 3. 它怎样操作 Explorer

### 3.1 枚举标签与定位当前标签

`InitializeShellObjects`（ExplorerWatcher 第 892 行）创建 `ShellWindows`，先枚举已有条目，再订阅 `WindowRegistered`。因此启动前已经打开的窗口也会进入它的观察集合。

`GetTabHandle`（第 707 行）从 `InternetExplorer` COM 对象取得 `IServiceProvider`，以 `IID_IShellBrowser` 查询浏览器接口，再调用 `GetWindow` 得到标签子窗口 HWND。缓存用两个键关联：COM 对象和标签 HWND。

这里 `InternetExplorer` 是 SHDocVw 自动化接口类型名，不是在运行旧 IE 浏览器。

`GetActiveTabHandle`（第 736 行）将 `FindWindowEx(parent, 0, "ShellTabWindowClass", null)` 找到的第一个子窗口视作当前标签。源码注释假设活动标签在 Z 顺序最前面。这个规则可用作候选方案，但不是已公开的活动标签 API，必须在目标版本验证。

TabTail 的 `src/platform/shell_tabs.cpp` 已采用类似的 `IShellWindows → IWebBrowser2 → IServiceProvider → IShellBrowser::GetWindow` 路线，并增加了用户会话／进程身份和父子窗口验证。参考项目的 COM 枚举不是我们此前完全没有的能力。

重要差别是：TabTail 还使用原生清单补齐 Shell 自动化集合遗漏的标签。参考项目主要依赖 `ShellWindows` 的枚举与注册事件；源码没有提供解决“所有真实标签均已注册到 Shell COM 集合”的通用补齐机制。因此不能据此认定我们之前遇到的扫描不完整问题已经有免适配解法。

### 3.2 新建、选择、关闭标签

| 操作 | 实现 | 性质及边界 |
| --- | --- | --- |
| 新建标签 | `RequestToOpenNewTab` 第 169 行，向 `ShellTabWindowClass` 投递 `WM_COMMAND / 0xA21B` | 内部命令编号，对应 Ctrl+T；不是稳定公开的标签 API |
| 按位置选择 | `SelectTabByIndex` 第 164 行，向顶层窗口发送 `WM_COMMAND / 0xA221`，`lParam = index + 1` | 内部命令编号；需要实际验证目标是否激活 |
| 按句柄选择 | `SelectTabByHandle` 第 141 行，逐个位置尝试，再检查活动标签 HWND | 可见切换与异步等待，不直接读取内部标签 GUID |
| 分离当前标签 | `DetachCurrentTab` 第 275 行，记录位置和选择项，发送 `0xA021` 关闭，再新开窗口 | 实质是关闭后重建；并非原生完整移动标签 |

这些操作不需要知道 `ExplorerFrame.dll` 中某个函数位于哪个 RVA，因而比固定地址更容易跨更新工作；但窗口类名、消息编号和参数语义仍然可能改变。

### 3.3 在新标签中打开路径

`OpenTabNavigateWithSelection`（第 590 行）的主要流程：

1. 用 `SemaphoreSlim` 串行化新建操作。
2. 根据目录身份查找是否可以复用已有标签。
3. 记录目标窗口当前的标签 HWND 集合。
4. 投递新建标签命令。
5. 最多等待约 2 秒，比较集合差异取得新标签 HWND。
6. 等待取得对应的 COM 对象，再调用 `Navigate2`。
7. 通过 `NavigateComplete2` 处理选择项恢复，最多等待约 5 秒。

无可用窗口时，以 Shell 自动化的 `ShellExecute(..., "opennewwindow")` 新开。`Navigate` 对路径包含 `#`／`%23` 的情况另走 `Shell.NameSpace` 对象导航；虚拟目录在 `LocationURL` 不可用时从 `ShellFolderView.Folder.Self.Path` 读取。

可借鉴的是串行化、异步等待、PIDL 目录身份比较以及特殊目录处理；集合差异识别新标签仍需应对用户同时开关标签造成的歧义。

### 3.4 窗口自动转标签

这不是修改 Explorer 默认创建逻辑。它订阅新窗口注册与 `EVENT_OBJECT_SHOW`，必要时先把新窗口透明化或移出屏幕，再在目标窗口重建路径，最后 `Quit` 原窗口。

`Helper.HideWindow` 第 385 行提供两种隐藏方式，`HaveThemeIssue` 设置用于选择替代路径；恢复显示放在 `finally` 中并带重试。`OnShellWindowRegistered` 还对 Ctrl+Shift、控制面板和最近关闭／分离的窗口作例外处理。

所以“自动合并窗口”具有闪烁、主题、焦点、选中项和失败恢复等额外成本。它不应作为普通命令系统上线的前置条件。

### 3.5 生命周期与线程

- Explorer 进程退出后释放 Shell 对象、缓存和事件订阅，再以约 1 秒周期寻找新的主 Explorer 并重新初始化。
- 已有窗口枚举和新窗口事件配合，避免只处理程序启动后新建的窗口。
- 部分 COM 工作提交给单独 STA 调度器，界面事件通过同步上下文投递，快捷键动作在线程池执行。
- `StaTaskScheduler` 的循环是任务消费循环，没有显式 Windows 消息泵；并非所有 COM 调用都集中于该线程。迁移到 C++ 时应明确 COM 对象所属 apartment、跨线程封送及事件接收方式，不能只复制“建一个 STA 线程”的表面结构。

## 4. 快捷键设计与用户当前设想的对应关系

参考项目已经实现两种作用范围：`Global` 和 `FileExplorer`。

`Keyboard.LowLevelKeyboardHook_Down` 的顺序是：过滤禁用／空绑定 → 匹配按键 → 必要时检查前台是否为 Explorer → 决定是否吞键 → 线程池执行动作。前台判断在一次按键处理中最多执行一次，实际操作不在按键回调内执行。

前台判断仅检查 `GetForegroundWindow` 对应的类名是否为 `CabinetWClass`。它没有在该判断中验证 Explorer 进程身份，也没有区分文件列表、搜索框、地址栏和重命名输入框。

### 配置模型

| 字段 | 含义 | TabTail 建议 |
| --- | --- | --- |
| Name / Action | 用户命名和操作类型 | 采用稳定命令 ID，显示名支持中英文 |
| HotKeys / Scope | 按键与作用范围 | 保留全局／Explorer 前台两类 |
| Path | Open 参数 | 改为按命令类型校验的参数 |
| IsEnabled | 是否启用 | 保留 |
| IsHandled | 阻止或允许按键继续传播 | 普通操作默认独占；透传作为高级选项，提示可能执行两次操作 |
| IsAsTab | 新标签或新窗口 | 仅在适用命令上显示 |
| Delay | 执行前延迟 | 首期不作为核心选项；目标确认和事件等待优先于固定延迟 |
| IsMouse / IsDoubleClick | 鼠标及双击触发 | 可以后续扩展，不阻塞键盘首期 |

该项目 UI 会按作用范围限制动作列表。全局范围没有提供 Duplicate、Detach、前进后退等全部动作；这是值得借鉴的“命令声明适用范围”设计。

快捷键录入时会暂停原键鼠绑定，避免录入动作本身触发执行。TabTail 应继续保持录入与运行状态隔离，同时保留已有的独立输入线程和低开销按键回调。

### 不宜直接照搬的地方

1. 键盘回调对所有匹配项都继续遍历，没有首个匹配后退出；全局与 Explorer 前台重叠绑定可能执行多个动作，`IsHandled` 还可能被后面的项覆盖。`ProfileManager.SaveProfiles` 没有进行绑定冲突校验。TabTail 应明确局部优先、同范围冲突拒绝或由用户显式解决。
2. 部分动作在线程池或延迟后执行。模拟 Alt+Up、窗口贴靠等路径没有统一的“仍是触发时窗口”复查机制。TabTail 应冻结目标身份，执行前再检查前台、进程创建时间和标签存活状态，超时丢弃。
3. 源码的业务层没有统一可见的按键自动重复、注入事件和队列容量策略；H.Hooks 的具体内部行为不在本副本中，不能据此断言依赖库完全没有处理。TabTail 已有这些防护，不应迁移时丢失。
4. 判断 Explorer 前台不等于判断可以在输入期间执行。新的局部命令应提供输入焦点保护，特别是普通字母、单键或容易影响文本编辑的绑定。
5. 不少异常被空 `catch` 吞掉。TabTail 应保留请求失败、部分成功、已请求但未确认等不同结果，在 TUI／通知中可查看原因。

## 5. 为什么 TabTail 现在要求精确版本

| TabTail 层次 | 当前实现与限制 | 参考项目能否直接解决 |
| --- | --- | --- |
| 配置、规则、格式化、TUI | 大部分是本地数据处理 | 不需要 Explorer 内部版本适配 |
| Shell 标签观察 | COM 枚举，加父子 HWND、进程和会话验证 | 基本路线已有；它未给出所有遗漏标签的通用补齐方式 |
| 普通导航、开路径、复制／选中项读取 | 部分已有公开 Shell 路线，部分当前仍经原生动作 | 可新增 COM／消息后端，逐项验证 |
| 标题改写与完整原生清单 | `src/hook/native.hpp` 固定 ExplorerFrame SHA-256、vtable RVA；`native.cpp` 验证模块 PE／PDB，读取 frame 标签表与 host 布局；`dll.cpp` Hook 标题入口 | 未实现相同功能，不能代替 |
| 固定／保护／锁定 | `features_dll.cpp` Hook 关闭、移除、导航内部入口，并验证入口字节 | 未实现相同保护；只拦 Ctrl+W 无法覆盖鼠标和系统关闭路径 |
| 图标／文本／背景 | `style_dll.cpp` 经 XAML 诊断接口定位标签，GUID 映射还依赖 ViewModel 内部布局与另外两个模块哈希 | 未实现这些功能 |
| 宽度与排序 | 宽度控制器仍有原生适配检查；排序还涉及原生接口及 XAML 顺序读回 | 未提供可直接替代的通用方案 |

当前 ExplorerFrame 适配标识为 **26100.8457 / exact SHA256**。核对的哈希是 `4e28d2076608df5525ec1b7b2ed8dc0e866f43bf2ded6a5e4213ccd1c2284cbe`，不是仅比较 Windows 主版本或“26100”即可使用。

外观定位还有额外限制：`style_dll.cpp::verify_style_build` 检查 `FileExplorerExtensions.dll` 与 `Windows.UI.FileExplorer.dll`，`context_guid` 使用固定 vtable 项、RVA 和字段偏移。因此只扩充 ExplorerFrame 的哈希也不等于整套外观支持新版本。

TabTail 目前存在可以拆开的耦合：

- `runtime.cpp` 的 `enable` 一次性以 `native_build_supported()` 拦住定制启用。
- `TabFeatures::change` 在分派 `tabs.locate` 前就取得 `NativeTab`，使普通定位也依赖原生适配。
- 目录入口复用已有标签、向当前窗口开新标签仍调用原生动作；无前台 Explorer 时的新开窗口则走另一条路线。
- `ExplorerShortcuts` 的窗口清单和当前标签 GUID 读取依赖 `NativeTab`，所以现有局部快捷键后端不宜原样承担免适配命令。

新命令系统应拆除这些普通功能对原生定制的依赖，而不是允许不受支持的内部偏移继续执行。

## 6. “广泛兼容”仍有哪些边界

1. **要求标签功能存在**：Windows 11 早期无标签版本不属于其声明范围；也不能仅看主版本号。
2. **消息不是公开契约**：`0xA21B / 0xA221 / 0xA021` 和活动子窗口 Z 顺序都需要能力验证。
3. **未证明跨版本全覆盖**：本地发布流程在 `windows-latest` 构建不同框架与 x64／x86／arm64 包，没有看到跨 Explorer 版本的自动 UI 验收矩阵。构建产物支持某架构不等于所有功能已经在该架构真机验收。
4. **进程边界与权限仍存在**：窗口消息、模拟输入与 COM 可能因权限、会话、忙碌或重启而失败。不能从参考项目免 DLL 注入推导“管理员与普通 Explorer 总能互相操作”。
5. **同步消息可能阻塞**：部分位置使用 `SendMessage`，C++ 后端宜考虑有界等待和 UI 线程隔离。
6. **重建不保留全部状态**：复制／分离主要保留位置与选中项名称，不等价于保留完整导航历史、滚动位置及应用内部状态；分离代码还先关闭再打开，后续打开失败会影响原标签。
7. **搜索按目录去重**：`GetWindows` 把打开和关闭记录合并后按 `Location` 分组，不能直接充当精确到每个重复目录标签的身份列表。

## 7. 对 TabTail 的改造建议与顺序

### 阶段 A：先建立免内部适配的命令底座

将快捷键触发与命令执行拆开。CLI、TUI 按钮和快捷键调用同一命令入口；每条命令声明参数、允许作用范围、所需能力与确认方式。

建议的能力项如 `shell_observe`、`shell_navigate`、`tab_open_message`、`tab_activate_message`、`native_title`、`native_guard`、`xaml_style`。能力既包含后端支持，也包含目标窗口当前是否可用；版本号仅用于诊断与选择候选适配器。

普通命令不要求用户先启用标题定制。可先做：打开路径、前进／后退／上级、读取当前目录或选中项路径并复制、在当前文件系统目录打开终端。这里复制路径、打开终端等是 TabTail 的候选新增功能，不是参考项目已经提供的完整命令清单。

全局绑定可继续利用已有 `RegisterHotKey` 路线；Explorer 前台绑定扩展现有独立输入线程，但普通目标上下文使用已验证的 HWND／COM 身份，不强求原生 GUID。消息后端须标记其未公开性质。

所有键盘回调只做匹配、快速上下文筛选与有界入队。COM、剪贴板、文件系统、进程启动、模拟输入和通知均在回调外执行。

### 阶段 B：普通标签操作按目标能力逐步迁移

为“新标签打开”“激活／复用已有标签”“复制当前标签”新增 COM／消息后端。先在单独测试窗口验收，再作为可选后端使用；保留精确原生后端用于已经验证的版本。

不把消息投递成功当作完成。新建应确认唯一的新标签身份和实际路径，激活应确认目标标签，不应仅检查窗口到了前台。出现歧义、前台变化或超时要保留原标签，不盲目重复发送造成额外标签。

分离与自动窗口转标签放在后续：先创建并确认新目标，再决定是否关闭旧目标；要与固定、独立保护及目录锁定策略协调。

### 阶段 C：保留并扩展高级版本适配

将固定常量整理为“模块身份 → 可用能力 → 地址与结构描述”的适配记录。新增版本分别验证标题、保护、布局、外观模块，不承诺单个版本号覆盖全部能力。

后续可以研究符号辅助定位、结构特征或更稳定的 XAML 目标映射，但这些是独立工作，参考项目没有提供现成实现。发现地址只是候选，仍需身份检查、边界验证、真实行为验收和恢复验证。

TUI 建议按功能显示“可用／实验／不可用＋原因”，例如新系统上普通命令可用而图标替换尚无适配；配置仍可编辑。避免一个笼统的“不支持当前版本”使用户误认为整套工具都无法使用。

## 8. 建议的验收矩阵

以下是待执行的验收计划，不代表已经支持这些版本：

| 维度 | 场景 |
| --- | --- |
| 系统与架构 | 至少选有标签的 22H2／23H2／24H2／25H2 代表环境，记录实际 Explorer 模块版本；x64 优先，ARM64 独立验收 |
| 程序启动时机 | 空窗口、预先打开多窗口多标签、代理运行后新开、Explorer 重启后重新连接 |
| 目标身份 | 多窗口同目录、同窗口重复目录、超过 9 个标签、执行中切换前台／关闭目标 |
| 目录 | 本地、UNC、离线网络、中文、空格、`#`、`%`、虚拟目录 |
| 输入 | 地址栏、搜索框、重命名、中文输入法、持续输入、长按、注入事件、修饰键释放 |
| 冲突 | 同范围重复、全局和局部同组合、其他程序已绑定、录入期间暂停运行绑定 |
| 失败恢复 | COM 暂忙、消息超时、窗口重建、权限差异、部分失败与过期请求 |
| 高级功能边界 | 普通命令在无原生适配时仍可运行；不触发内存读写；已适配版本原有标题／图标／保护／恢复无回归 |

## 9. 迁移注意事项

参考项目为 MIT 许可证；本地 LICENSE 的版权占位符尚未填写。如果直接移植代码，保留原许可与相关声明，记录具体来源和改写范围；不能因换成 C++ 就忽略许可。仅借鉴算法也应在调研与实现说明中保留来源。

本次只新增调研文档，不修改运行行为。推荐下一步先定义命令模型、作用范围和能力报告，再实现普通命令与标签消息后端；高级标签外观继续保留精确适配。
