# appfw Ribbon 待办

> 只列开放项；没有触发条件就不做。关闭项直接删除。

## 1. 现在可做（无触发条件）

- **Ribbon 命令状态绑定**：按钮/菜单项/Backstage 条目跟随 `CommandManager` 的 `enabled`、
  `NeedsDocument`/`NeedsView`（当前目标不满足时灰显）与 `CommandInfo::description`（tooltip）；
  勾选态用可选 provider。真实消费者：model_viewer 的五档着色按钮没有勾选态。
  约束：`NotApplicable` 不表达成禁用（禁用是持久化偏好）；新增 `CommandManager::commandFlags(name)`
  而不是给 `CommandInfo` 加字段（ABI）。见 `../design/appfw-ribbon.md` §9.2。

## 2. 触发条件型

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T1 | 显式 `order` / 并列排序 | 同一锚点上按业务顺序而不是注册序排列 | 两个插件都锚定同一位置，且需要稳定的业务次序（不是加载序） | `../design/appfw-ribbon.md` §9.2 |
| T2 | item id 与无命令控件 | `RibbonButton`/`RibbonAction` 的显式 id、`addControl` 的 id | 用户自定义布局落地，或同一个组里出现两个无命令控件需要区分 | `../design/appfw-ribbon.md` F6 |
| T3 | 用户自定义布局持久化 | 按 id 隐藏/移动 tab/group/item 并保存 | 产品要求“用户可自定义功能区” | `../design/appfw-ribbon.md` F7 |
| T4 | 上下文 tab | 按条件显示/隐藏 tab | 出现“选中某类对象才显示”的真实功能 | `../design/appfw-ribbon.md` F8 |
| T5 | 声明式贡献（manifest） | 插件在清单里声明 tab/group，宿主解析 | 出现进程外/第三方二进制之外的插件分发方式 | `../design/appfw-ribbon.md` D5 |
| T6 | group 级 role | 组也能按语义锚定 | 标准 tab 内出现多个可扩展的语义组，且 id 不足以表达 | `../design/appfw-ribbon.md` §2.7 |
| T7 | Backstage 自定义页面内容 | 页面主体可嵌任意 `Control`（选项页/最近文件页） | 需要一个页面承载非命令内容（例如配置窗口、最近文件列表） | `../design/appfw-backstage.md` F7 |
| T8 | 最近文件 / MRU | Backstage 的“最近”页与文件入口 | 产品要求最近打开列表，且已有持久化位置 | `../design/appfw-backstage.md` F8 |
