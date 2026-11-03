# core 要点

- **类型描述符只有一个入口 `vn::Type::obtain()`**：按 `typeid` 查注册表，没有就建、有就返回已经注册的那个。
  于是同一个类型被问多少次（内联副本、静态库被多个映像链接）都只得到一个 `Type*`，`TypeId` 按地址比较仍然成立
  （`isKindOf` / `obj_cast` / `EventBus` / DI 容器都用它当键）。描述符永不析构，构造私有，只能经 `obtain()`。
- **两种声明形式，按「定义在哪」选**：
  - 定义全在头文件里（`Mesh` 家族、测试内的类型）→ 内联 `VN_OBJECT_META(Sub, Parent, ...)`；不需要导出，不需要额外 TU。
  - 模块导出的类（`class VN_X_API Name`）→ `VN_OBJECT_META_DECL` + 本模块 `.cpp` 里的 `VN_OBJECT_META_IMPL`，一份定义。
  - 接口：头里 `VN_DECLARE_INTERFACE(Itf)`（不带父接口），定义在本模块 `.cpp` 的 `VN_DEFINE_INTERFACE(Itf, ...)`（带父接口）；
    核心接口的定义都在 `src/base/core/src/Interfaces.cpp`，接口类本身带 `VN_CORE_API`（否则消费者 LNK2019）。
- 历史坑：`desc()` 里直接 `new Type(...)`，而构造函数遇重复就抛 `ITEM_ALREADY_EXISTS`——这条路径在 `noexcept` 的
  `Object::getType()` 里，结果是 `std::terminate`、退出码 3、gtest 没有任何失败信息。现在重复注册不是错误，
  但**不要绕过 `obtain()` 直接建描述符**。
