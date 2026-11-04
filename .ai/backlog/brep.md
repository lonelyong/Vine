# brep —— 开放待办

> 只写**还没做、有触发条件**的条目；做完删除，决定不做也删除（理由进 ADR）。
> 现行设计见 `../design/brep-operations.md`。

## 1. 布尔与缝合

- **触发**：出现具体需求（例如机器人连杆的差集、装配体融合、胶囊体）。
- **说明**：用 `TKBO` / `TKBool`，通过 `vn_link_opencascade(... BOOLEANS)` 打开；需求出现前不引入。
