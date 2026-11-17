# crypto notes

- The backend is wolfSSL **v5.9.4-stable** (a pinned tag in `src/base/crypto/CMakeLists.txt`), compiled
  statically into `VineCrypto`; `WOLFSSL_USE_OPTIONS_H` makes the headers and the library see the same
  `options.h`.
- Backend global initialisation happens only in `detail::ensureLibraryInitialized()` in
  `src/base/crypto/src/WolfSsl.cpp` (`std::call_once`). Skipping it crashes: `wc_InitRng` locks
  `drbgStateMutex`, which only `wolfCrypt_Init()` initialises.
- wolfSSL's headers define `Md5` / `Sha256` / `Sha384` as macros → our own enum values all take a `k`
  prefix (`Digest::kSha256`).
- `vn::crypto::Hmac` collides with wolfSSL's `::Hmac`: the backend context inside the PImpl must spell
  `::Hmac`.
- Digests have exactly one vocabulary: the `Digest` enum + one-shot `digest(Digest, span)` + the
  incremental `Hasher`; `Hmac` / `RsaKey` / `EccKey` all take a `Digest`. `Hash` / `HashCalculator` /
  `Hmac::Algorithm` are deleted (`ADR-2026-11-07-digest-vocabulary`).
- Symmetric: `AesGcm` (AEAD, the default), `AesCbc` (PKCS#7), `AesCtr` (no padding) - the latter two
  exist for interoperability only and neither authenticates.
- Password hashing: `Argon2` (recommended; needs the CMake option `WOLFSSL_ARGON2`, which pulls in
  BLAKE2b) and `Pbkdf2` (interoperability). Test vectors: PBKDF2 = RFC 6070's four vectors; AES-CBC =
  NIST SP 800-38A F.2.1; AES-CTR = F.5.1.
- Password-protected private keys: `RsaKey` / `EccKey`'s `toEncryptedPrivateDer/Pem` and
  `fromEncryptedPrivateDer/Pem`, implemented only in `src/Pkcs8.cpp` (`detail::encryptPkcs8Der` /
  `decryptPkcs8Der`). wolfSSL entry points: `wc_CreateEncryptedPKCS8Key(..., PKCS5, PBES2, AES256CBCb,
  salt=nullptr, 0, itt, rng, nullptr)` to write, `wc_DecryptPKCS8Key` +
  `wc_GetPkcs8TraditionalOffset` to read back; all three live under `HAVE_PKCS8` (enabled when
  `NO_PKCS8` is undefined, which this build satisfies). `AES256CBCb` / `PBES2` / `PKCS5` come from
  `wolfssl/wolfcrypt/asn.h` (via `types.h` → `oid_sum.h`); they are not home-made constants.
- `AesGcm`'s incremental form buffers all input (`P4`); the ciphertext is as long as the plaintext and
  the 16-byte tag is stored separately.
- `AesGcm::finish()` verifies the tag on the decrypt side before returning plaintext; a failure throws
  `std::runtime_error` and drops the buffer.
- `Random` is one process-level DRBG plus one mutex, chunked in 32-bit pieces; `size == 0` is a no-op.
- Test vectors: HMAC = RFC 4231 case 2; AES-GCM = NIST SP 800-38D case 2; HKDF = RFC 5869 test case 1.
- After a wolfSSL upgrade, check: `HAVE_GETPID` is misdetected on MSVC (otherwise `pid_t` inside
  `WC_RNG` does not compile), and CMake's preset `set(HAVE_GETPID OFF CACHE BOOL "" FORCE)` must come
  before `FetchContent_MakeAvailable`.
- Constant-time comparison uses the backend's `wc_ConstantCompare()` (public since 5.9.4, in
  `wolfssl/wolfcrypt/memory.h`); we no longer hand-roll the loop.
- **`options.h` `#undef`s every macro it knows**, so a macro a consumer adds through the command line
  or `target_compile_definitions` does not survive settings.h; a macro consumers must also see (e.g.
  `WOLFSSL_AESGCM_STREAM`) is handled in `src/WolfSsl.hpp` by "include settings.h, then `#define`, then
  include aes.h".
- AES-GCM is streaming (`wc_AesGcm{Encrypt,Decrypt}{Init,Update,Final}`, needing `WOLFSSL_AESGCM_STREAM`);
  decrypted output is untrusted before `finish()`.
- HKDF lives in `wc_HKDF` / `wc_HKDF_Extract` / `wc_HKDF_Expand`
  (`wolfssl/wolfcrypt/hmac.h`); the output is capped at `255 × digestSize`.
- RSA public-key export uses `wc_RsaPublicKeyDerSize(key, 1)` + `wc_RsaKeyToPublicDer()` (the latter in
  `asn_public.h`, not `rsa.h`).
- PEM: encoding uses `wc_DerToPem(der, sz, out, outSz, type)` (where `type` is a `CertType`:
  `PRIVATEKEY_TYPE` / `PUBLICKEY_TYPE` / `ECC_PRIVATEKEY_TYPE`); decoding is ours (`src/Pem.cpp`)
  because `wc_KeyPemToDer` is hard-wired to private keys and cannot read `PUBLIC KEY`.
- **wolfSSL's build knobs**: the CMake option name differs from the macro name, and a wrong one fails
  silently - `WOLFSSL_KEYGEN` (macro `WOLFSSL_KEY_GEN`), `WOLFSSL_AESCTR` (macros
  `WOLFSSL_AES_COUNTER` / `WOLFSSL_AES_DIRECT`), `WOLFSSL_SP_MATH_ALL` (the big-integer backend RSA/ECC
  require), `WOLFSSL_SYS_CA_CERTS off` (otherwise a crypt-only build misses `GetCA`).
- `WOLFCRYPT_ONLY` must be passed `PUBLIC` to wolfssl and to every consumer: it changes the layout of
  `RsaKey` and friends, and giving it only to the library produces "signing works, verification is
  always false".
- RSA call notes: `wc_RsaPublicEncrypt` / `wc_RsaPrivateDecrypt` default to PKCS#1 v1.5 (use the `_ex`
  forms for OAEP); `wc_RsaPSS_Verify` only decrypts the signature, so the comparison needs
  `wc_RsaPSS_CheckPadding`; under `WC_RSA_BLINDING` private operations read `key->rng`, so call
  `wc_RsaSetRNG` first.
- ECC call notes: under `ECC_TIMING_RESISTANT` `wc_ecc_shared_secret` reports `MISSING_RNG_E` (-236)
  when `key->rng` is missing, so call `wc_ecc_set_rng` first; `wc_ecc_sign_hash` writes a **DER**
  signature, so the buffer needs `2×order+16` of headroom (else `BUFFER_E`).
- **SEC1's public point is optional**: on a file without the public point `wc_EccPrivateKeyDecode` sets
  `key->type` to `ECC_PRIVATEKEY_ONLY`, after which `wc_ecc_export_x963` / `wc_EccPublicKeyToDer` simply
  fail (while `wc_ecc_verify_hash` computes it itself, so the symptom is "sometimes it works").
  `EccKey::fromPrivateDer` therefore calls `wc_ecc_make_pub_ex(&key, nullptr, rng)` right after import.
  Easy to miss when self-testing: verification hides the problem and only exporting the public key
  exposes it - a round-trip test must call `publicX963()` once.
- `wc_DecryptPKCS8Key` decrypts **in place** (the plaintext overwrites the ciphertext), so pass a buffer
  you copied and call `wc_ForceZero` afterwards.
- Legacy encrypted PEM (`Proc-Type: 4,ENCRYPTED` + `DEK-Info:`) goes through `wc_KeyPemToDer` and needs
  `WOLFSSL_ENCKEYS`; its label is still `RSA PRIVATE KEY`, so you cannot tell it by the label - look for
  a `Proc-Type:` header in the body. It wants a **NUL-terminated** password.
- A certificate yields its public key only: `wc_GetSubjectPubKeyInfoDerFromCert(certDer, sz, out,
  &outSz)` (`out` NULL returns 0 and writes `outSz`), and the result feeds
  `RsaKey::fromPublicDer` / `EccKey::fromPublicDer` directly - both importers already take SPKI.
- **`WOLFSSL_MD5` is disabled by default**, so `Digest::kMd5`'s availability cannot rely on a preset's
  side effect; `src/base/crypto/CMakeLists.txt` sets `set(WOLFSSL_MD5 yes ...)` explicitly, otherwise
  `wc_Md5` is undeclared in another build tree.
- RNG's shared piece: `detail::RandomHandle` in `src/base/crypto/src/WolfSsl.hpp` (one context per
  operation, no lock).
- `AesGcm` uses the backend's one-shot `wc_AesGcmEncrypt/Decrypt` entry points and lays the buffer out
  itself; the tag is always 16 bytes.
- Commands: `cmake --build build --config Debug --target test_crypto` →
  `build\bin\Debug\test_crypto.exe`.
- Memory checking (Windows / MSVC ASAN, verified: 114 cases, no report):
  `cmake -S . -B build-asan -G "Visual Studio 18 2026" -A x64 -DCMAKE_BUILD_TYPE=Debug `
  `-DCMAKE_PREFIX_PATH=D:/Qt/6.8.2/msvc2022_64 -DCMAKE_CXX_FLAGS="/fsanitize=address /Zi" `
  `-DCMAKE_EXE_LINKER_FLAGS=/fsanitize=address -DCMAKE_SHARED_LINKER_FLAGS=/fsanitize=address`
  plus `-DFETCHCONTENT_SOURCE_DIR_<NAME>=build/_deps/<name>-src` to reuse already-downloaded
  dependencies (wolfssl / googletest / …). Before running, put `VC\Tools\MSVC\<ver>\bin\Hostx64\x64`
  (which holds `clang_rt.asan_dbg_dynamic-x86_64.dll`) on `PATH`, otherwise the process exits with
  `0xC0000135` immediately. `ASAN_OPTIONS=detect_leaks=1` is unsupported on this platform and makes the
  process fail outright.
