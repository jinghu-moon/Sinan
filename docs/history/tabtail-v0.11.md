> 历史文档：命令、产物及默认目录属于重构前版本。当前用法请参阅仓库 README 与 06 号重构文档。

# TabTail 0.11.0 历史功能记录

Customize Windows Explorer tab titles with configurable path depth.

自定义 Windows 资源管理器标签页的路径显示层级。

正式名称 **TabTail**，简称 **TT**，命令 `tabtail`，仓库 `TabTail`，配置文件 `tabtail.toml`，C++ 命名空间 `tabtail`。

## 当前实现

v0.11.0 将符号适配扩展到三个 Explorer 模块：关闭保护、目录锁定、标签选择、新建标签、固定重排，以及图标／文字／背景的身份映射均使用独立契约。新增字段从指令引用中推导，移除这些路径的单一构建哈希门槛；运行时继续核验对象并读回实际效果。CLI `symbols prepare` 与 TUI F6 一次准备三个模块，部分失败保留已通过能力。见 [兼容设计](../05-symbol-compatibility.md) 与 [0.11.0 测试步骤](../testing-v0.11.0.md)。尚未完成其他 Windows 构建的真机验收，不宣称全版本兼容。

v0.10.0 新增微软 PDB 符号准备、按系统 DLL／PDB 身份绑定的缓存，以及逐功能兼容报告。标题与标签枚举可通过符号 RVA 和 x64 布局契约适配地址变化的构建；独立工作进程负责下载和 DbgHelp，不在 Explorer 中联网。CLI `symbols status/prepare` 和 TUI F6“详细检查”均可使用，支持离线解析。见 [兼容设计](../05-symbol-compatibility.md) 与 [验收步骤](../testing-v0.10.0.md)。

v0.9.0 对照 CLI／配置补齐 TUI 入口：逐标签能力与原生／图标探测、临时外观重置、完整状态与配置原文、安装／卸载与 PATH、显示默认值恢复、任意路径预览、外观文件检查与确认导入。标签状态、通知、自动恢复、加粗与图标来源改为选择组件，外观规则增加列表及大小写开关。见 [功能自检表](../04-tui-feature-audit.md) 与 [验收步骤](../testing-v0.9.0.md)。

v0.8.9 重做工具页：批量操作、标签存档、诊断与帮助、运行维护四组圆角卡片；批量类型与状态直接选择，显示筛选目标数，空目标禁用应用，提交前确认。宽窗口居中限制内容宽度，右侧紧凑展示运行状态与策略；窄窗口随焦点滚动。见 [工具页验收](../testing-v0.8.9.md)。

v0.8.8 重做全局设置页：标题显示、标签宽度、界面语言分组卡片，圆角输入与选项整项换行；大窗口居中限制内容宽度，预览突出计算标题和路径，并解释观察模式。各组保存／重载独立，重载标题不会清除宽度草稿。见 [设置页验收](../testing-v0.8.8.md)。

v0.8.7 将统一表单扩展到全部七类 TUI 弹窗：圆角外框与输入框、固定底栏、分组滚动和可换行的选项；外观设置按文字／图标／浅深色主题分组，帮助与结果支持键盘翻页和鼠标滚动，确认框显示操作影响并默认聚焦取消。中英文及 72×24 布局适配，中文操作按钮统一四字。见 [全部弹窗验收](../testing-v0.8.7.md)。

v0.8.6 重做目录规则弹窗：圆角输入框、内部留白、聚焦／悬停／错误边框和独立占位提示；宽窗口分列编辑与预览，窄窗口纵向布局并固定操作栏。预览显示规则名称、标题结果与影响数量，打开弹窗默认聚焦名称；背景压暗，保留四字操作按钮。见 [规则表单验收](../testing-v0.8.6.md)。

v0.8.5 将所有中文操作按钮统一为四字文案。固定、独立保护和目录锁定按钮按所选标签状态显示单个动作：固定标签／取消固定、保护标签／解除保护、锁定目录／解除锁定；开启状态分别使用黄色、绿色、紫色背景。规则按钮也在启用规则／停用规则间切换。请求期间禁用，收到快照后更新，不提前假定成功。见 [状态按钮验收](../testing-v0.8.5.md)。

v0.8.4 修复后台代理拖慢其他软件输入的问题：Explorer 快捷键低级钩子改由独立消息线程处理，目标标签读取移至另一工作线程，回调只筛选和有界投递；观察模式、无目标或无快捷键时不安装钩子。TUI 录入钩子只在快捷键框聚焦期间启用。见 [输入卡顿修复与复测](../testing-v0.8.4.md)。

v0.8.3 优化标签详情与策略概览的可读性：分组标题、对齐字段、加粗预览与数值、颜色与状态符号；区分暂无验证结果、已确认和未确认。保护开关使用实心／空心标记；显示已应用数量占比和纵向匹配优先级。长路径及无空格中文可完整换行。见 [详情界面验收](../testing-v0.8.3.md)。

v0.8.2 统一重构 TUI 按钮：常驻 Nord 背景、左右留白、按钮间隔，无方括号；主要操作使用蓝色底，危险操作使用红色提示，键盘焦点增加下划线。支持整块点击和 Enter／空格激活；无选中对象或请求处理中禁用相应按钮，Tab 跳过禁用项。保留中英文、窄窗口换行和工具页滚动。见 [按钮界面验收](../testing-v0.8.2.md)。

v0.8.1 修复快捷键输入时 Ctrl+C／Ctrl+Q 触发终端或 TUI 退出的问题：F8／F9 快捷键框支持按键录入，并将快捷键占用显示为错误。标题桥接升级为独立 v3 协议，保留原始错误；检测到旧路径驻留钩子时显示完整 DLL 路径，拒绝覆盖。见 [0.8.1 升级与复测](../testing-v0.8.1.md)。

v0.8.0 新增目录全局快捷键、路径别名，以及固定标签保持位置／左置／右置。F9“目录入口”统一管理路径、名称、快捷键和别名范围；按键优先定位已打开目录，未打开时在前台 Explorer 新建标签，其他应用前台则请求新窗口。固定排序通过原生接口移动，使用实际 TabItems 顺序确认并保留选中标签。配置 schema 4，首次写入保留旧配置备份。见 [设计与边界](../03-directory-entries-and-docking.md)、[测试命令](../testing-v0.8.md)。

v0.7.2 将整窗关闭确认改为系统任务对话框（TaskDialogIndirect），使用 Common Controls v6。简体中文／英文按 TabTail 语言设置显示单份文案；默认“保留窗口”，明确选择“关闭整个窗口”才放行。仅目录锁定不会触发关闭保护。

v0.7.1 修正标签图标替换：通过原生 `TabViewItem.IconSource` 显示圆润实心图钉／锁，保留任务栏预览的窗口图标。成功判定检查标签实时视觉树中的实际几何、颜色和尺寸；首次 XAML 初始化作为待确认并有界重试。0.7.0 的 `WM_GETICON` 结果仅证明窗口图标改变，不能证明标签已替换，已撤回这项判断。用户截图已确认修正后的标签图钉。

v0.7 新增标签实例级固定、独立关闭保护、目录锁定、Explorer 前台快捷键和系统通知；支持全局／目录／状态／临时标签样式，包括字体、加粗、文字颜色、深浅主题三态标签背景、独立图钉／锁图标颜色。F8 打开双语编辑器，工具页可批量管理及保存／恢复工作目录。可选登录自动恢复默认关闭。配置升级为 schema 3，首次显式升级保留旧文件备份。

阶段 0–4 的代码与接口已实现，当前属于精确系统版本的实验功能。原生入口探针已覆盖关闭保护、导航前取消和新标签转发、同路径实例隔离、实际图标／文字／背景读回及恢复、租约失效解除；真实鼠标／键盘路由、通知点击、多 DPI 和主题视觉仍待人工验收。完整命令和分阶段步骤见 [标签保护与外观测试](../testing-tab-features.md)，边界见 [实现状态](../implementation.md)。

v0.6.1 修复宽度编辑的配置版本冲突：无关配置更新不再使草稿失效；界面显示未保存状态，真实冲突保留草稿并提供“重新载入宽度”。

v0.6 新增全局标签宽度：系统默认、固定宽度、随标题内容自适应（最小 / 最大宽度）。TUI 的 F4 设置页与 `appearance` CLI 共用配置；尺寸为 DIP，跟随 Windows 缩放。现有标签实测三模式切换与恢复通过，标签拖动、跨窗口移动和连续缩放仍需真机交互验收。详见 [标签宽度与测试步骤](../tab-width.md)。

v0.5 新增简体中文 / English TUI、持久化目录规则和正向通配符。策略优先级为 **标签临时覆盖 > 从上到下首条命中的已启用目录规则 > 全局默认**。每个标签可独立选择 1..32 级、完整路径或自动匹配规则；临时覆盖跟随导航，关闭标签或退出代理后清除。目录规则和语言偏好保存在 `tabtail.toml`。

**兼容能力按功能分别校验**：内置精确 ExplorerFrame 适配可离线使用；其他 x64 构建可准备符号，只有 PDB 身份、函数入口和对象布局契约均通过时才开放标题／枚举。关闭保护、目录锁定、标签选择／重排仍使用精确构建适配，图标及文字背景另校验两个 XAML 模块。符号名称相同不代表任意 Windows 版本都可使用；尚未完成其他系统构建的真机验收。

已在 5 个现有标签上完成 UI 线程读取、两个不同层级的应用确认、全局修改保留覆盖、reset、恢复和异常退出租约接管测试。当前无法使用桌面截图通道，尚未完成视觉核验、真实导航/重命名、标签拖动和同路径双标签的完整集成验收。

Windows 11 x64；C++20、MSVC、CMake。TOML 使用仓库内固定版本的 toml++ v3.4.0 单头文件 `toml.hpp`，无需独立链接 toml++ 库。

## 构建与验证

需要 Visual Studio 2022 的 C++ 桌面开发工具、Windows SDK、CMake 3.24+ 和 Python 3.11+（仅测试使用）。首次配置下载固定版本并校验 SHA-256 的 CLI11、nlohmann/json、MinHook 1.3.4 和 FTXUI 7.0.3。

```powershell
cmake --preset windows-msvc
cmake --build --preset release
ctest --preset release
.\build\msvc\Release\tabtail.exe --help
```

生成分发包（含依赖许可证）：

```powershell
cpack --config build/msvc/CPackConfig.cmake -C Release -B build/dist
```

## 使用

### 交互界面

```powershell
# 本地构建：隔离配置，打开中文 TUI
.\build\msvc\Release\tabtail.exe --data-dir .\build\manual-v05-data tui --lang zh-CN

# 安装后
tabtail                         # 交互终端中无参数进入 TUI
tabtail tui --lang en            # 只覆盖本次会话
tabtail tui                     # 使用保存偏好；auto 检测系统界面语言
```

四个页面：标签页、目录规则、全局设置、工具。`F2/F3/F4/F6` 切页，`F1` 帮助，`F5` 刷新，`F7` 切换中英文（编辑器内也可用），`F8` 编辑保护与外观，`Tab/Shift+Tab` 移动焦点，方向键选择，`Enter` 执行，`Esc` 取消弹窗，`Ctrl+Q` 退出界面。最低 72 列 × 24 行，建议 110 列 × 36 行；窄窗口中按 Tab 聚焦后会滚动到相应控件。

首次打开只启动观察代理，需明确选择“启用标题定制”才会改写真实标题。连接已有定制代理时保持其当前模式。退出 TUI 不退出代理；工具页“退出代理”会恢复标题并清除临时覆盖。设置页修改语言即时预览，点击“保存语言”后下次恢复；不会翻译用户路径或规则名称。底层诊断详情保留原始语言。

规则编辑器可预览现有标签受影响数量，支持从标签创建精确目录规则。设置草稿遇到外部配置变更会拒绝覆盖，可先“重新载入已保存的显示设置”再编辑。

v0.5.1 使用 Nord 24 位真彩色：深灰蓝背景、冰蓝强调色、深灰蓝选中行。顶部为紧凑搜索与快捷键导航，底部为操作和状态；标签列表和目录规则列表保留固定表头及 Unicode 对齐列。110 列起显示独立详情面板，170 列起标签页增加策略概览；窄窗口将详情放到列表下方。

标签编辑器输入层级数字会自动选择“指定层级”，实时预览当前目录最后 N 级；设置页显示全局草稿的实际标题预览。规则编辑器自动带入当前标签路径，也可手动替换测试路径。预览受标签覆盖、规则优先级约束，草稿在点击应用后保存。

### 安装与 CLI

```powershell
# 在 ZIP 解压目录执行首次安装（从本地构建目录也可运行）
.\bin\tabtail.exe install

# 完全退出并重新启动终端应用，再使用已加入 PATH 的命令
tabtail config set depth 2
tabtail preview 'D:\100_Projects\110_Daily\VoiceInput'
# 输出：110_Daily\VoiceInput

tabtail config set depth all
tabtail config set separator ' / '
tabtail config show
tabtail config reset
tabtail path status
tabtail status --json
tabtail doctor --json
```

### 目录规则

```powershell
tabtail rules add source --name '源码与文档' --pattern 'D:\Projects\**\{src,docs}' --depth 3
tabtail rules add cache --pattern 'D:\Projects\**\cache\**' --native
tabtail rules move cache 1       # 越靠前优先级越高，位置从 1 开始
tabtail rules add project --pattern 'D:\Projects\**' --depth 2
tabtail rules list
tabtail rules test 'D:\Projects\App\src'
tabtail rules toggle project
tabtail rules delete project
# 编辑现有 ID 时显式加 --replace；可重复 --pattern，多个模式为“或”
tabtail rules add source --replace --pattern 'D:\Projects\**\src' --depth 4
```

支持 `*`、`**`、`?`、`{a,b}`、`[abc]`、`[0-9]`。`*` / `?` 不跨目录分隔符；`**` 必须独占一个路径段，可匹配零级或多级目录，尾部 `\**` 包含该目录自身。模式锚定绝对盘符或 UNC 完整路径，默认忽略大小写。`--exact` 按精确目录匹配，适合名称中包含方括号的目录。`--native` 保留 Shell 普通显示名并终止后续规则查找。不支持正则、前置 `!` 排除、`[!abc]` / `[^abc]` 否定字符集。

规则持久化并在导航时重新匹配；临时标签覆盖优先，`tabs reset` 恢复自动匹配。观察模式保存规则可能返回 5，表示已记录但尚未应用真实标题。`rules test` 为本地匹配解释，不加载 DLL；原生标题动作返回空预览，因为没有真实 Shell 对象。

### 定制真实标签并分别设置层级

```powershell
tabtail doctor
tabtail start
tabtail tabs list

# 从列表复制完整 tab_id；下例 ID 仅为占位符
tabtail tabs set '<tab-id-A>' --depth 1
tabtail tabs set '<tab-id-B>' --depth 3
tabtail tabs set '<tab-id-C>' --depth all
tabtail tabs list --json
tabtail tabs reset '<tab-id-B>'

tabtail agent status
tabtail stop       # 恢复 Shell 显示名，保留观察代理与运行期覆盖
tabtail agent stop # 退出代理，清除运行期覆盖
```

`start` 保存 enabled=true 并启用定制；`stop` 保存 enabled=false，恢复当前 Shell 显示名。单独执行 `agent start` 始终先进入观察模式，不自动恢复上次启用意图；观察模式下 `tabs set/reset` 只保存策略并返回 5。`tabs probe <tab_id>` 临时加载桥接 DLL，在 UI 线程读取原生位置，不安装标题 Hook、不改变标题。

JSON 的 `preview_title` 是代理计算值，`applied_title` 是 Explorer UI 线程实际提交的值，`title_applied` 表示当前策略获得调用确认，不表示已进行视觉检查。失败项见 `apply_error`。默认每两秒扫描和续订，执行 tabs 命令时再次扫描；扫描不完整时拒绝修改策略。关闭标签在两次完整缺失扫描后清理，旧 tab_id 不复用。

标题策略采用约 10 秒租约；代理意外退出后，Explorer UI 线程有响应时恢复 Shell 显示名。`stop` 尝试立即恢复。Hook 安装后 DLL 保持加载并在无策略时透传，直到 Explorer 进程退出，因此被加载的 DLL 不能覆盖或删除。当前版本发现标题入口已被其他 DLL 修改时拒绝覆盖。

代理管道限定当前用户、当前会话及数据目录，连接双方验证为同一路径的 TabTail EXE。请使用启动代理的那份 EXE 进行后续操作；切换到另一份安装或构建路径前，先执行 `agent stop`。`--data-dir` 可隔离测试代理与配置，不修改真实用户配置。

CLI tab_id 是代理生成的不透明生命周期标识；支持的文件版本通过浏览器子窗口关联真实原生 GUID，单独在 `native_guid` 字段提供。虚拟位置保留 Shell 显示名。恢复操作使用当前 Shell 普通显示名，尚未验收系统完整路径标题偏好和第三方标题插件的还原。

默认配置在 `%LOCALAPPDATA%\TabTail\tabtail.toml`。安装默认禁用标题定制。`depth` 接受 `1..32` 或 `all`；分隔符接受 `\`、` / `、` · `。完整路径模式使用原生路径分隔符。

```toml
schema_version = 2
revision = 1
enabled = false
autostart = false

[display]
depth = 2
separator = '\'

[ui]
language = 'auto'

[[rules]]
id = 'projects'
name = '项目目录'
enabled = true
match = 'glob'
patterns = ['D:\Projects\**']
action = 'depth'
depth = 3
case_sensitive = false
```

v0.5 可以只读加载旧 schema 1；首次成功保存时升级为 schema 2，不在读取时改写。旧程序不能读取 schema 2，请保留升级前配置备份。非法配置拒绝保存，不自动重置。

需要独立配置进行预览或脚本测试时，可使用 `tabtail --data-dir <目录> ...`；该参数不支持安装和 PATH 修改。

v0.5 标题桥接协议升级为 2，与旧 DLL 不互通。已启用过旧版 Hook 时，请先用原版停止代理，再在方便时显式重启 Explorer 或注销，让驻留 DLL 退出，然后使用新版启用。程序不会为升级自动重启 Explorer，也不支持覆盖被占用的安装文件。

### Explorer 重启

当前版本尚未验证正常退出 Shell 的机制，重启需要显式指定 `--force`。该操作会关闭当前会话的 Explorer、桌面和任务栏，可能中断文件复制。普通终端中会要求确认；脚本必须加 `--yes`。v0.4.1 起支持管理员终端：优先等待系统自动恢复 Shell；需要主动启动时，优先使用经过校验的当前用户普通权限令牌。如果原桌面本身就是管理员权限且没有可用的普通权限令牌，则保持原桌面的权限启动。启动上下文在结束 Explorer 前准备；没有可用桌面上下文时提前报错。

```powershell
tabtail explorer restart --force
# 脚本：tabtail explorer restart --force --yes
```

配置更新和安装卸载均不会自动重启 Explorer。定制代理仍运行时，显式重启后会扫描新标签并尝试重新接入，原标签覆盖不迁移。

v0.5.2 修复重启后旧标签长期残留：即使 Shell 集合扫描不完整，已确认退出的进程记录仍会清理；进程查询失败时保留记录，不把权限不足当成进程退出。临时未观察到的标签保留覆盖，但暂停标题续订。启动时会扫描已经打开的标签，无需重新打开文件夹；启用定制后应用到已发现的标签。

v0.5.3 改用 Shell 集合枚举器遍历有效窗口，修复空注册槽被误报为扫描不完整的问题；正常的枚举结束不再显示“函数不正确 (1)”。真实 COM 失败和枚举期间集合变化仍会报告。

### 卸载

```powershell
tabtail uninstall
# 同时清理配置：tabtail uninstall --purge
```

默认保留配置。卸载只移除本工具登记的用户 PATH 项。Windows 不允许删除正在运行的 EXE：若从已安装命令卸载，程序会返回退出码 5 并列出待清理文件；已启用过 Hook 时 DLL 也会保持占用，需待 Explorer 进程退出后，从 ZIP 解压目录再次执行 `bin\tabtail.exe uninstall` 完成清理。不会启动后台脚本删除任意目录。

### 退出码

`0` 完成，`1` 一般错误，`2` 参数/配置错误，`3` 暂不支持，`4` 权限不足，`5` 部分完成，`6` 取消或缺少必要确认。

## 设计与后续

[设计文档](../design.md) 定义完整产品方案；[实现状态](../implementation.md) 记录实际交付和待验证部分；[v0.5 真机测试步骤](../testing-v0.5.md) 给出隔离测试命令和验收清单。当前对第三方 Explorer 插件没有兼容性承诺。

MIT License；依赖许可证见分发包 `licenses/`。
