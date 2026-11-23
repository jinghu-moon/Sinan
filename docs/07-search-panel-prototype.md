# 07：Sinan 搜索面板交互原型

日期：2026-11-18。目标：先验证原生 GUI 的外观、输入和搜索流程，再决定是否需要 WinUI 3。

## 范围与结构

- 新增可单独构建的 `sinan-panel.exe`，C++20、Win32 原生输入框、Direct2D／DirectWrite 绘制、DWM 窗口圆角；无 WebView、Electron 或新的运行时框架。
- 面板只消费共享应用动作目录及本地目录入口，不链接 TUI 或 Agent 实现。
- Windows 11 Fluent 风格：中性明暗背景、细边框、4 DIP 控件圆角、8 DIP 卡片圆角、DWM 窗口圆角和阴影；实心主按钮、搜索结果与详情分栏。
- 默认跟随 Windows 应用明暗设置；`--theme system|light|dark` 可覆盖。使用 DWM 窗口强调色，并按背景调整对比度；系统主题／配色变化后重绘。主题不写入配置或系统设置。
- 优先使用 Segoe UI Variable，未安装时回退到 Segoe UI。图标为程序绘制的简单向量，不增加图标包或字体依赖。
- 中英文、键盘导航、无结果态、Per-Monitor V2 DPI 缩放。原生 EDIT 前景／背景与外层主题一致，焦点用强调色底线指示。
- Fluent 是本轮的视觉规范，当前实现仍为 Win32／Direct2D，未集成 WinUI 3；未实现 Mica／Acrylic 材质或过渡动画。
- 原型的“预览操作”只展示操作说明，不执行标签修改、导航、脚本、符号下载或代理启动；明确标识“交互原型”。
- `--demo` 提供虚构目录与标签用于视觉评审，演示数据不会保存或写入真实标签状态；未指定时只读取实际本地动作／目录配置。

## 交互

1. 打开面板后聚焦原生搜索框，支持中文 IME、复制粘贴、Ctrl+A。
2. 中文、英文、关键词／部分拼音和路径实时搜索；分类为全部／操作／目录／标签。
3. ↑／↓ 循环选择，PageUp／PageDown 跳页；鼠标单击选择、双击预览，滚轮滚动。
4. Enter 预览所选操作；Ctrl+1…4 切分类；Ctrl+L 聚焦搜索；Esc 先清空搜索，再关闭窗口。
5. 语言按钮切换中英文；中文动作按钮保持四字；列表和详情随语言变化。
6. 按需绘制，不安装全局键盘钩子、不轮询 Explorer，不登记开机启动或 PATH。

## 测试先行

- 主题测试：先增加明暗背景、正文／次要文本／强调色／主按钮对比度断言并确认 Red，再实现调色板。覆盖白、黑、蓝、黄、粉等强调色。
- 模型测试：复用真实动作目录；中英／拼音／路径搜索；分类隔离；演示数据隔离；空结果／选择边界；预览不暗示已经执行。
- 原生窗口 smoke：创建真实 EDIT、设置中文／英文查询、分类／语言／选择、窗口缩放、安全预览、明暗切换、原生 EDIT 颜色及主题消息刷新，输出 JSON 断言结果。
- 依赖测试：面板不链接 Agent runtime／FTXUI；完整和无 TUI 构建均可用；关闭面板选项时不生成面板目标。
- 视觉验收：共享绘制函数导出明暗两套默认／搜索／无结果／英文 PNG，人工查看布局；这些是 WIC 离屏渲染样本，输入框文字用同样式代理绘制，不是桌面窗口实拍。真实 HWND／EDIT 已由 smoke 创建；中文 IME 候选、鼠标和系统阴影仍需真机评审。
- 回归原有 CLI／TUI／Agent 测试；不借原型测试更改用户 Explorer。

## 非本轮交付

正式动作执行／目标捕获、全局唤出快捷键、完整结果列表 UI Automation provider、历史／收藏持久化、GUI 配置页、高对比度专用适配与动画。WinUI 3 未纳入构建；先由用户评审当前视觉结果。

## 启动与评审

在仓库根目录执行以下命令。

```powershell
cmake --build build/sinan --config Release --target sinan_panel --parallel 6
$panel = (Resolve-Path './build/sinan/Release/sinan-panel.exe').Path
& $panel --demo
# 分别查看两种主题，关闭前一个窗口后运行下一条
& $panel --demo --theme light
& $panel --demo --theme dark --lang en
```

建议检查：输入“固定”／“guding”／“pin”；输入路径片段；用 Ctrl+1…4 分类、↑↓／PageUp／PageDown 选择和滚动；Tab／Shift+Tab 切换可见按钮并用 Enter 激活；空结果清空搜索；切换中英文；缩放窗口及移动到不同 DPI 显示器。预览按钮有真实按钮背景；不使用方括号模拟按钮。

原型从 `actions.list` 获取共享动作，从 `features.get` 读取已启用目录；`--demo` 加入六条虚构目录／标签。非演示模式暂不扫描真实标签。主按钮始终为“预览操作”，不承诺已执行。

## 可复现测试

```powershell
ctest --test-dir build/sinan -C Release -R 'panel_|dependencies' --output-on-failure
& $panel --demo --smoke-report "$PWD\build\panel-window-report.json"
& $panel --demo --snapshot-dir "$PWD\build\panel-fluent-artifacts"
```

渲染样本位于输出目录的 `light/`、`dark/`，每种四张；真实窗口 smoke 输出 JSON。自动测试使用临时数据目录，不登记 PATH，不重启／导航／注入 Explorer，不启动定制代理。完整回归仍包含原有隔离的只观察代理测试。

## 执行记录

- 原始原型 Red：缺少 `sinan_panel` 构建目标；添加目标与实现后，模型和窗口测试通过。
- Fluent Red：主题单元测试因缺少 `panel/theme.hpp` 编译失败；实现调色板后通过。
- 视觉检查：150% DPI，1296×921 的明暗默认、搜索、无结果、英文共八张离屏图；未见文字重叠或按钮裁切。
- 桌面自动化通道返回 native pipe unavailable，本轮未完成真实鼠标／IME 候选操作或真实桌面截图，不能以离屏图替代这些人工验收。
- 分发保持可选：CMake 安装／压缩包纳入面板；用户安装器仅在来源包存在面板时复制，卸载时清理。没有面板的构建继续使用原有八个运行文件。未执行会修改本机 PATH 的真实用户安装。
- 完整构建回归 25/25、关闭 TUI 构建回归 24/24 通过；两种安装目录的文件清单／许可证／本地命令检查均通过，完整包的进程角色与隔离检查通过。
- `SINAN_BUILD_PANEL=OFF` 在全新目录配置通过，确认未生成 GUI 项目或 GUI 安装条目；该变体未重复编译公共实现。
- 依赖回归最初识别到 VS 生成文件还含有不带 `Include` 的默认 `ProjectReference` 元数据；测试改为只遍历 `ItemGroup` 中的实际项目引用后，依赖边界检查通过。
