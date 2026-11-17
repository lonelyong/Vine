# core notes

- **A type descriptor has exactly one entry point, `vn::Type::obtain()`**: it looks the registry up by
  `typeid`, creates the descriptor when it is missing and returns the registered one otherwise. However
  often one type is asked for (inline copies, a static library linked into several images) it yields one
  `Type*`, so comparing `TypeId` by address still holds (`isKindOf` / `obj_cast` / `EventBus` / the DI
  container all key on it). A descriptor is never destroyed, its constructor is private, and `obtain()`
  is the only way in.
- **Two declaration forms, chosen by where the definition lives**:
  - The definition is entirely in the header (the `Mesh` family, types inside tests) → the inline
    `VN_OBJECT_META(Sub, Parent, ...)`; no export and no extra TU.
  - A class a module exports (`class VN_X_API Name`) → `VN_OBJECT_META_DECL` plus
    `VN_OBJECT_META_IMPL` in that module's `.cpp`, one definition.
  - Interfaces: `VN_DECLARE_INTERFACE(Itf)` in the header (no parent interface) and
    `VN_DEFINE_INTERFACE(Itf, ...)` in the module's `.cpp` (with the parents). The core interfaces are
    all defined in `src/base/core/src/Interfaces.cpp`, and the interface class itself carries
    `VN_CORE_API` (otherwise a consumer gets LNK2019).
- Historical trap: `desc()` used to `new Type(...)` directly, and the constructor threw
  `ITEM_ALREADY_EXISTS` on a duplicate - inside the `noexcept` `Object::getType()`, so the result was
  `std::terminate`, exit code 3 and no gtest failure at all. A duplicate registration is not an error any
  more, but **never build a descriptor without going through `obtain()`**.
