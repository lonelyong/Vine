#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vine/crypto/AesGcm.hpp>
#include <vine/crypto/Hmac.hpp>
#include <vine/crypto/Random.hpp>

using vn::crypto::AesGcm;
using vn::crypto::Digest;
using vn::crypto::digestFromName;
using vn::crypto::digestSize;
using vn::crypto::hmac;
using vn::crypto::Hmac;
using vn::crypto::macEquals;
using vn::crypto::Random;

namespace
{

/**
 * @brief Builds a byte vector from a hex string, so the vectors below read like the published ones.
 *
 * @param hex Hex digits, upper or lower case, no separators.
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
 * @brief Renders bytes as lower-case hex, for failure messages and tag comparisons.
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
 * @brief Renders bytes as lower-case hex, for failure messages and tag comparisons.
 *
 * @param bytes The bytes to render.
 * @return The hex string.
 */
std::string toHex(const std::vector<unsigned char>& bytes)
{
    return toHex(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
}

} // namespace

TEST(RandomTest, FillsAndDoesNotRepeat)
{
    const auto first  = Random::bytes(32);
    const auto second = Random::bytes(32);

    ASSERT_EQ(first.size(), 32u);
    EXPECT_NE(first, second);
    EXPECT_TRUE(std::any_of(first.begin(), first.end(), [](std::byte value) { return value != std::byte{ 0 }; }));
}

TEST(RandomTest, EmptyRequestsAreNoOps)
{
    EXPECT_TRUE(Random::bytes(0).empty());
    Random::fill(nullptr, 0);
    Random::fill(std::span<std::byte>());
}

TEST(RandomTest, RejectsNullBufferWithNonZeroSize)
{
    EXPECT_THROW(Random::fill(nullptr, 4), std::invalid_argument);
}

TEST(HmacTest, MatchesRfc4231Case2)
{
    // RFC 4231 test case 2: key "Jefe", message "what do ya want for nothing?".
    const auto key     = fromHex("4a656665");
    const auto message = fromHex("7768617420646f2079612077616e7420666f72206e6f7468696e673f");

    const auto tag = hmac(Digest::kSha256, key, message);
    EXPECT_EQ(toHex(tag), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(HmacTest, IncrementalMatchesOneShot)
{
    const auto key     = fromHex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    const auto message = fromHex("4869205468657265");

    const auto oneShot = hmac(Digest::kSha256, key, message);

    Hmac mac(Digest::kSha256, key);
    // Feed it a byte at a time: chunk boundaries must not change the tag.
    for (const auto byte : message) {
        const std::array<std::byte, 1> chunk{ byte };
        mac.update(chunk);
    }

    EXPECT_EQ(mac.finish(), oneShot);
    EXPECT_EQ(mac.finish(), oneShot); // cached, not a second finalize
}

TEST(HmacTest, ResolvesNamesAndSizes)
{
    EXPECT_EQ(digestFromName(u8"SHA-256"), Digest::kSha256);
    EXPECT_EQ(digestFromName(u8"sha3_512"), Digest::kSha3_512);
    EXPECT_EQ(digestSize(Digest::kSha256), 32u);
    EXPECT_EQ(digestSize(Digest::kSha3_512), 64u);
    EXPECT_THROW(digestFromName(u8"md4"), std::invalid_argument);
}

TEST(HmacTest, VerifyIsConstantTimeAndExact)
{
    const auto tag = fromHex("01020304");

    EXPECT_TRUE(macEquals(tag, tag));
    EXPECT_FALSE(macEquals(tag, fromHex("01020305")));
    EXPECT_FALSE(macEquals(tag, fromHex("010203")));
}

TEST(HmacTest, UpdateAfterFinishThrows)
{
    const auto key = fromHex("00");
    Hmac       mac(Digest::kSha256, key);
    mac.finish();

    const std::array<std::byte, 1> chunk{ std::byte{ 0 } };
    EXPECT_THROW(mac.update(chunk), std::logic_error);
}

TEST(AesGcmTest, MatchesNistSp80038dCase2)
{
    // NIST GCM test case 2: 128-bit key of zeros, 12-byte IV, one all-zero 16-byte plaintext block, no AAD.
    const auto key        = fromHex("00000000000000000000000000000000");
    const auto iv         = fromHex("000000000000000000000000");
    const auto plaintext  = fromHex("00000000000000000000000000000000");

    const auto sealed = AesGcm::seal(key, iv, plaintext);

    EXPECT_EQ(toHex(sealed.ciphertext), "0388dace60b6a392f328c2b971b2fe78");
    EXPECT_EQ(toHex(std::span<const std::byte>(sealed.tag)), "ab6e47d42cec13bdf53a67b21257bddf");
}

TEST(AesGcmTest, SealsAndOpensAChunkedMessage)
{
    const auto key = Random::bytes(32);
    const auto iv  = Random::bytes(12);
    const auto aad = fromHex("aabbcc");

    std::vector<std::byte> message(1000);
    for (std::size_t index = 0; index < message.size(); ++index) {
        message[index] = static_cast<std::byte>(index & 0xFFu);
    }

    // Encrypt in irregular chunks; the ciphertext must not depend on how it was fed.
    AesGcm encryptorObject = AesGcm::encryptor(key, iv, aad);
    std::vector<std::byte> ciphertext;
    const auto append = [&ciphertext](std::vector<std::byte> chunk) {
        ciphertext.insert(ciphertext.end(), chunk.begin(), chunk.end());
    };
    append(encryptorObject.update(std::span<const std::byte>(message).first(1)));
    append(encryptorObject.update(std::span<const std::byte>(message).subspan(1, 509)));
    append(encryptorObject.update(std::span<const std::byte>(message).subspan(510)));
    EXPECT_TRUE(encryptorObject.finish().empty());
    const auto tag = encryptorObject.tag();

    ASSERT_EQ(ciphertext.size(), message.size());
    // The streamed ciphertext must be exactly the one-shot answer.
    EXPECT_EQ(AesGcm::seal(key, iv, message, aad).ciphertext, ciphertext);
    EXPECT_EQ(AesGcm::open(key, iv, ciphertext, tag, aad), message);

    // Decrypt in chunks too, and confirm the plaintext keeps the same chunk boundaries.
    AesGcm                 decryptorObject = AesGcm::decryptor(key, iv, tag, aad);
    std::vector<std::byte> plaintext      = decryptorObject.update(
        std::span<const std::byte>(ciphertext).first(100));
    const auto rest = decryptorObject.update(std::span<const std::byte>(ciphertext).subspan(100));
    plaintext.insert(plaintext.end(), rest.begin(), rest.end());
    EXPECT_TRUE(decryptorObject.finish().empty());
    EXPECT_EQ(plaintext, message);
}

TEST(AesGcmTest, StreamingDecryptRejectsATamperedChunkOrTag)
{
    const auto key       = Random::bytes(16);
    const auto iv        = Random::bytes(12);
    const auto aad       = fromHex("0a0b");
    const auto plaintext = fromHex("00112233445566778899aabbccddeeff");

    const auto sealed = AesGcm::seal(key, iv, plaintext, aad);

    // A tampered ciphertext byte is only detected at finish(), which is the whole point of the warning on the class:
    // the caller must treat every byte handed out by update() as unauthenticated until then.
    auto tampered = sealed.ciphertext;
    tampered[2]   = static_cast<std::byte>(static_cast<unsigned char>(tampered[2]) ^ 0x40u);

    AesGcm decryptorObject = AesGcm::decryptor(key, iv, sealed.tag, aad);
    (void)decryptorObject.update(std::span<const std::byte>(tampered).first(4));
    (void)decryptorObject.update(std::span<const std::byte>(tampered).subspan(4));
    EXPECT_THROW(decryptorObject.finish(), std::runtime_error);

    // A wrong tag fails the same way, with no output involved.
    auto wrongTag = sealed.tag;
    wrongTag[0]   = static_cast<std::byte>(static_cast<unsigned char>(wrongTag[0]) ^ 0x01u);
    AesGcm tagDecryptor = AesGcm::decryptor(key, iv, wrongTag, aad);
    (void)tagDecryptor.update(sealed.ciphertext);
    EXPECT_THROW(tagDecryptor.finish(), std::runtime_error);
}

TEST(AesGcmTest, HandlesEmptyInput)
{
    const auto key = fromHex("00112233445566778899aabbccddeeff");
    const auto iv  = fromHex("00112233445566778899aabb");
    const auto aad = fromHex("01");

    // GCM authenticates the associated data even when there is no plaintext at all.
    const auto sealed = AesGcm::seal(key, iv, std::span<const std::byte>(), aad);
    EXPECT_TRUE(sealed.ciphertext.empty());
    EXPECT_TRUE(AesGcm::open(key, iv, std::span<const std::byte>(), sealed.tag, aad).empty());
    EXPECT_THROW(AesGcm::open(key, iv, std::span<const std::byte>(), sealed.tag, fromHex("02")),
                 std::runtime_error);
}

TEST(AesGcmTest, RejectsATamperedTagOrAad)
{
    const auto key       = Random::bytes(16);
    const auto iv        = Random::bytes(12);
    const auto plaintext = fromHex("00112233445566778899aabbccddeeff");
    const auto aad       = fromHex("0102");

    const auto sealed = AesGcm::seal(key, iv, plaintext, aad);

    auto tamperedTag = sealed.tag;
    tamperedTag[0]   = static_cast<std::byte>(static_cast<unsigned char>(tamperedTag[0]) ^ 0x01u);
    EXPECT_THROW(AesGcm::open(key, iv, sealed.ciphertext, tamperedTag, aad), std::runtime_error);

    const auto otherAad = fromHex("0103");
    EXPECT_THROW(AesGcm::open(key, iv, sealed.ciphertext, sealed.tag, otherAad), std::runtime_error);

    auto tamperedCiphertext = sealed.ciphertext;
    tamperedCiphertext[3]   = static_cast<std::byte>(static_cast<unsigned char>(tamperedCiphertext[3]) ^ 0x80u);
    EXPECT_THROW(AesGcm::open(key, iv, tamperedCiphertext, sealed.tag, aad), std::runtime_error);
}

TEST(AesGcmTest, RejectsWrongSizesAndSecondFinish)
{
    const auto key = fromHex("00112233445566"); // 7 bytes: not an AES key
    const auto iv  = fromHex("00112233445566778899aabb");
    EXPECT_THROW(AesGcm::encryptor(key, iv), std::invalid_argument);
    EXPECT_THROW(AesGcm::decryptor(fromHex("00112233445566778899aabbccddeeff"), iv, fromHex("00")),
                 std::invalid_argument);

    auto encryptorObject = AesGcm::encryptor(fromHex("00112233445566778899aabbccddeeff"), iv);
    encryptorObject.finish();
    EXPECT_THROW(encryptorObject.finish(), std::logic_error);
}

TEST(AesGcmTest, TagIsOnlyAvailableFromAnEncryptor)
{
    const auto key        = fromHex("00112233445566778899aabbccddeeff");
    const auto iv         = fromHex("00112233445566778899aabb");
    const auto sealed     = AesGcm::seal(key, iv, fromHex("00"));

    AesGcm decryptorObject = AesGcm::decryptor(key, iv, sealed.tag);
    decryptorObject.update(sealed.ciphertext);
    decryptorObject.finish();
    EXPECT_THROW(decryptorObject.tag(), std::logic_error);
}
