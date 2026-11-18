# ADR-2026-11-07 AES-GCM streaming

- Status: Accepted
- Date: 2026-11-07
- Related: `.ai/design/crypto.md` (`C14`, `P4`, `D2`), `ADR-2026-11-07-crypto-api-shape.md` (whose "buffered incremental GCM" decision is superseded by this document)

## Background

The first `AesGcm` had a **buffered** incremental form: `update()` accumulated input into a vector and `finish()` computed it in one go.
The reason at the time was that wolfSSL's incremental GCM entry points had different signatures between versions, and the failure mode of a wrong call was "admitting unauthenticated data" - an unacceptable cost.

The price was a recorded defect (the old `P4`): memory grew linearly with the input, so a large file meant reading the whole file into memory.

After upgrading to wolfSSL v5.9.4 the backend offers stable and explicit streaming entry points:

- `wc_AesGcmEncryptInit/Update/Final`, `wc_AesGcmDecryptInit/Update/Final`;
- `wc_AesGcmDecryptFinal` carries `WARN_UNUSED_RESULT`, so a caller that ignores the return value gets a compiler error;
- the output length of `update()` always equals the input length (GCM is a CTR stream), so no fallback block has to be kept.

## Decision

Switch to a real streaming implementation and drop the buffer:

| Shape | Behaviour |
|---|---|
| Construction | Initialise the context, set the key and IV, and feed all AAD at once (GCM requires every associated datum before the first ciphertext byte) |
| `update(span)` | Process this block and return an equal-length output (ciphertext on the encrypt side, plaintext on the decrypt side) |
| `update(span, out&)` | The same, with the output written into caller storage, avoiding one allocation per block |
| `finish()` | Write the tag on the encrypt side; make the tag decision on the decrypt side, throwing `std::runtime_error` on failure |

## Consequences

- Any input size is processed in constant memory; `seal` / `open` take the same path, and a test locks the consistency with "chunked result == one-shot result".
- **A new contract** (written into the header's `@warning` and the design page's `C14`/`P4`): when decrypting, the bytes `update()` hands out are untrusted until `finish()` succeeds. This is inherent to streaming AEAD and not an implementation defect; the caller either writes the output somewhere it can roll back or commits at the end.
  The old implementation's advantage of "verify first, then hand out plaintext" is therefore gone, and what it bought is bounded memory - a trade-off that has to be stated, not silently guaranteed by the implementation.
- `AesGcm` no longer provides `operator<<`: the input is no longer accumulated, so the stream operator's "feed it and take it out later" semantics no longer holds.
- `finish()` returns nothing for GCM (GCM has no trailing block); the return value is kept for consistency with the gestures of `Hasher` / `Hmac`.

## Evidence

- `tests/test_crypto/CipherTest.cpp`: 1000 bytes encrypted in chunks of 1, 509 and the rest, with the ciphertext byte-identical to `seal()`; chunked decryption restores the plaintext; tampering with any ciphertext byte or the tag throws at `finish()`; an empty input still authenticates the AAD.
- The NIST SP 800-38D case 2 vector still passes, which shows the streaming path and the one-shot path give the same result.
- How it is enabled: `target_compile_definitions(wolfssl PRIVATE WOLFSSL_AESGCM_STREAM)`, with the macro re-declared in `src/WolfSsl.hpp` - the generated `options.h` `#undef`s it, so a definition passed on the command line does not survive that line, while the layout of `Aes` depends on it.
