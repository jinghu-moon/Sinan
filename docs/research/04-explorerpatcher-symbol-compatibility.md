# ExplorerPatcher 深度调研：符号、特征码与多版本兼容

日期：2026-11-18。主对象：本地 `refer/ExplorerPatcher-master`。`version.h` 为 **26100.9711.71.1，prerelease**；不据目录名推定上游最新版本。

对照：TabTail 0.9.0。调研包含静态源码分析和独立 Python 进程中的只读符号实验，没有安装或运行 ExplorerPatcher，没有附加到 Explorer、注入 DLL、修改注册表、重启 Explorer 或替换现有代理。

## 1. 结论

用户对符号机制的判断有直接源码依据：ExplorerPatcher 按当前系统模块获取对应 PDB，按函数名解析 RVA，缓存结果，系统文件改变后重新解析。这条路线适合 TabTail，且本次已经核验部分目标符号能够实际解析。

但它的多版本兼容由多种机制共同完成：

1. **符号定位**：从当前模块对应的 PDB 获得函数 RVA。
2. **文件身份与缓存版本**：按模块文件哈希及符号清单版本判断缓存能否复用。
3. **特征码与指令解析**：找不到符号或没有适用符号时，按代码形态找到函数、调用目标、字段偏移或补丁点。
4. **版本／架构分支和替代实现**：对行为或接口发生变化的版本采用不同路径，某些旧组件已消失时引入重实现。
5. **局部降级与恢复**：缺少某项能力时跳过对应功能；另有重复崩溃保护。

**符号能解决很多“函数地址移动”的问题；不能自动证明函数 ABI、对象布局及功能行为仍然兼容。**

TabTail 的下一步不必停留在扩充静态地址表，可以先建设只读符号解析器；高级功能开放仍须依赖结构、接口和行为验证。

## 2. 源码范围与依赖来源

ExplorerPatcher 不是纯 C 工程：符号管理等主体使用 C，`TwinUIPatches.cpp`、`Taskbar10.cpp`、`StartExperience.cpp` 等使用 C++，工程设置包含 C++20。C++ TabTail 可以采用相同底层能力，无需为符号解析改用 C。

| 位置 | 用途 |
| --- | --- |
| [symbols.h](../../refer/ExplorerPatcher-master/ExplorerPatcher/symbols.h) | 模块／符号清单、清单版本、RVA 数组、下载状态 |
| [symbols.c](../../refer/ExplorerPatcher-master/ExplorerPatcher/symbols.c) | 符号下载协调、按模块写缓存、读取缓存和失效处理 |
| [dllmain.c](../../refer/ExplorerPatcher-master/ExplorerPatcher/dllmain.c) | 初始化、下载线程、Explorer 特征码回退、注入与崩溃保护 |
| [TwinUIPatches.cpp](../../refer/ExplorerPatcher-master/ExplorerPatcher/TwinUIPatches.cpp) | 特征码回退、字段偏移提取、版本与架构分支 |
| [utility.c](../../refer/ExplorerPatcher-master/ExplorerPatcher/utility.c) | 文件哈希等工具 |
| [utility.h](../../refer/ExplorerPatcher-master/ExplorerPatcher/utility.h) | 扫描辅助、指令解析、系统分支和 taskbar DLL 选择 |
| [CHANGELOG](../../refer/ExplorerPatcher-master/CHANGELOG.md) | 已测试系统清单、特征码维护、功能移除／重实现记录 |
| [.gitmodules](../../refer/ExplorerPatcher-master/.gitmodules) | 确认符号实现来自 libvalinet 子模块 |

本地 `libs/libvalinet` 子模块为空。为追踪 `VnDownloadSymbols`／`VnGetSymbols` 的实现，本次另行读取上游 libvalinet：

- 仓库：<https://github.com/valinet/libvalinet>。
- 调研提交：`797404f1e343eaccfeabcd3de6d66f6820732b91`。
- [对应 PDB 实现](https://github.com/valinet/libvalinet/blob/797404f1e343eaccfeabcd3de6d66f6820732b91/valinet/pdb/pdb.h)。
- 本地研究副本：`build/research/ep-libvalinet`。

这是补充证据，不能声称已还原本地 ExplorerPatcher 压缩包原本锁定的子模块提交。ExplorerPatcher 本地调用接口与该实现一致。

关键本地文件 SHA-256：

```text
ExplorerPatcher/symbols.c
3ED8EB1115114DE9772C850D59E9EA4E969F3AF78647E43A8CAA5EAFB245A731
ExplorerPatcher/TwinUIPatches.cpp
F3BA5D4E2215C91EEFD63CABB3385173997E3B7B9A2D0394D8E2CDB56DA566D2
version.h
16D26962D9CD26205329D11DE3E099E96ADE29659F188997BAC2D5BE2B512C69
```

## 3. 符号机制的实际流程

### 3.1 先定义需要找的名称

`symbols.h` 为以下四个模块定义符号清单及版本：

| 模块 | 清单规模 | 示例 |
| --- | --- | --- |
| explorer.exe | 6 项，清单版本 3 | `ImmersiveTray::AttachWindowToTray`、`TrayUI::_UpdatePearlSize` |
| twinui.pcshell.dll | 7 项，清单版本 2 | 上下文菜单、Win+X 命令、多任务视图入口 |
| StartDocked.dll | 5 项，清单版本 1 | `StartDocked::LauncherFrame::ShowAllApps` 等 |
| StartUI.dll | 1 项，清单版本 1 | `StartUI::SystemListPolicyProvider::GetMaximumFrequentApps` |

这些是其符号下载表的目标，不是 TabTail 所需 `ExplorerFrame.dll`、`FileExplorerExtensions.dll` 和 `Windows.UI.FileExplorer.dll` 的现成适配清单。

符号列表包含 C++ 类／方法名。解析依赖发布的符号名称，不要求拿到微软源代码；但符号可能被删除、改名、内联或产生歧义。

### 3.2 从 PE 文件取得匹配 PDB 的身份

libvalinet 的 `VnDownloadSymbols` 读取目标 PE 的调试目录，查找 CodeView 的 `RSDS` 记录，获得 PDB 文件名、GUID 和 Age，构建微软符号服务器路径：

```text
msdl.microsoft.com/download/symbols/<PDB 文件名>/<GUID + Age>/<PDB 文件名>
```

因此选择依据是具体模块携带的 PDB 身份，不是“Windows 11 24H2 共用一个 PDB”。不同累积更新可能对应不同符号文件。

研究副本通过 WinINet 下载，使用 HTTP 端口；TabTail 若独立实现应使用 HTTPS、下载临时文件和验证后的原子提交。此处是原实现行为与迁移建议的区别，不能把下载成功视为身份校验已完成。

### 3.3 用 DbgHelp 解析名称为 RVA

libvalinet 的 `VnGetSymbols` 流程：

1. 设置 DbgHelp 选项，调用 `SymInitialize`。
2. 用 `SymLoadModuleEx` 把 PDB 加载到一个人为选择的基址 `0x400000`。
3. 对每个名称调用 `SymEnumSymbols`。
4. 要求每次搜索恰好增加一个结果，否则失败。
5. 保存 `symbol_address - 0x400000`，即 RVA。
6. 卸载符号并清理。

运行时使用 `actual_module_base + RVA` 取得函数地址。因此 ASLR 不要求重新计算磁盘缓存。

它没有通过这个流程自动恢复完整类型系统或成员布局。`SymEnumSymbols` 的名称还具有匹配语义；重载、模板和符号命名变化要单独处理，TabTail 宜保存完整目标名称并验证唯一性。

### 3.4 缓存的关键：哈希是身份键，不是单版本白名单

`LoadSymbols` 从 HKCU 的各模块子键读取 `Hash`、`Version` 和 DWORD 类型的 RVA。只有当前文件哈希和缓存清单版本都匹配时才复用；否则标记需要下载，删除过期模块缓存。

`ComputeFileHash` 当前用的是 **MD5**。它用于判断文件是否变化；TabTail 已有 SHA-256，可以继续使用，不必复制 MD5。

这与 TabTail 当前的核心差异是：

| 设计 | 文件哈希的用途 |
| --- | --- |
| TabTail 当前原生适配 | 只接受源码写死的一个哈希，然后使用该版本的常量 |
| ExplorerPatcher 符号缓存 | 当前文件没有匹配缓存时，尝试为这个文件重新解析符号并生成缓存 |
| TabTail 建议 | 哈希绑定解析结果，结构／ABI 验证另行决定哪些功能可启用 |

哈希校验可以保留，单版本限制来自静态适配结果的覆盖范围，而不是哈希算法本身。

### 3.5 异步下载和部分成功

`dllmain.c` 第 11351 行调用 `LoadSymbols`；发现缺项且开启下载后，创建后台线程。`DownloadSymbols` 延迟约 6 秒后开始，按模块独立调用解析函数并保存结果，用 `bAnySuccess`／`bAllSuccess` 区分通知。

`LoadSymbolsResult.bSuccess` 表示加载流程状态，不代表所有目标符号齐全，必须结合四个下载标志判断。某个模块失败不会撤销另一个模块已经写入的缓存。

`ProcessExplorerSymbols` 还有针对较新系统的缩小清单回退：完整清单失败时尝试只解析 `SetColorPreferenceForLogonUI`。因此“模块缓存存在”也不等价于该模块每个功能均可用。

本地 `DownloadSymbols` 代码负责写缓存与通知，未看到它直接把新 RVA 替换进已经运行的 Hook。不能泛化为“下载完成就能无重启热切换全部补丁”。

## 4. 多版本兼容的其他支柱

### 4.1 特征码回退

`TryToFindExplorerOffsets`（dllmain 第 10288 行）在某些 RVA 缺失或为 `0xFFFFFFFF` 时扫描代码。比如 `HandleFirstTimeLegacy` 分短跳转／长跳转两种形态，找到 `CALL rel32` 后计算实际目标，再换算 RVA。

`TryToFindTwinuiPCShellOffsets`（TwinUIPatches 第 3308 行）先读磁盘文件的代码节，以避免其他定制软件改动内存入口影响扫描。它为 x64 与 ARM64 分别维护特征；ARM64 需要按指令对齐，并解析 BL／其他指令。

磁盘文件偏移、RVA 和运行时 VA 不是同一概念。该项目包含 `FileOffsetToRVA` 等辅助；TabTail 的实现应显式区分，不能把任意磁盘扫描结果直接加到模块基址。

这些模式不是“自动理解所有 Windows 实现”。代码和 CHANGELOG 中有持续增加新模式的记录，说明它们也需要维护。

### 4.2 字段偏移提取

TwinUIPatches 第 1995 行附近，先匹配构造函数的指令，再从指令位移中读取 `SingleViewShellExperience` 的成员偏移。动画对象偏移也采用类似方法。另一些偏移仍按版本分支选择，例如第 573 行按 Build／UBR 调整类内接口位置。

这正对应 TabTail 的第二类难题：公开符号能给函数／vtable 地址，但成员在哪里、接口指针如何调整，仍可能需要指令分析或已验证的布局描述。

### 4.3 针对不同版本选不同实现

`PickTaskbarDll`（utility.h 第 1711 行）按系统范围选择 `ep_taskbar.rs2.dll`、`fe`、`co`、`ni`、`zn`、`ge` 等不同实现。

CHANGELOG 还记录系统移除旧开始菜单的 tile layout engine 后，先隐藏相应选项，再引入重实现。这表明有些兼容工作必须重写功能；符号服务器不能找回已经被系统删除的代码。

README 明确写着部分功能可能在某些 Windows 版本不可用。CHANGELOG 给出具体已测试构建列表，这种逐项验收比宣称“所有版本”更有参考价值。

### 4.4 降级与崩溃恢复

`RunTwinUIPCShellPatches` 逐项检查 RVA 再安装对应功能；替代 taskbar 初始化失败时保留其他可用功能。CHANGELOG 中也有接口不兼容时退回系统 taskbar 的记录。

`CrashCounterHandleEntryPoint` 维护短时间内启动计数；达到阈值或按 Ctrl+Shift+Alt 时，阻止走通常的完整注入路径，以脱离重复崩溃。

不要只看函数名推断验证力度：本地 `CheckExplorerSymbols` 的完整检查循环已被注释，实际直接返回真；`LoadSymbols` 对若干注册表值的读取也没有逐项验证返回值。TabTail 宜实现明确的按能力校验，而非照搬这些宽松判断。

## 5. TabTail 的只读符号实测

证据：[04-symbol-resolution.json](../evidence/04-symbol-resolution.json)。本次使用独立 64 位 Python 进程、系统 DbgHelp 和已有本地 PDB，对真实模块加载符号；启用 `SYMOPT_EXACT_SYMBOLS`，没有使用 `SYMOPT_LOAD_ANYTHING`，核对 `PdbUnmatched=false` 及实际加载的 PDB 路径。

还读取了 PE RSDS、PDB GUID 和 DBI Age；另外按 PE 的符号服务器地址重新下载一份 ExplorerFrame.pdb，其 SHA-256 与已有文件一致。公开 PDB 的 Info stream Age 与 DBI stream Age 在本样本中不同，证据分别保存两者，匹配结果以模块身份及 DbgHelp 实际加载校验共同确认，不简单比较任意一个叫 Age 的字段。

| 当前模块 | 符号 | 解析 RVA | 与现有常量 |
| --- | --- | --- | --- |
| ExplorerFrame.dll | `CExplorerFrame::SetFrameTitle` | `0x42cc0` | 一致 |
| ExplorerFrame.dll | `CBrowserHost::GetLocationItem` | `0xe7040` | 一致 |
| ExplorerFrame.dll | `CExplorerFrame::RemoveTab` | `0x1a6be0` | 一致 |
| ExplorerFrame.dll | `CBrowserHost::OnClose` | `0x125da4` | 一致 |
| ExplorerFrame.dll | `CShellBrowser::_NavigateToPidl` | `0x137b08` | 一致 |
| ExplorerFrame.dll | `CExplorerFrame` 的 `CImpWndProc` vtable | `0x24d2e0` | 一致 |
| ExplorerFrame.dll | `CBrowserHost` 的 `DWebBrowserEvents2` vtable | `0x241958` | 一致 |
| ExplorerFrame.dll | `CBrowserHost` 的 `IBrowserHost` vtable | `0x241858` | 一致 |
| FileExplorerExtensions.dll | `FileExplorerTabItemControlVM` 接口访问器 `get_FileExplorerTab` | `0xee230` | 一致 |

9 个目标全部解析成功。证据保留完整修饰名，表格仅简写便于阅读。

两个模块均报告 `Publics=true`、`TypeInfo=false`、`GlobalSymbols=false`。这支持“公共函数／vtable 可以定位”的结论，不支持“完整成员字段布局已自动获得”的结论。

实验只查询地址，不调用这些内部函数，不安装 Hook；验证的是当前已适配样本上的符号可用性，不是新 Windows 版本的功能验收。

研究脚本位于 `build/research/ep-symbol-proof.py`，依赖本地已有 PDB 和历史名称映射文件；不是已接入产品的解析模块。脚本中没有 Explorer 进程句柄获取或内存写入逻辑。

## 6. TabTail 还需要解决什么

### 可以直接推进的部分

将 `native.hpp` 的函数／vtable RVA 和 `features_dll.cpp` 的关闭／移除／导航入口改为解析结果，是有实测依据的工作。外观访问器也存在至少一个可解析样本。

### 不能只替换 RVA 的部分

| 当前依赖 | 为什么仍需单独处理 |
| --- | --- |
| frame 标签表 `+0x2f0`，40 字节条目 | 是成员与容器布局，不是函数地址 |
| 当前标签 GUID `+0x318` | 现有快捷键目标识别依赖该字段位置 |
| host 内接口／窗口／browser 位置 | 即使找到 vtable，指针调整和字段仍可能改变 |
| `remove_detour` 的 `this - 0xe8` | 是类内接口位置假设，符号命中不证明它稳定 |
| XAML vtable 槽位 12、GUID `+0x78` | 仍依赖接口和对象布局 |
| 内部函数签名及语义 | 同名函数在新版本可能改变参数、对象语义或执行线程要求 |
| XAML 树与模板 | 控件类型、属性及目标映射可能随 Explorer 前端更新变化 |

`Windows.UI.FileExplorer.dll` 的剩余目标未在本次进行新符号解析验证；不能把一个 ViewModel 访问器的成功扩大为整套图标／背景／排序已解决。

现有 PE SizeOfImage、PDB 身份、入口字节和哈希检查也要改为跟随对应模块的描述与结果，不能只删掉一个 `native_build_supported()` 检查。

## 7. 推荐实施路径

### 阶段一：只读解析与能力报告

先做独立的解析器／诊断命令，不安装 Hook：读取当前模块身份 → 查缓存 → 按需下载 PDB → DbgHelp 解析目标 → 检查结果唯一、RVA 范围、代码／只读数据节和磁盘／加载模块一致性 → 输出能力报告。

建议缓存字段：架构、模块 SHA-256、PDB 身份、解析器版本、符号清单版本、完整符号名、RVA、结果来源及验证状态。不要把运行时绝对地址持久化；用原子写入和互斥避免多个进程写坏缓存。

DbgHelp 操作应集中到隔离的工作进程或串行任务中，不放进 Explorer UI 线程、键盘回调或 TUI 绘制路径。下载需要取消、超时及离线状态；失败时保留已有仍有效的能力。

微软 [SymFromName 文档](https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-symfromname) 明确要求同步并发 DbgHelp 调用；[SymSetOptions 文档](https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-symsetoptions) 说明 `SYMOPT_EXACT_SYMBOLS` 拒绝不匹配 PDB，`SYMOPT_LOAD_ANYTHING` 则会禁用身份检查。本次实验使用前者，没有使用后者。

### 阶段二：替换当前版本的地址来源

首先只在现有已验证模块上，将固定 RVA 替换为动态结果，保留原结构描述和行为。这样可以验证解析、缓存、失效与回退机制，又不会把“解析器改造”和“新版本布局适配”混为一次变更。

标题、保护、外观和排序分组列出依赖；允许模块内部分符号失败，但不得把缺少必需依赖的能力标记为可用。

### 阶段三：扩展布局族与新版本验收

建立布局描述，例如旧布局与新布局族。优先通过稳定接口或具有明确锚点的指令分析减少硬编码；无法可靠确定的字段继续要求已验证适配。

如果使用特征码，要求唯一匹配、正确指令边界和控制流／引用交叉验证。不能把“第一段字节看起来像”直接升级为启用条件。磁盘与内存入口不一致时，应报告冲突，避免覆盖别的工具的 Hook。

对每个新目标版本验证：标签身份映射、单独标题、所有关闭路径、锁定导航、图标读回、主题／DPI、宽度、顺序和停止／租约恢复。出现歧义只禁用对应能力。

### 与普通命令系统的关系

前一份 [ExplorerTabUtility 调研](03-explorertabutility-compatibility.md) 仍适用于公开 Shell／窗口消息层。两条路线可以并行存在：普通命令尽量免原生结构依赖，高级定制用符号与布局适配。

TUI 可展示“无需原生适配／正在准备符号／符号不可用／布局未验证／能力可用”等状态。下载与解析本身不重启 Explorer；是否需要重启由具体已安装模块和启用策略决定。

## 8. 迁移边界与后续验收

- 本地 ExplorerPatcher 与补充 libvalinet 均包含 GPLv2 许可证文本，TabTail 当前为 MIT。借鉴机制与复制实现应区分；若维持现有许可路线，可依据公开接口独立实现，不能直接把相应 GPL 代码当作 MIT 代码移入。
- 本次没有获取其他 Windows 构建的模块样本，也没有证明跨版本 ABI／布局不变。新增支持范围须记录具体模块身份、架构和实际验收结果。
- 除成功路径外，需要验证符号未发布、断网、缓存过期／损坏、下载中断、文件更新、符号不存在／多义、布局未知、已有 Hook 和解析进程失败。
- 本次新增调研文档与只读证据，不更改 TabTail 发布版本或运行路径。

建议优先做符号解析器的只读阶段。现有 9 项解析成功已足以支持这项投入，同时把成员布局验证作为后续明确任务保留下来。

## 0.11.0 后续实施：从命名入口推导字段

本轮继续阅读 `dllmain.c` 的 `TryToFindExplorerOffsets`（约 10288 行）与 `TwinUIPatches.cpp` 约 1915–1980 行：其兼容工作不仅寻找符号，还利用稳定代码引用、相对地址、vtable 写入及版本分支恢复内部偏移。TabTail 独立实现相应思路，未复制源码或采用该项目的私有结构声明。

具体落地是 `src/compat/feature_contracts.cpp` 中的有界指令契约，以及三个模块独立的清单、缓存、失败原因和运行期检查。构造函数中命名 vtable 的安装确认接口距离；SelectTab 与命名查找函数的调用确认 active GUID；MoveSelectedTabToPosition 引用的 service／IID 确认 provider 字段；XAML 标签构造函数完整 GUID 复制确认接口相对字段。目录锁定改为通过浏览器接口取得对象，避免沿用旧固定浏览器字段。

本轮没有引入不受限的全文件模糊特征扫描。缺失符号、ABI 或布局约定变化时仅关闭有关能力，并保留已验证路径。已完成 29 个本机目标符号的只读验证，尚无其他系统构建的实际交互通过证明。详见 [0.11.0 兼容设计](../05-symbol-compatibility.md) 与 [证据](../evidence/06-native-xaml-compatibility.json)。
