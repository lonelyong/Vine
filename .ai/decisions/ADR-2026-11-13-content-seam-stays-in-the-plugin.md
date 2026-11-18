# ADR-2026-11-13: the content bookkeeping half stays in the backend plugin instead of being promoted to an SDK library early

> **Status**: Accepted
> **Date**: 2026-11-13
> **Scope**: `.ai/backlog/graphics.md` (deletes the "promote the content layer into a shared library" entry), `.ai/design/vsg-backend.md` §2.7

## Background

Half of what is inside `src/plugins/gfx_backend_vsg` is **bookkeeping unrelated to vsg** (fact tables, halves, keys, sources, sweeping), guarded by
`scripts/check_vsg_content_seam.cmake` (10 TUs may not mention `::vsg::`). The backlog used to carry "promote it into a shared SDK library",
triggered by a second backend appearing.

## Decision

- Do not promote it. This half **stays inside `gfx_backend_vsg`**, and the seam guard and the design document (`vsg-backend.md` §2.7) stay as they are.
- Delete the backlog entry; when a second backend really appears, **move it across or copy it across** along the seam's boundary - that is when its real shape becomes known.

## Rationale

- `G8`: zero consumers today. Raising a plugin's internal bookkeeping to an SDK library means the install surface, the ABI and the version constraints all pay
  for a backend that does not exist.
- `G1` / `G7`: the seam is already drawn correctly (it is a checkable rule), and promoting changes only the **packaging location**, not the design or the reusability;
  packaging must not precede the consumer.
- A recorded fact: if the promotion is really wanted later, the cost is 10 TUs (about 3.5k lines) + a new target + installed headers; there is no difficulty that
  would vanish by waiting.

## Rejected options

| Option | Rejection reason |
|---|---|
| Promote it into a `vnContent` library now | There is no second backend (`G8`); the ABI / install surface would precede the consumer. |
| Promote it without exporting (target split only) | It only adds a build edge and a directory, reusability is unchanged, and the fact that "it lives in the plugin" becomes harder to read. |
| Keep it in the plugin but drop the seam guard | The guard is the only evidence that this half is reusable; dropping it means losing the boundary. |

## Consequences

- When a second backend appears, the first step is to **move** this half (or copy it and delete the original), not to change one `target_link_libraries`.
- The backlog no longer tracks this; the seam rule in §2.7 of `vsg-backend.md` remains its home.
