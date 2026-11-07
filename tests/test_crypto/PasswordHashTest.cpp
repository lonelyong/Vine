#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vine/crypto/Argon2.hpp>
#include <vine/crypto/Pbkdf2.hpp>

using vn::crypto::Argon2;
using vn::crypto::Digest;
using vn::crypto::Pbkdf2;

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
 * @brief Renders bytes as lower-case hex.
 *
 * @param bytes The bytes to render.
 * @return The hex string.
 */
std::string toHex(std::span<const std::byte> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        out.push_back(kDigits[value >> 4]);
        out.push_back(kDigits[value & 0x0Fu]);
    }
    return out;
}

/**
 * @brief Encodes ASCII text as bytes, for the RFC vectors that spell the password out.
 *
 * @param text The text.
 * @return The bytes.
 */
std::vector<std::byte> text(std::string_view value)
{
    std::vector<std::byte> out;
    out.reserve(value.size());
    for (const char ch : value) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

} // namespace

TEST(Pbkdf2Test, MatchesRfc6070Vectors)
{
    // RFC 6070 (PBKDF2-HMAC-SHA1): P = "password", S = "salt".
    const auto password = text("password");
    const auto salt     = text("salt");

    EXPECT_EQ(toHex(Pbkdf2::derive(Digest::kSha1, password, salt, 1, 20)),
              "0c60c80f961f0e71f3a9b524af6012062fe037a6");
    EXPECT_EQ(toHex(Pbkdf2::derive(Digest::kSha1, password, salt, 2, 20)),
              "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
    EXPECT_EQ(toHex(Pbkdf2::derive(Digest::kSha1, password, salt, 4096, 20)),
              "4b007901b765489abead49d926f721d065a429c1");

    // RFC 6070's fourth vector uses its own password and a 25-byte output.
    const auto longPassword = text("passwordPASSWORDpassword");
    const auto longSalt     = text("saltSALTsaltSALTsaltSALTsaltSALTsalt");
    EXPECT_EQ(toHex(Pbkdf2::derive(Digest::kSha1, longPassword, longSalt, 4096, 25)),
              "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038");
}

TEST(Pbkdf2Test, IterationsAndSaltChange)
{
    const auto password = text("correct horse battery staple");
    const auto salt     = fromHex("00112233445566778899aabbccddeeff");

    const auto once   = Pbkdf2::derive(Digest::kSha256, password, salt, 1000, 32);
    const auto twice  = Pbkdf2::derive(Digest::kSha256, password, salt, 2000, 32);
    const auto other  = Pbkdf2::derive(Digest::kSha256, password, fromHex("ffeeddccbbaa99887766554433221100"),
                                       1000, 32);
    EXPECT_EQ(once.size(), 32u);
    EXPECT_NE(once, twice);
    EXPECT_NE(once, other);
    // Deterministic for identical inputs, which is what verification relies on.
    EXPECT_EQ(once, Pbkdf2::derive(Digest::kSha256, password, salt, 1000, 32));
}

TEST(Pbkdf2Test, RejectsWeakParameters)
{
    const auto password = text("pw");
    EXPECT_THROW(Pbkdf2::derive(Digest::kSha256, password, std::span<const std::byte>(), 1000, 32),
                 std::invalid_argument);
    EXPECT_THROW(Pbkdf2::derive(Digest::kSha256, password, fromHex("0011223344556677"), 0, 32),
                 std::invalid_argument);
    EXPECT_THROW(Pbkdf2::derive(Digest::kSha256, password, fromHex("0011223344556677"), 1000, 0),
                 std::invalid_argument);
}

TEST(Argon2Test, IsDeterministicAndParameterDependent)
{
    // Small parameters keep the test fast; production values are far higher (see the header).
    const auto password = text("password");
    const auto salt     = fromHex("00112233445566778899aabbccddeeff");

    const auto baseline = Argon2::derive(Argon2::Variant::kId, password, salt, 1, 1024, 2, 32);
    EXPECT_EQ(baseline.size(), 32u);
    EXPECT_EQ(baseline, Argon2::derive(Argon2::Variant::kId, password, salt, 1, 1024, 2, 32));

    // Every input that a verifier stores separately must change the answer.
    EXPECT_NE(baseline, Argon2::derive(Argon2::Variant::kId, password, salt, 1, 1024, 3, 32));
    EXPECT_NE(baseline, Argon2::derive(Argon2::Variant::kId, password, salt, 1, 2048, 2, 32));
    EXPECT_NE(baseline, Argon2::derive(Argon2::Variant::kId, password, fromHex("ffeeddccbbaa99887766554433221100"),
                                       1, 1024, 2, 32));

    // The three variants are different functions.
    const auto dataDependent = Argon2::derive(Argon2::Variant::kD, password, salt, 1, 1024, 2, 32);
    const auto independent   = Argon2::derive(Argon2::Variant::kI, password, salt, 1, 1024, 2, 32);
    EXPECT_NE(baseline, dataDependent);
    EXPECT_NE(baseline, independent);
    EXPECT_NE(dataDependent, independent);
}

TEST(Argon2Test, RejectsImpossibleParameters)
{
    const auto password = text("pw");
    const auto salt     = fromHex("00112233445566778899aabbccddeeff");

    EXPECT_THROW(Argon2::derive(Argon2::Variant::kId, password, fromHex("00112233"), 1, 1024, 2, 32),
                 std::invalid_argument);
    EXPECT_THROW(Argon2::derive(Argon2::Variant::kId, password, salt, 0, 1024, 2, 32), std::invalid_argument);
    EXPECT_THROW(Argon2::derive(Argon2::Variant::kId, password, salt, 4, 16, 2, 32), std::invalid_argument);
    EXPECT_THROW(Argon2::derive(Argon2::Variant::kId, password, salt, 1, 1024, 0, 32), std::invalid_argument);
    EXPECT_THROW(Argon2::derive(Argon2::Variant::kId, password, salt, 1, 1024, 2, 3), std::invalid_argument);
}
