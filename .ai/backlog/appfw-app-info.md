# 应用信息 待办

> 只列开放项；没有触发条件就不做。关闭项直接删除。

## 1. 现在可做（无触发条件）

（无）

## 2. 触发条件型

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T1 | 版本与构建事实从构建系统生成 | CMake 由 `project(VERSION/DESCRIPTION/HOMEPAGE_URL)` + git 提交 + 编译器/Qt 版本 `configure_file` 出一个头文件；`main.cpp` 不再写 `"1.0.0"` | 发第一个对外版本前（版本号要进安装包与问题报告） | `../design/appfw-app-info.md` F5 |
| T2 | 清单文件 + 第三方致谢生成 | `appinfo.json`（版权/许可/链接/致谢）在 **configure 期**读并生成 C++ 初始化；第三方清单从依赖表生成 | 出现第二个应用 target，或发行包需要同一份元数据 | `../design/appfw-app-info.md` F6 |
