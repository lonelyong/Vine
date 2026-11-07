#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vine/crypto/RsaKey.hpp>

using vn::crypto::RsaKey;

namespace
{

/**
 * @brief Builds a byte vector from a hex string.
 *
 * @param hex Hex digits, no separators.
 * @return The decoded bytes.
 */
std::vector<std::byte> fromHex(std::string_view hex)
{
    const auto nibble = [](char ch) -> unsigned {
        if (ch >= '0' && ch <= '9') {
            return static_cast<unsigned>(ch - '0');
        }
        if (ch >= 'a' && ch <= 'f') {
            return static_cast<unsigned>(ch - 'a') + 10u;
        }
        return static_cast<unsigned>(ch - 'A') + 10u;
    };

    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
        const auto value = static_cast<unsigned char>((nibble(hex[index]) << 4) | nibble(hex[index + 1]));
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

/**
 * @brief Returns the message the RSA tests sign.
 *
 * @return A short message whose bytes are stable across runs.
 */
std::vector<std::byte> sampleMessage()
{
    return fromHex("56696e652063727970746f207273612074657374");
}

/**
 * @brief Generates one 2048-bit key for a test; generation is the slow part, so each test does it once.
 *
 * @return The generated key pair.
 */
RsaKey generateTestKey()
{
    return RsaKey::generate(2048);
}

} // namespace

TEST(RsaKeyTest, GeneratesSignsAndVerifies)
{
    const RsaKey key = generateTestKey();

    EXPECT_EQ(key.bits(), 2048u);
    EXPECT_TRUE(key.hasPrivate());

    const auto message   = sampleMessage();
    const auto signature = key.sign(message);
    EXPECT_EQ(signature.size(), 256u);
    EXPECT_TRUE(key.verify(message, signature));

    // A different message must not verify, and neither must a tampered signature.
    EXPECT_FALSE(key.verify(fromHex("00"), signature));

    auto tampered = signature;
    tampered[8]   = static_cast<std::byte>(static_cast<unsigned char>(tampered[8]) ^ 0x01u);
    EXPECT_FALSE(key.verify(message, tampered));
    EXPECT_FALSE(key.verify(message, std::span<const std::byte>()));
}

TEST(RsaKeyTest, Pkcs1SchemeIsIndependentOfPss)
{
    const RsaKey key     = generateTestKey();
    const auto   message = sampleMessage();

    const auto pkcs1 = key.sign(message, RsaKey::Scheme::kPkcs1);
    EXPECT_TRUE(key.verify(message, pkcs1, RsaKey::Scheme::kPkcs1));
    // Verifying a PKCS#1 signature as PSS must fail: the schemes are not interchangeable.
    EXPECT_FALSE(key.verify(message, pkcs1, RsaKey::Scheme::kPss));

    const auto pss = key.sign(message, RsaKey::Scheme::kPss);
    EXPECT_FALSE(key.verify(message, pss, RsaKey::Scheme::kPkcs1));
}

TEST(RsaKeyTest, SealsAndOpensAShortPayload)
{
    const RsaKey key     = generateTestKey();
    const auto   payload = fromHex("00112233445566778899aabbccddeeff");

    const auto ciphertext = key.encrypt(payload);
    EXPECT_EQ(ciphertext.size(), 256u);
    EXPECT_EQ(key.decrypt(ciphertext), payload);

    // 2048 bits minus the 66-byte OAEP-SHA-256 overhead leaves room for 190 bytes, not for 200.
    const std::vector<std::byte> too_long(200, std::byte{ 0x5A });
    EXPECT_THROW(key.encrypt(too_long), std::invalid_argument);
    EXPECT_THROW(key.encrypt(std::span<const std::byte>()), std::invalid_argument);
}

TEST(RsaKeyTest, RoundTripsThroughDer)
{
    const RsaKey key = generateTestKey();
    const auto   der = key.toPrivateDer();
    EXPECT_FALSE(der.empty());

    const RsaKey imported = RsaKey::fromPrivateDer(der);
    EXPECT_EQ(imported.bits(), key.bits());
    EXPECT_TRUE(imported.hasPrivate());

    // The re-imported key verifies signatures made by the original one, and re-encodes identically.
    const auto message   = sampleMessage();
    const auto signature = key.sign(message);
    EXPECT_TRUE(imported.verify(message, signature));
    EXPECT_EQ(imported.toPrivateDer(), der);
}

TEST(RsaKeyTest, PublicKeyDerVerifiesButCannotSign)
{
    const RsaKey key = generateTestKey();
    const auto   der = key.toPublicDer();
    EXPECT_FALSE(der.empty());

    // A verifier can hold only this: SubjectPublicKeyInfo, the same shape openssl -pubout writes.
    const RsaKey verifier = RsaKey::fromPublicDer(der);
    EXPECT_FALSE(verifier.hasPrivate());
    EXPECT_EQ(verifier.bits(), key.bits());

    const auto message   = sampleMessage();
    const auto signature = key.sign(message);
    EXPECT_TRUE(verifier.verify(message, signature));

    EXPECT_THROW(verifier.sign(message), std::logic_error);
    EXPECT_THROW(verifier.decrypt(signature), std::logic_error);
    EXPECT_THROW(verifier.toPrivateDer(), std::logic_error);
}

TEST(RsaKeyTest, RoundTripsThroughPem)
{
    const RsaKey key = generateTestKey();

    const auto privatePem = key.toPrivatePem();
    EXPECT_NE(privatePem.find(u8"-----BEGIN RSA PRIVATE KEY-----"), std::u8string::npos);
    EXPECT_NE(privatePem.find(u8"-----END RSA PRIVATE KEY-----"), std::u8string::npos);

    const RsaKey fromPem = RsaKey::fromPrivatePem(privatePem);
    EXPECT_TRUE(fromPem.hasPrivate());
    EXPECT_EQ(fromPem.bits(), key.bits());

    const auto message   = sampleMessage();
    const auto signature = key.sign(message);
    EXPECT_TRUE(fromPem.verify(message, signature));

    // A public-only consumer gets SubjectPublicKeyInfo, the same shape `openssl rsa -pubout` writes.
    const auto publicPem = key.toPublicPem();
    EXPECT_NE(publicPem.find(u8"-----BEGIN PUBLIC KEY-----"), std::u8string::npos);

    const RsaKey verifier = RsaKey::fromPublicPem(publicPem);
    EXPECT_FALSE(verifier.hasPrivate());
    EXPECT_TRUE(verifier.verify(message, signature));

    EXPECT_THROW(RsaKey::fromPrivatePem(u8"not a pem block"), std::invalid_argument);
    EXPECT_THROW(RsaKey::fromPublicPem(std::u8string_view()), std::invalid_argument);
}

TEST(RsaKeyTest, RejectsInvalidImports){
    EXPECT_THROW(RsaKey::fromPrivateDer(std::span<const std::byte>()), std::invalid_argument);
    EXPECT_THROW(RsaKey::fromPrivateDer(fromHex("0102030405060708")), std::invalid_argument);
    EXPECT_THROW(RsaKey::fromRawPublic(std::span<const std::byte>(), std::span<const std::byte>()),
                 std::invalid_argument);
    EXPECT_THROW(RsaKey::generate(1024), std::invalid_argument);
    EXPECT_THROW(RsaKey::generate(2049), std::invalid_argument);
}

TEST(RsaKeyTest, RejectsACiphertextFromAnotherKey)
{
    const RsaKey first  = generateTestKey();
    const RsaKey second = generateTestKey();

    const auto ciphertext = first.encrypt(fromHex("aabbccdd"));
    // OAEP is bound to the key: another private key rejects the block instead of returning garbage.
    EXPECT_THROW(second.decrypt(ciphertext), std::runtime_error);
}
