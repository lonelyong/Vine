#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vine/crypto/EccKey.hpp>

using vn::crypto::EccKey;

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
 * @brief Returns the message the ECC tests sign.
 *
 * @return A short message whose bytes are stable across runs.
 */
std::vector<std::byte> sampleMessage()
{
    return fromHex("56696e652063727970746f206563632074657374");
}

} // namespace

TEST(EccKeyTest, SignsAndVerifiesOnP256)
{
    const EccKey key = EccKey::generate();

    EXPECT_EQ(key.curve(), EccKey::Curve::kP256);
    EXPECT_TRUE(key.hasPrivate());

    const auto message   = sampleMessage();
    const auto signature = key.sign(message);
    // DER-encoded ECDSA-Sig-Value: about 70–72 bytes on P-256, and the length moves with the integer contents.
    EXPECT_GE(signature.size(), 68u);
    EXPECT_LE(signature.size(), 74u);
    EXPECT_TRUE(key.verify(message, signature));

    EXPECT_FALSE(key.verify(fromHex("00"), signature));

    auto tampered = signature;
    tampered[5]   = static_cast<std::byte>(static_cast<unsigned char>(tampered[5]) ^ 0x01u);
    EXPECT_FALSE(key.verify(message, tampered));
    EXPECT_FALSE(key.verify(message, std::span<const std::byte>()));
}

TEST(EccKeyTest, SignsAndVerifiesOnP384)
{
    const EccKey key = EccKey::generate(EccKey::Curve::kP384);

    EXPECT_EQ(key.curve(), EccKey::Curve::kP384);

    const auto message   = sampleMessage();
    const auto signature = key.sign(message);
    // DER encoding on P-384 lands around 102–104 bytes.
    EXPECT_GE(signature.size(), 100u);
    EXPECT_LE(signature.size(), 108u);
    EXPECT_TRUE(key.verify(message, signature));
    EXPECT_FALSE(key.verify(fromHex("00"), signature));
}

TEST(EccKeyTest, RoundTripsThroughDer)
{
    const EccKey key = EccKey::generate();
    const auto   der = key.toPrivateDer();
    EXPECT_FALSE(der.empty());

    const EccKey imported = EccKey::fromPrivateDer(der);
    EXPECT_EQ(imported.curve(), key.curve());
    EXPECT_TRUE(imported.hasPrivate());

    const auto message   = sampleMessage();
    const auto signature = key.sign(message);
    EXPECT_TRUE(imported.verify(message, signature));
    EXPECT_EQ(imported.publicX963(), key.publicX963());
}

TEST(EccKeyTest, PublicPointVerifiesButCannotSign)
{
    const EccKey key = EccKey::generate();
    const auto   point = key.publicX963();
    // Uncompressed X9.63 point: 0x04 followed by two 32-byte coordinates.
    EXPECT_EQ(point.size(), 65u);
    EXPECT_EQ(point.front(), std::byte{ 0x04 });

    const EccKey publicOnly = EccKey::fromPublicX963(point);
    EXPECT_FALSE(publicOnly.hasPrivate());
    EXPECT_EQ(publicOnly.curve(), key.curve());

    const auto message   = sampleMessage();
    const auto signature = key.sign(message);
    EXPECT_TRUE(publicOnly.verify(message, signature));

    EXPECT_THROW(publicOnly.sign(message), std::logic_error);
    EXPECT_THROW(publicOnly.toPrivateDer(), std::logic_error);
    EXPECT_THROW(publicOnly.sharedSecret(key), std::logic_error);
}

TEST(EccKeyTest, BothSidesAgreeOnTheSharedSecret)
{
    const EccKey alice = EccKey::generate();
    const EccKey bob   = EccKey::generate();

    const auto aliceSecret = alice.sharedSecret(bob);
    const auto bobSecret   = bob.sharedSecret(EccKey::fromPublicX963(alice.publicX963()));

    EXPECT_EQ(aliceSecret.size(), 32u);
    EXPECT_EQ(aliceSecret, bobSecret);
    EXPECT_NE(aliceSecret, alice.sharedSecret(EccKey::generate()));
}

TEST(EccKeyTest, RoundTripsThroughPem)
{
    const EccKey key = EccKey::generate();

    const auto privatePem = key.toPrivatePem();
    EXPECT_NE(privatePem.find(u8"-----BEGIN EC PRIVATE KEY-----"), std::u8string::npos);

    const EccKey fromPem = EccKey::fromPrivatePem(privatePem);
    EXPECT_TRUE(fromPem.hasPrivate());
    EXPECT_EQ(fromPem.curve(), key.curve());

    const auto message   = sampleMessage();
    const auto signature = key.sign(message);
    EXPECT_TRUE(fromPem.verify(message, signature));

    const auto publicPem = key.toPublicPem();
    EXPECT_NE(publicPem.find(u8"-----BEGIN PUBLIC KEY-----"), std::u8string::npos);

    const EccKey verifier = EccKey::fromPublicPem(publicPem);
    EXPECT_FALSE(verifier.hasPrivate());
    EXPECT_TRUE(verifier.verify(message, signature));

    // The SubjectPublicKeyInfo export must agree with the X9.63 point export about the key itself.
    EXPECT_EQ(verifier.publicX963(), key.publicX963());

    EXPECT_THROW(EccKey::fromPrivatePem(u8"not a pem block"), std::invalid_argument);
}

TEST(EccKeyTest, RejectsMismatchedCurvesAndBadImports)
{
    const EccKey p256 = EccKey::generate(EccKey::Curve::kP256);
    const EccKey p384 = EccKey::generate(EccKey::Curve::kP384);

    EXPECT_THROW(p256.sharedSecret(p384), std::invalid_argument);
    EXPECT_THROW(EccKey::fromPrivateDer(std::span<const std::byte>()), std::invalid_argument);
    EXPECT_THROW(EccKey::fromPrivateDer(fromHex("0102030405060708")), std::invalid_argument);
    EXPECT_THROW(EccKey::fromPublicX963(std::span<const std::byte>()), std::invalid_argument);
    EXPECT_THROW(EccKey::fromPublicX963(fromHex("04aabbcc")), std::invalid_argument);
}
