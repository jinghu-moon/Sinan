# 04：TUI 功能覆盖自检（0.9.0）

范围：对照 `src/cli/main.cpp` 的实际命令、`src/app/service.cpp` 的服务路由、配置校验和 TUI 组件树。早期设计文档中的提案不等于已实现能力。

## 本次补齐的缺口

| 已有能力 | 0.8.9 的缺口 | 0.9.0 的 TUI 入口／组件 |
| --- | --- | --- |
| tabs.capabilities / probe / visual-probe | 无入口 | F6 → 详细检查 → 标签能力／原生验证／图标探测 → 查看报告；报告类型选择器及可滚动只读文档 |
| tabs.style-reset | 缺少清除临时外观动作 | F6 → 详细检查 → 重置外观；针对 F2 当前标签，确认后清除临时覆盖 |
| agent.start / status | 观察代理启动依赖刷新，完整状态不可查看 | F6 → 启动观察；报告类型 → 代理状态 |
| status / config.show / style.show / appearance.status | 摘要无法查看全部字段 | F6 → 完整状态／配置原文／已存外观／宽度实测；TOML／JSON 可滚动报告 |
| install / uninstall / path status/add/remove | 无入口 | F6 → 配置与安装 → 安装管理；安装、PATH 查询／登记／移除、卸载按钮及配置清理复选框 |
| config.reset | 只有重载保存值，没有恢复默认 | 安装管理 → 还原显示；恢复两级和反斜杠，保留其他配置，提交前确认 |
| preview PATH --depth --separator | 只能预览所选标签或示例 | F6 → 路径预览；路径输入、层级数字框、分隔符选项、实时标题卡片，不连接代理 |
| style.load FILE | 无文件导入入口 | F6 → 导入外观；文件路径输入 → 检查文件 → 确认导入 → 替换确认 |
| tab_style.rules.case_sensitive | 保存表单会遗漏该字段 | F8 → 外观 → 目录外观规则 → 区分大小写复选框；加载、草稿及保存均保留 |
| 已有目录外观规则的定位 | 需记住并输入规则 ID | F8 → 目录外观规则列表，Enter 载入；新增外观按钮生成 ID，仍允许输入 ID |
| 标签状态筛选 | 需输入 all/pinned/protected/locked | F2 → 全部标签／固定／独立保护／目录锁定选项 |
| 通知与自动恢复 | 需输入 all/errors/off 与 true/false | F8 → 通知方式选择器；恢复开关选择器 + 标签数量输入框 |
| 加粗、图标来源、状态外观启用、外观规则匹配／启用 | 有入口但需输入内部值 | F8 改为继承／系统／开启／关闭、内置图标、匹配模式等选项组件 |

导入先读取并验证不超过 200000 字节的 JSON 与字体／颜色／规则，确认后保存已验证的快照；检查过程中修改路径会使结果失效。保存采用原分组基线，配置并发改变时不会静默覆盖。验证失败保留文件路径并在弹窗内显示错误。

安装和 PATH 修改仅针对标准用户目录。使用自定义数据目录启动时，修改按钮禁用，仍可查看 PATH 状态和重置该目录的显示配置。安装、卸载、PATH 写入、显示还原、临时外观重置、外观导入均需具体确认，默认聚焦取消。实际安装不覆盖另一版本；卸载遇到运行中或 Explorer 已加载的文件会报告待清理项。

## 原有可用入口

| CLI／配置能力 | TUI |
| --- | --- |
| start / stop / agent.stop / explorer.restart | 全局启用／停止按钮；F6 维护，保留确认 |
| tabs.list / set / reset / pin / protect / lock / locate / style | F2 标签列表、标签编辑、自动匹配、各状态按钮、定位标签；F8 当前标签临时外观 |
| tabs.batch | F6 批量类型与目标状态，沿用筛选，确认时冻结目标 ID |
| rules.list / add / delete / toggle / move / test | F3 列表与编辑器、状态按钮、上移／下移、测试路径；多次上下移动等价于指定排序位置 |
| config.set depth/separator / appearance.show/set/reset / ui.language | F4 分组表单、宽度选项／数值、还原宽度、语言选择 |
| shortcuts.show/set | F8 快捷键录入，留空解除绑定 |
| entries.list/add/remove/open | F9 列表、新建／编辑、删除、测试打开；启用、子目录别名与标题策略在入口编辑器 |
| pinned.position/show | F9 固定标签位置选项及应用位置 |
| workspace.save/show/restore/automatic / restore.max_tabs | F6 标签存档；F8 自动恢复及上限 |
| 默认／固定／保护／锁定／目录／临时样式；字体、文字、加粗、图标、浅深色状态背景及文字 | F8 外观目标与各组字段；颜色使用 #RRGGBB／system／inherit，字体使用已安装字体名称 |
| doctor | F6 运行诊断；包含系统、安装路径与代理信息 |
| symbols status／prepare／prepare --offline（0.10.0） | F6 → 符号兼容卡片：兼容检查、准备符号、离线解析；不依赖选中标签 |

## 不提供为普通 TUI 开关的项目

| 项目 | 原因与替代 |
| --- | --- |
| agent run | 前台运行代理会占用当前进程，属于 CLI 调试执行模式；TUI 提供后台启动与状态报告 |
| --json、--snapshot、--width/--height、--data-dir | 输出、静态截图、启动上下文参数，继续作为 CLI 参数；TUI 报告可查看原始数据 |
| autostart 配置字段 | 现有运行代码未消费该旧字段，不提供无效开关；实际登录恢复使用 restore.automatic，F8 已提供 |
| pinned_tabs_protect_close=false / locked_navigation 的其他值 | 当前校验明确拒绝，仅支持固定标签防关闭、锁定导航改开新标签；不能做成可选行为 |
| 任意新图片图标、正则、文字图片混排 | 运行接口不支持，属于新功能而非遗漏的 TUI 入口 |

本次不替换图标适配或原生 ABI；打开表单与本地预览不需重启 Explorer。原生验证、图标探测仍受当前 Windows 适配能力限制，报告保留部分成功与错误信息。
