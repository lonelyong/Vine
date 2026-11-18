# ADR-2026-11-07 crypto API shape

- Status: Accepted
- Date: 2026-11-07
- Related: `.ai/design/crypto.md`, `.ai/backlog/crypto.md`

## Background

`vn::crypto` had only digests (`Hash` / `HashCalculator`) and fingerprints. To go on with HMAC, AES-GCM and random bytes,
and to do RSA / ECC along that road, three things had to be settled first: how a cipher object expresses direction, how the incremental form relates to the one-shot form,
and where the third-party library's global initialisation lives. All three harden into the public interface, and changing them later means touching every caller.

## Options

### A. Cipher direction

1. One class plus a `mode` flag (whether it is an `encryptor` is decided by a constructor argument).
2. One class plus two factories (`encryptor()` / `decryptor()`), with the direction fixed at construction and immutable.
3. Two classes (`AesGcmEncryptor` / `AesGcmDecryptor`).

### B. Incremental form

1. Provide a one-shot API only, and let the caller accumulate its own buffer.
2. The incremental API hands input straight to the backend's incremental entry points (true streaming).
3. The incremental API buffers input and computes it in one go at `finish()`, taking the same path as the one-shot form.

### C. Backend initialisation

1. Each type initialises the backend in its own `std::call_once` on first use.
2. One `detail::ensureLibraryInitialized()` inside the module, which every user calls.
3. Require the caller to initialise by hand at startup.

## Decision

- A: chose **2**, one class with two factories. Direction is a fact of construction, not run-time state; `isEncrypting()` exists only for diagnostics.
  Rejected 1: it lets an illegal state - "decrypt with an encryptor" - exist. Rejected 3: the key/IV/AAD checks shared by the two classes would be written twice (G1, G8).
- B: chose **3**, buffer and compute in one go. The reason is correctness first: wolfSSL's incremental GCM entry points have different signatures across versions, and
  the failure mode of a wrong call is **admitting unauthenticated data**, not reporting an error; buffering makes the incremental and one-shot forms take exactly the same code path (which is why `R2` is testable).
  The cost (memory growing with the input) is written into the header's `@warning` and the design page's `P4`; a bounded-memory version stays in the backlog as its own item,
  and it will need its own tests when it is taken up (`D2`).
- C: chose **2**, one entry point inside the module plus `std::call_once`. Rejected 1: with several initialisation sites it is easy to add one for a new type and miss another, and a miss crashes (see below). Rejected 3:
  it pushes the order requirement onto every caller, against "hide complexity, do not transfer it" (G7).

## Consequences

- Public surface: `AesGcm` has no `mode` parameter, a `decryptor` must be given a tag, and after `write()` there is exactly one `finish()`.
- An authentication failure surfaces as `std::runtime_error`, and no plaintext leaves the module; the caller cannot use a return value to "carry on with unauthenticated plaintext".
- The memory cost of incremental GCM is a known defect, recorded in the design page and the backlog rather than hidden in the implementation.
- Backend initialisation goes from "each type handles its own" to "handled in one place": adding RSA / ECC means calling the same function, with no risk of missing it.

## Evidence

- When `wolfCrypt_Init()` has not run before `wc_InitRng()`, the first `Random::fill` hits an access violation at
  `wc_InitRng → wc_LockMutex → EnterCriticalSection` (0xc0000005, confirmed with the cdb stack); `drbgStateMutex` is initialised only by `wc_DrbgState_MutexInit()`, that is by `wolfCrypt_Init()`
  (`wolfcrypt/src/random.c`).
- wolfSSL's headers define macros such as `#define Md5 wc_Md5` and `#define Sha256 wc_Sha256`, which is why this module's algorithm enumerators all carry the `k` prefix;
  that is not a matter of taste but of keeping macro substitution from breaking the enum.
- `tests/test_crypto`: 42 cases pass, including the HMAC RFC 4231 case 2 and AES-GCM NIST SP 800-38D case 2 vectors, chunked consistency and tamper rejection.
