# geometry 要点

- **纯内联的 SDK 类必须由本模块某个 TU include**：`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` 的成员全部定义在头文件里，
  DLL 只有在带 `VN_GEOMETRY_LIB` 的 TU 里看到这个类时才会实例化并导出它们。现在由 `Primitive.cpp` 显式列出这五个头。
- 缺了这一步的表现是 **Debug 消费者链接失败**：`LNK2019 __declspec(dllimport) ... Box::Box(...)`；Release（内联）会“碰巧通过”，
  所以只在 Debug 才暴露。`test_proximity` 与 `test_robotics_io` 都曾被它挡住。
- **值类型（`Aabb` / `Obb` / `Line` / `Plane` / `Ray`）相反：类上没有 `VN_GEOMETRY_API`，短查询定义在头文件，不导出也不再显式实例化**，
  消费者各自编译。只有长算法留在 `src/`（`Line::intersectWith(Line)`、`Plane::intersectWith(Plane, Line&)`、`Obb::compute*`），
  这些 `.cpp` 保留 `template class VN_GEOMETRY_API`。改完不需要“某个 TU include 它”这一步。理由与实测见 `design/geometry-primitives.md` 2.8。
- 新增同类的纯内联形状时，同样要在本模块的某个 `.cpp` 里 include 它，或者把成员改成出实参（out-of-line）实现。
- **`Mesh` / `TriangleMesh` / `QuadMesh` / `MixedMesh` 是纯头文件类**（不导出）：成员和 `desc()` 都在头里，消费者各自编译；
  它们的描述符用内联 `VN_OBJECT_META`，靠 `Type::obtain()` 保证全进程只有一个，见 `core.md`。
- 五个基本体的局部空间：`Box` / `Sphere` / `Ellipsoid` 原点居中；`Cylinder` / `Cone` 沿 Z 跨 `[-h/2, +h/2]`，锥底在 `-h/2`。
