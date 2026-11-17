# 0.9.0：TUI 功能覆盖验收

完整入口映射见 [04：功能自检](04-tui-feature-audit.md)。本次新增工具页详细检查、配置与安装两个分组，以及预览／导入／安装管理表单；各中文操作按钮保持四字。

```powershell
Set-Location 'D:\100_Projects\110_Daily\test2'
$tt = (Resolve-Path '.\build\package-v0.9.0\bin\tabtail.exe').Path
$data = Join-Path $PWD 'build\manual-v090-data'
& $tt --data-dir $data tui --lang zh-CN
```

1. F6，Tab 到详细检查：依次查看完整状态、代理状态、配置原文、已存外观、宽度实测。TOML／JSON 报告应可滚动到首末行。
2. 有标签时先 F2 选择一个标签，F6 选标签能力、原生验证、图标探测，再查看报告。发生适配失败或部分成功也应展示结果；重置外观应先确认并仅清除当前标签的临时外观。没有选中标签时相关动作不可提交。
3. F6 → 路径预览：输入 `D:\A\B\C`，层级从 2 改为 3，选择不同分隔符，标题实时变化；不会保存配置或接入 Explorer。
4. F8：通知、恢复、文字加粗、图标来源、规则匹配和启用应直接选择；恢复上限仍为数字输入框。F2 状态筛选也应直接选择。
5. F8 → 目录外观规则：用列表选择已有规则，Enter 载入；新建外观可生成新 ID。改变“区分大小写”，应用并重新载入，值应保留。
6. 在资源管理器或编辑器中准备一个与 `style show` 输出结构一致的 JSON 文件。F6 → 导入外观：无验证时确认导入不可用；检查后显示文件与规则数。修改文件路径需重新检查；确认替换后才保存外观，其他设置保持。损坏文件应保留输入并显示错误。
7. F6 → 安装管理：上述自定义数据目录下，安装／卸载／PATH 写入禁用；查看路径仍可用。还原显示需确认，且只还原层级和分隔符。
8. 仅在确需管理真实安装时，不带 `--data-dir` 启动：`& $tt tui --lang zh-CN`。修改操作都应弹出具体确认；默认取消。卸载可选择清理配置，被占用文件会报告未完成及待清理项。
9. F7 切换英文，分别在 72×24 与 140×36 通过 Tab／Shift+Tab 检查新入口、固定底栏与字段滚动。

## 自动验证

```powershell
cmake --build build/msvc-lifecycle --config Release --parallel 6
ctest --test-dir build/msvc-lifecycle -C Release --output-on-failure
```

组件测试捕获请求，验证真实组件的请求映射、选项持久值、缺少标签禁用、导入验证与确认／过期结果、标准／自定义安装上下文、错误反馈、中英文和窄版布局。服务层在隔离临时目录检查导入尺寸／格式验证、读取不写入、显示还原的范围，以及安装路径保护；不会实际安装、卸载或修改用户 PATH。

原有测试继续覆盖工作区、快捷键与全局输入线程隔离。预览源为 `build/msvc-lifecycle/tui-artifacts/*-coverage-*.cells.json`，可用 `tools/render-tui-snapshot.py` 转为 PNG；真实终端字体、输入法和鼠标手感仍需检查。
