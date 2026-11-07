#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vine/crypto/Hkdf.hpp>

using vn::crypto::Digest;
using vn::crypto::Hkdf;

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
 * @brief Renders bytes as lower-case hex, for comparing against the RFC's printed vectors.
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

/// Repeats one byte, which is how RFC 5869 writes its inputs.
std::vector<std::byte> repeat(std::byte value, std::size_t count)
{
    return std::vector<std::byte>(count, value);
}

} // namespace

TEST(HkdfTest, MatchesRfc5869TestCase1)
{
    // RFC 5869 A.1: SHA-256, IKM = 0x0b * 22, salt = 0x000102...0c, info = 0xf0f1...f9, L = 42.
    const auto ikm  = repeat(std::byte{ 0x0b }, 22);
    const auto salt = fromHex("000102030405060708090a0b0c");
    const auto info = fromHex("f0f1f2f3f4f5f6f7f8f9");

    const auto prk = Hkdf::extract(Digest::kSha256, salt, ikm);
    EXPECT_EQ(toHex(prk), "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");

    const auto okm = Hkdf::expand(Digest::kSha256, prk, info, 42);
    EXPECT_EQ(toHex(okm),
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
}

TEST(HkdfTest, DeriveEqualsExtractThenExpand)
{
    const auto ikm  = fromHex("00112233445566778899aabbccddeeff");
    const auto salt = fromHex("aabbcc");
    const auto info = fromHex("6b65792d31");

    const auto combined = Hkdf::derive(Digest::kSha256, ikm, salt, info, 64);
    const auto split    = Hkdf::expand(Digest::kSha256, Hkdf::extract(Digest::kSha256, salt, ikm), info, 64);

    EXPECT_EQ(combined, split);
    EXPECT_EQ(combined.size(), 64u);
}

TEST(HkdfTest, InfoSeparatesOutputs)
{
    // The same PRK under different info values must give unrelated keys.
    const auto ikm  = fromHex("00112233");
    const auto salt = fromHex("44556677");

    const auto first  = Hkdf::derive(Digest::kSha256, ikm, salt, fromHex("01"), 32);
    const auto second = Hkdf::derive(Digest::kSha256, ikm, salt, fromHex("02"), 32);

    EXPECT_NE(first, second);
}

TEST(HkdfTest, AcceptsAnEmptySaltAndInfo)
{
    // RFC 5869 allows both to be absent; the extract step still runs with a zero-filled salt.
    const auto ikm = fromHex("00112233445566");

    const auto derived = Hkdf::derive(Digest::kSha256, ikm, std::span<const std::byte>(),
                                      std::span<const std::byte>(), 32);
    EXPECT_EQ(derived.size(), 32u);
    EXPECT_EQ(derived, Hkdf::derive(Digest::kSha256, ikm, std::span<const std::byte>(),
                                    std::span<const std::byte>(), 32));
}

TEST(HkdfTest, RejectsImpossibleLengths)
{
    const auto ikm = fromHex("00112233");

    EXPECT_THROW(Hkdf::derive(Digest::kSha256, ikm, {}, {}, 0), std::invalid_argument);
    // 255 * 32 bytes is the RFC limit for SHA-256.
    EXPECT_THROW(Hkdf::derive(Digest::kSha256, ikm, {}, {}, 255 * 32 + 1), std::invalid_argument);
    EXPECT_EQ(Hkdf::derive(Digest::kSha256, ikm, {}, {}, 255 * 32).size(), 255u * 32u);
}
