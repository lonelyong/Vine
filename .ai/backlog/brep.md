# brep —— 开放待办

> 只写**还没做、有触发条件**的条目；做完删除，决定不做也删除（理由进 ADR）。
> 现行设计见 `../design/brep-operations.md`。

## 1. OpenCASCADE 三角化函数体

- **触发**：有 OpenCASCADE 的构建（`VN_HAS_OCCT`），或需要显示 STEP/BREP 文件。
- **内容**：`Tessellation.cpp` 在 `VN_BREP_HAS_OCCT` 下实现——逐面 `BRepMesh_IncrementalMesh`，`deflection` / `angle` 直接映射，`compute_normals` 时逐顶点法线；面缝不焊接、索引按面朝向。
- **前置**：`include(VineOpenCASCADE)` + `vn_link_opencascade(${BREP_TARGET_NAME} VN_BREP_HAS_OCCT)`。
- **验收**：`tests/test_brep` 增加盒体用例（12 个三角形、法线单位长度、顶点在盒面上）；无后端的构建仍报 `Unsupported`。

## 2. `makeBrep(primitive)`

- **触发**：需要把参数化基本体导出成 STEP/IGES，或运行时布尔需要实体。
- **内容**：`BRepPrimAPI_MakeBox/MakeCylinder/MakeSphere/MakeCone`，椭球用 `gp_GTrsf` 非均匀缩放；胶囊体需要 `BRepAlgoAPI_Fuse`，因此要 `vn_link_opencascade(... BOOLEANS)`。
- **验收**：每个基本体走一遍"造 → 三角化 → 包围盒等于解析尺寸"的往返。

## 3. 布尔与缝合

- **触发**：出现具体需求（例如机器人连杆的差集、装配体融合）。
- **说明**：与第 2 条共用 `TKBO`/`TKBool`；需求出现前不引入。
