---
name: Framework Design Constraints
description: 'Use when designing or reviewing a framework, module boundary, layering, public API, class responsibility, extension point, or writing a design doc / ADR / refactor plan: how to cite the G1-G12 constraints, what a design point must state, and when a violation is acceptable.'
applyTo:
  - '.ai/design/**'
  - '.ai/decisions/**'
  - 'src/**/sdk/**'
  - 'docs/guides/**'
---

# Framework Design Constraints — How to Apply

The twelve principles `G1`–`G12` and the question each one turns into live in
`.github/copilot-instructions.md` (`## Framework Design Constraints`). Read them there; this
file does not repeat the table.

## Rules

- **Name the principle, not the taste.** In a review or a design discussion write "this is
  `G7`: the complexity moved into a callback the caller must now learn", not "this is ugly".
  Check correctness first, then `G1`–`G12`, then style.
- **Design points state the trade-off.** A design point in `.ai/design/` says which `G#` it
  serves and which one it pays for. A point that trades away a principle names the principle.
- **Rejected options name their `G#`.** A `D#` row says which principle the option would have
  violated, or which one it was trying to protect — that is why it lost.
- **Non-goals and known gaps are `G8` evidence.** An extension point without a consumer today
  belongs in `.ai/backlog/`, not in the code.
- **Violations are allowed, unstated ones are not.** If a deadline, an ABI constraint or a
  measured cost wins over a principle, write the number and the reason where the code lives
  or in the ADR.
- **One fact one home.** Do not restate a header contract, a test or a principle body in a
  design doc; cite the `G#`, the header or the test name.

## Scope

These rules apply to design docs, ADRs, public SDK headers and user guides. They do not
require a design doc for internal refactors that change no boundary — `G8` applies there too.
