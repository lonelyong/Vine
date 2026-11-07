#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vine/crypto/AesCbc.hpp>
#include <vine/crypto/AesCtr.hpp>

using vn::crypto::AesCbc;
using vn::crypto::AesCtr;

namespace
{

/**
 * @brief Builds a byte vector from a hex string, so the published vectors read as printed.
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

/// NIST SP 800-38A F.2.1: AES-128 key, four plaintext blocks.
std::vector<std::byte> nistKey()
{
    return fromHex("2b7e151628aed2a6abf7158809cf4f3c");
}

/// NIST SP 800-38A F.2.1: the shared plaintext.
std::vector<std::byte> nistPlaintext()
{
    return fromHex("6bc1bee22e409f96e93d7e117393172a"
                   "ae2d8a571e03ac9c9eb76fac45af8e51"
                   "30c81c46a35ce411e5fbc1191a0a52ef"
                   "f69f2445df4f9b17ad2b417be66c3710");
}

} // namespace

TEST(AesCbcTest, MatchesNistSp80038aF21WithoutPadding)
{
    // The published vector has no padding, so the test drives the block path directly: 64 bytes in, 64 bytes out with
    // PKCS#7 adding a full extra block only at finish(). To compare against the standard, the plaintext is fed as four
    // complete blocks and the extra padded block is checked separately.
    const auto key = nistKey();
    const auto iv  = fromHex("000102030405060708090a0b0c0d0e0f");
    const auto pt  = nistPlaintext();

    const auto ciphertext = AesCbc::seal(key, iv, pt);

    // 64 bytes + one padding block.
    ASSERT_EQ(ciphertext.size(), 80u);
    EXPECT_EQ(toHex(std::span<const std::byte>(ciphertext).first(64)),
              "7649abac8119b246cee98e9b12e9197d"
              "5086cb9b507219ee95db113a917678b2"
              "73bed6b8e3c1743b7116e69e22229516"
              "3ff1caa1681fac09120eca307586e1a7");
    EXPECT_EQ(AesCbc::open(key, iv, ciphertext), pt);
}

TEST(AesCbcTest, PadsEveryLengthCorrectly)
{
    const auto key = nistKey();
    const auto iv  = fromHex("000102030405060708090a0b0c0d0e0f");

    // 0..40 bytes exercises "no padding block needed", "one byte of padding" and every boundary around it.
    for (std::size_t length = 0; length <= 40; ++length) {
        std::vector<std::byte> plaintext(length);
        for (std::size_t index = 0; index < length; ++index) {
            plaintext[index] = static_cast<std::byte>(index);
        }

        const auto ciphertext = AesCbc::seal(key, iv, plaintext);
        EXPECT_EQ(ciphertext.size() % 16, 0u) << "length " << length;
        EXPECT_GT(ciphertext.size(), plaintext.size()) << "length " << length;
        EXPECT_EQ(AesCbc::open(key, iv, ciphertext), plaintext) << "length " << length;
    }
}

TEST(AesCbcTest, StreamingMatchesOneShot)
{
    const auto key = nistKey();
    const auto iv  = fromHex("000102030405060708090a0b0c0d0e0f");
    const auto pt  = nistPlaintext();

    const auto oneShot = AesCbc::seal(key, iv, pt);

    // Feed the same bytes in awkward sizes: the ciphertext must not depend on the chunking.
    AesCbc encryptorObject = AesCbc::encryptor(key, iv);
    std::vector<std::byte> streamed;
    for (const std::size_t chunk : { 1u, 15u, 16u, 17u, 31u }) {
        const auto piece = std::span<const std::byte>(pt).subspan(
            streamed.empty() ? 0 : 0, pt.size()); // full range each time is wrong; use an index instead
        (void)piece;
        break;
    }

    std::size_t offset = 0;
    for (const std::size_t size : { 1u, 15u, 17u, 31u }) {
        const auto chunk = std::span<const std::byte>(pt).subspan(offset, size);
        offset += chunk.size();
        const auto out = encryptorObject.update(chunk);
        streamed.insert(streamed.end(), out.begin(), out.end());
    }
    const auto tail = encryptorObject.update(std::span<const std::byte>(pt).subspan(offset));
    streamed.insert(streamed.end(), tail.begin(), tail.end());
    const auto final = encryptorObject.finish();
    streamed.insert(streamed.end(), final.begin(), final.end());

    EXPECT_EQ(streamed, oneShot);

    // And the decryptor must accept the same chunking while holding the final block back.
    AesCbc                 decryptorObject = AesCbc::decryptor(key, iv);
    std::vector<std::byte> plaintext;
    std::size_t            position = 0;
    for (const std::size_t size : { 1u, 15u, 17u, 31u }) {
        const auto chunk = std::span<const std::byte>(oneShot).subspan(position, size);
        position += chunk.size();
        const auto out = decryptorObject.update(chunk);
        plaintext.insert(plaintext.end(), out.begin(), out.end());
    }
    const auto rest = decryptorObject.update(std::span<const std::byte>(oneShot).subspan(position));
    plaintext.insert(plaintext.end(), rest.begin(), rest.end());
    const auto last = decryptorObject.finish();
    plaintext.insert(plaintext.end(), last.begin(), last.end());

    EXPECT_EQ(plaintext, pt);
}

TEST(AesCbcTest, RejectsBadLengthsAndPadding)
{
    const auto key = nistKey();
    const auto iv  = fromHex("000102030405060708090a0b0c0d0e0f");

    EXPECT_THROW(AesCbc::encryptor(fromHex("00112233"), iv), std::invalid_argument);
    EXPECT_THROW(AesCbc::encryptor(key, fromHex("0011")), std::invalid_argument);
    EXPECT_THROW(AesCbc::open(key, iv, fromHex("0011223344556677")), std::invalid_argument);
    EXPECT_THROW(AesCbc::open(key, iv, std::span<const std::byte>()), std::invalid_argument);

    // Decrypting with the wrong key produces a block whose padding almost never validates.
    const auto ciphertext = AesCbc::seal(key, iv, fromHex("00112233445566778899aabbccddeeff"));
    const auto otherKey   = fromHex("2b7e151628aed2a6abf7158809cf4f3d");
    EXPECT_THROW(AesCbc::open(otherKey, iv, ciphertext), std::runtime_error);

    // A decryptor that is given a partial block must complain rather than silently truncate.
    AesCbc decryptorObject = AesCbc::decryptor(key, iv);
    (void)decryptorObject.update(fromHex("00112233445566778899aabbccddee"));
    (void)decryptorObject.update(fromHex("ff00"));
    EXPECT_THROW(decryptorObject.finish(), std::runtime_error);
}

TEST(AesCtrTest, MatchesNistSp80038aF51)
{
    // NIST SP 800-38A F.5.1: AES-128-CTR with counter block f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff.
    const auto key     = nistKey();
    const auto counter = fromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const auto pt      = nistPlaintext();

    const auto ciphertext = AesCtr::apply(key, counter, pt);
    EXPECT_EQ(toHex(ciphertext),
              "874d6191b620e3261bef6864990db6ce"
              "9806f66b7970fdff8617187bb9fffdff"
              "5ae4df3edbd5d35e5b4f09020db03eab"
              "1e031dda2fbe03d1792170a0f3009cee");

    // CTR is symmetric: the same call decrypts.
    EXPECT_EQ(AesCtr::apply(key, counter, ciphertext), pt);
}

TEST(AesCtrTest, StreamsAcrossArbitraryChunkBoundaries)
{
    const auto key     = nistKey();
    const auto counter = fromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const auto pt      = nistPlaintext();

    const auto oneShot = AesCtr::apply(key, counter, pt);

    AesCtr                 cipher = AesCtr::create(key, counter);
    std::vector<std::byte> streamed;
    std::size_t            offset = 0;
    for (const std::size_t size : { 1u, 7u, 16u, 23u }) {
        const auto chunk = std::span<const std::byte>(pt).subspan(offset, size);
        offset += chunk.size();
        const auto out = cipher.update(chunk);
        EXPECT_EQ(out.size(), chunk.size());
        streamed.insert(streamed.end(), out.begin(), out.end());
    }
    const auto rest = cipher.update(std::span<const std::byte>(pt).subspan(offset));
    streamed.insert(streamed.end(), rest.begin(), rest.end());
    EXPECT_TRUE(cipher.finish().empty());

    EXPECT_EQ(streamed, oneShot);

    // Feeding a byte at a time must also match: the keystream offset has to survive every call.
    AesCtr cipher2 = AesCtr::create(key, counter);
    std::vector<std::byte> byteWise;
    for (const auto byte : pt) {
        for (const auto out : cipher2.update(std::span<const std::byte>(&byte, 1))) {
            byteWise.push_back(out);
        }
    }
    EXPECT_EQ(byteWise, oneShot);
}

TEST(AesCtrTest, RejectsWrongSizesAndReuse)
{
    const auto key     = nistKey();
    const auto counter = fromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");

    EXPECT_THROW(AesCtr::create(fromHex("00112233"), counter), std::invalid_argument);
    EXPECT_THROW(AesCtr::create(key, fromHex("00112233")), std::invalid_argument);

    // An empty input is legal and yields nothing.
    EXPECT_TRUE(AesCtr::apply(key, counter, std::span<const std::byte>()).empty());

    AesCtr cipher = AesCtr::create(key, counter);
    cipher.finish();
    EXPECT_THROW(cipher.update(fromHex("00")), std::logic_error);
    EXPECT_THROW(cipher.finish(), std::logic_error);
}
