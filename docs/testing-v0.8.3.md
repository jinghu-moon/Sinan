# 0.8.3：详情与概览可读性

标签详情划分为标题显示、保护状态、外观状态、窗口信息；存在错误时增加诊断分组。小标题使用青色与菱形分隔，标题预览和层级加粗，字段标签与值对齐。空提交显示“尚未提交”。

状态使用颜色和文字／符号共同表达：

| 表示 | 含义 |
| --- | --- |
| 绿色 `✓ 已确认` | 目标适配器已返回确认结果 |
| 黄色 `○ 未确认` | 已有验证结果，但尚未确认成功 |
| 低对比色 `— 暂无结果` | 未收到对应验证结果 |
| 红色 `!` | 原始诊断错误，完整保留 |
| 彩色实心 `●`／低对比空心 `○` | 保护开关开启／关闭 |

保护开关表示配置意图，行为／外观验证表示实际结果，二者分开显示。外观优先采用包含图标结果的 `appearance_confirmed`；只返回旧样式结果时回退到 `style_applied`。这些适配器结果不替代视觉验收。

策略概览去掉重复标题，字段与数值分列对齐，已应用显示为“已应用数量 / 总数”（沿用 `title_applied` 状态）；匹配优先级按 1、2、3 和箭头纵向排列。长路径和无空格中文按完整字符换行，保留组合字符。窄窗口继续使用紧凑详情。

## 启动验收

```powershell
Set-Location 'D:\100_Projects\110_Daily\test2'
$tt = (Resolve-Path '.\build\package-v0.8.3\bin\tabtail.exe').Path
# 使用独立测试目录，避免连接旧路径程序的代理
$data = Join-Path $PWD 'build\manual-v083-data'
& $tt --data-dir $data tui --lang zh-CN
# 退出后检查英文
& $tt --data-dir $data tui --lang en
```

检查 110 列两栏、至少 170 列三栏，以及 72×24 紧凑布局。查看空提交、不同开关组合和已有的适配错误，确认文字完整、状态能同时通过文字和符号辨认。中英文切换不得丢失选择与草稿。只检查界面时无需点击启用、停止或重启。

## 自动检查

```powershell
cmake --build build/msvc-lifecycle --config Release --parallel 6
ctest --test-dir build/msvc-lifecycle -C Release --output-on-failure
```

TUI 检查覆盖中英文宽窄布局、完整保护标签、缺失验证结果、图标失败但文字样式成功的部分确认、实际状态单元格的颜色与加粗，以及长路径和组合字符完整性。`build/msvc-lifecycle/tui-artifacts/*-detail-*.cells.json` 可用 `tools/render-tui-snapshot.py` 生成视觉预览；预览字体与实际终端字体可能不同。
