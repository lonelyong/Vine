# crypto open backlog

- Status: open
- Trigger: start an item when a real consumer or a peer's format requirement appears; delete an item once it is done

## 1. Engineering quality

- **Error classification**: everything throws `std::runtime_error` / `std::invalid_argument` today; introduce an error
  enum when "invalid input / backend failure / authentication failure" has to be told apart - and only when a caller
  really has to branch on it.
  Note that a wrong password and a corrupt structure deliberately share one `std::invalid_argument` (see `P21` in
  `crypto.md`).
- **Argon2 threading**: `WOLFSSL_ARGON2_THREADS` would let the lanes run in parallel; it is off today, and one thread is
  enough for password scenarios.
  Trigger: re-evaluate when password derivation becomes a perceptible wait.
- **The encrypted-private-key scheme could be configurable**: it is fixed today at PBES2 + AES-256-CBC + PBKDF2 (the
  default PRF, i.e. HMAC-SHA1).
  The `hmacOid` parameter of `wc_EncryptPKCS8Key_ex` already supports another PRF, and AES-128 is one `encAlgId` away;
  raise it to the public API only when a peer really requires it (`G8`: no extension point without a consumer).
- **One key, one thread**: `RsaKey` / `EccKey` bind the random context of each operation into the key object (`P18`).
  Sharing one key across threads would need a lock per object; no consumer needs that today, so "one thread per key"
  stands and is written down in the documentation.

## 2. Work that needs a new capability

- **A second AEAD (ChaCha20-Poly1305)**: already compiled into the backend library (`chacha20_poly1305.c`), with no entry
  point in the module.
  Trigger: the target platform has no AES hardware acceleration, or a peer that only speaks ChaCha has to interwork.
- **Ed25519 / X25519**: available in the backend, with no entry point in the module. Trigger as above, but the shape
  needs a decision (a new class vs making `EccKey` fat).
- **PKCS#12 key store**: not researched. Most likely it belongs to appfw's key management rather than to the algorithm
  layer.

## 3. Verification

- Leak detection: Windows ASAN does not support `detect_leaks` (it reports "not supported on this platform" at run
  time), so "no leaks" currently has no evidence. Trigger: connect `scripts/asan_check.sh` on the Linux side and add a
  crypto target.
