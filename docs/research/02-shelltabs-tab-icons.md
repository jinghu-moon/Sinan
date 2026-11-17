# ShellTabs 标签图标实现调研

日期：2026-11-16。参考目录：`refer/shelltabs-main`。目标：定位 TabTail 0.7.0 只改变任务栏预览图标、未改变原生标签图标的问题。

## 结论

ShellTabs 的图标功能属于它自己创建和绘制的经典 Explorer 标签栏；它没有通过 `WM_SETICON` 替换 Windows 11 原生 WinUI 标签的图标。它能提供绘制与资源管理思路，但不是可以直接移植的原生标签接口。

该项目的 README 开头明确说明：这是经典 Explorer 的 deskband；Windows 11 需要 ExplorerPatcher 等恢复经典工具栏。参考目录下 AGENTS.md 的兼容范围与此一致。

## 代码路径

| 环节 | 位置与行为 |
| --- | --- |
| 自建标签窗口 | `src/TabBandWindow.cpp:957`，`CreateWindowExW` 创建自己的 `WS_CHILD` 窗口 |
| 获取目录图标 | `src/TabBandWindow.cpp:5099`，`LoadItemIcon` 按 PIDL／路径调用 `SHGetFileInfoW`，请求小／大图标及 Shell overlays |
| 图标缓存 | `src/IconCache.cpp`、`include/IconCache.h`，缓存引用管理 `HICON`，按目录身份／规格复用资源 |
| 标签绘制 | `src/TabBandWindow.cpp:4571`，在当前标签矩形内调用 `DrawIconEx`，位置和尺寸来自自己的布局计算 |
| 固定标识 | `src/TabBandWindow.cpp:4561`，固定时先画 `DrawPinnedGlyph` 并增加文字偏移；目录图标仍按后续逻辑绘制，属于独立标记 |

因此，ShellTabs 控制的是自己标签的绘制目标，图标改变立即反映在这个目标上。TabTail 操作的是 Windows 11 已存在的原生标签，需要修改对应 XAML 对象。

## 对 TabTail 的修正

0.7.0 曾通过 `CBrowserHost::SetIcon` 修改窗口图标，并以 `WM_GETICON` 位图读回判断成功。用户截图证明这条路径影响任务栏预览，不能证明原生标签已改变；此前“标签图标已确认”的结论应撤回。

0.7.1 改为：

1. 用原生 GUID 和 Explorer 进程创建时间定位对应 `TabViewItem`。
2. 从已批准的圆润实心图钉／锁 SVG 几何构造 `PathIconSource`，保留图钉旋转、锁的镂空填充规则与独立颜色。
3. 替换该实例的 `IconSource`；保留原值和原来的本地／继承来源。Explorer 导航更新原图标时重新保存最新来源。
4. 从该标签根对象实时遍历 `VisualTreeHelper`，核对实际生成的 `PathIcon` 几何、颜色和非零尺寸，避免诊断回调缓存遗漏新控件而误判。
5. 解除状态、显式 `system`、正常停止或租约到期时恢复原图标源；本地原值恢复需要对象身份读回确认。
6. 移除原生窗口图标替换和 SetIcon Hook，保留任务栏预览的系统行为。
7. 将 XAML 首次初始化的 `E_PENDING` 作为正在初始化，执行有界重试；仍未完成时报告待确认，不能发出完整成功通知。

整个过程保持标签实例级身份隔离，不修改目录自身的图标设置，不创建替代标签栏。ShellTabs 源码没有复制进 TabTail。

## 验证边界

用户已经看到开发测试期间标签图标成功替换。这补充了人工视觉证据，但最终发布包仍需重复检查固定 → 锁定 → 解除锁定 → 取消固定，确认任务栏预览图标不变。同路径双实例、正常停止、租约到期与系统图标恢复仍由独立 acceptance 探针和发布测试覆盖。
