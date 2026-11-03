# brep —— 开放待办

> 只写**还没做、有触发条件**的条目；做完删除，决定不做也删除（理由进 ADR）。
> 现行设计见 `../design/brep-operations.md`。

## 1. 在带 OpenCASCADE 的构建里验证三角化与构造

- **触发**：有 OpenCASCADE 的构建（`VN_HAS_OCCT`），或需要显示 STEP/BREP 文件，或需要把参数化基本体导出成 STEP/IGES。
- **内容**：`Tessellation.cpp` 与 `Construction.cpp` 的 `VN_BREP_HAS_OCCT` 分支都已实现；需要在真实后端下编译并跑用例，按实际 OCCT 版本修正 API 差异（`Poly_Triangulation` 访问器、`IMeshTools_Parameters` 字段、`BRepBuilderAPI_GTransform` 签名）与实际需要的模块表（`TKPrim` / `TKG3d` / `TKGeomBase`）。
- **前置**：无需改动，`vn_link_opencascade(${BREP_TARGET_NAME} VN_BREP_HAS_OCCT)` 已接，`TKPrim` 已在基础模块表里。
- **验收**：`tests/test_brep` 的盒体三角化用例（12 个三角形、法线单位长度、顶点在盒面上）与 `everyPrimitiveFillsItsAnalyticBounds`、`theSolidCarriesThePrimitiveName` 在带后端的构建里通过；无后端的构建仍走空值分支。

## 2. 布尔与缝合

- **触发**：出现具体需求（例如机器人连杆的差集、装配体融合、胶囊体）。
- **说明**：用 `TKBO` / `TKBool`，通过 `vn_link_opencascade(... BOOLEANS)` 打开；需求出现前不引入。
