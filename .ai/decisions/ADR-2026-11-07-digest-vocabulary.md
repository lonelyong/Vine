# ADR-2026-11-07 digest vocabulary

- Status: Superseded (the decision that digests have one vocabulary still stands; the class name "Calculator" and the calling convention are superseded by
  `ADR-2026-11-07-incremental-api-shape.md`, and `DigestCalculator` is now called `Hasher`)
- Date: 2026-11-07
- Related: `.ai/design/crypto.md`, `ADR-2026-11-07-crypto-api-shape.md`

## Background

"Pick a hash" had three spellings inside `vn::crypto`: static functions written per algorithm such as `Hash::sha256(data, size)` (10 algorithms × 2 overloads),
the run-time enum `HashCalculator::Algorithm::ALG_SHA256`, and the later `Hmac::Algorithm::kSha256`.
At the same time `Hash`'s return type was fixed per algorithm (`array<uint8_t,16>` … `array<uint8_t,64>`), so a caller could not write code independent of the algorithm:
HMAC, RSA and ECC each had SHA-256 written into their own implementation. `Hash::sha256(std::ostream&)` also dragged I/O into an algorithm header,
and each one-shot call internally wrapped `(ptr, size)` into a `MemoryStream` and read it back.

## Options

1. **Keep the status quo**: one static function per algorithm. The benefit is that there is no risk; the costs are three vocabularies, no generic code, and the I/O dependency.
2. **One class per algorithm** (`class Sha256 { update(); final(); }`, .NET / Java style).
   The cost is that every class has exactly the same shape, so N identical types appear; choosing at run time then needs a base class or a variant, adding a layer of virtual calls.
3. **One vocabulary plus two forms**: `enum class Digest` (the only algorithm identity) plus the free function `digest()` (one-shot) plus
   `DigestCalculator` (incremental); size, name and name resolution are one set of functions.

## Decision

Chose **3**.

- `Digest`'s enumerators are this module's own consecutive numbers, unrelated to wolfSSL's `WC_HASH_TYPE_*`; the mapping lives in one function.
- The one-shot and incremental forms share one backend context and dispatch table in `src/DigestOps.cpp`, so the implementation is written once.
- `digest()` returns `std::vector<std::byte>`: the length is decided by the algorithm, and the caller asks `digestSize()`.
- Every failure throws (`std::runtime_error` / `std::invalid_argument`), including the old "return an all-zero array on failure":
  an all-zero digest cannot be told apart from a real one, and that is exactly the implicit failure G11 asks to avoid.
- `Hash` / `HashCalculator` / `Hmac::Algorithm` are removed. The only consumers in the repository are `ByteSequenceFingerprint` and the tests, both migrated in the same change;
  `ByteSequenceFingerprint`'s public interface is unchanged.

## Consequences

- A caller writes `digest(Digest::kSha256, bytes)`, and when it has a `Digest` from a name or a configuration it only needs `digestFromName`.
- Adding an algorithm edits the `Digest` enum plus the four switches in `DigestOps`, with no new type and no caller change.
- The public API has one breaking change (`Hash` disappears). There is no consumer outside the repository, and the call sites inside it were changed in the same commit.
- The stream usage of `Hash::*(std::ostream&)` no longer has a dedicated entry point; where it is needed, feed blocks into `DigestCalculator`,
  which keeps the I/O dependency apart from the algorithm implementation.

## Evidence

- `tests/test_crypto`: 54 cases all green (known vectors for 9 digests, incremental equal to one-shot, name normalisation, empty input).
- Gates: `check_public_docs` (baseline updated), `check_include_hygiene` and `check_export_annotations` report nothing.
- Two things hit during the port are written into the design page: SHA-3 has only per-variant entry points (`P14`), and a free function in the SDK must be marked `VN_CRYPTO_API` (`P15`).
