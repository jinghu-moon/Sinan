# 05：符号兼容与逐功能准入（0.11.0）

TabTail 将标题、原生枚举、保护、目录锁定、选择、新建、重排及外观身份从单一文件哈希限制，扩展为“匹配的微软 PDB + RVA + 已验证 x64 布局契约”。内置的精确适配继续作为离线回退。配置、路径预览、普通 COM 观察不需要下载符号。

设计依据见 [ExplorerPatcher 调研](research/04-explorerpatcher-symbol-compatibility.md) 和 [ExplorerTabUtility 调研](research/03-explorertabutility-compatibility.md)。实现依据 Windows 官方 API 独立编写，没有引入 ExplorerPatcher／libvalinet 的 GPL 源码。

## 实现结构

| 组件 | 责任 |
| --- | --- |
| `src/compat/image.cpp` | 有界解析磁盘 PE64、节区、RSDS 和 x64 异常表；区分文件偏移与 RVA；计算 SHA-256 |
| `src/compat/resolver.cpp` | 仅链接到独立的 `tabtail-symbols.exe`，下载 PDB，使用 DbgHelp 精确匹配并枚举符号 |
| `src/compat/profile.cpp` | 布局契约、缓存验证、内置回退与逐功能报告；不链接 DbgHelp／WinHTTP |
| `src/compat/feature_contracts.cpp` | 各项原生行为、XAML 身份和重排的独立契约与字段推导 |
| `src/compat/xaml.cpp` | 两个外观模块的路径、缓存、内置回退和加载模块身份验证 |
| `src/compat/prepare.cpp` | 启动隐藏工作进程，Job 约束生命周期，90 秒超时后终止本次工作进程 |
| `src/hook/native.cpp` | 消费本地配置；核验目标进程、模块、PDB 身份、vtable、窗口归属与标签表稳定性 |

CLI 与 TUI 走同一服务。准备符号不启用定制、不重启 Explorer、不自动停止或替换代理，也不修改 `tabtail.toml`。

## 身份和缓存

运行期共同读取 `%LOCALAPPDATA%\TabTail\cache\symbols-v1`。配置目录可能由 `--data-dir` 改写，但注入 Explorer 的 DLL 无法从配置目录推断调用者，因此符号缓存独立于配置目录。

布局为 `<cache>/<模块 SHA256>/profile.json` 和同目录对应 PDB；三个模块分别加锁、验证和提交。配置同时绑定缓存 schema、架构、映像大小、时间戳、SHA-256、PDB 文件名和 GUID/Age。更新系统后哈希改变，旧缓存不会套用到新 DLL。提交使用临时文件与原子替换；准备操作按模块加文件锁。再次读取系统 DLL 核验哈希后才提交。

`symbols status/prepare --cache-dir <目录>` 可用于隔离验证。这类自定义缓存不被运行期读取；正式准备应省略此选项。缓存校验和用于发现损坏，不是签名或针对同权限恶意修改者的安全边界。运行期会重新校验节区、入口与布局，不直接相信 JSON 中的地址。

微软符号服务器 URL 由 PE 中的 RSDS GUID/Age 生成，域名固定为 `msdl.microsoft.com`，使用 HTTPS，允许 HTTPS CDN 重定向并禁止降级 HTTP。下载有连接／读写超时、70 秒传输期限和 128 MiB 大小上限；父进程另设 90 秒总超时。解析器仅搜索本次本地暂存目录，忽略 `_NT_SYMBOL_PATH`，不隐式访问符号服务器。

DbgHelp 全部调用在工作进程的单线程上执行，开启 `SYMOPT_EXACT_SYMBOLS`，禁止 `LOAD_ANYTHING`；核验 `SymPdb`、`PdbUnmatched`、实际 PDB 路径及 GUID/Age。必须出现唯一的完整修饰符号名，缺失或多义会记录并限制相应能力。这里使用 DbgHelp 接受的 PDB Age，不误把 PDB Info stream 的内部 Age 当作 PE／DBI Age。

## 布局契约 v1

公开 PDB 给出了函数与 vtable 地址，但没有本次所需的完整成员字段类型。符号名称相同不足以证明对象布局相同。

本版支持地址变化、对象布局与入口约定仍满足契约的 AMD64 构建，使用 10 个精确符号：3 个 host vtable、frame vtable、2 个构造函数、标签查找、窗口读取、位置读取及标题设置函数。

准入检查包括：

1. 函数 RVA 位于不可写可执行节，vtable 位于只读数据节。
2. 在构造函数首次调用／跳转前，按 x64 指令边界匹配 RIP 相对 vtable 引用与相邻写入，确认 frame 主接口偏移 0，以及 host 的 0／0x10／0x70 接口位置。命中须唯一。
3. 标签查找函数边界来自 x64 unwind 表；在函数内按指令边界核验标签表起止字段 0x2f0／0x2f8、40 字节步长、host 与 GUID 字段位置。不是全文件任意字节首匹配。
4. host 的窗口 getter 确认相对偏移 0x18，结合 IOleWindow 的 0x70 得到 HWND 字段 0x88；位置读取 thunk 确认接口调整与调用约定，且在 host interface vtable 中唯一出现。
5. 标题入口保留已核验的 ABI／prologue 契约。标题入口未通过时，已通过的只读枚举仍可用。

不匹配的寄存器分配、接口位置、步长、函数形状或架构会拒绝当前契约，并解释原因。它不是任意布局推断器，也不支持 ARM64 内部适配。后续布局族需要独立的明确证据和验收，不能放宽匹配来掩盖未知字段。

## 运行期隔离与能力降级

| 功能 | 本版准入 |
| --- | --- |
| 配置、预览、普通 COM 观察 | 不依赖原生符号 |
| 原生标签枚举、原生位置读取 | 内置精确构建，或符号配置和布局契约通过 |
| 逐标签标题／层级／规则／别名 | 上述条件，加标题符号及入口契约；目标 UI 线程上执行 |
| 固定／关闭保护 | frame 枚举通过，加 RemoveTab／OnClose／actions vtable 独立契约 |
| 目录锁定 | frame 枚举通过，加 Navigate／GetBrowser／浏览器接口及新建能力契约 |
| 标签选择／当前窗口新建标签 | 各自方法、接口、字段与 vtable 槽契约独立准入 |
| 图标、字体、文字及标签背景 | frame 枚举通过，加两个 XAML 模块身份契约及实际属性／图标读回 |
| 固定重排 | frame 选择及服务字段契约，加扩展模块 MoveTabIdToPosition 符号／实时入口及真实顺序读回 |
| 标签宽度 | frame 枚举通过，加 XAML 属性查找和尺寸读回 |

仅允许枚举的 feature bridge 路由单独执行：不读取 active GUID，不安装保护 Hook，不进入重排或导航字段。前台快捷键在缺少行为适配时不读取 active GUID 字段。

每个目标仍验证当前用户／会话／进程创建时间、系统模块路径、内存 PE 和 RSDS、位置函数实时字节、frame／host vtable、子 HWND 归属、非空 GUID、表容量及读前读后稳定性。标题 Hook 另检查实时入口，已有其他 Hook 或入口变化时拒绝覆盖。

已完整准入的配置在进程生命周期内保持不变。部分准入时每 5 秒最多检查一次本地缓存，只允许同一模块身份、原有 RVA／字段完全不变且已有能力不回退的增补；不能在 Hook 安装后切换地址。尚未准入时同样每 5 秒最多重试一次；准备成功会刷新发起命令进程的缓存。其他运行进程可在后续轮询中识别新配置，显式重启 TabTail 代理也可重新读取，但准备操作本身不会执行重启。系统文件更新而 Explorer 仍加载旧模块时，内存身份验证会拒绝操作，不能靠新缓存绕过。

## 验证范围

自动测试覆盖非内置地址的合成 PE、GUID/Age 编码、文件偏移／RVA 区分、unwind 边界、缺失符号、错误节区、表和接口偏移变化、标题单项降级、vtable 多义、缓存损坏／过期、ARM64、截断 PE 和越界输入。集成测试在临时缓存中验证无副作用的状态读取、离线缺失、工作进程错误返回与临时结果清理。TUI 覆盖双语、四字按钮、最小尺寸滚动及三个独立动作。

本机微软 PDB 的联网下载、离线解析、缓存复用和错误 PDB 拒绝已有只读验证，见 [实现证据](evidence/05-symbol-compatibility.json)。未自动接入 Explorer、启用标题 Hook或重启用户代理；其他 Windows 构建的功能与视觉仍需逐构建真机验收。

## 0.11.0：独立行为与 XAML 契约 v2

借鉴 ExplorerPatcher 的“符号定位 + 稳定引用推导字段 + 分项回退”方式独立实现，没有复制其 GPL 代码。保留上述 v1 枚举／标题布局校验，frame 符号从 10 个扩展到 24 个；另增加 view 2 个、ui 3 个，共 29 个。磁盘缓存仍是 schema 1，`contract_revision` 升为 2，旧 PDB 可重新解析。

- actions 接口位置从 frame 构造函数的命名 vtable 写入提取，并与 SelectTab／RemoveTab 中的表引用交叉核验。
- active GUID 从 SelectTab 调用 GetBrowserTabFromId 的输出记录推导；缺少选择能力时不读取该字段，并报告 `active_known: false`。
- 目录锁定通过已核验 IBrowserHost::GetBrowser 获取 IShellBrowser，再由构造函数推导接口到浏览器对象基址的距离，核对两个实际 vtable。无需读取旧 host+0xa0 字段。
- 重排从 MoveSelectedTabToPosition 对私有 service／IID 符号的引用提取 provider 字段，并检查方法槽和 active GUID 引用；实际 QueryService 返回的方法必须匹配扩展模块符号和实时入口。
- FileExplorerExtensions 的 getter 必须唯一出现在对应 ViewModel vtable 中，槽位可以变化。
- Windows.UI.FileExplorer 的标签接口位置与完整 GUID 复制序列（包括来源字段和目标连续性）必须通过，GUID 相对接口偏移从序列推导；MoveTabIdToPosition 独立准入，不与外观身份互相绑定。

各组按指令边界在有界函数／构造函数的首次控制转移前验证，使用精确修饰符号、只读／执行节、唯一命中、字段范围和对齐检查。仍保留 v1 的标签表布局、host 接口、位置 thunk 约定和各功能入口形状约束；寄存器、ABI、布局族改变时可能拒绝相应能力，不能把符号存在等同于任意布局已知。

`symbols prepare` 顺序处理三个模块，各模块失败独立记录；`prepared/complete` 表示三个模块所有当前目标契约都通过，`partial` 表示部分模块完整通过。单个模块内部的可用能力仍逐项列出，不能仅根据汇总布尔值判断全部功能。总超时终止工作进程时保留已提交缓存。注入端只读本地缓存，不联网、不运行 DbgHelp；外观遍历不逐元素重新读取大文件哈希。

本机三个 PDB 全部通过、无缺失符号；合成 PE 测试覆盖不同 actions／active GUID／浏览器接口／provider／getter 槽／标签 GUID 偏移，验证单项失败及仅增补刷新。没有自动在用户 Explorer 中安装 Hook或运行 UI 操作，也没有其他系统构建的真机通过证明。详见 [新版验收步骤](testing-v0.11.0.md) 和 [证据](evidence/06-native-xaml-compatibility.json)。
