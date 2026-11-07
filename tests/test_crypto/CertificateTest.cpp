#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <vine/crypto/EccKey.hpp>
#include <vine/crypto/RsaKey.hpp>

using vn::crypto::EccKey;
using vn::crypto::RsaKey;

namespace
{

/// A self-signed RSA certificate, written by `openssl req -x509 -newkey rsa:2048`.
constexpr std::string_view kRsaCertificate = R"(-----BEGIN CERTIFICATE-----
MIIC3zCCAcegAwIBAgIUE/1hslY0EVA5G/kY+b+Picj+6gcwDQYJKoZIhvcNAQEL
BQAwGDEWMBQGA1UEAwwNdmluZSByc2EgdGVzdDAeFw0yNjExMDcwMzQ1MzVaFw0z
NjExMDQwMzQ1MzVaMBgxFjAUBgNVBAMMDXZpbmUgcnNhIHRlc3QwggEiMA0GCSqG
SIb3DQEBAQUAA4IBDwAwggEKAoIBAQCkY4DIDkNwPobC9ArMAWHDNs9sykkPcGBF
1fr+to+OHuXpTe3ywYZTYxSSjBVrIJDTMjcGA1mb8RvLz0bL7s1x5Usc+kndpeKH
XpVd9qQlQ7dbo8n7DsPhIaTjyNS7xVFCViIwaCPhG6jDV5TxzHNLj+HeyyDmbUb6
KmruhwHylNWRPDDeM9O2GgvS4kCn9AXK8Pm381UtE6Jgihxj/ICb+ko4Dt7IHFwV
QY9Ho8m3RjqQEzilWbsVGc9iPF2ao8SDhD0Jers6sGGjA7aFDWh3Dy7ptujHUXBP
t1kXi2mvz/l9D2jXjDW5R6BjQ6stseEY8SB6PqC2dIOUZTWB14AdAgMBAAGjITAf
MB0GA1UdDgQWBBQdwPizBP+X8V7B4SsNo/z5DrqZ1jANBgkqhkiG9w0BAQsFAAOC
AQEAJ46J7K7Kvb/cBSNWWiAKHkefh55uDIu5NW6kYmLnBFE0TvFPJc4H0M7KGxVx
vei0j+JReispbUMnRAWQz2zlZALI085UlE2gM7Xwdv2QA0f7naRZL/04sNkpSCtL
kZDY1a/MfJqyjDsFuWXGcsO56sRwMw01syLaMkVfAJ1qzJL36lhratFn3tHm7oRd
fRlldHzQVwzkJ+TWRJll9ntEL8kQa7L6nU07fMVXOclL622v7thhyXXVX6w+TZrN
TF5igt18h3v3yww/mUiwaldFcnTWRzzw0LQ8FRniquLqJDrOu8nqhJsXjcSaS/YV
DyQV3ZfO5sSu6jlVpQLfBTlrmA==
-----END CERTIFICATE-----)";

/// The public key `kRsaCertificate` vouches for, written by `openssl x509 -pubkey -noout`.
constexpr std::string_view kRsaCertificatePublicKey = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEApGOAyA5DcD6GwvQKzAFh
wzbPbMpJD3BgRdX6/raPjh7l6U3t8sGGU2MUkowVayCQ0zI3BgNZm/Eby89Gy+7N
ceVLHPpJ3aXih16VXfakJUO3W6PJ+w7D4SGk48jUu8VRQlYiMGgj4Ruow1eU8cxz
S4/h3ssg5m1G+ipq7ocB8pTVkTww3jPTthoL0uJAp/QFyvD5t/NVLROiYIocY/yA
m/pKOA7eyBxcFUGPR6PJt0Y6kBM4pVm7FRnPYjxdmqPEg4Q9CXq7OrBhowO2hQ1o
dw8u6bbox1FwT7dZF4tpr8/5fQ9o14w1uUegY0OrLbHhGPEgej6gtnSDlGU1gdeA
HQIDAQAB
-----END PUBLIC KEY-----)";

/// A self-signed P-256 certificate, written by `openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256`.
constexpr std::string_view kEccCertificate = R"(-----BEGIN CERTIFICATE-----
MIIBUDCB96ADAgECAhRUnaDCCQCPSbmOtEwP9Fsz1jFe7DAKBggqhkjOPQQDAjAX
MRUwEwYDVQQDDAx2aW5lIGVjIHRlc3QwHhcNMjYxMTA3MDM0NTM1WhcNMzYxMTA0
MDM0NTM1WjAXMRUwEwYDVQQDDAx2aW5lIGVjIHRlc3QwWTATBgcqhkjOPQIBBggq
hkjOPQMBBwNCAAQ/NM80uQh904cJKnJAAWDf0ML6WLCVl1/MkHdRwDCkADFAA2fe
mmTH43dtzi2JCKCwiemq5YFPCmRSbUGMJMKfoyEwHzAdBgNVHQ4EFgQUU3fgQcLC
IlfrhJ62kkP65xuLkxcwCgYIKoZIzj0EAwIDSAAwRQIhAOWXGsjJbZfzIYvlZknA
016RlTvQSS9+YVSC0HcNZLj2AiA9SSsPo+rB43lO+flSEABzGtWmqHXC3ecwPHPT
iIcm2A==
-----END CERTIFICATE-----)";

/// The public key `kEccCertificate` vouches for, written by `openssl x509 -pubkey -noout`.
constexpr std::string_view kEccCertificatePublicKey = R"(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEPzTPNLkIfdOHCSpyQAFg39DC+liw
lZdfzJB3UcAwpAAxQANn3ppkx+N3bc4tiQigsInpquWBTwpkUm1BjCTCnw==
-----END PUBLIC KEY-----)";

/**
 * @brief Turns a std::string_view of ASCII into the UTF-8 string type the API takes.
 *
 * @param text The ASCII text.
 * @return The same bytes as a UTF-8 string.
 */
std::u8string asU8(std::string_view text)
{
    return std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size());
}

} // namespace

TEST(CertificateTest, RsaCertificateYieldsTheKeyItVouchesFor)
{
    const RsaKey from_certificate = RsaKey::fromCertificatePem(asU8(kRsaCertificate));
    const RsaKey expected         = RsaKey::fromPublicPem(asU8(kRsaCertificatePublicKey));

    EXPECT_FALSE(from_certificate.hasPrivate()) << "a certificate carries the public half only";
    EXPECT_EQ(from_certificate.bits(), 2048u);
    EXPECT_EQ(from_certificate.toPublicDer(), expected.toPublicDer());
}

TEST(CertificateTest, EccCertificateYieldsTheKeyItVouchesFor)
{
    const EccKey from_certificate = EccKey::fromCertificatePem(asU8(kEccCertificate));
    const EccKey expected         = EccKey::fromPublicPem(asU8(kEccCertificatePublicKey));

    EXPECT_EQ(from_certificate.curve(), EccKey::Curve::kP256);
    EXPECT_EQ(from_certificate.publicX963(), expected.publicX963());
}

TEST(CertificateTest, OnlyTheFirstCertificateOfABundleIsRead)
{
    // A chain is a sequence of certificate blocks; picking the right one out of it is the caller's decision, so the
    // documented behaviour is "first block wins" rather than "search for a key type".
    const std::u8string bundle = asU8(kRsaCertificate) + u8"\n" + asU8(kEccCertificate);

    const RsaKey from_bundle  = RsaKey::fromCertificatePem(bundle);
    const RsaKey expected     = RsaKey::fromPublicPem(asU8(kRsaCertificatePublicKey));
    EXPECT_EQ(from_bundle.toPublicDer(), expected.toPublicDer());
}

TEST(CertificateTest, CertificateOfTheOtherKeyTypeIsRejected)
{
    // The extractor does not decide the key type; the importer does, and says so.
    EXPECT_THROW((void)RsaKey::fromCertificatePem(asU8(kEccCertificate)), std::invalid_argument);
    EXPECT_THROW((void)EccKey::fromCertificatePem(asU8(kRsaCertificate)), std::invalid_argument);
}

TEST(CertificateTest, RejectsTextThatIsNotACertificate)
{
    EXPECT_THROW((void)RsaKey::fromCertificatePem(u8""), std::invalid_argument);

    // A public key where a certificate is expected: the armour is fine, the structure is not.
    EXPECT_THROW((void)RsaKey::fromCertificatePem(asU8(kRsaCertificatePublicKey)), std::invalid_argument);

    // A certificate label over bytes that are not a certificate.
    EXPECT_THROW((void)RsaKey::fromCertificatePem(u8"-----BEGIN CERTIFICATE-----\nMIIBAAAA\n-----END CERTIFICATE-----"),
                 std::invalid_argument);
}
