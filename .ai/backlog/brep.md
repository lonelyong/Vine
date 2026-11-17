# brep backlog

> Only items that are **not done and carry a trigger condition**; a finished item is deleted, and so is one that is
> decided against (the reason goes to an ADR).
> Current design: `../design/brep-operations.md`.

## 1. Booleans and sewing

- **Trigger**: a concrete need appears (a robot link's difference, an assembly fuse, a capsule).
- **What**: `TKBO` / `TKBool`, switched on through `vn_link_opencascade(... BOOLEANS)`; not introduced before the need
  appears.
