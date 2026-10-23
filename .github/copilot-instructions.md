# C++ Coding Guidelines

## Framework Design Constraints

These twelve principles define what "well designed" means in this repository. They are
**constraints, not aspirations**: a change that violates one must say so, and say why, at the
point of violation — in its design doc or in an ADR. Cite them as `G#` from `.ai/design/` and
from review comments instead of restating the principle.

| # | Principle | The question it turns into |
|---|---|---|
| G1 | 概念清晰，抽象自然 | Does each core concept have one definition, one owner and a stated boundary? Does a new abstraction remove more complexity than it introduces? |
| G2 | 职责分明，层次合理 | Does the edge point toward `base` only — no back edge, no cross-layer shortcut? |
| G3 | 流程清晰，逻辑自然 | Can create → configure → run → get result → release be read in one pass, with a simple job staying simple? |
| G4 | 接口简洁，语义明确 | Do name, parameters, return value, ownership, lifetime and failure mode mean what they say, without an unwritten convention? |
| G5 | 内聚而低耦合 | Does the expected change stay inside the module? If it must cross a boundary, is the boundary drawn wrong? |
| G6 | 统一而一致 | Do naming, error handling, resource management and abstraction level match the rest of the repo — and where they don't, is the deviation justified? |
| G7 | 隐藏复杂性，不转移复杂性 | Is the complexity absorbed here, or pushed into one more layer / config / callback the caller now has to learn? |
| G8 | 保持克制，避免过度设计 | Is there a real consumer for this extension point today? Would merging these two concepts lose a distinction that actually exists? |
| G9 | 易于扩展，持续演进 | Can a new format, device or algorithm be added without editing the core model? |
| G10 | 关注真实成本 | Are lifetime, ownership, allocation, layout, exception safety, concurrency and ABI costed — not only drawn? |
| G11 | 易于理解、调试、维护 | Can a failure be located, state observed and the module tested alone, without guessing an implicit mechanism? |
| G12 | 设计意图体现在代码中 | Do class boundaries show responsibility, interface shape show constraints, dependencies show architecture, names show the domain? |

How a design point, a rejected option or a review cites a `G#` — and when a violation is
acceptable — is in `.github/instructions/framework-design.instructions.md`.

## Project Layout & Documentation

* `docs/` is the unified repository documentation folder AND the GitHub Pages
  site root: user/developer-facing docs live under `docs/guides/` (so the site
  can serve them later), while `index.html` + `CNAME` at `docs/` are the site
  itself. Do NOT place design docs or notes there — those belong in `.ai/`.
* `.ai/` holds AI-visible, repo-versioned knowledge, split by the question it answers:
  * `.ai/design/` — **current, normative** design: what the system IS now. Stable
    `<area>-<topic>.md` names, **no date suffix**, no closed or historical items, one topic
    per file, aim for ≤300 lines.
  * `.ai/decisions/` — **ADR** (`ADR-YYYY-MM-DD-<slug>.md`): why a decision was made, the
    options rejected and their consequences. Dated, immutable; mark `已取代` and link the
    successor. This is the only history kept in-tree.
  * `.ai/backlog/` — **open, trigger-gated** work only (`<area>.md`, no date suffix).
    Closed or explicitly not-doing items are deleted, not marked.
  * `.ai/memory/` — concise current facts, traps and commands; no chronology.
  * `.ai/bugs/` — one postmortem per bug.
  * `.ai/research/` — dated, non-authoritative proposals/experiments; convert to
    design/ADR and delete when done.
* `docs/guides/` is user-facing; internal design, decisions and backlog never go there.
* **Design docs**: one fact one home. Contracts live in headers, behaviour in tests; a
  design doc summarises and links, never duplicates. Every design doc uses the same
  numbered chapters, with sub-numbers when a chapter has parts:
  1. **概要设计** — 1.1 problem/background, 1.2 goals, 1.3 non-goals, 1.4 one-paragraph conclusion.
  2. **设计要点** — `2.x`, one point each: what / why / boundary.
  3. **功能点** — `3.x` groups; a feature index table, then each `F#` states behaviour,
     constraints (`C#`), requirements (`R#`), acceptance (`T#`) and caveats (`N#`).
  4. **公开接口** — grouped by class/module, contract + pointer to the header.
  5. **约束、要求与不变量** — global `C#` / `R#` / `N#`; a feature may reference these.
  6. **注意事项与陷阱** — `P#`; each says how it fails, how to detect it, how to prevent it.
  7. **已否决选项** — `D#`, one line + ADR link.
  8. **测试与验收** — `T#` criterion + test case/command.
  9. **状态与演进** — current status, known gaps/backlog links, compatibility/migration.
  10. **参考** — code, docs/ADR, external sources.
  Put a status / last-verified / scope block at the top; Mermaid diagrams sit next to the
  section they explain. Stable IDs (`F#`, `C#`, `R#`, `N#`, `T#`, `D#`) are for
  cross-references. The full template lives in `.github/skills/design-docs/SKILL.md`.
* **History**: never add dated, closed or historical docs to `.ai/design/`; git is the
  archive. Keep only an ADR for a decision likely to be re-litigated.
* Before touching a module, read its `.ai/design/` doc and `.ai/backlog/`; after changing
  any `.ai/` file run `python3 scripts/check_ai_docs.py --strict` and
  `python3 scripts/check_design_docs.py`.

## Language Standard

* Use C++20.
* Prefer standard library solutions.
* Do not introduce third-party dependencies unless explicitly requested.
* Keep code portable across Windows and Linux.

## Naming Convention

### Functions

* Static member functions use camelCase, starting with an lowercase letter (e.g. `getName() const`).
* Instance member functions use camelCase, starting with a lowercase letter (e.g. `setName()`, `loadMesh()`).
* Free (global) functions use camelCase, starting with an lowercase letter (e.g. `getAppConfig()`).

### Property Access (Getters / Setters)

* Use Qt-style accessors:
  * Getters use the plain property name without a `get` prefix (e.g. `name()`, `width()`).
  * Boolean getters use an `is` / `has` prefix (e.g. `isEnabled()`, `hasDefault()`).
  * Setters use a `set` prefix (e.g. `setName(...)`, `setEnabled(bool)`).
* Do NOT use Java-style `getXXX()` / `setXXX()` getters.
* Exception: fluent builder APIs are exempt from this rule. A setter that returns the
  object itself for chaining uses the property name without a `set` prefix and returns
  a reference (e.g. `ConfigItem& description(const String&)`, `ConfigItem& range(int, int)`).


### Fields

Fields use `snake_case`:

* `private` and `protected` fields get a trailing underscore `_` (e.g. `file_path_`, `val3_`).
* `public` fields do NOT get a trailing underscore (e.g. `val1`).
* Class static fields additionally get an `s_` prefix (e.g. `s_count`, `s_val2`, `s_val5_`).
  * **Exception - constants**: a static whose value is fixed for the program's lifetime is a CONSTANT and is named like
    every other constant - a `k` prefix followed by PascalCase (`Color::kWhite`, `Colorf::kTransparent`). `s_` stays
    for statics that hold state (`MemoryStream::s_maxBump`), and a static *accessor* for such a value is a function and
    follows the function rule instead (`Vector3::unitX()`). The prefix is not decoration: it marks what a name IS
    (constant / state / function), so a constant does not get an exemption for being public API vocabulary.
* Under PImpl:
  * The `impl` member does NOT get a trailing underscore (`Impl* const impl;` or `std::unique_ptr<Impl> impl;`).
  * appfw's data blocks are named the Qt way instead - `d` with a `dptr()` accessor - also
    without a trailing underscore (`std::unique_ptr<ApplicationData> d;`). Keep whichever
    name the class family around you already uses.
  * Fields of the `Impl` object do NOT get a trailing underscore.
* The Qt Designer generated UI member `Ui::XXX ui;` does NOT get a trailing underscore, regardless of access level (even when private).
* Local variables use `snake_case` without a trailing underscore.

### File-Scope Constants

* File-scope constants (`constexpr`, `inline constexpr`) use a `k` prefix followed by
  PascalCase (e.g. `kConfigFolder`, `kFirstPaintDeadlineMs`).
* A file-scope object that is not a constant - a registrar kept for its constructor's
  side effect, a mutable global - is not a constant and keeps the `s_` prefix, like
  class and function-local statics (e.g. `s_detachedExceptionHandler`).

### Class Layout

For simple classes, group members with access control specifiers (`public:` /
`protected:` / `private:`) and lay them out in blocks in the following order:

1. Type declarations (`enum`, `using`, `friend`).
2. Constructors / destructor.
3. Methods.
4. Fields.

The block names below (类型声明区块 / 构造函数区块 / 方法区块 / 字段区块) are
labels used ONLY to describe what each block holds. They are NOT comments and
MUST NOT be inserted into the code: write the layout with plain access-specifier
blocks and no block-label comments.

Example:

```cpp
class Cls
{
  public:
    enum Type {};
    using Id = std::uint64_t;
    friend class Xxx;

  public:
    Cls();
  protected:
    Cls(Val v);

  public:
    void method1(int param);
  protected:
    void method2();
  private:
    void method3();

  public:
    int val1{ 1 };
    inline static int s_val2{ 1 };
  protected:
    int val3_{ 1 };
  private:
    int val4_{ 1 };
    inline static int s_val5_{ 1 };

    struct Impl;
    Impl* const impl;  // or std::unique_ptr<Impl> impl;
};
```

## Header Include Order

### .hpp files

Group includes in this order, separated by blank lines:

1. The project `xxxGlobal.hpp` first (if any).
2. C++ standard library headers.
3. Third-party library headers.
4. Local library headers.
5. Same-directory headers (quoted).

Example:

```cpp
#include <string>
#include <vector>

#include <Eigen/xxx>

#include <MYLib/xxx>

#include "local.h"
```

### .cpp files

The first include is the header corresponding to this `.cpp` file; the remaining includes follow the same grouping as `.hpp` files.

### Exceptions

Headers with special requirements (e.g. `GL.h` which must be included first) may be handled specially.


## Comments and Documentation

* Public interfaces must use Doxygen-style comments.
* Do not create section comments.
* Use `/** @brief ... */` format with `@`-prefixed tags.
* Structure a comment in labelled sections, so the key facts are obvious at a glance:
  * `@brief` — one line: what the function does, plus its most important premise or failure mode.
  * `@details` — the facts a caller needs (mechanism, defaults, ranges, limits, cross-references), as `- ` bullets.
  * `@warning` — hazards only: thread safety, unchecked input, settings that must change together. Never boilerplate.
  * `@note` — non-obvious facts that do not warn: why it is designed this way, what it replaces, what it deliberately does not do.
  * `@param` — one tag per parameter, describing its meaning and any constraints.
  * `@return` — the return value / success semantics (required for every non-void return).
* A section appears only when it carries content: never leave one empty, and never fill one with a restatement of the code.
* Constructors and destructors use `@brief` only; use `@param` only for parameters with non-obvious meaning.
* Comments must use `@param` for EVERY parameter of a function, never omit one.
* Prefer multi-line comments; do not condense them into a single line.
* Keep each sentence on one line: wrap to a new line after a sentence completes, so a full sentence occupies one line. A line should generally not exceed 160 characters (you may break earlier to keep a sentence on its own line).

Example:

```cpp
/**
 * @brief Sets the depth range the clip-space depth is remapped into [min_depth, max_depth].
 *
 * @details
 * - The range maps the values a projection produces; it is state, applied to draws recorded after the call.
 * - It can be used together with Reverse-Z: that convention flips this range, the comparison function
 *   and the clear value together.
 *
 * @warning
 * - A reversed range needs the device's depth-clamp behaviour checked before it is used.
 * - The clear value must match the range's far end, or the first frame reads depth from stale values.
 *
 * @note
 * - A depth range is not a depth comparison mode: the test itself belongs to the pass' depth state.
 *
 * @param min_depth Value mapped to the near plane (0.0 by default; 1.0 under Reverse-Z).
 * @param max_depth Value mapped to the far plane (1.0 by default; 0.0 under Reverse-Z).
 */
void setDepthRange(float min_depth, float max_depth);
```

* Comments should explain design intent, constraints, and non-obvious logic.
* Do not add comments that only restate the code.

## Code Quality

* Prefer RAII.
* Avoid raw owning pointers.
* Use `std::span` for non-owning contiguous data access.
* Use `constexpr` where appropriate.
* Use `noexcept` when correctness allows.
* Consider thread safety when shared state is involved.

## API Design

* Do not change existing public APIs unless explicitly requested.
* Preserve existing interfaces and behavior.
* Avoid unnecessary abstractions.
* Prefer simple, maintainable designs.

## File Modification Safety

Before editing any file:

1. Check the latest file contents from the workspace.
2. Do not rely on previously generated code if the file may have changed.
3. Preserve user modifications.
4. Never silently overwrite user changes.
5. If generated changes conflict with user edits, explain the conflict before replacing code.

## Code Generation Rules

When generating code:

* Provide complete compilable implementations.
* Do not provide pseudo-code unless explicitly requested.
* Include required headers.
* Match the existing project style.

## Review Rules

When reviewing code, check:

* Correctness.
* Undefined behavior.
* Memory safety.
* Exception safety.
* Performance issues.
* Thread safety.
* API consistency.
* Naming and style compliance.
* Design against `G1`–`G12`; name the violated principle instead of arguing taste.
