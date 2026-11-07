#pragma once

#include <vine/crypto/crypto_global.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

VN_CRYPTO_NS_BEGIN

namespace detail
{

/**
 * @brief Wraps a traditional private-key DER in a password-protected PKCS#8 `EncryptedPrivateKeyInfo`.
 *
 * @details
 * - A traditional DER has no room for a password: PKCS#1 `RSAPrivateKey` and SEC1 `ECPrivateKey` are just the numbers.
 *   PKCS#8 adds an outer structure whose payload is encrypted, which is the only interoperable way to protect a key
 *   file with a password.
 * - The scheme is PBES2 with AES-256-CBC and a random 16-byte IV, keyed by PBKDF2 with a random 8-byte salt; the
 *   iteration count is `iterations`. Every parameter is stored in the output, so a reader does not need to be told
 *   any of them, and `openssl pkcs8 -in key.pem -passin ...` reads the result.
 * - The input may be an RSA or an ECC key: the algorithm and curve identifiers are taken from the key itself.
 *
 * @warning
 * - The password is the whole of the protection; a weak one is attackable offline, because the ciphertext is the
 *   password verifier. There is no iteration count that rescues a guessable password.
 *
 * @param traditional_der The unencrypted DER of the private key.
 * @param password        The password; must not be empty.
 * @param iterations      PBKDF2 iteration count; must be at least 1.
 * @return The DER of the `EncryptedPrivateKeyInfo`.
 * @throws std::invalid_argument when the input is empty, the password is empty or `iterations` is 0.
 * @throws std::runtime_error    when the backend cannot encode or encrypt the key.
 */
std::vector<std::byte> encryptPkcs8Der(std::span<const std::byte> traditional_der, std::u8string_view password,
                                       unsigned iterations);

/**
 * @brief Decrypts a PKCS#8 `EncryptedPrivateKeyInfo` back to the traditional private-key DER.
 *
 * @details
 * - The inner key is returned without the PKCS#8 wrapper, so it can be handed straight to the existing importers.
 * - PBES2 and the older PBES1 / PKCS#12 password schemes are all accepted, because the parameters travel inside the
 *   structure; only the password has to be supplied from outside.
 *
 * @param encrypted_der The DER of the `EncryptedPrivateKeyInfo`.
 * @param password      The password; must not be empty.
 * @return The unencrypted DER of the private key.
 * @throws std::invalid_argument when the password is wrong, the input is empty, or the structure is not a supported
 *                               encrypted private key.
 */
std::vector<std::byte> decryptPkcs8Der(std::span<const std::byte> encrypted_der, std::u8string_view password);

} // namespace detail

VN_CRYPTO_NS_END
