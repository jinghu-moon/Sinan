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
7. 在 72×24 的终端内打开两个同名标签，按路径区分并定位其中一个。按「更多操作」查看标签详情、固定、保护和锁定。设置筛选后打开「批量操作」，确认目标数量与筛选结果一致。在底栏验证定制开关随运行状态更新，F1 帮助中可刷新和退出。

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

## 5. 搜索、定位与目录快捷键

在同一个 PowerShell 中用临时配置启动观察代理。以下命令会创建两个同名目录供测试。

```powershell
$data = Join-Path $env:TEMP ('Sinan-tabs-' + [guid]::NewGuid())
$a = Join-Path $data 'A\src'
$b = Join-Path $data 'B\src'
New-Item -ItemType Directory -Force -Path $a, $b | Out-Null
& $sn --data-dir $data agent start --json
& $sn --data-dir $data entries add source $a --name '项目源码' --shortcut 'Ctrl+Alt+Shift+F9'
```

1. 在一个 Explorer 窗口打开 `$a` 和 `$b` 两个标签，再在另一窗口打开 `$a`。运行 `tabs list --query 'SRC' --json`，确认同名结果各有自己的路径、窗口和 ID。再用 `--query 'a/src'` 确认只匹配 A 目录。打开 TUI 标签页，输入相同关键词，对照结果并切换状态筛选。
2. 用下方命令定位 A 窗口中的具体标签。分别测试同一窗口的后台标签、其他窗口和已最小化的窗口。`selection_confirmed` 表示 Explorer 读回了选中的标签，`foreground_activated` 表示窗口确实切到前台。只有前者为真时返回退出码 5。手动关闭该标签后重试旧 ID，应报失效。
3. 在打开 `$a` 的 Explorer 窗口内按 `Ctrl+Alt+Shift+F9`，再切到其他应用按一次。检查目标标签被复用，窗口和标签总数没有增加。关闭所有 `$a` 标签后再按快捷键，预期当前 Explorer 窗口新增一个标签，从其他应用触发时请求打开新窗口。快速连续按两次，确认有待确认的打开请求时没有重复打开。
4. 用 `entries add ... --replace --disabled` 禁用原入口，从 `agent status` 核对 `directory_shortcuts.bound` 变为 0。用 `--replace` 改绑后确认旧组合键失效，新组合键生效。在其他应用的输入框中持续输入，观察按键响应。

```powershell
$matches = & $sn --data-dir $data tabs list --query 'a/src' --json | ConvertFrom-Json
$matches.tabs | Format-Table tab_id, window, path
$id = '<上表中的完整 tab_id>'
& $sn --data-dir $data tabs locate $id --json
& $sn --data-dir $data entries open source --json
(& $sn --data-dir $data agent status --json | ConvertFrom-Json).last_directory_open
& $sn --data-dir $data agent stop
```

扫描不完整时，可以定位本轮已观察到且身份通过校验的标签。只有确认目录未打开时才会发起打开请求。新窗口请求返回 `confirmation: launch_requested`，实际窗口在后续扫描中核对。

## 6. Explorer 动作与快捷键

在 PowerShell 中使用前一节的临时配置，重新启动观察代理后列出动作。绑定命令中的组合键可以按需替换。

~~~powershell
& $sn --data-dir $data agent start
& $sn --data-dir $data actions list
& $sn --data-dir $data shortcuts set open_terminal 'Ctrl+Alt+Shift+F5'
& $sn --data-dir $data shortcuts set copy_directory 'Ctrl+Alt+Shift+F6'
& $sn --data-dir $data shortcuts set copy_selection 'Ctrl+Alt+Shift+F7'
& $sn --data-dir $data shortcuts set new_folder 'Ctrl+Alt+Shift+F8'
& $sn --data-dir $data shortcuts set locate_clipboard_global 'Ctrl+Alt+Shift+F10'
& $sn --data-dir $data shortcuts set new_file 'Ctrl+Alt+Shift+F11'
& $sn --data-dir $data entries add source $a --name '项目源码' --shortcut 'Ctrl+Alt+Shift+F9' --scope explorer --replace
& $sn --data-dir $data actions run directories.open --parameters '{"id":"source"}'
$params = @{ parent = $a; name = '测试文件夹' } | ConvertTo-Json -Compress
& $sn --data-dir $data actions run explorer.new_folder --parameters $params
~~~

1. 在 Explorer 前台触发终端快捷键，检查 Windows Terminal 的起始目录。打开其他活动标签后重试，再切到其他应用验证快捷键作用范围。
2. 复制当前目录路径并粘贴到记事本。选中多个文件后触发复制选中项，核对每行都是一个完整路径。再取消选择，验证原剪贴板内容保留。
3. 把目录路径、带双引号的文件路径以及通过 Explorer 复制的单个文件分别放入剪贴板。切到其他应用按定位快捷键，核对目录标签复用和文件选中。多路径或无效文本应报错。
4. 在 Explorer 前台按新建文件夹快捷键，核对创建和内联重命名。用 `shortcuts set new_folder 'Ctrl+Alt+Shift+F8' --name assets` 配置预设后重试，核对 `assets` 创建和同名提示。
5. 在 F8「快捷键」选中动作，按 Enter 编辑键位。「新建文件夹」的编辑窗口也可设置预设名称。应用后检查表格中的草稿状态，给两个动作临时设置相同键位并核对冲突标记，修正后选择「保存键位」。在 F9「目录入口」中切换目录快捷键范围。回到其他应用输入文本，确认未绑定的按键照常工作。
6. 在 F8「资源管理器前台」选中「新建文件」，编辑并应用绑定，再选择「保存键位」。打开两个不同目录的标签，在其中一个按下绑定，确认弹窗显示当前目录。输入新文件名并确认，检查原标签选中文件。再测取消、重名和「创建后打开」，最后切到其他应用按同一组合键，确认弹窗仅在资源管理器前台触发。测试构建须包含 `sinan-panel.exe`。
7. 用另一款热键工具绑定同一个测试键，再切到 Explorer 按键，核对只有司南执行对应动作。保持 Explorer 前台，让另一款工具重新安装它的键盘钩子，等一秒后再试。切到其他应用后验证原有热键。用 `agent status --json` 查看 `shortcuts.bound`、`shortcuts.priority_rearms` 和 `shortcuts.errors`。Windows 会把后来安装的同类钩子插到链首，因此重新安装之间存在竞争窗口。

### 发送到标签

在 F8「资源管理器前台」为「发送到标签」绑定快捷键并保存。也可使用以下命令，选择当前未占用的组合键。

```powershell
& $sn --data-dir $data agent start
& $sn --data-dir $data shortcuts set transfer_selection 'Ctrl+Shift+Y'
& $sn --data-dir $data agent status --json
```

1. 在资源管理器打开两个本地目录标签，选中一个测试文件或文件夹，按下快捷键。检查搜索框获得焦点，标题下方显示选中数量，同一窗口的目标排在其他窗口之前。
2. 搜索目标路径并点击蓝色复制图标。确认源项目保留、目标产生副本。重复操作，在提示时检查 Windows 的重名处理界面。
3. 选中另一个测试项目，重新打开弹窗。点击添加路径图标，输入一个现有文件夹路径并保存。将该路径置顶，关闭再打开，确认置顶状态保留。
4. 从资源管理器拖动当前选中的项目。悬停绿色移动图标时，检查目标提示和高亮，松手后确认文件移到对应目录。拖到路径名称上松手应取消。
5. 打开弹窗后改变目标标签所在目录，再点击该条目的操作图标。确认司南刷新列表并提示重新选择。无选中项时触发快捷键，确认司南提示先选中文件或文件夹。

终端、复制路径和预设名称的新建动作支持通过 `--parameters '{"tab_id":"…"}'` 从终端指定标签。交互式新建需要 Explorer 窗口和标签处于前台。使用 `agent status --json` 的 `last_action` 查看最近一次 Explorer 动作结果，测试结束后运行 `agent stop`。

## 7. 安装与 PATH（手工选择执行）

```powershell
& $sn install
# 完全关闭并重新打开 Windows Terminal
sinan --version
sinan path status
sinan tui
```

默认目录为 `%LOCALAPPDATA%\Sinan`。安装复制全部八个运行文件；`--data-dir` 不能重定向安装或真实 PATH 修改。卸载命令为 `sinan uninstall`，默认保留配置。自动构建验证使用 CMake 安装目录，不自动执行用户安装。
## 资源管理器标签动作

启动观察代理后，在 TUI 的 F8「快捷键」给四个动作配置按键。F6「工具」的「常用操作」可以直接执行这四个动作。先在标签列表选择测试目录，以指定「另开父级」的来源和「合并标签」的作用窗口。

```powershell
$sn = '.\build\sinan\explorer-actions\sinan.exe'
$data = Join-Path $PWD 'build\manual-explorer-actions'
& $sn --data-dir $data agent start
& $sn --data-dir $data tabs list --json
$target = @{ tab_id = '<测试子目录的 tab_id>' } | ConvertTo-Json -Compress
& $sn --data-dir $data actions run explorer.open_parent_tab --parameters $target
& $sn --data-dir $data actions run explorer.open_parent_tab --parameters $target
& $sn --data-dir $data actions run explorer.merge_duplicate_tabs --parameters $target
& $sn --data-dir $data actions run explorer.reopen_closed_tab
& $sn --data-dir $data actions run explorer.toggle_file_extensions
& $sn --data-dir $data actions run explorer.toggle_file_extensions
```

先确认两次「另开父级」各新增一个父目录标签。合并后父目录应只剩一个普通标签，当前、固定、保护和锁定的标签都应保留。重开应恢复最近关闭的文件系统目录。关闭历史保留在代理内存中，最多 20 条。扩展名动作使用当前 Windows 用户的显示设置，执行两次可恢复原值。

在 F4「设置」启用「双击文件列表空白处返回父目录」，或运行以下命令。在普通子目录内双击文件列表空白处，检查是否返回父级。分别双击文件夹、列标题和侧栏，检查是否保留各区域原有行为。

```powershell
& $sn --data-dir $data explorer double-click-parent on
& $sn --data-dir $data explorer double-click-parent status
& $sn --data-dir $data explorer double-click-parent off
& $sn --data-dir $data agent stop
```

## 外部目录在标签中打开

在 F4「设置」启用「外部目录标签化」，也可以使用 CLI。当前适配 Windows 11 24H2 x64 的 ExplorerFrame 26100.8457，并校验模块哈希。目标窗口需要已被司南识别、位于当前桌面且未最小化。普通本地目录打开和「在文件夹中显示」会进入最近使用的可用窗口。无法确认请求或迁移失败时，系统窗口继续处理原请求。

```powershell
$sn = '.\build\sinan\external-tabs-activation\sinan.exe'
& $sn explorer external-tabs on
& $sn explorer external-tabs status --json
& $sn explorer external-tabs off
```

运行以下脚本先记录 Windows 基线，再用独立数据目录验证启用和停用。脚本会检查下载目录首次打开、已有下载标签的切换和文件选中、单选、多选以及显式新窗口。迁移用例会检查目标窗口是否切到新标签、源窗口是否退出，以及是否收到未遮蔽的源窗口显示事件。复用用例会核对原标签的句柄和标签数量。

```powershell
pwsh -NoProfile -File .\tests\integration\external_folder_open.ps1
pwsh -NoProfile -File .\tests\integration\external_folder_open.ps1 -SinanExecutable $sn
```

`status --json` 中的 `listener.active` 表示有已接入的目标窗口，`listener.activation.active` 表示已接入现有标签的激活路径。`reused` 统计已确认的标签切换，`redirected` 统计完成的窗口迁移，`fallback` 和 `rollback_failed` 分别统计迁移回退和回退失败。手工验收可从两个 Explorer 窗口依次切换焦点，再从其他应用打开目录，检查落点。另需用 Ctrl+N 新建窗口，拖出标签，以及在没有合适目标窗口时打开目录，确认 Windows 原行为。

## 用 Everything 加速资源管理器原生搜索

先启动 Everything 并等待索引就绪，再用 F4「设置」的「启用快搜」或 CLI 启用。当前适配 Windows 11 24H2 x64 上已验证的 `Windows.Storage.Search.dll`，加速本地固定卷上由 Explorer 文件系统扫描处理的新搜索。Everything 按自己的语法解析搜索框提交的文本，结果显示在当前标签页。

```powershell
$sn = '.\build\sinan\scripts-verified\sinan.exe'
$data = Join-Path $PWD 'build\manual-everything-search'
& $sn --data-dir $data explorer everything-search on
& $sn --data-dir $data explorer everything-search status --json
```

在两个不同目录放置同名文件，从每个目录发起搜索，分别测试 `README.MD`、`*.md`、`ext:md` 和无匹配项的关键词。按 Enter 后验证结果留在原标签、路径位于本次搜索目录，并测试打开、右键、复制及返回。用 `status --json` 检查对应 Explorer 进程的 `everything_queries` 是否增加。`query_ms` 记录第一批 SDK 查询耗时，Everything 返回零条时显示空结果。

搜索一个在 Everything 索引中有超过 1,000 条匹配的目录，核对 Explorer 显示的结果。`everything_pages` 记录累计成功读取的批数，`peak_cached_paths` 应不超过 256。第一批之后查询失败时，`partial_queries` 记录已开始展示但未读完的查询。

在当前结果页按 Ctrl+E，替换为不同的搜索词后按 Enter。检查结果列表替换上一轮结果，并确认 `everything_queries` 再增加 1。在另一标签从不同目录发起搜索，切回第一标签重搜，检查两个标签分别维持各自的搜索范围。Windows 索引处理的查询以及 Everything 在返回首条结果前失败的查询仍可由 Windows 处理。

运行 `& $sn --data-dir $data explorer everything-search off` 停用。

## 解压并定位

在 F6「工具」的「解压与脚本」打开「解压设置」。添加 7-Zip、RAR / UnRAR 或 Windows tar 的命令行程序，设为默认工具，按需为 `.rar`、`.tar.gz` 等后缀添加专属规则，然后保存。以 7-Zip 为例，也可从 CLI 配置。

```powershell
$sn = (Resolve-Path '.\build\sinan\archive-final\sinan.exe').Path
$data = Join-Path $PWD 'build\manual-archives'
New-Item -ItemType Directory -Force $data | Out-Null
$settings = Join-Path $data 'archives.json'
@{
    tools = @(@{ id = 'seven'; name = '7-Zip'; type = '7zip'; path = 'C:\A_Softwares\7-Zip\7z.exe' })
    default_tool = 'seven'
    formats = @{ '.zip' = 'seven'; '.7z' = 'seven' }
    recycle = $false
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8NoBOM -LiteralPath $settings
& $sn --data-dir $data archives configure $settings
& $sn --data-dir $data tui
```

1. 在 F8 为「解压定位」绑定未占用的快捷键。在 Explorer 选中普通 ZIP 后按下快捷键，检查没有弹出解压工具窗口，解压目录出现在压缩包旁边，Windows 通知报告任务结果。
2. 测试只含 `资料\文档\说明.txt` 的归档，核对发布目录为「资料」，打开「文档」并选中 `说明.txt`。重复解压，检查第二份结果使用 `资料 (2)`。用包含多个顶层条目的归档检查包名作为外层目录。
3. 对 7-Zip 分卷分别选择中间卷和同组多个卷，核对每次只加入一个任务。取走一卷后重试，确认任务失败且源卷保留。将同一组分卷恢复完整后检查可再次解压。
4. 启动耗时较长的 7-Zip 解压，切换到其他应用。在 F6「解压与脚本」的「解压任务」核对任务自动刷新并显示工具报告的百分比。验证解压完成时前台应用保留，点击结果通知或「打开结果」可定位。用另一份测试包验证「取消任务」。
5. 默认设置下，核对归档仍在原目录。用一次性测试归档开启「成功后将原包移入回收站」，分别检查成功、取消、缺卷和损坏时的源文件去向。测试成功的归档可从回收站恢复。

CLI 使用 `archives jobs` 列出任务，`archives open <id>` 打开结果，`archives cancel <id>` 取消任务。也可从 CLI 触发共享动作，在对应 Explorer 标签选好归档，并将 `tabs list --json` 中的 `tab_id` 传给动作。

```powershell
$target = @{ tab_id = '<测试标签的 tab_id>' } | ConvertTo-Json -Compress
& $sn --data-dir $data actions run explorer.extract_archive --parameters $target --json
& $sn --data-dir $data archives jobs
```

## PowerShell 脚本动作

脚本从 `$env:SINAN_CONTEXT` 读取 JSON，其中 `directory` 是当前文件系统目录，`selected_paths` 是选中项的绝对路径数组。有目录时，PowerShell 在该目录启动。全局快捷键在其他应用前台时，目录为 `null`，选中项为空数组，工作目录为脚本所在目录。司南按需启动系统 Windows PowerShell，采用系统当前的执行策略。执行结果中的 `started` 和 `pid` 表示进程已创建，脚本的执行结果可由脚本自行记录。

```powershell
$sn = '.\build\sinan\scripts-verified\sinan.exe'
$data = Join-Path $PWD 'build\manual-scripts'
New-Item -ItemType Directory -Force $data | Out-Null
$script = Join-Path $data 'show-context.ps1'
@'
$env:SINAN_CONTEXT | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot 'context.json')
'@ | Set-Content -Encoding UTF8 -LiteralPath $script

& $sn --data-dir $data scripts add context $script --name '记录当前目录' --scope explorer --shortcut 'Ctrl+Alt+Shift+F2'
& $sn --data-dir $data scripts add context-global $script --name '记录全局上下文' --scope global --shortcut 'Ctrl+Alt+Shift+F3'
& $sn --data-dir $data actions list --query PowerShell --json
& $sn --data-dir $data agent start
& $sn --data-dir $data tabs list --json
$target = @{ tab_id = '<测试标签的 tab_id>' } | ConvertTo-Json -Compress
& $sn --data-dir $data actions run script.context --parameters $target --json
Get-Content -LiteralPath (Join-Path $data 'context.json')
& $sn --data-dir $data tui
```

在 TUI 的 F6「解压与脚本」或 F8「快捷键」打开「脚本动作」，可以添加、编辑、运行和移除脚本。运行前从标签列表选中测试标签。切到 Explorer 选中几个文件，按 Explorer 范围快捷键并检查 `context.json` 中的目录和文件列表。在终端按全局快捷键，检查目录和选中项是否为空。使用 `agent status --json` 的 `last_action` 查看最近启动结果，测试结束后运行 `agent stop`。
