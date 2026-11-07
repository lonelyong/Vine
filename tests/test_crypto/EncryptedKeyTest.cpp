#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
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

/// The password the committed OpenSSL fixtures were produced with.
constexpr std::u8string_view kFixturePassword = u8"correct horse battery";

/**
 * @brief An RSA key produced by OpenSSL 3, protected with PBES2 / AES-256-CBC.
 *
 * @details
 * - Generated once by hand with `openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048` followed by
 *   `openssl pkcs8 -topk8 -v2 aes-256-cbc -iter 4096`. It is committed rather than produced by the test because the
 *   point of the fixture is that this module did not write it.
 */
constexpr std::string_view kOpensslRsaEncryptedPrivateKey = R"(-----BEGIN ENCRYPTED PRIVATE KEY-----
MIIFNTBfBgkqhkiG9w0BBQ0wUjAxBgkqhkiG9w0BBQwwJAQQmUrQ7yTXAqBaSbA3
Mhx21wICEAAwDAYIKoZIhvcNAgkFADAdBglghkgBZQMEASoEEBEvNqaa68BQCzsM
IiNmy90EggTQtzx6VaOoWyjNyZ+cOy48GsKNunQhStBbIRpmTpu9zmGGB8qBtgIZ
lubV4dm78cZPQHA52tfyoUTlToum9qyzF2EXQTxk1jr0bd4zfAec2xgOsfEPMIUV
xatdeQIFfmodcyA1hMA2jxWoCYhnmQsCL5HBji2At/3O/4zycW+OR2QwXnDfnCnw
5zq8vTYncOtbGaWBn0Mv2TKCLW141MfCQzoq7OY9rEf00VWwmgxYI6+Fyg/iQVYr
VdSs/u7t7oWmw76/PBXPeP0VZ+Y4X86iidmFXMexYOxJx/jJTilp4YNsDyAsEDoS
Zfh5ysUwB4qiRyjatVAvyBpmlJxUknVkanJgmEDrYa8KJ+G6vLdJg2gRxfZ7V3qK
MV5nrlharbtyBHtP4MD4k1jbTEbnUzqvpk5molaWOeyYH4FU1I8QWv9YUhAPaDKm
RMJwB8Eq/umSjDWdex6C0f+to79PbsHdDwXlRhkgsHJCSy8UGu7kLod7mU4yGYKH
5Ktjq2dpGk38YppwIuC/60WrcVtrs+T67u5SsCwZi3VdQy3W7eG1U+Z59bPz/2K2
s3vI9kJcELeSHgAhrDOFjlmT8GpIuIp/DEbsfpuzv5r6wxqMd1qMijjpOkUB/cDl
40V+/MKxiWk8VD8E8PBkDsZPXRH4jpk6e4fF3TgiRla8TqeD0ueWSYdzyyZps4Om
CbEVcDcDsGLGFVS0Kgx8ywmhTQoBGtrt5/oKfWS/RcFADOLlYWMMzz2UDwWiLo2i
dcteXqZFlWIlKTrovUBQrZ8uUjvbMs6pMs8RM/6oXCz+O7ouwoJrWPvh2zHIfCvJ
oMggMUX1lpC9o7xzP8rgmH0kWe15leatcTmwBf5PownQBRTsbueY05KKzCQYaHfo
HMNYFtypNG5kRlCY+4Ws4EjLRgzPm/4B8vxvyWUrnKH+76bLI5Z8f9BwakTNQN1O
B7yiJhQd9E6FiAlcabcQ6KCVfIhWFAAQYKY6agoJTVObj1kJuMim46v814vL/J12
yVWYUpBxHga4hwBdTkqyBTt7zW2USQ/oghliK1lxHhc4sHRfto6zEiBQ3O6FB478
WN/vOcl+3zdJvYVsEVZXFEsdo6q5YqDnAlmYCBjgmKMuyrda0COEBad+WKUSufp8
RklJpYuArCTgJJBk2p1F5eOnoINjAMjoApG1VLEgELL/al9CtsRpRaV9kyJlIaC2
SGXGoqUudqH+ecPSYfQrjs7ESgmH6ZqsbHX4PranwhEGAdUZ+VnnKPKQu7adkRkI
loH+AmpFmVXx+0Ojdtzxx9vPOJpjq0yoySOv19wNJsMDKEwFPsFBL+YPDE5Jcwzn
7+ByRG1VmgPDjXKLxiN8kaRh7uON+wVbt0jksnhFlgDMlSMiX4Tthr+Xl8LwsoX2
9GQSNnNIMcZmB9NkN8C/80k7R2Hp6nFqtUPV/VdtTk1ovt4y0HgNyiUCruCI5feW
4ZiVcFa+xb/+5eZa7vp57BZwalJ107LfksiukISJAxxoxzVoYOp/k6jmm+KjSSoe
/qDyr8QNd4HJV99InQ4VCRDn1CSCp5JnskRZol61+tABU51sLbwn/3kd/PeBRgbu
w7Y0eOf1/VsOkQDK7shn6t6TumiPNuotvoCCzWWV1jeAUG5yTPmK9n8=
-----END ENCRYPTED PRIVATE KEY-----)";

/// The public half of `kOpensslRsaEncryptedPrivateKey`, written by `openssl pkey -pubout`.
constexpr std::string_view kOpensslRsaPublicKey = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsDVWjKDlHD3uI8PE725n
Ubyab8+lh6Yu8QkIhTznVLtBeYyz+CjeQ4vVcjEb3Z+G/ncmDZvfRhovyjF1f2cm
a3tRq71u0ZDd7qZ7EEz23p3P0l48yVlsPuoFTnGSetN+84qd33naxVnvHpxBGtli
l21BBppX3S6j7D3GNpbRo7ldkEJ1LDyyGoUN+R7UCVS5Igkg33Y4CWKKTr4z0snT
DxaeLYlJKten+GWCeIq/+jvvR1NOsbEN50VmEoNIX0s+aR1EMK/lRuGHv5d/ZX6T
YvPi50AZ2RQdQu82+NUEOYgjxJtO8OnqKwIaf2iRRsqUSSC70CP9QoLQv3j2bTa1
wwIDAQAB
-----END PUBLIC KEY-----)";

/**
 * @brief An ECC (P-256) key produced by OpenSSL 3, protected with PBES2 / AES-256-CBC.
 *
 * @details
 * - Generated with `openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256` and
 *   `openssl pkcs8 -topk8 -v2 aes-256-cbc -iter 4096`.
 */
constexpr std::string_view kOpensslEccEncryptedPrivateKey = R"(-----BEGIN ENCRYPTED PRIVATE KEY-----
MIH0MF8GCSqGSIb3DQEFDTBSMDEGCSqGSIb3DQEFDDAkBBD03AtmJh3MsjDHRGKZ
vCo1AgIQADAMBggqhkiG9w0CCQUAMB0GCWCGSAFlAwQBKgQQojH7Dt4zdvvv9BQ3
Y4MdBwSBkP1p2biQinDptYIYHqc5szynEA30Hk0KHnt+D6Wlx3ipEFnya5VhfIYS
44hG+edKncFqrEc8/CEgXSZN7sPGPlokbXqh1w6hjPx3BlxpiHFQMksUJgsWffYK
XmlOmkibTqk7Sc8CY4nxknAcX2wYw2sY/o/tAyVZ0VPrMm7H9O3ltyeAfx77Qzc+
UNGsrbs6hA==
-----END ENCRYPTED PRIVATE KEY-----)";

/// The public half of `kOpensslEccEncryptedPrivateKey`, written by `openssl pkey -pubout`.
constexpr std::string_view kOpensslEccPublicKey = R"(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEW/bpwmV1Sg42ytV7YNP7r8VQhVGD
U6/YJfeDxSvXn+uP33CCofqdLkh2pfefOw2arIXTEsI4MKjDtS0UB7DYzA==
-----END PUBLIC KEY-----)";

/**
 * @brief An RSA key in the *legacy* encrypted form, produced by OpenSSL 3.
 *
 * @details
 * - Generated with `openssl genrsa -traditional -aes128 -passout ...`. The label is still `RSA PRIVATE KEY`; the
 *   password parameters live in the `Proc-Type` / `DEK-Info` headers inside the body, which is what every tool wrote
 *   before PKCS#8 took over. This is the shape a plain base64 reader mistakes for corrupt data.
 */
constexpr std::string_view kOpensslLegacyEncryptedPrivateKey = R"(-----BEGIN RSA PRIVATE KEY-----
Proc-Type: 4,ENCRYPTED
DEK-Info: AES-128-CBC,D6A0139CC77E27912550D748E357B605

aXHw6miaL9UMuDlK08hvg9oMkm8lP/qg7vXJAQF8uY1YXbelA6gsvIFlCKbgpmLZ
5wRv5Olq6/VJ4Gu2D756xi1cUi8KPau3N2VlaUrSfapDP4r58JoqvupjoNQiJdYP
CVzQV9SWlH0uzaHt0AW9Zw/Nh5NHKj0O9wkXLcit3+2GInzU4UtJqp/rVfI8uXye
9g5Hklcg2vIFULoM34zTuEp6OmXKhtoUpC2kF0I2HenBFNodP6Zhc/MDNBaj5UmT
UV4GkC33+NH7+LRCJIp7vwhSUDNlGJjBO67sBwWMRPc++qlJCyV9aBe8IE/s+Ti5
r59+/Lj/lrkwx97Gt1pUMErHKL0ht3sGEgpzLIeHfp7bGq6LapNZmE75oGHaIaz6
wUPj2CFfrKNp9voEIuvtoNo9ohZM0fZnkSBhQZgQgZZiZVBdCZrCmIy3fARFZuJ3
Bte+4HswWEARYJlGPKDvtpP7f5joWE0AMWQV/UWhZhkW5oMUam5vg6wIeyBig/X/
cGEKs+TjV5rSpPncGWtvRlzZQb5ib0W93tg8Xx9sWP/kr1E0gZ+GZZBmI2/nml3O
krVCboKThgvT2t067JSRj+f5FqoHrpFWMW0+ONavPWQ9NZMua5U2PYt055+4eVOc
FthXAuQ3xrkW0xPTRD0aQOtDuUixoUE/PJQbABBlmaRViP7BlB7DzO1rEqejAkqc
xYNRY3AQqh5Ofn76StNR6x5PRIh0N+GUrjfE4fU2PmnBoFRZc0ch6/A93Ti97eJx
r/KhC8pmGYPKgvYjVZ2vzpY3WnOBXt+ENr2PxOL7V6rDz4uP4XfdMFin2GSBHGAk
GfTK+9mAu+KhRHVqpj1TcdSHmBE2L8mfY/izoA5IRyIGxs3WwOTwXsgozohGInfD
xRPFP3l291bRXeiAI/S3hsfIJee8gZFdvgXG1AmyVJCgKdbGYQNAcsNJEH0LFgpV
beZvv1itxyiSf4XG4344OmSSAYG7s+EdkqhEmHhLuOAPhGOjpBX8P2SVRU6Sdx6o
qXA4kB+E2LHX38eoj7Fcqp36qyp2RMLLWn5NkQdArBqlGoMgI8NaQw9shoizISjR
hXnxS664xoNXX0ugBfjuGco76NjC5BH4z14QFm5p1ZsoMzVyKa6lfFdlXPVrqJzR
UVI04ozGDVjWXxtHSMyLXOYIAEcwXb+Sx7xH81B00i2v7wYKvhQIoZugkLs8SJr0
pWXZR2ZMoAupGlfxTMj2ylenCpsyZZ+zW0Nljfpi9wUXlWTGPedm974/4jeRXdS7
o+KJO+nY46ajkGTelEYR59C71s04LataeljuacLwOM4lkie2+lw8PFXKxncRpybc
pzd8/1DxbMi72xMxssbUUtoCd2wXtKa0JSIycM4qtvCs2mxGV8SOM8LDRykzytpw
6jBF7UG99CCQEG6OLNKmzrpWFWSrz+pBHJhLolBwT55z40Kw7CmlE2iCRHZ5DFfJ
EJPY6IE7Wj1DKLyRBfIMiur+6irDJEzDZSEzEV6xl+m1SF8baAizqMHl0tCYbcXw
V7C6WeafWcxUiBNo0xsLmkNQkF9EwrRj2ZIwNU/AYCLjKXuMdtd+Za2Y9TqPVsmt
-----END RSA PRIVATE KEY-----)";

/// The public half of `kOpensslLegacyEncryptedPrivateKey`, written by `openssl rsa -pubout`.
constexpr std::string_view kOpensslLegacyPublicKey = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAx051wD2EPt1KkZhMLJlI
cfsrQzk9b+VwKVGEqO4UZa0ErMdBtJqWpLB5aIqhFPqsrMakXf9zIY8Bese8iR7l
d8YPOE9SHPi1gPaT26i3R6bGKppoyBOKxHnC34i6Bepk/KzDaIxMNixDlpUnqsJ5
yjzqqcTMueM6FTnqhDpJMsbZbLzx1rlg57X4vm3JAUBho0tPoWWCjydygv64zsXP
W/3fP2Yc4A6N7Bk/TbY9pI37XqPmBl8GA4qODHUVQ/RtsiVdHF6hICGO/Yz+ELK6
nSftKIvdAhqWrGHOC/psrkBBPFvn4s9Ca3vHndIXKh4CSjhv/c0mGfW71Alf0TNZ
yQIDAQAB
-----END PUBLIC KEY-----)";

/// PKCS#5 v2 PBES2, as wire bytes: 1.2.840.113549.1.5.13.
constexpr std::array<unsigned char, 11> kPbes2Oid{ 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86,
                                                   0xF7, 0x0D, 0x01, 0x05, 0x0D };

/// AES-256-CBC, as wire bytes: 2.16.840.1.101.3.4.1.42.
constexpr std::array<unsigned char, 11> kAes256CbcOid{ 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                                       0x65, 0x03, 0x04, 0x01, 0x2A };

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

/**
 * @brief Reports whether a byte sequence occurs inside a DER blob.
 *
 * @param der     The blob to search.
 * @param pattern The sequence to look for.
 * @return True when the pattern occurs at least once.
 */
bool contains(const std::vector<std::byte>& der, std::span<const unsigned char> pattern)
{
    const auto found = std::search(der.begin(), der.end(), pattern.begin(), pattern.end(), [](std::byte lhs, unsigned char rhs) {
        return std::to_integer<unsigned char>(lhs) == rhs;
    });
    return found != der.end();
}

/**
 * @brief Generates one 2048-bit RSA key per process, so the slow part runs once.
 *
 * @return A key pair that is safe to use from any test in this file.
 */
const RsaKey& sharedRsaKey()
{
    static const RsaKey key = RsaKey::generate(2048);
    return key;
}

} // namespace

TEST(EncryptedRsaKeyTest, RoundTripsThroughEncryptedPem)
{
    const RsaKey& original = sharedRsaKey();

    const std::u8string pem = original.toEncryptedPrivatePem(u8"pass phrase", 2048);
    EXPECT_EQ(pem.rfind(u8"-----BEGIN ENCRYPTED PRIVATE KEY-----", 0), 0u) << "the block must be the PKCS#8 one";
    EXPECT_NE(pem.find(u8"-----END ENCRYPTED PRIVATE KEY-----"), std::u8string::npos);

    const RsaKey restored = RsaKey::fromEncryptedPrivatePem(pem, u8"pass phrase");
    EXPECT_EQ(restored.bits(), original.bits());
    EXPECT_TRUE(restored.hasPrivate());
    EXPECT_EQ(restored.toPublicDer(), original.toPublicDer());

    // The private half must be usable, not merely present.
    const auto message = std::vector<std::byte>{ std::byte{ 0x01 }, std::byte{ 0x02 }, std::byte{ 0x03 } };
    const auto signature = restored.sign(message);
    EXPECT_TRUE(original.verify(message, signature));

    const auto secret = std::vector<std::byte>{ std::byte{ 0x2A } };
    EXPECT_EQ(original.decrypt(restored.encrypt(secret)), secret);
}

TEST(EncryptedRsaKeyTest, RoundTripsThroughEncryptedDer)
{
    const RsaKey& original = sharedRsaKey();

    const auto der      = original.toEncryptedPrivateDer(u8"pass phrase", 1024);
    const RsaKey restored = RsaKey::fromEncryptedPrivateDer(der, u8"pass phrase");
    EXPECT_EQ(restored.toPublicDer(), original.toPublicDer());
}

TEST(EncryptedRsaKeyTest, OpensslProducesReadableFile)
{
    const RsaKey restored = RsaKey::fromEncryptedPrivatePem(asU8(kOpensslRsaEncryptedPrivateKey), kFixturePassword);
    const RsaKey expected = RsaKey::fromPublicPem(asU8(kOpensslRsaPublicKey));

    EXPECT_EQ(restored.bits(), 2048u);
    EXPECT_EQ(restored.toPublicDer(), expected.toPublicDer());
}

TEST(EncryptedRsaKeyTest, WrongPasswordIsRejected)
{
    const auto der = sharedRsaKey().toEncryptedPrivateDer(u8"pass phrase", 1024);

    EXPECT_THROW((void)RsaKey::fromEncryptedPrivateDer(der, u8"pass phrase!"), std::invalid_argument);
    EXPECT_THROW((void)RsaKey::fromEncryptedPrivateDer(der, u8""), std::invalid_argument);
}

TEST(EncryptedRsaKeyTest, LegacyPemFromOpensslIsReadable)
{
    // The pre-PKCS#8 form: traditional label, Proc-Type/DEK-Info headers inside the body.
    const RsaKey restored =
        RsaKey::fromEncryptedPrivatePem(asU8(kOpensslLegacyEncryptedPrivateKey), kFixturePassword);
    const RsaKey expected = RsaKey::fromPublicPem(asU8(kOpensslLegacyPublicKey));

    EXPECT_EQ(restored.bits(), 2048u);
    EXPECT_TRUE(restored.hasPrivate());
    EXPECT_EQ(restored.toPublicDer(), expected.toPublicDer());
}

TEST(EncryptedRsaKeyTest, LegacyPemRejectsTheWrongPassword)
{
    EXPECT_THROW((void)RsaKey::fromEncryptedPrivatePem(asU8(kOpensslLegacyEncryptedPrivateKey), u8"nope"),
                 std::invalid_argument);
    EXPECT_THROW((void)RsaKey::fromEncryptedPrivatePem(asU8(kOpensslLegacyEncryptedPrivateKey), u8""),
                 std::invalid_argument);
}

TEST(EncryptedRsaKeyTest, LegacyPemIsNotMistakenForAPlainKey)
{
    try {
        (void)RsaKey::fromPrivatePem(asU8(kOpensslLegacyEncryptedPrivateKey));
        FAIL() << "a legacy encrypted block must not import as a plain key";
    }
    catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("fromEncryptedPrivatePem"), std::string::npos) << error.what();
    }
}

TEST(EncryptedRsaKeyTest, RejectsEmptyPasswordAndZeroIterations)
{
    const RsaKey& original = sharedRsaKey();

    EXPECT_THROW((void)original.toEncryptedPrivateDer(u8"", 1024), std::invalid_argument);
    EXPECT_THROW((void)original.toEncryptedPrivatePem(u8"", 1024), std::invalid_argument);
    EXPECT_THROW((void)original.toEncryptedPrivateDer(u8"pass phrase", 0), std::invalid_argument);
}

TEST(EncryptedRsaKeyTest, PublicOnlyKeyCannotBeExported)
{
    const RsaKey public_only = RsaKey::fromPublicPem(asU8(kOpensslRsaPublicKey));

    EXPECT_THROW((void)public_only.toEncryptedPrivateDer(u8"pass phrase"), std::logic_error);
    EXPECT_THROW((void)public_only.toEncryptedPrivatePem(u8"pass phrase"), std::logic_error);
}

TEST(EncryptedRsaKeyTest, CiphertextDoesNotCarryTheKeyAndIsFreshEachTime)
{
    const RsaKey& original = sharedRsaKey();
    const auto    plain    = original.toPrivateDer();

    const auto first  = original.toEncryptedPrivateDer(u8"pass phrase", 1024);
    const auto second = original.toEncryptedPrivateDer(u8"pass phrase", 1024);

    EXPECT_NE(first, second) << "a random salt and IV must make each file different";

    const auto plain_bytes = std::span<const std::byte>(plain);
    EXPECT_FALSE(contains(first, std::span<const unsigned char>(
                                    reinterpret_cast<const unsigned char*>(plain_bytes.data()), plain_bytes.size())))
        << "the encrypted file must not contain the private key in the clear";
}

TEST(EncryptedRsaKeyTest, UsesPbes2WithAes256Cbc)
{
    const auto der = sharedRsaKey().toEncryptedPrivateDer(u8"pass phrase", 1024);

    EXPECT_TRUE(contains(der, kPbes2Oid)) << "the algorithm identifier must name PBES2";
    EXPECT_TRUE(contains(der, kAes256CbcOid)) << "the cipher must be AES-256-CBC";
}

TEST(EncryptedEccKeyTest, RoundTripsThroughEncryptedPem)
{
    const EccKey original = EccKey::generate(EccKey::Curve::kP256);

    const std::u8string pem = original.toEncryptedPrivatePem(u8"pass phrase", 1024);
    EXPECT_EQ(pem.rfind(u8"-----BEGIN ENCRYPTED PRIVATE KEY-----", 0), 0u);

    const EccKey restored = EccKey::fromEncryptedPrivatePem(pem, u8"pass phrase");
    EXPECT_EQ(restored.curve(), EccKey::Curve::kP256);
    EXPECT_EQ(restored.publicX963(), original.publicX963());

    // ECDH agreement and ECDSA signing must both survive the trip.
    const EccKey peer    = EccKey::generate(EccKey::Curve::kP256);
    EXPECT_EQ(restored.sharedSecret(peer), original.sharedSecret(peer));

    const auto message   = std::vector<std::byte>{ std::byte{ 0x0A }, std::byte{ 0x0B } };
    const auto signature = restored.sign(message);
    EXPECT_TRUE(original.verify(message, signature));
}

TEST(EncryptedEccKeyTest, RoundTripsOnP384)
{
    const EccKey original = EccKey::generate(EccKey::Curve::kP384);

    const EccKey restored = EccKey::fromEncryptedPrivateDer(original.toEncryptedPrivateDer(u8"pass phrase", 1024),
                                                            u8"pass phrase");
    EXPECT_EQ(restored.curve(), EccKey::Curve::kP384);
    EXPECT_EQ(restored.publicX963(), original.publicX963());
}

TEST(EncryptedEccKeyTest, OpensslProducesReadableFile)
{
    const EccKey restored = EccKey::fromEncryptedPrivatePem(asU8(kOpensslEccEncryptedPrivateKey), kFixturePassword);
    const EccKey expected = EccKey::fromPublicPem(asU8(kOpensslEccPublicKey));

    EXPECT_EQ(restored.curve(), EccKey::Curve::kP256);
    EXPECT_EQ(restored.publicX963(), expected.publicX963());
}

TEST(EncryptedRsaKeyTest, PlainImporterNamesTheProtectedBlock)
{
    const std::u8string pem = sharedRsaKey().toEncryptedPrivatePem(u8"pass phrase", 1024);

    // The common mistake: pasting an OpenSSL-protected key into the unencrypted importer. The failure has to name the
    // right function instead of reporting a bad ASN.1 structure.
    try {
        (void)RsaKey::fromPrivatePem(pem);
        FAIL() << "a password-protected block must not import as a plain key";
    }
    catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("fromEncryptedPrivatePem"), std::string::npos) << error.what();
    }
}

TEST(EncryptedEccKeyTest, PlainImporterNamesTheProtectedBlock)
{
    const std::u8string pem = EccKey::generate(EccKey::Curve::kP256).toEncryptedPrivatePem(u8"pass phrase", 1024);

    try {
        (void)EccKey::fromPrivatePem(pem);
        FAIL() << "a password-protected block must not import as a plain key";
    }
    catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("fromEncryptedPrivatePem"), std::string::npos) << error.what();
    }
}

TEST(EncryptedEccKeyTest, WrongPasswordIsRejected)
{
    const auto der = EccKey::generate(EccKey::Curve::kP256).toEncryptedPrivateDer(u8"pass phrase", 1024);

    EXPECT_THROW((void)EccKey::fromEncryptedPrivateDer(der, u8"pass phrase!"), std::invalid_argument);
}

TEST(EncryptedEccKeyTest, PublicOnlyKeyCannotBeExported)
{
    const EccKey public_only = EccKey::fromPublicPem(asU8(kOpensslEccPublicKey));

    EXPECT_THROW((void)public_only.toEncryptedPrivateDer(u8"pass phrase"), std::logic_error);
}
