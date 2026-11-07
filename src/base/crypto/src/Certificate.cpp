#include "Certificate.hpp"

#include <stdexcept>
#include <vector>

#include <wolfssl/wolfcrypt/asn_public.h>

#include "WolfSsl.hpp"

VN_CRYPTO_NS_BEGIN

namespace detail
{

std::vector<std::byte> certificatePublicKeyDer(std::span<const std::byte> certificate_der)
{
    if (certificate_der.empty()) {
        throw std::invalid_argument("vn::crypto: the certificate bytes are empty");
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(certificate_der.data());
    const auto  size  = static_cast<word32>(certificate_der.size());

    // A null output asks for the length of the extracted structure.
    word32    needed = 0;
    const int result = wc_GetSubjectPubKeyInfoDerFromCert(bytes, size, nullptr, &needed);
    if (result < 0 || needed == 0) {
        throw std::invalid_argument("vn::crypto: the bytes are not an X.509 certificate");
    }

    std::vector<std::byte> public_key(static_cast<std::size_t>(needed));
    const int written =
        wc_GetSubjectPubKeyInfoDerFromCert(bytes, size, reinterpret_cast<unsigned char*>(public_key.data()), &needed);
    if (written < 0) {
        throw std::invalid_argument("vn::crypto: the bytes are not an X.509 certificate");
    }

    public_key.resize(static_cast<std::size_t>(needed));
    return public_key;
}

} // namespace detail

VN_CRYPTO_NS_END
