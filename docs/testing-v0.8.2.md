# 0.8.2：TUI 按钮重构

按钮采用常驻背景和左右各两格留白，不添加 `[]`。普通按钮使用 Nord 深灰底；保存／应用／启用为蓝底；删除、重启及确认操作使用红色提示。悬停改变背景，键盘焦点增加加粗及下划线。按钮之间保留水平间隔，换行或相邻操作组之间保留空行。

## 启动

在 Windows Terminal 中运行，建议至少 72 列 × 24 行：

```powershell
Set-Location 'D:\100_Projects\110_Daily\test2'
$tt = (Resolve-Path '.\build\package-v0.8.2\bin\tabtail.exe').Path
$data = Join-Path $PWD 'build\manual-v082-data'
& $tt --version
& $tt --data-dir $data tui --lang zh-CN
# 退出后切换英文
& $tt --data-dir $data tui --lang en
```

此命令使用独立测试配置。只验收界面时，无需点击“启用标题定制”或重启 Explorer。

## 人工验收

1. 不移动鼠标：按钮仍有背景，四周留白与相邻按钮边界清晰，不显示方括号。
2. Tab／Shift+Tab 遍历按钮：焦点加粗、下划线；Enter 和空格均触发一次。鼠标悬停有反馈，点击背景留白也可操作。
3. 无标签时，标签编辑、固定、保护、锁定、定位显示为禁用。无规则／目录入口时，其编辑与删除按钮禁用。规则位于首／尾时对应移动按钮禁用。Tab 跳过这些按钮。
4. 执行请求期间，操作按钮禁用；取消、帮助及退出保留可用。请求结束后恢复。
5. F2／F3／F4／F6／F9 逐页检查，F7 切换语言。缩到 72×24，按钮完整换行、行间留空，数据仍可见；工具和设置页可随焦点滚动。
6. 打开编辑框或重启确认框，只检查样式后取消：应用为主要操作，确认有危险色，取消为普通操作，按钮不粘连。

## 自动验证

```powershell
cmake --build build/msvc-lifecycle --config Release --parallel 6
ctest --test-dir build/msvc-lifecycle -C Release --output-on-failure
```

TUI 测试使用生产组件树，校验实际单元格背景／文字颜色、按钮留白命中、悬停、Enter／空格、禁用状态、Tab 跳过及中英文宽窄布局。产物在 `build/msvc-lifecycle/tui-artifacts`，包含文字与逐格样式 JSON。

可选安装 Pillow 后，从实际单元格产物生成预览（字体为本机 Cascadia Mono 和黑体，终端显示仍以自身字体为准）：

```powershell
python tools/render-tui-snapshot.py build/msvc-lifecycle/tui-artifacts/zh-CN-minimum-tabs.cells.json
```

自动测试不向 Explorer 写入外观，也不替代真机鼠标、终端字体及 DPI 验收。
