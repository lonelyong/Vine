# geometry 要点

- **纯内联的 SDK 类必须由本模块某个 TU include**：`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` 的成员全部定义在头文件里，
  DLL 只有在带 `VN_GEOMETRY_LIB` 的 TU 里看到这个类时才会实例化并导出它们。现在由 `Primitive.cpp` 显式列出这五个头。
- **参数化几何是建模类型**：`Primitive : Shape` + `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` **全部在 `Primitive.hpp` 一个文件**里
  （原来五个分开的头已删）。子类把 `ShapeType` 传给受保护构造，不再手写 `shape_type_`。
  消费点：`obj_cast<vn::geometry::Box>`、`switch (shapeType())`；它们**不提供**几何算法（射线–盒用 `Aabb::intersectWith`，
  射线–球面属于 `SphericalSurface`）。理由见 `design/geometry-parametric.md`、`decisions/ADR-2026-11-04-primitive-is-a-shape.md`。
- **值类型（`Aabb` / `Obb` / `Line` / `Plane` / `Ray`）相反：类上没有 `VN_GEOMETRY_API`，短查询定义在头文件，不导出也不再显式实例化**，
  消费者各自编译。只有长算法留在 `src/`（`Line::intersectWith(Line)`、`Plane::intersectWith(Plane, Line&)`、`Obb::compute*`），
  这些 `.cpp` 保留 `template class VN_GEOMETRY_API`。改完不需要“某个 TU include 它”这一步。理由与实测见 `design/geometry-primitives.md` 2.8。
- 新增同类的纯内联形状时，同样要在本模块的某个 `.cpp` 里 include 它，或者把成员改成出实参（out-of-line）实现。
- **`Mesh` / `TriangleMesh` / `QuadMesh` / `MixedMesh` 是纯头文件类**（不导出）：成员和 `desc()` 都在头里，消费者各自编译；
  它们的描述符用内联 `VN_OBJECT_META`，靠 `Type::obtain()` 保证全进程只有一个，见 `core.md`。
- 五个基本体的局部空间：`Box` / `Sphere` / `Ellipsoid` 原点居中；`Cylinder` / `Cone` 沿 Z 跨 `[-h/2, +h/2]`，锥底在 `-h/2`。
