# ADR-2026-11-07-certificates-and-legacy-keys

- Status: Accepted
- Date: 2026-11-07
- Scope: the certificate entry points of `vn::Crypto`, legacy encrypted PEM, and the MD5 build switch of wolfSSL

## Background

Three things were left when the crypto module was closed out. All of them look like "one more small feature", but they carry different weight as decisions:

1. **Certificates**: the comment on `RsaKey::fromRawPublic` had always said "this is the shape a certificate or JWK parse hands over", meaning that the certificate-parsing step
   never existed. Whether to do it, and up to which layer, needed an explicit line.
2. **Legacy encrypted PEM**: before PKCS#8, `openssl rsa -aes128` wrote
   `BEGIN RSA PRIVATE KEY` plus the `Proc-Type: 4,ENCRYPTED` / `DEK-Info:` headers in the body. This module rejected such a file as "not base64",
   pointing the error in the wrong direction.
3. **MD5**: `Digest::kMd5` is part of the public enum, but wolfSSL's `WOLFSSL_MD5` defaults to **disabled**,
   and this build was only accidentally switched on by some preset. Another configuration does not compile, and the failure point sits far from the cause (`wc_Md5` undeclared in `DigestOps.cpp`).

## Decision

### 1. Certificates: take the public key only, make no trust judgement

`RsaKey` / `EccKey` each gain two factories: `fromCertificateDer` / `fromCertificatePem`. The implementation is a one-line delegation:
the backend's `wc_GetSubjectPubKeyInfoDerFromCert` extracts the `SubjectPublicKeyInfo`, and the existing public-key importer takes it.

Explicit non-goals: signature verification, validity period, issuer, SAN, trust chain. These are **policy** and need to know "whom to trust" and "what time it is",
whereas this layer only answers "what did this algorithm compute". It also reads only the **first** certificate in the bundle - picking one out of a chain is the caller's decision.

### 2. Legacy encrypted PEM: read it, but do not write it

`WOLFSSL_ENCKEYS` is switched on (macro `WOLFSSL_ENCRYPTED_KEYS`), and the read direction goes through `wc_KeyPemToDer(pem, sz, out, sz, pass)`;
recognition keys off "the body contains a `Proc-Type` header" rather than the label, because the label of this form is still `RSA PRIVATE KEY` - the label lies.

The standing position is unchanged: **writing uses PKCS#8 only** (see `ADR-2026-11-07-encrypted-private-keys.md`). The old form is historical baggage,
and supporting it is so that old files can be read in, not so that new files turn old.

### 3. MD5: switch it on explicitly

`set(WOLFSSL_MD5 yes CACHE STRING "" FORCE)`. Since the module's public enum contains `Digest::kMd5`,
its availability must not depend on a side effect of some wolfSSL preset. Incidentally, the key derivation of legacy PEM uses MD5 too.

## Rejected options

- **Wrap a certificate whole as an `X509Certificate` class** (subject / issuer / validity / serial / `isValidAt()`): no consumer in this layer
  reads those fields, and "is the certificate valid" is a policy judgement. `G8`: no abstraction without a consumer. When appfw really needs it, that is a new module's business.
- **Walk the DER by hand to parse a certificate**: the backend already has `wc_GetSubjectPubKeyInfoDerFromCert`, and walking it again only adds
  a parser that can drift from the standard (`G5`: an expected change should stay inside the module, but "rewrite an existing implementation" is not that change).
- **Return `std::variant<RsaKey, EccKey>` once**: it pushes the knowledge the caller already has - "which key am I expecting" - into the return value,
  and makes the caller `visit` it. Two static factories plus an error on a type mismatch are plainer.
- **Writing legacy PEM**: that would drop new files back to AES-128-CBC with MD5 derivation and no KDF parameters, for no reason at all.
- **Leave `WOLFSSL_ENCKEYS` off and parse `DEK-Info` by hand, calling `wc_DecryptContent`**: the same work plus a layer of glue that is easy to get wrong.

## Consequences

- `wc_KeyPemToDer` wants a NUL-terminated passphrase, whereas our API passes a `std::u8string_view`: the implementation makes a copy and does not zero it afterwards
  (the passphrase belongs to the caller and this layer does not hold it; the copy is reclaimed with the stack).
- For the legacy form, a wrong passphrase and "an algorithm this build does not support" return the same error code, so the message has to cover both (`P23`).
- A certificate import error comes from the public-key importer: when the certificate carries an ECC key and the caller uses `RsaKey::fromCertificatePem`, the message is
  "not an RSA public key" (`N12`).
- Mutation testing (`T15`) turned up a fact worth remembering along the way: roughly a quarter of single-byte mutations in a passphrase-protected private-key file are
  still accepted, because PBES2 is CBC without a MAC. That is not a defect but a property of this kind of container (`P24`).
