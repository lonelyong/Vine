#pragma once

#include <vine/crypto/crypto_global.hpp>

#include <cstddef>
#include <span>
#include <vector>

VN_CRYPTO_NS_BEGIN

namespace detail
{

/**
 * @brief Extracts the SubjectPublicKeyInfo DER from a DER-encoded X.509 certificate.
 *
 * @details
 * - This is the whole of what a certificate is to this layer: a container that vouches for a public key. Who signed the
 *   certificate, whether the chain reaches a trusted root and whether the validity window has passed are policy
 *   questions, and policy belongs to the caller (appfw), not to an algorithm module.
 * - The result is exactly the `SubjectPublicKeyInfo` structure, which is what `RsaKey::fromPublicDer()` and
 *   `EccKey::fromPublicDer()` already accept, so no key type has to be known in advance.
 * - Only the first certificate of a bundle is read: a chain is a sequence of certificates, and picking the right one
 *   out of it is a decision this function cannot make.
 *
 * @param certificate_der The DER bytes of the certificate.
 * @return The DER of its `SubjectPublicKeyInfo`.
 * @throws std::invalid_argument when the bytes are empty or are not a certificate this build can parse.
 */
std::vector<std::byte> certificatePublicKeyDer(std::span<const std::byte> certificate_der);

} // namespace detail

VN_CRYPTO_NS_END
