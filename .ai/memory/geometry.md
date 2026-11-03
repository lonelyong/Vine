# geometry 要点

- **纯内联的 SDK 类必须由本模块某个 TU include**：`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` 的成员全部定义在头文件里，
  DLL 只有在带 `VN_GEOMETRY_LIB` 的 TU 里看到这个类时才会实例化并导出它们。现在由 `Primitive.cpp` 显式列出这五个头。
- 缺了这一步的表现是 **Debug 消费者链接失败**：`LNK2019 __declspec(dllimport) ... Box::Box(...)`；Release（内联）会“碰巧通过”，
  所以只在 Debug 才暴露。`test_proximity` 与 `test_robotics_io` 都曾被它挡住。
- 新增同类的纯内联形状时，同样要在本模块的某个 `.cpp` 里 include 它，或者把成员改成出实参（out-of-line）实现。
- 五个基本体的局部空间：`Box` / `Sphere` / `Ellipsoid` 原点居中；`Cylinder` / `Cone` 沿 Z 跨 `[-h/2, +h/2]`，锥底在 `-h/2`。
