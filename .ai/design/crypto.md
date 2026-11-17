# The crypto module

> **Status**: current
> **Last verified**: 2026-11-07 (`test_crypto` 62 cases pass; `cmake --build build --config Debug` 0 errors)
> **Scope**: `src/base/crypto` (SDK root `src/base/crypto/sdk/vine/crypto`, implementation `src/base/crypto/src`, library `vn::Crypto`)
> **Sources**: the contract comments in the headers and the executable behaviour of `tests/test_crypto`; this page summarises and indexes only, it never restates a contract
> **Backend**: wolfSSL **v5.9.4-stable** (pinned in `src/base/crypto/CMakeLists.txt`; upgrading it means reading its release notes again and re-running `test_crypto`).

## 1. Overview

### 1.1 Problem and background

The repository needs a crypto module that does algorithms and not policy: the layers above (appfw and higher) want
digests, authentication tags, symmetric encryption and random bytes, and do not want to deal with a third-party
library's context initialisation, memory management and "what is returned on failure" conventions.
This module keeps wolfSSL's choice, its initialisation timing and the error contract in one layer, and exposes only
`vn::crypto` vocabulary.

### 1.2 Goals

- Every algorithm has one home; a caller finds it by name and never needs to know the underlying library (G1, G12).
- The one-shot (`hmac()` / `seal` / `bytes` / `digest()`) and incremental (`update` + `finish`) forms exist in pairs;
  feeding the same input in chunks must produce byte-identical output to the one-shot call (G3, G4).
- Failure is explicit: an encryption failure and a rejected authentication tag both raise, and nothing ever returns
  an empty result that "looks fine" (G11).

### 1.3 Non-goals

- No key management, no certificate chain, no trust policy and no choice of password-derivation parameters - those
  belong to appfw and above.
- No TLS, no protocol, no wire format; wolfSSL is built with `WOLFSSL_CRYPT_ONLY`, so the protocol stack stays out
  of the binary.
- No side-channel hardening promise beyond constant-time comparison; the timing-sensitive comparison is implemented
  in constant time in exactly one place, `macEquals()`.

### 1.4 Conclusion

`crypto` is a thin layer of "algorithms + error contract + two input forms", with exactly one holder of backend
initialisation and one holder of the random source; the algorithm families are grouped by use (digest / MAC /
symmetric encryption / random), and a new algorithm adds one class without touching an existing one.

## 2. Design Points

### 2.1 One object, one direction

An `AesGcm` instance either encrypts or decrypts, decided by the `encryptor()` / `decryptor()` factories; there is
no `mode` flag. The reason: the direction is fixed at construction, and expressing it as a flag would allow states
such as "decrypting with an encrypting object"; the factory name is itself the documentation.
Boundary: `isEncrypting()` serves diagnostics and assertions only, never branch selection.

### 2.2 The incremental form must mean the same as the one-shot form

The incremental form is not a second implementation: all three forms feed the backend context directly, `update()`
emits output per chunk and `finish()` closes it. The criterion: whatever way the same input is split, the result must
be byte-identical; the tests verify it by feeding one byte at a time (`T2`, `T4`).
Boundary: the decrypted output of `AesGcm` is **not trustworthy** until `finish()` has verified the tag; that is in the
header's `@warning`, not hidden in the implementation.

### 2.3 Backend initialisation lives in exactly one place

wolfSSL's global state (the random-state mutex, the ASN.1 and DRBG tables) requires `wolfCrypt_Init()` to have run;
taking random numbers before that crashes on an internal lock (see `P2`). Initialisation therefore lives only in
`detail::ensureLibraryInitialized()`, where `std::call_once` guarantees one success with a retry after a failure, and
no other file calls the backend initialiser directly.

### 2.4 Failure never hands out unauthenticated data

On the decrypt path `AesGcm::finish()` IS the tag decision: success means every byte `update()` handed out was real,
failure means all of it is void; the `Hash` family returns an all-zero array when the backend fails, because a digest
has no "authentication" semantics and a status return would pollute the caller's flow (G11).

### 2.5 Names are normalised; case and separators do not matter

`digestFromName()` strips `-` and `_` and lowercases, so `"SHA-256"`, `"sha256"` and `"sha3_512"` are all recognised;
an unknown name throws `std::invalid_argument` instead of silently falling back to a default algorithm.

### 2.6 There is one random source

`Random` holds the process-wide DRBG context and one mutex, and hands requests to the backend in 32-bit chunks. The
reason: the DRBG reseeds itself, so more than one context only costs more; the price is serialisation, and the scale
of this API's use is keys, nonces and salts.

### 2.8 Digests have one vocabulary and two forms

The `Digest` enum is the only spelling of "which hash": the one-shot `digest(Digest, span)`, the incremental `Hasher`,
`Hmac`, `RsaKey` and `EccKey` all select the algorithm with it.
The reason: previously `Hash` (one static function per algorithm), `HashCalculator::ALG_*` and `Hmac::Algorithm`
coexisted, three names for one concept (G1, G6); and the return type was fixed per algorithm, so a caller could not
write code independent of the algorithm.
Boundary: **no class per algorithm** (`class Sha256` and friends) - every algorithm has exactly the same shape, so
splitting adds N identical types plus a layer of run-time selection (G8). The one-shot and incremental forms share one
backend context through `DigestOps`, so the implementation is written once.

### 2.7 Asymmetric keys: one key per object, operations carry their own context

An instance of `RsaKey` IS a key: generating or importing yields an object with the private key, while an object built
from `n`/`e` alone can only verify and encrypt.
Every operation that needs randomness (generate, PSS signing, encrypt/decrypt) requests a backend RNG context for
itself instead of holding one inside the key for its lifetime: the reason is that a shared RNG needs a lock and a
lifetime, while the frequency here is "once per signature".
Boundary: `sign()` / `decrypt()` throw `std::logic_error` when only the public key is present, instead of degrading
into "cannot compute, so hand back a wrong result".

## 3. Features

| # | Feature | Entry point |
|---|---|---|
| F1 | Cryptographically random bytes | `Random::fill` / `Random::bytes` |
| F2 | HMAC, one-shot and incremental | `hmac()` / `Hmac` |
| F3 | Constant-time tag comparison | `macEquals()` |
| F4 | AES-GCM, one-shot | `AesGcm::seal` / `AesGcm::open` |
| F5 | AES-GCM streaming (bounded memory) | `AesGcm::encryptor` / `decryptor` + `update` / `finish` |
| F6 | Digests: one-shot and incremental (one vocabulary) | `digest()` / `Hasher` |
| F8 | ECDSA signatures and ECDH key agreement | `EccKey` |
| F9 | HKDF extract and expand | `Hkdf::extract` / `expand` / `derive` |
| F10 | AES-CBC (PKCS#7, interoperability only) | `AesCbc` |
| F11 | AES-CTR (the caller owns the counter) | `AesCtr` |
| F12 | PBKDF2 password hashing (interoperability) | `Pbkdf2::derive` |
| F13 | Argon2 password hashing (preferred) | `Argon2::derive` |
| F14 | Password-protected private keys (PKCS#8 encryption plus the legacy `Proc-Type` container) | `toEncryptedPrivateDer` / `toEncryptedPrivatePem` / `fromEncryptedPrivateDer` / `fromEncryptedPrivatePem` on `RsaKey` / `EccKey` |
| F15 | Public key from an X.509 certificate | `RsaKey::fromCertificateDer/Pem`, `EccKey::fromCertificateDer/Pem` |

### 3.1 F1 Cryptographically random bytes

Behaviour: `fill(std::span<std::byte>)`, `fill(void*, size_t)` and `bytes(size)` draw from the process-wide DRBG.
Constraints: `C1`, `C2`; requirements: `R1`; acceptance: `T1`.
Notes: `size == 0` is a no-op (`nullptr` is accepted too); `nullptr` with a non-zero length throws
`std::invalid_argument`.
Caveat: `N1` - a failure never returns a partially filled buffer; there is no "acceptable to be non-random" case such
as unavailability, it always throws.

### 3.2 F2 HMAC and F3 constant-time comparison

Behaviour: `hmac(Digest, key, data)` yields the tag in one shot; `Hmac` yields the same tag incrementally through
`update` / `finish`.
Constraints: `C3`; requirements: `R2`; acceptance: `T2`.
Notes: `finish()` may be called repeatedly, the second call returns the cached value instead of finalising the
context again; `update()` after `finish()` throws `std::logic_error`.
`verify` returns `false` immediately when the lengths differ, and otherwise XOR-accumulates byte by byte without an
early return.
Caveat: `N2` - the length check in `macEquals()` is not constant-time (see `P6`); it is usable only when the tag
length is not a secret.

### 3.3 F4 / F5 AES-GCM

Behaviour: `seal(key, iv, plaintext, aad)` returns `{ciphertext, tag}`; `open(...)` verifies the tag and then returns
the plaintext; `encryptor` / `decryptor` accept input split any way, `finish()` emits the output, and the encryption
side then takes the tag with `tag()`.
Constraints: `C4`, `C5`, `C14`; requirements: `R3`; acceptance: `T3`, `T4`.
Notes: the ciphertext length equals the plaintext length and the 16-byte tag is stored separately; `AAD` is
authenticated but not encrypted.
Caveat: `N3` - when decrypting, the output of `update()` is not trustworthy before `finish()` succeeds (`C14`, `P4`);
and an IV must never repeat under one key (`P5`).

### 3.4 F6 Digests

Behaviour: `digest(Digest, span<const std::byte>)` computes a digest in one shot; `Hasher` computes the same bytes
incrementally through `update` / `finish`; `digestSize` / `digestName` / `digestFromName` supply the length and the
name (case, `-` and `_` are all ignored).
Constraints: `C6`; requirements: `R4`; acceptance: `T5`.
Notes: `ByteSequenceFingerprint` is the value-type fingerprint built on top (11 sizes over 9 algorithms, no repeats).
Caveat: `N4` - a digest must not be used as a MAC (that is `Hmac`); the two are different things.

### 3.5 F7 RSA

Behaviour: `RsaKey::generate(bits)` creates a key; `fromPrivateDer` / `fromRawPublic` import, `toPrivateDer` exports;
`sign()` / `verify()` support the PSS (default) and PKCS#1 v1.5 paddings; `encrypt()` / `decrypt()` go through
OAEP + SHA-256.
Constraints: `C7`~`C9`; requirements: `R5`; acceptance: `T6`.
Notes: a signature first hashes the message into a SHA-256 digest and its length equals the modulus length; a
public-key object can only verify and encrypt.
A key can be stored as DER (PKCS#1 private key / SubjectPublicKeyInfo public key) or PEM (`RSA PRIVATE KEY` /
`PUBLIC KEY`).
Caveat: `N5` - three backend defaults run against intuition, see `P9`~`P11` (PKCS#1 v1.5 padding by default, PSS
verify only decrypts without comparing, blinding needs `key->rng`).

### 3.6 F8 ECC

Behaviour: `EccKey::generate(curve)` creates a P-256 / P-384 key; `fromPrivateDer` / `fromPublicX963` import,
`toPrivateDer` / `publicX963()` export; `sign()` / `verify()` are ECDSA (P-256 with SHA-256, P-384 with SHA-384);
`sharedSecret(peer)` is ECDH.
Constraints: `C10`, `C11`; requirements: `R6`; acceptance: `T7`.
Notes: the byte form of a signature is the DER-encoded `ECDSA-Sig-Value` (the same as openssl), and its length varies
between 70-72 / 102-104 with the content.
Caveat: `N6` - the ECDH result is key material, not a usable key; it must go through `Hkdf::derive()` (see F9) before
it can serve as a symmetric key.
A public key has two exports: `publicX963()` (bare point, for protocols) and `toPublicDer()` / `toPublicPem()`
(SubjectPublicKeyInfo).

### 3.7 F9 HKDF

Behaviour: `Hkdf::derive(digest, ikm, salt, info, length)` performs extract + expand in one call; `Hkdf::extract()`
and `Hkdf::expand()` are exposed separately for cases where a protocol has already fixed the salt/PRK.
Constraints: `C12`; requirements: `R7`; acceptance: `T8`.
Notes: the output is capped at `255 × digestSize` (RFC 5869) and exceeding it throws `std::invalid_argument`; salt
and info may be empty.
Caveat: `N7` - HKDF is not a password hash: it is fast by design, and a password must go through a slow KDF (see F12,
F13).

### 3.8 F10 / F11 AES-CBC and AES-CTR

Behaviour: `AesCbc` and `AesCtr` are both streaming objects that follow `create/encryptor` → `update()` →
`finish()`, plus a one-shot `seal`/`open` (CBC) and `apply` (CTR). CBC brings its own PKCS#7 padding and unpadding;
CTR has no padding and its output length always equals the input length.
Constraints: `C15`; requirements: `R8`; acceptance: `T11`.
Notes: both are **for interoperability only**: CBC has no authentication and CTR does not even have padding, so
security rests entirely on the caller pairing them with a MAC. `AesGcm` remains the default.
Caveat: `N8` - a CBC padding error turns into an observable difference (`P19`); the CTR counter is the caller's
responsibility (`P20`).

### 3.9 F12 / F13 Password hashing

Behaviour: `Pbkdf2::derive(algorithm, password, salt, iterations, length)` and
`Argon2::derive(variant, password, salt, parallelism, memory, iterations, length)`; both only derive and never
verify (verification = derive again + `macEquals()`).
Constraints: `C16`, `C17`; requirements: `R9`; acceptance: `T12`.
Notes: a new password uses Argon2id (memory-hard); PBKDF2 exists only because a format or a peer prescribes it, and it
requires the salt only to be non-empty (the RFC 6070 vectors use a 4-byte salt).
Caveat: `N9` - both are deliberately slow, so do not run them on the UI thread; the parameters must be chosen from a
measurement on the deployment machine and stored with the credential.

### 3.10 F14 Password-protected private keys

Behaviour: `toEncryptedPrivateDer(password, iterations)` / `toEncryptedPrivatePem(password, iterations)` write a
private key as a PKCS#8 `EncryptedPrivateKeyInfo` (PBES2 + AES-256-CBC + PBKDF2, random salt and random IV);
`fromEncryptedPrivatePem` / `fromEncryptedPrivateDer` read it back and import it straight into `RsaKey` / `EccKey`.
Each of the four methods is one line: the real coding lives in `detail::encryptPkcs8Der` /
`detail::decryptPkcs8Der`, shared by both key types (G5: password encryption must have exactly one implementation).
Constraints: `C18`; requirements: `R10`; acceptance: `T13`.
Notes: traditional DER (PKCS#1 / SEC1) has no place for a password, so encryption can only come from an outer
structure; what this produces is therefore always PKCS#8, not "an old format with a password bolted on".
The reading side accepts PBES2 as well as the older PBES1 / PKCS#12 password schemes - the parameters are all in the
structure, so all it needs is the password.
Caveat: `N10` - a password-protected file is an **offline** password verifier, and nothing limits the rate of
attempts (`P21`).
Feeding a password-protected block to `fromPrivatePem` fails with a pointer to `fromEncryptedPrivatePem`
(`detail::pemIsEncryptedPrivateKey` / `detail::pemIsLegacyEncrypted` check the label first), instead of letting the
ASN.1 parser report "this is not a private key".

The reading side also accepts the **legacy** form: the `RSA PRIVATE KEY` / `EC PRIVATE KEY` label is unchanged and the
parameters sit in the body's `Proc-Type: 4,ENCRYPTED` and `DEK-Info: <cipher>,<iv>` headers (what
`openssl rsa -aes128` produced before 2020). The label of this shape lies, so it cannot be recognised by label; the
headers come first (`detail::pemIsLegacyEncrypted`) and decryption is left to the backend's `wc_KeyPemToDer`
(`detail::pemDecryptLegacy`).
Caveat: `N11` - the cipher suite of the legacy form is named by `DEK-Info`, so whether it can be opened depends on
which algorithms the build enabled (`P23`).

### 3.11 F15 Public key from an X.509 certificate

Behaviour: `RsaKey::fromCertificatePem/Der` and `EccKey::fromCertificatePem/Der` pull the `SubjectPublicKeyInfo` out
of a certificate and hand it to the existing public-key importer. The implementation is only
`detail::certificatePublicKeyDer` (`src/Certificate.cpp`, a single `wc_GetSubjectPubKeyInfoDerFromCert`), so each of
the two classes is a one-line delegation.
Constraints: `C19`; requirements: `R11`; acceptance: `T14`.
Notes: this layer treats a certificate only as "a container wrapping a public key". The signature, the validity
period, the issuer, the SAN and the trust chain are policy and belong to appfw; the module makes no judgement.
Only the **first** certificate in a bundle is read - which one to pick from a chain is the caller's decision, not
something an algorithm can decide.
Caveat: `N12` - when the certificate's key type is wrong, the error comes from the public-key importer ("not an RSA
public key") rather than from "not a certificate".

## 4. Public Interface

| Location | Type | Shape |
|---|---|---|
| `vine/crypto/Random.hpp` | `Random` | all-static, stateless |
| `vine/crypto/Hmac.hpp` | free functions `hmac()` / `macEquals()` + the `Hmac` class | one-shot is a function, the incremental object is PImpl, movable, non-copyable |
| `vine/crypto/AesGcm.hpp` | `AesGcm`, `AesGcm::Sealed` | factory + PImpl, movable, non-copyable |
| `vine/crypto/Digest.hpp` | `Digest` + `digest()` / `digestSize()` / `digestName()` / `digestFromName()` | enum + free functions |
| `vine/crypto/Hasher.hpp` | `Hasher` | PImpl, incremental, movable |
| `vine/crypto/ByteSequenceFingerprint.hpp` | `ByteSequenceFingerprint` | value type |
| `vine/crypto/RsaKey.hpp` | `RsaKey`, `RsaKey::Scheme` | factory + PImpl, movable, non-copyable; the factories cover private key / public key / password protection / certificate |
| `vine/crypto/EccKey.hpp` | `EccKey`, `EccKey::Curve` | factory + PImpl, movable, non-copyable; the same four factory groups |
| `vine/crypto/Hkdf.hpp` | `Hkdf` | all-static (extract / expand / derive) |
| `vine/crypto/AesCbc.hpp` | `AesCbc` | factory + PImpl, streaming (`update` / `finish`) |
| `vine/crypto/AesCtr.hpp` | `AesCtr` | factory + PImpl, streaming |
| `vine/crypto/Pbkdf2.hpp` | `Pbkdf2` | all-static |
| `vine/crypto/Argon2.hpp` | `Argon2`, `Argon2::Variant` | all-static |
| `vine/crypto/crypto_global.hpp` | macros | `VN_CRYPTO_API`, namespace macros |

The contract details (parameter constraints, what is thrown, what is returned) are written once, in the headers; this
page does not repeat them.

## 5. Constraints, Requirements and Invariants

- `C1`: Key lengths are accepted only at 16 / 24 / 32 bytes; any other length throws `std::invalid_argument` at
  construction time.
- `C2`: An IV must not be empty; 12 bytes is the recommended GCM length, and combinations outside 1-16 bytes are
  rejected by the backend.
- `C3`: The HMAC key length is unrestricted (the backend hashes a long key on demand), but the tag length is decided
  by the algorithm and cannot be truncated.
- `C4`: An `AesGcm` instance can `finish()` only once; the second call throws `std::logic_error`.
- `C5`: `tag()` is usable only on an encrypting object and only after `finish()`; any other case throws
  `std::logic_error`.
- `C6`: `digest()` throws `std::runtime_error` on failure (it no longer returns an all-zero array); the incremental
  form throws as well and never silently answers 0.
- `C7`: An `RsaKey` modulus must be a multiple of 8 and lie in [2048, 8192]; the public exponent is fixed at 65537.
- `C8`: The plaintext of `RsaKey::encrypt()` is capped at `modulus length − 66` bytes (the OAEP + SHA-256 overhead);
  anything longer throws `std::invalid_argument`.
- `C9`: Signature and encryption schemes cannot be mixed: a PSS signature cannot be verified as PKCS#1 v1.5, and an
  OAEP ciphertext cannot be decrypted as v1.5.
- `R1`: `Random` must be usable for keys, nonces and salts; it must never return a partially filled buffer on any
  failure path.
- `R2`: The incremental and one-shot results must be byte-identical, including with a chunk size of 1.
- `R3`: A decrypted plaintext leaves the module only after the tag has been verified.
- `R4`: The public interface of `ByteSequenceFingerprint` stays unchanged (it has been rebuilt on the new `Digest`
  vocabulary).
- `R5`: A failed verification or decryption in `RsaKey` always answers `false` or throws; it never returns a
  plaintext of suspicious length.
- `C10`: `EccKey` supports P-256 and P-384 only; ECDH requires both sides on the same curve, and a mismatch throws
  `std::invalid_argument`.
- `C11`: An ECDSA signature is DER-encoded and its length is not fixed; a caller must not assume a constant 64/96
  bytes.
- `C12`: The HKDF output length must lie between 1 and `255 × digestSize`; `extract` and `expand` must use the same
  digest.
- `C13`: PEM reads and writes **unencrypted** blocks only; password-protected private keys are outside its scope
  (`wc_KeyPemToDer` can decrypt them, but that needs a password source and error semantics, which is a separate
  discussion).
- `C14`: `AesGcm` processes one chunk at a time: the output of `update()` belongs to that input alone, it is neither
  buffered nor recovered; decrypted output is not trustworthy before `finish()` succeeds.
- `C15`: The `AesCbc` IV is fixed at 16 bytes and the ciphertext must be a multiple of 16; the initial counter block
  of `AesCtr` is fixed at 16 bytes.
- `C16`: The `Pbkdf2` salt must not be empty, `iterations ≥ 1` and the output length ≥ 1 (`length` has no upper bound;
  the caller is responsible for the memory).
- `C17`: For `Argon2`, salt ≥ 8 bytes, `parallelism ≥ 1`, `memory ≥ 8 × parallelism`, `iterations ≥ 1` and output
  ≥ 4 bytes.
- `C18`: A password-protected private key rejects an empty password; `iterations` must lie in `1 .. INT_MAX`, because
  it is written into the PBKDF2 parameters verbatim.
- `C19`: Certificate import accepts the DER / PEM of a single certificate only; an empty input or bytes that do not
  match the structure always yield `std::invalid_argument`, with no partial parsing.
- `R6`: Both ECDH sides must arrive at a byte-identical secret; a signature must be verifiable with the public key
  alone.
- `R7`: `Hkdf::derive` must be byte-identical to "extract then expand"; different `info` values must yield unrelated
  outputs.
- `R8`: The streaming output of `AesCbc` must be byte-equal to the one-shot result, and its padding must be rejected
  by self-verification (bad padding reports an error instead of silently truncating).
- `R9`: The same (password, salt, parameters) must derive the same bytes; changing any one of them must yield a
  different result; and the output must match the RFC vectors byte for byte.
- `R10`: The output of `toEncryptedPrivatePem` must be readable by OpenSSL, which must recover the same public key;
  conversely, an encrypted private key written by OpenSSL must be readable; and a wrong password must report an error
  instead of handing back half a key.
- `R11`: A public key taken from an OpenSSL-generated certificate must match `openssl x509 -pubkey` field by field.
## 6. Caveats and Traps

- `P1`: The wolfSSL headers define `Md5`, `Sha256`, `Sha384` and friends as macros (`#define Md5 wc_Md5`), so an enum
  that reuses those names gets macro-replaced into type names inside a `.cpp` and the compiler reports "qualified name
  in member declaration is illegal". Every enumerator therefore carries the `k` prefix (`Digest::kSha256`), which
  matches the repository's constant naming and sidesteps the problem.
  Detection: the build fails after a wolfSSL header is included and the error points at an enumerator; prevention: do
  not use bare names such as `Sha256` inside the namespace.
- `P2`: `wc_InitRng()` locks the DRBG state mutex internally, and only `wolfCrypt_Init()` initialises that mutex;
  skipping the initialisation faults inside `wc_LockMutex` (this repository measured 0xc0000005).
  Detection: the first `Random::fill` crashes with `wc_InitRng → wc_LockMutex` on the stack; prevention: go through
  `detail::ensureLibraryInitialized()` only.
- `P3`: `vn::crypto::Hmac` shares its name with wolfSSL's global `::Hmac`. Inside `vn::crypto`, `Hmac x;` declares this
  module's type, so the backend context must be written `::Hmac`. Detection: `wc_HmacInit` gets an argument of the
  wrong type; prevention: spell `::Hmac` explicitly in the PImpl.
- `P4`: AES-GCM decryption emits before it verifies: `update()` hands out plaintext right away, while whether the tag
  matches is only known at the last chunk. So **the final destination must not be written before `finish()` succeeds**
  (or must be written somewhere rollback is possible), otherwise unauthenticated bytes have been committed.
  Detection: any call site that commits output before `finish()`; prevention: read into a temporary file/memory, then
  rename/commit after verification.
- `P5`: Reusing a GCM IV under one key destroys both confidentiality and authentication; a random IV must respect the
  12 bytes and the birthday bound, and a counter IV is safe only when the caller can guarantee it never wraps.
- `P6`: The length check in `macEquals` is not constant-time: different lengths return immediately, equal lengths
  compare byte by byte; that is acceptable when the length is not a secret.
  The byte-wise part uses the backend's `wc_ConstantCompare()` (public since 5.9.4) instead of a hand-written loop.
- `P7`: For the wolfSSL fetched through FetchContent, **features are enabled by its CMake option names only**; writing
  the macro name silently does nothing: `WOLFSSL_KEY_GEN` (a macro name) has no effect, the option is
  `WOLFSSL_KEYGEN`; the AES-CTR option is `WOLFSSL_AESCTR`.
  When it is written wrongly, the macro shows up in `options.h` while the library's own compile definitions lack it,
  so `rsa.c` does not export `wc_MakeRsaKey` (LNK2019).
- `P8`: `WOLFSSL_CRYPT_ONLY` only trims the source-file list, it defines no macro. The library sources test
  `WOLFCRYPT_ONLY`, and that macro **changes the layout of structures such as `RsaKey`**, so it must be passed
  `PUBLIC` to every consumer; otherwise the symptom is "signing succeeds, verification is forever false".
  For the same reason `WOLFSSL_SYS_CA_CERTS` must stay off: `asn.c` would reference `GetCA/GetCAByName`, which only
  the TLS layer defines.
- `P9`: The backend's `wc_RsaPublicEncrypt` / `wc_RsaPrivateDecrypt` default to **PKCS#1 v1.5**, so OAEP in this
  module means calling the `_ex` versions explicitly with `WC_RSA_OAEP_PAD` + SHA-256 + MGF1-SHA-256.
- `P10`: `wc_RsaPSS_Verify()` only **decrypts the signature** and returns the PSS data without comparing anything; it
  must be followed by `wc_RsaPSS_CheckPadding()`.
  Treating its return value as "the recovered digest" makes verification forever false.
- `P11`: This build enables `WC_RSA_BLINDING`, and the backend reads `key->rng` during a private-key operation; without
  an RNG bound, decryption fails.
  Call `wc_RsaSetRNG()` before every operation, do not expect `wc_InitRsaKey()` to fill it in.
- `P12`: ECC is the same and easier to trip: under `ECC_TIMING_RESISTANT`, `wc_ecc_shared_secret()` requires
  `key->rng` and returns `MISSING_RNG_E` (-236) when it is absent; `wc_ecc_set_rng()` must come first.
- `P14`: wolfSSL's SHA-3 has no generic entry point: `wc_Sha3_224_Update` / `_Final` / `_Free` come as four separate
  variant sets, and a name such as `wc_Sha3Update` fails to compile outright. The four variants share the `wc_Sha3`
  context type and differ only in entry point.
- `P15`: A free function in the SDK must be marked `VN_CRYPTO_API`. A missing mark does not fail the build; it only
  surfaces as LNK2019 when another library links against it.
- `P18`: **One object of `RsaKey` / `EccKey` does not belong to multiple threads**: verification and key agreement
  bind the per-operation RNG into the key's context (`key->rng`), so two threads sharing one object collide on that
  field. `Random` / `Hasher` / `Hmac` / `AesGcm` are not affected (the latter holds its own context).
- `P19`: A CBC padding error is **observable**: an attacker can tell "padding wrong" from "padding right but the
  plaintext is garbage", which is the padding-oracle attack.
  `open()` / `finish()` report the same error text for both cases, but the timing still differs, so the conclusion is:
  **do not decrypt untrusted data with raw CBC**.
- `P20`: Reusing a CTR counter hands the attacker the XOR of two plaintexts; CTR has no authentication either, so
  flipping a ciphertext bit flips a plaintext bit.
  `AesCtr` provides a mechanism, not protection - if authentication is needed, use `AesGcm`.
- `P21`: A password-protected private-key file IS its password verifier: whoever has the file can try offline, with no
  rate limit.
  Detection: there is no "be patient and a weak password is recovered" case. Prevention: the 200000 PBKDF2 iterations
  by default only raise the unit price; the real line of defence is password strength.
- `P22`: The **public point of SEC1 is optional**. When the file carries no public point, `wc_EccPrivateKeyDecode`
  marks the key `ECC_PRIVATEKEY_ONLY`, after which `publicX963()` / `toPublicDer()` / ECDH all fail outright (while
  `verify()` computes it itself, so it looks like "sometimes it works").
  `EccKey::fromPrivateDer` therefore recomputes the public point with `wc_ecc_make_pub_ex` right after import.
- `P23`: The `DEK-Info` of a legacy encrypted PEM can name an algorithm the build does not have (RC2, DES-EDE3, ...).
  `wc_KeyPemToDer` returns a negative value for that case, indistinguishable from a wrong password; the error text
  therefore says "wrong password or unsupported legacy encoding".
- `P24`: Mutation testing found a fact worth remembering: in a password-protected private-key file **about a quarter**
  of single-byte mutations are still accepted (becoming another key).
  The cause is not a defect in the importer: PBES2 is CBC without a MAC, and the body of an RSA key is opaque integer
  content, so flipping one byte still yields a legal structure.
  Detection: the ASAN command in `.ai/memory/crypto.md` reproduces it; prevention: use an authenticated container when
  tamper detection is needed, not password-encrypted PKCS#8.
- `P16`: wolfSSL's PEM decode entry `wc_KeyPemToDer()` hard-codes `PRIVATEKEY_TYPE` internally and **cannot decode a
  `PUBLIC KEY` block**; it also only understands its own encoding details. This module therefore does the armour
  stripping + base64 decoding itself (`src/Pem.cpp`) and still encodes with `wc_DerToPem()`:
  that way the public-key PEM and the private-key PEM take one path, and whether the label matches is decided by the
  later DER decoder, which reports an accurate error.
- `P17`: On MSVC, `HAVE_GETPID` is mis-detected as present by wolfSSL's `check_symbol_exists(getpid "unistd.h")`
  (a toolchain that only links static libraries reports every symbol as found), so `random.h` declares `pid_t pid;`
  inside `WC_RNG` while Windows has no `pid_t`.
  `src/base/crypto/CMakeLists.txt` presets `HAVE_GETPID` to `OFF` before `FetchContent_MakeAvailable` (a cache
  variable makes the probe skip entirely).
  Detection: the build reports `random.h(...): syntax error: identifier "pid_t"`; prevention: confirm the preset is
  still there after a wolfSSL upgrade.

## 7. Rejected Options

- `D1`: Let `AesGcm` encrypt and decrypt with a `mode` flag telling them apart. Rejected: one more state and one more
  error class; see `ADR-2026-11-07-crypto-api-shape.md`.
- `D2`: Expose wolfSSL's incremental GCM entry point directly as the streaming form. ~~Rejected~~ **Adopted**: 5.9.4
  provides the stable `wc_AesGcm{Encrypt,Decrypt}{Init,Update,Final}` set, and `DecryptFinal` carries
  `WARN_UNUSED_RESULT`, so the "one wrong call lets unauthenticated data through" risk is covered by "the return value
  must be used" plus the header contract; see `ADR-2026-11-07-aesgcm-streaming.md`.
- `D3`: Compare tags with `std::equal` / `memcmp`. Rejected: an early return leaks the position of the difference.
- `D4`: Let `Random` create a context per call. Rejected: it pays initialisation and seeding over and over and buys no
  isolation in return.
- `D5`: Give HMAC its own algorithm enum instead of the shared `Digest` vocabulary. ~~Rejected~~ **Adopted**: `Digest`
  is now the module's only digest vocabulary, and the three parallel vocabularies `Hash` / `HashCalculator` /
  `Hmac::Algorithm` are gone; see `ADR-2026-11-07-digest-vocabulary.md`.
- `D7`: Keep the `*Calculator` naming (a coined word no one else uses). Rejected: replaced by "a free function for the
  one-shot form, the object's own name for the incremental form" - `digest()` / `Hasher`, `hmac()` / `Hmac`, the same
  shape as OpenSSL, libsodium, RustCrypto and Qt; see `ADR-2026-11-07-incremental-api-shape.md`.
- `D6`: One class per digest (`class Sha256` ...) or a common base class by inheritance. Rejected: every algorithm has
  exactly the same shape, so splitting adds N identical types and run-time selection still needs another virtual-call
  layer; `enum + one table` is equally safe and smaller.

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| `T1` | two 32-byte draws differ and non-zero bytes appear; an empty request is a no-op; `nullptr` with a non-zero length throws | `RandomTest.*` |
| `T2` | the RFC 4231 case 2 HMAC-SHA256 vector matches; byte-by-byte feeding equals the one-shot call; names are normalised; a `verify` with a wrong length is false | `HmacTest.*` |
| `T3` | the NIST SP 800-38D case 2 ciphertext and tag match; tampering with the tag, the AAD or the ciphertext throws | `AesGcmTest.MatchesNistSp80038dCase2`, `RejectsATamperedTagOrAad` |
| `T4` | 1000 bytes encrypted as 1 + 509 + the rest equals the one-shot ciphertext; chunked decryption restores it; tampering with any byte or the tag throws at `finish()`; an empty input still authenticates the AAD | `AesGcmTest.SealsAndOpensAChunkedMessage`, `StreamingDecryptRejectsATamperedChunkOrTag`, `HandlesEmptyInput` |
| `T5` | known vectors for the 9 digests, an empty input, incremental equal to one-shot, name normalisation, an illegal name throws; the `ByteSequenceFingerprint` cases keep passing | `CryptoTest.*` |
| `T6` | a 2048-bit key: sign/verify and cross-verification failure for PSS and PKCS#1 v1.5 each, an OAEP round trip, a DER round trip, cross-key rejection, illegal import rejected | `RsaKeyTest.*` |
| `T7` | sign/verify for P-256 and P-384, tamper rejection, a DER round trip, verification with a public point and "a public key cannot sign", both ECDH sides agree, cross-curve rejection | `EccKeyTest.*` |
| `T8` | the RFC 5869 test case 1 PRK and OKM match byte for byte; derive == extract+expand; empty salt/info work; an out-of-range length throws | `HkdfTest.*` |
| `T9` | private/public PEM round trips (correct labels, verifiable from the PEM, a public-key PEM cannot be used as a private key), illegal PEM throws | `RsaKeyTest.RoundTripsThroughPem`, `EccKeyTest.RoundTripsThroughPem` |
| `T10` | the hardening layer: concurrent draws never repeat, every legal AES key/IV combination, illegal parameters rejected, an incremental object keeps working after a move, HKDF boundary lengths, malformed/wrong-label/non-base64 PEM, skip-slot and skip-curve rejection | `CryptoRobustnessTest.*` |
| `T11` | CBC against the NIST SP 800-38A F.2.1 vector (first four blocks) and every length from 0 to 40 bytes padded; chunking at 1/15/17/31 bytes equals the one-shot call; illegal lengths and bad padding report errors. CTR against the F.5.1 vector, any chunking (byte-by-byte included) equals the one-shot call | `AesCbcTest.*`, `AesCtrTest.*` |
| `T12` | PBKDF2 against the four RFC 6070 vectors; Argon2 determinism and parameter sensitivity (variant/memory/iterations/salt) plus the parameter floors | `Pbkdf2Test.*`, `Argon2Test.*` |
| `T13` | password-protected private keys: PEM and DER round trips for RSA and ECC (P-256/P-384), signing/ECDH still work after the round trip, two identical inputs produce different ciphertexts, the ciphertext contains no plaintext private key, it contains the PBES2 and AES-256-CBC OIDs, a wrong password / empty password / 0 iterations / exporting the public key are all rejected, a fixed OpenSSL-generated fixture decrypts, a password-protected block fed to `fromPrivatePem` reports an error pointing at `fromEncryptedPrivatePem`; the legacy `Proc-Type` fixture decrypts and a wrong password is rejected | `EncryptedRsaKeyTest.*`, `EncryptedEccKeyTest.*` |
| `T14` | the public keys taken from OpenSSL-generated RSA and P-256 certificates match `openssl x509 -pubkey`; a mismatched key type and "right label but not a certificate" are both rejected; only the first certificate of a bundle is read | `CertificateTest.*` |
| `T15` | mutation testing of the parse entry points: 240 fixed-seed mutants each for private/public DER, X9.63 points, encrypted private keys and PEM text, asserting no crash and that an accepted result must be a usable key; GCM accepts not one tampered ciphertext or tag | `ParserMutationTest.*` |

Command: `cmake --build build --config Debug --target test_crypto`, then `build\bin\Debug\test_crypto.exe`.

The memory check (this machine, MSVC AddressSanitizer) is the `build-asan` command in `.ai/memory/crypto.md`; all 114
cases currently pass with no ASAN report.

## 9. Status and Evolution

- Implemented: digests, HMAC (one-shot + incremental + constant-time verification), AES-GCM (one-shot + **bounded-memory
  streaming**), random bytes, backend initialisation, RSA (generate / DER and PEM import/export / PSS and PKCS#1 v1.5
  signatures / OAEP encrypt/decrypt), ECC (P-256 / P-384, ECDSA, ECDH, DER / X9.63 / PEM import/export), HKDF, AES-CBC,
  AES-CTR, PBKDF2, Argon2, password-protected private keys (PKCS#8 + PBES2, plus reading the legacy `Proc-Type` form),
  and a public key from an X.509 certificate.
- Known gaps: no missing feature. The **other facts** of a certificate (validity period, Subject, SAN, trust chain) are
  a boundary rather than a gap: they belong to appfw's policy layer, and this module treats a certificate only as "a
  container wrapping a public key" (the `G13`-style division of work). Other engineering items are in
  `.ai/backlog/crypto.md`.
- Compatibility: wolfSSL is now built with the real crypt-only configuration (`P7`, `P8`), and the Debug static library
  `wolfssld.lib` grew from around 2.2 MB because it contains the big-integer and RSA/ECC implementations for the first
  time; it is linked statically into `VineCrypto`, with no ABI effect on consumers.

## 10. References

- Code: `src/base/crypto/sdk/vine/crypto/{Digest,Hasher,Hmac,Hkdf,AesGcm,AesCbc,AesCtr,Pbkdf2,Argon2,RsaKey,EccKey,Random}.hpp`,
  `src/base/crypto/src/{Digest,DigestOps,Hasher,Hmac,Hkdf,AesGcm,AesCbc,AesCtr,Pbkdf2,Argon2,RsaKey,EccKey,Random,WolfSsl,Pem,Pkcs8,Certificate}.cpp`
- User documentation: `docs/guides/crypto.md`
- Decision: `ADR-2026-11-07-crypto-api-shape.md`
- Backlog: `.ai/backlog/crypto.md`
- External: RFC 2104 (HMAC), RFC 4231 (HMAC-SHA2 test vectors), NIST SP 800-38D (GCM), RFC 5869 (HKDF), RFC 8017 (RSA),
  RFC 6090/6979 (ECDSA), the wolfSSL v5.9.4 manual
