#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <vine/crypto/AesCbc.hpp>
#include <vine/crypto/AesGcm.hpp>
#include <vine/crypto/EccKey.hpp>
#include <vine/crypto/Random.hpp>
#include <vine/crypto/RsaKey.hpp>

using vn::crypto::AesCbc;
using vn::crypto::AesGcm;
using vn::crypto::EccKey;
using vn::crypto::Random;
using vn::crypto::RsaKey;

namespace
{

/// Fixed seed: a mutant that breaks something must be reproducible from the test name alone.
constexpr std::uint32_t kSeed = 20261107u;

/// Mutants per input. Enough to reach every region of a few-hundred-byte key file, cheap enough to keep the suite fast.
constexpr int kMutantsPerInput = 240;

/**
 * @brief Produces one damaged copy of the input: a flipped bit, a truncation, or one injected byte.
 *
 * @details
 * - The three shapes cover what a truncated download, a bit rot and an off-by-one edit do to a file; the point is not
 *   to model an attacker but to reach code paths the hand-written cases never visit.
 *
 * @param bytes The original bytes.
 * @param rng   The generator, seeded once per test.
 * @return The damaged copy.
 */
std::vector<std::byte> mutate(std::vector<std::byte> bytes, std::mt19937& rng)
{
    switch (rng() % 3u) {
        case 0: {
            if (!bytes.empty()) {
                bytes[rng() % bytes.size()] ^= static_cast<std::byte>(1u << (rng() % 8u));
            }
            break;
        }
        case 1: {
            if (bytes.size() > 1) {
                bytes.resize(rng() % bytes.size());
            }
            break;
        }
        default: {
            const auto position = static_cast<std::ptrdiff_t>(rng() % (bytes.size() + 1));
            bytes.insert(bytes.begin() + position, static_cast<std::byte>(static_cast<unsigned char>(rng())));
            break;
        }
    }
    return bytes;
}

/**
 * @brief Damages text the same way, over characters rather than bytes.
 *
 * @param text The original text.
 * @param rng  The generator, seeded once per test.
 * @return The damaged copy.
 */
std::string mutateText(std::string text, std::mt19937& rng)
{
    switch (rng() % 3u) {
        case 0: {
            if (!text.empty()) {
                text[rng() % text.size()] = static_cast<char>('!' + (rng() % 90u));
            }
            break;
        }
        case 1: {
            if (text.size() > 1) {
                text.resize(rng() % text.size());
            }
            break;
        }
        default: {
            text.insert(static_cast<std::ptrdiff_t>(rng() % (text.size() + 1)), 1, static_cast<char>('!' + (rng() % 90u)));
            break;
        }
    }
    return text;
}

/**
 * @brief The outcome of feeding every mutant of one input to one parser.
 */
struct MutantOutcome
{
    int accepted{ 0 }; ///< Mutants that still produced a value.
    int rejected{ 0 }; ///< Mutants that were refused with an exception.

    int total() const
    {
        return accepted + rejected;
    }
};

/**
 * @brief Runs one parser over every mutant of one input.
 *
 * @details
 * - The assertion the harness can make is "no crash and no silent half-object": a mutant is allowed to produce a
 *   different but usable value (flipping a bit inside a private scalar yields another valid key), and it is allowed to
 *   be refused. What it may not do is abort the process, and what it returns must survive a cheap consistency check.
 * - Memory errors are not visible here; they are what the sanitizer run in `.github` covers.
 *
 * @param original The valid input.
 * @param parse    A callable taking the mutant bytes and returning a value or throwing.
 * @param check    A callable that exercises the returned value; called only for accepted mutants.
 * @return How many mutants were accepted and rejected.
 */
template <typename Parse, typename Check>
MutantOutcome runMutants(const std::vector<std::byte>& original, Parse parse, Check check)
{
    std::mt19937  rng(kSeed);
    MutantOutcome outcome;

    for (int index = 0; index < kMutantsPerInput; ++index) {
        const auto mutant = mutate(original, rng);
        try {
            check(parse(std::span<const std::byte>(mutant)));
            ++outcome.accepted;
        }
        catch (const std::exception&) {
            ++outcome.rejected;
        }
    }
    return outcome;
}

/// One RSA key per process: generating it again for each test would dominate the run time.
const RsaKey& sharedRsaKey()
{
    static const RsaKey key = RsaKey::generate(2048);
    return key;
}

/// One ECC key per process, for the same reason.
const EccKey& sharedEccKey()
{
    static const EccKey key = EccKey::generate(EccKey::Curve::kP256);
    return key;
}

} // namespace

TEST(ParserMutationTest, PrivateKeyDerNeverCrashesAndNeverYieldsAHalfKey)
{
    const auto original = sharedRsaKey().toPrivateDer();

    const auto outcome = runMutants(
        original,
        [](std::span<const std::byte> bytes) { return RsaKey::fromPrivateDer(bytes); },
        [](const RsaKey& key) {
            EXPECT_GT(key.bits(), 0u);
            EXPECT_TRUE(key.hasPrivate());
        });

    EXPECT_EQ(outcome.total(), kMutantsPerInput);
    // A flipped bit inside the private scalar is still a valid key, so most mutants are expected to parse.
    EXPECT_GT(outcome.accepted, 0);
}

TEST(ParserMutationTest, PublicKeyDerNeverCrashesAndNeverYieldsAHalfKey)
{
    const auto original = sharedRsaKey().toPublicDer();

    const auto outcome = runMutants(
        original,
        [](std::span<const std::byte> bytes) { return RsaKey::fromPublicDer(bytes); },
        [](const RsaKey& key) {
            EXPECT_GT(key.bits(), 0u);
            EXPECT_FALSE(key.hasPrivate());
        });

    EXPECT_EQ(outcome.total(), kMutantsPerInput);
    EXPECT_GT(outcome.accepted, 0);
}

TEST(ParserMutationTest, EccPointNeverCrashesAndRejectsWhatIsOffTheCurve)
{
    const auto original = sharedEccKey().publicX963();

    const auto outcome = runMutants(
        original,
        [](std::span<const std::byte> bytes) { return EccKey::fromPublicX963(bytes); },
        [](const EccKey& key) {
            EXPECT_EQ(key.curve(), EccKey::Curve::kP256);
            EXPECT_FALSE(key.hasPrivate());
        });

    EXPECT_EQ(outcome.total(), kMutantsPerInput);
    // An imported point is checked for being on the curve, so almost every damaged point must be refused.
    EXPECT_GT(outcome.rejected, outcome.accepted);
}

TEST(ParserMutationTest, EncryptedPrivateKeyRejectsDamagedCiphertext)
{
    const auto original = sharedRsaKey().toEncryptedPrivateDer(u8"pass phrase", 1024);

    const auto outcome = runMutants(
        original,
        [](std::span<const std::byte> bytes) { return RsaKey::fromEncryptedPrivateDer(bytes, u8"pass phrase"); },
        [](const RsaKey& key) { EXPECT_GT(key.bits(), 0u); });

    EXPECT_EQ(outcome.total(), kMutantsPerInput);
    // Measured on this key: about a quarter of the single-byte mutants are still *accepted*, as a different key.
    // That is not a bug in the importer: PBES2 is CBC under a password, so the ciphertext carries no MAC, and almost
    // all of an RSA key is opaque integer content whose structure survives any bit flip. It is the empirical form of
    // `P19` in `.ai/design/crypto.md`: a password-protected key file detects structural damage, not tampering.
    EXPECT_GT(outcome.rejected, outcome.accepted);
}

TEST(ParserMutationTest, PemTextNeverCrashes)
{
    const std::u8string pem = sharedRsaKey().toPrivatePem();
    const auto          original =
        std::string(reinterpret_cast<const char*>(pem.data()), pem.size());

    std::mt19937 rng(kSeed);
    int          accepted = 0;
    int          rejected = 0;
    for (int index = 0; index < kMutantsPerInput; ++index) {
        const auto mutant = mutateText(original, rng);
        try {
            const auto key = RsaKey::fromPrivatePem(
                std::u8string_view(reinterpret_cast<const char8_t*>(mutant.data()), mutant.size()));
            EXPECT_GT(key.bits(), 0u);
            ++accepted;
        }
        catch (const std::exception&) {
            ++rejected;
        }
    }

    EXPECT_EQ(accepted + rejected, kMutantsPerInput);
    // Base64 has no redundancy worth speaking of here: most damage lands in the decoded key and is caught by ASN.1.
    EXPECT_GT(rejected, 0);
}

TEST(ParserMutationTest, AesGcmRejectsEveryMutation)
{
    const auto key       = Random::bytes(32);
    const auto iv        = Random::bytes(12);
    const auto plaintext = Random::bytes(64);
    const auto sealed    = AesGcm::seal(key, iv, plaintext);

    const std::vector<std::byte> original_tag(sealed.tag.begin(), sealed.tag.end());

    std::mt19937 rng(kSeed);
    int          rejected = 0;
    for (int index = 0; index < kMutantsPerInput; ++index) {
        auto ciphertext = sealed.ciphertext;
        auto tag        = original_tag;

        switch (rng() % 3u) {
            case 0:
                ciphertext = mutate(ciphertext, rng);
                break;
            case 1:
                tag = mutate(tag, rng);
                break;
            default:
                break;
        }

        const bool changed = ciphertext != sealed.ciphertext || tag != original_tag;
        if (changed) {
            // Which exception depends on where the damage landed: a wrong tag length is an argument error, a wrong tag
            // value is a verification failure. The invariant is that no damaged input is ever opened.
            EXPECT_ANY_THROW((void)AesGcm::open(key, iv, ciphertext, tag));
            ++rejected;
        }
    }

    // AEAD is authenticated, so this one is an absolute rule rather than a probability.
    EXPECT_GT(rejected, 0);
}

TEST(CipherModeTest, ObjectsReportTheirOwnDirection)
{
    const auto key = Random::bytes(32);
    const auto iv  = Random::bytes(16);

    EXPECT_TRUE(AesGcm::encryptor(key, iv).isEncrypting());
    EXPECT_FALSE(AesGcm::decryptor(key, iv, std::vector<std::byte>(AesGcm::kTagSize)).isEncrypting());

    EXPECT_TRUE(AesCbc::encryptor(key, iv).isEncrypting());
    EXPECT_FALSE(AesCbc::decryptor(key, iv).isEncrypting());
}
