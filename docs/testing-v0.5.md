# TabTail 真机测试

v0.6 的三模式标签宽度、原路径升级和实测状态见 [标签宽度测试](tab-width.md)。以下保留标题、目录规则与 TUI 基础验收；使用 v0.6 配套副本时还需复制 `tabtail-layout.dll`。

## 准备独立副本

在项目根目录的 PowerShell 中执行。下例从构建输出复制配套 EXE / DLL，避免启用 Hook 后占用编译目录；已经存在不同版本的副本时，请换一个新的目录名。

```powershell
$ttBin = Join-Path $PWD 'build\manual-v053-bin'
New-Item -ItemType Directory -Force $ttBin | Out-Null
Copy-Item .\build\msvc-lifecycle\Release\tabtail.exe, .\build\msvc-lifecycle\Release\tabtail-hook.dll $ttBin
$ttExe = Join-Path $ttBin 'tabtail.exe'
$ttData = Join-Path $PWD 'build\manual-v053-data'
function ttv { & $ttExe --data-dir $ttData @args }
ttv --version
ttv doctor
```

也可以使用分发 ZIP 中的配套 `bin`，将 `$ttExe` 指向解压后的 `bin\tabtail.exe`。所有命令始终使用同一副本和 `$ttData`；不需要安装或修改 PATH。日常正式命令仍为 `tabtail`，简称 TT 不自动创建 `tt` 别名。

若旧版曾经启用 Hook，先用旧版停止代理，在方便时显式重启 Explorer 或注销，让驻留 DLL 退出后再启用新版。旧协议与 v0.5 不互通。不要覆盖仍被 Explorer 占用的 DLL。

本机从 0.5.1 / 0.5.2 升级代理修复必须保持原 `tabtail.exe` 路径：IPC 和已驻留 DLL 都校验该路径。先 Ctrl+Q 退出旧 TUI，使用原路径停止代理，再仅替换 EXE、保留原 0.5.1 DLL。不要把 EXE 改名运行。以下脚本保留原先启用/观察模式，不重启 Explorer：

```powershell
.\tools\update-agent.ps1 -CurrentExecutable .\build\msvc-nord\Release\tabtail.exe -NewExecutable .\build\msvc-lifecycle\Release\tabtail.exe -DataDirectory .\build\nord-manual-data
.\build\msvc-nord\Release\tabtail.exe --data-dir .\build\nord-manual-data tui --lang zh-CN
```

停止代理会清除临时单标签覆盖；同一个数据目录中的全局设置和目录规则保留。此路径适用于 0.5.1 到 0.5.3 的代理修复，不适用于 0.4.x 的旧协议 DLL。

## TUI 与语言

```powershell
ttv tui --lang en
```

1. 顶部应显示 Observation only；已有定制代理除外。首次进入不会改变标签标题。
2. `F4` 进入设置；`Tab` 移动到语言选项，方向键选择“简体中文”，页面应即时切换；再到“保存语言”并回车。
3. `Ctrl+Q` 退出后执行 `ttv tui`，应恢复中文；`ttv tui --lang en` 应暂时用英文，退出后不保存该覆盖。
4. 再进入 TUI，修改全局层级/分隔符但不提交，切换页面后草稿应保留；应用时显示受影响标签数量。
5. 用中文输入法输入规则名称、中文目录路径；检查组合输入、退格和粘贴。在编辑器内按 F7，草稿应保留且界面切换语言。语言选项不会翻译用户目录名称。
6. 将终端调整到 72×24；通过 `Tab/Shift+Tab` 应可滚动到设置页保存按钮和规则编辑器“应用”。低于最低尺寸时应显示缩放提示，只保留 Ctrl+Q 退出。
7. `F1` 打开帮助，`Esc` 关闭。`F6` 工具页打开重启确认，按 `Esc` 取消，应不重启 Explorer。
8. Windows Terminal 应显示 Nord 背景 `#2E3440`、前景 `#D8DEE9`、冰蓝选中页签 `#88C0D0`，输入框和弹窗也采用相同配色。110 列起标签详情移到右侧；170 列起增加第三栏策略概览，表头和中文数据仍对齐。
9. 选中至少三级深的目录标签，在标签编辑器保持初始“自动匹配规则”，直接将数字改为 3：应自动选择“指定层级”，预览立即显示最后三级目录，尚未点击应用时不会提交。按 Esc 取消。
10. 设置页把全局默认改为 3，检查草稿预览；已有目录规则或临时覆盖时，应显示优先级说明。规则编辑器应默认带入当前目录作为测试路径；原生动作下修改数字，应改为路径层级动作并重新预览。

`F2` 标签页、`F3` 目录规则、`F4` 设置、`F5` 刷新、`F6` 工具。方向键只在当前控件中选择，Tab 在所有可见控件间切换。

退出 TUI 后代理继续运行：

```powershell
ttv agent status --json
```

## 目录规则与匹配解释

可先退出 TUI，用 CLI 建立规则，再进入界面检查。观察代理运行时修改规则可能返回退出码 5，表示策略已记录、标题尚未应用。

```powershell
$sample = Join-Path $PWD 'build\manual-paths'
New-Item -ItemType Directory -Force "$sample\项目A\src", "$sample\项目B\docs", "$sample\项目C\cache", "$sample\项目D\assets" | Out-Null
ttv config set depth 2
ttv rules add sources --name '源码与文档' --pattern "$sample\**\{src,docs}" --depth 3
ttv rules add caches --name '缓存原始标题' --pattern "$sample\**\cache\**" --native
ttv rules test "$sample\项目A\src" --json
ttv rules test "$sample\项目C\cache" --json
ttv rules test "$sample\项目D\assets" --json
```

预期依次为：`source=rule, rule_id=sources, depth=3`；`rule_id=caches, action=native, preview_title=null`；`source=global, depth=2`。native 的本地空预览表示需要真实 Shell 对象取得显示名。

继续检查排序和持久化：

```powershell
ttv rules add broad --pattern "$sample\**" --depth 1
ttv rules move broad 1
ttv rules test "$sample\项目A\src" --json  # broad 首先命中
ttv rules toggle broad
ttv rules test "$sample\项目A\src" --json  # 恢复 sources
ttv rules delete broad
ttv rules list
ttv tui
```

在目录规则页新增/编辑规则，输入测试路径，检查草稿预览，取消不应保存；应用后退出再进入，规则、顺序和启停状态应保留。从当前标签创建规则默认采用精确目录模式。

额外语法测试可用 `*`、`**`、`?`、`{src,docs}`、`[ABC]` 和 `[0-9]`。`**` 独占目录段且可匹配零级；末尾 `\**` 包含目录自身。模式必须为绝对盘符或 UNC 路径。不支持前置 `!`、`[!abc]`、`[^abc]` 或 `a**b`；输入这些语法应报错并保留原配置。包含字面方括号的目录使用精确目录模式。

## 真实标题与优先级

这一步会改变真实标签标题，需 `doctor` 显示精确文件适配。用 Explorer 手工打开上面的四个目录，然后：

```powershell
ttv tui
```

1. 标签页应列出目录、预览、来源和应用状态；选择“启用标题定制”。实际可见标签标题需由人眼核验，`title_applied` 只是调用确认。
2. src/docs 应显示最后 3 级；cache 保留 `cache` 普通显示名；assets 显示最后 2 级。
3. 为 src 标签设置层级 1，标题应变为 `src`，来源变成标签独立设置；改变全局或目录规则不得覆盖它。
4. 在这个标签中导航到 docs，临时层级 1 应跟随，显示 `docs`。选择“自动匹配规则”后应立即使用目录规则的 3 级。
5. 无覆盖标签从 src 导航到 assets，应切换到全局 2 级；再导航到 cache，应保留原生显示名。等待一次续订后不应回退或串用旧目录规则。
6. 同一窗口打开两个相同路径标签，分别设置 1 / 3 级；检查前台/后台、切换标签和关闭标签后，另一标签不受影响。
7. 修改已有规则的层级、启停和顺序，观察无覆盖标签重新匹配；`F5` 后核对来源和已提交标题。
8. “停止标题定制”应恢复普通 Shell 显示名；退出代理应清除临时覆盖；再次启动观察代理时，目录规则仍在。

```powershell
ttv tabs list --json
ttv stop
ttv agent stop
```

管理员终端也可操作 TUI；如需重启 Explorer，只通过工具页确认或显式 `ttv explorer restart --force` 执行。配置更新不会自动重启。已启用过 Hook 的副本在 Explorer 退出前需要保留。

## 启动前已打开标签与重启后清理

1. 启动程序前，在多个 Explorer 窗口打开多个不同目录标签，然后执行 `ttv tui`。已有标签应直接出现在列表中，无需重新打开目录；Shell 暂时不可枚举时等待后按 F5。
2. 启用定制后检查已有标签的全局/目录规则，并为其中一个标签设置独立层级。
3. 如需重启验收，先结束文件复制等操作，再由用户通过工具页确认“重启资源管理器”。重启后旧 PID 对应的记录应在下轮扫描消失，即使本次 Shell 集合不完整，也不再永久显示旧标签的身份错误。
4. 在新 Explorer 中打开目录，新标签使用全局/目录规则，不能继承已退出进程的临时覆盖。未重启的独立 Explorer 进程中的标签及覆盖应继续保留。
5. 暂时漏扫但进程仍存活的标签可以保留在列表中，JSON 的 `observed` 应为 false、`title_applied` 为 false；再次观察到后恢复原覆盖。进程查询失败或权限不足不能导致批量删除。
6. 空闲时连续执行 `ttv tabs list --json`；稳定窗口应有 `scan_complete=true`、空 `warnings`，已启用时检查逐标签 `title_applied`。Shell 注册槽有空缺时不应再出现“函数不正确 (1)”。打开/关闭窗口时短暂集合变化仍可能报不完整，下次稳定扫描应恢复。

## 自动检查

```powershell
cmake --build build/msvc-lifecycle --config Release --parallel 6
ctest --test-dir build/msvc-lifecycle -C Release --output-on-failure
```

默认测试不启用真实标题 Hook、不重启 Explorer、不修改用户 PATH。测试包括中英文生产组件树、最小尺寸的焦点可达性、CLI 规则操作、配置升级、规则持久化和只读真实标签观察。中文输入法、真实终端连续缩放与实际标题视觉效果仍需完成上面的人工步骤。
