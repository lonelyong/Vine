# ADR-2026-11-07 incremental API shape

- Status: Accepted
- Date: 2026-11-07
- Related: `.ai/design/crypto.md`

## Background

Digests and MACs each come in two forms: the data is already in memory, or it arrives in blocks. The repository used to cover both with `Hash` (one static function per algorithm)
and `HashCalculator` / `HmacCalculator`. Once the vocabulary merged into `Digest`, the naming problem showed itself:

- `Calculator` is a word we invented; the industry has no such name, so a reader has to guess what it does first.
- One concept had two entry points: `Hash::sha256(...)` (class + static) and `HashCalculator` (class + instance),
  plus `Hmac::compute` and `HmacCalculator` - four spellings.
- The incremental object's method names, `write()` / `digest()`, collided with the one-shot entry points, and `digest()` meant both "compute the result" and "that free function".

## Options

1. Keep `*Calculator`. Cost: an invented word, hard to line up with the industry.
2. One class per algorithm (`class Sha256 { update(); final(); }`). Cost: N types of exactly the same shape;
   run-time selection then needs a base class or a variant on top.
3. **A free function for the one-shot form, an object named for the thing for the incremental form**, with the methods uniformly `update()` / `finish()`.
   This is exactly the shape OpenSSL (`EVP_DigestUpdate` / `EVP_DigestFinal`, `HMAC_CTX`), libsodium
   (`crypto_hash_sha256` + `..._state` + `init/update/final`), RustCrypto (`Hasher::update/finalize`),
   Go (`hash.Hash::Write/Sum`) and Qt (`QCryptographicHash::addData/result`) all use.
4. Use static classes throughout (`Hasher::create(Digest)`) - that only hides the state in a handle, one more layer of indirection with no benefit.

## Decision

Chose **3**:

| Concept | One-shot | Incremental object | End |
|---|---|---|---|
| Digest | `digest(Digest, span)` | `Hasher` (`Hasher(Digest)` / `Hasher(name)`) | `finish()` |
| MAC | `hmac(Digest, key, span)` | `Hmac` (`Hmac(Digest, key)`) | `finish()` |
| AEAD | `AesGcm::seal` / `open` | `AesGcm::encryptor` / `decryptor` | `finish()` |

- The incremental object's input method is uniformly `update()` (not `write()`): `write` suggests writing to a file, whereas `update` is
  the verb OpenSSL / libsodium / RustCrypto agree on.
- The end method is uniformly `finish()`, the same as `AesGcm::finish()`; the name `digest()` stays with the free function, so a method name does not collide with a function name.
- The constant-time comparison is renamed `macEquals()`: it is a free function, so its name has to say "compare two MACs" by itself and cannot hang off the `Hmac` class
  (which is now an incremental object, not a namespace).
- `Hasher` uses the industry name (RustCrypto `Hasher`, Go `hash.Hash`); it is not called `DigestContext`, because that is OpenSSL's C-style context name and reads like a
  backend pipeline rather than "the thing being computed".

## Consequences

- The breaking renames happen in one go (every call site inside the repository changes in the same commit): `DigestCalculator` → `Hasher`,
  `HmacCalculator` → `Hmac`, `Hmac::compute` → `hmac()`, `Hmac::verify` → `macEquals()`,
  `AesGcm::write` → `AesGcm::update`. The one-shot and incremental method names no longer overlap.
- The incremental form of the three classes (`Hasher` / `Hmac` / `AesGcm`) is now one gesture: construct → `update()` → `finish()`,
  so a caller does not have to remember a different verb per type.
- The cost: `Hmac` changes from "a static utility class" into "an incremental object", which a long-time reader has to re-learn briefly; what it buys is
  naming that matches experience from outside the library.

## Evidence

- `tests/test_crypto`: 54 cases all green (including the RFC 4231 HMAC vectors, the NIST GCM vectors, chunked consistency and tag failure).
- Plain `std::ostream::write` calls that a bulk replacement hit by mistake during the migration were restored, which shows that this kind of rename has to be confirmed
  call site by call site rather than replaced globally - recorded in `.ai/memory/`.
