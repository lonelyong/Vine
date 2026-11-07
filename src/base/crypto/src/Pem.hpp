#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <vine/crypto/crypto_global.hpp>

VN_CRYPTO_NS_BEGIN

namespace detail
{

/**
 * @brief Wraps DER bytes in PEM armour.
 *
 * @details
 * - PEM is DER in Base64 with a label line; `type` is wolfSSL's `CertType` value, which decides the label
 *   (`PRIVATEKEY_TYPE` → `RSA PRIVATE KEY`, `PUBLICKEY_TYPE` → `PUBLIC KEY`, ...). The caller, not this function,
 *   knows what the DER actually is — a mismatched label produces a file other tools will parse wrongly.
 *
 * @param der  The DER bytes to wrap.
 * @param type wolfSSL `CertType` value selecting the label.
 * @return The PEM text, newline terminated, without a trailing NUL.
 * @throws std::runtime_error when the backend cannot encode it.
 */
std::u8string derToPem(std::span<const std::byte> der, int type);

/**
 * @brief Reports whether a PEM block is the password-protected one.
 *
 * @details
 * - An encrypted PKCS#8 block carries the same DER shape as any other PEM block, so a plain importer would fail deep
 *   in an ASN.1 parser with a message about a bad key; asking the label first lets it name the real problem.
 *
 * @param pem The PEM text.
 * @return True when the block is labelled `ENCRYPTED PRIVATE KEY`; false for empty text or text with no PEM BEGIN line.
 */
bool pemIsEncryptedPrivateKey(std::u8string_view pem);

/**
 * @brief Reports whether a PEM block is the legacy `Proc-Type: 4,ENCRYPTED` form.
 *
 * @details
 * - Before PKCS#8, encrypted keys kept the traditional label (`RSA PRIVATE KEY`) and put the parameters inside the
 *   body as `Proc-Type: 4,ENCRYPTED` and `DEK-Info: <cipher>,<iv>` headers. The label alone therefore says nothing;
 *   the header area has to be read.
 *
 * @param pem The PEM text.
 * @return True when the body carries a `Proc-Type` header; false for empty text or text with no PEM BEGIN line.
 */
bool pemIsLegacyEncrypted(std::u8string_view pem);

/**
 * @brief Decrypts a legacy `Proc-Type: 4,ENCRYPTED` block and returns the decrypted key DER.
 *
 * @details
 * - The cipher and IV travel in the `DEK-Info` header, so only the password is needed; the backend reads both.
 * - Unlike the PKCS#8 path this one is not a two-step decode: the backend strips the armour, decrypts and returns the
 *   traditional DER in one call.
 *
 * @warning
 * - The backend takes a NUL-terminated password, so the caller's view is copied; the copy is wiped before returning.
 *
 * @param pem      The PEM text.
 * @param password The password; must not be empty.
 * @return The unencrypted DER of the private key.
 * @throws std::invalid_argument when the password is empty or wrong, or the block is not a supported legacy encoding.
 */
std::vector<std::byte> pemDecryptLegacy(std::u8string_view pem, std::u8string_view password);

/**
 * @brief Removes PEM armour and returns the DER bytes.
 *
 * @details
 * - The label line is validated by the backend against the content it decodes, so a `PUBLIC KEY` file cannot be
 *   decoded as a private key. A password-protected block is not decrypted here: this function has no password, so the
 *   caller has to strip the armour and decrypt the bytes itself (`detail::decryptPkcs8Der`).
 *
 * @param pem The PEM text.
 * @return The DER bytes.
 * @throws std::invalid_argument when the text is not a supported, unencrypted PEM block.
 */
std::vector<std::byte> pemToDer(std::u8string_view pem);

} // namespace detail

VN_CRYPTO_NS_END
