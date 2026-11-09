#include "Pem.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <wolfssl/wolfcrypt/asn_public.h>

VN_CRYPTO_NS_BEGIN

namespace detail
{

namespace
{

/// The armour a PEM block is wrapped in.
constexpr std::string_view kBeginMarker = "-----BEGIN ";
constexpr std::string_view kEndMarker   = "-----END ";
constexpr std::string_view kDashes      = "-----";

/**
 * @brief Returns the 6-bit value of one base64 character.
 *
 * @param ch The character.
 * @return 0..63 for a base64 digit, or -1 when the character is not one.
 */
int base64Value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

/**
 * @brief Reads a view of the caller's UTF-8 text as plain characters.
 *
 * @param text The text.
 * @return The same bytes, typed as chars.
 */
std::string_view asChars(std::u8string_view text)
{
    return std::string_view(reinterpret_cast<const char*>(text.data()), text.size());
}

/// The label of a password-protected PKCS#8 block; the only label whose bytes are still encrypted.
constexpr std::string_view kEncryptedPrivateKeyLabel = "ENCRYPTED PRIVATE KEY";

/// The header line the legacy encrypted form puts inside the body, before the base64.
constexpr std::string_view kLegacyProcTypeMarker = "Proc-Type:";

/**
 * @brief The parts of a PEM block's label line.
 */
struct PemHeader
{
    std::string_view label;      ///< The label without its dashes, for example `PRIVATE KEY`.
    std::size_t      body_begin; ///< Index of the first character after the label line.
};

/**
 * @brief Reads the label line of the first PEM block, without throwing.
 *
 * @param text The PEM text, already viewed as plain characters.
 * @return The label and where its body starts, or nothing when the text has no terminated BEGIN line.
 */
std::optional<PemHeader> tryParsePemHeader(std::string_view text) noexcept
{
    const auto begin = text.find(kBeginMarker);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const auto label_begin = begin + kBeginMarker.size();
    const auto label_end   = text.find(kDashes, label_begin);
    if (label_end == std::string_view::npos) {
        return std::nullopt;
    }
    return PemHeader{ text.substr(label_begin, label_end - label_begin), label_end + kDashes.size() };
}

/**
 * @brief Reads the label line of the first PEM block.
 *
 * @param text The PEM text, already viewed as plain characters.
 * @return The label and where its body starts.
 * @throws std::invalid_argument when the text has no terminated PEM BEGIN line.
 */
PemHeader parsePemHeader(std::string_view text)
{
    const auto parsed = tryParsePemHeader(text);
    if (!parsed.has_value()) {
        throw std::invalid_argument("vn::crypto: the text has no PEM BEGIN line");
    }
    return *parsed;
}

} // namespace

bool pemIsEncryptedPrivateKey(std::u8string_view pem)
{
    if (pem.empty()) {
        return false;
    }
    const auto parsed = tryParsePemHeader(asChars(pem));
    return parsed.has_value() && parsed->label == kEncryptedPrivateKeyLabel;
}

bool pemIsLegacyEncrypted(std::u8string_view pem)
{
    if (pem.empty()) {
        return false;
    }
    const auto text   = asChars(pem);
    const auto parsed = tryParsePemHeader(text);
    if (!parsed.has_value()) {
        return false;
    }
    const auto end = text.find(kEndMarker, parsed->body_begin);
    if (end == std::string_view::npos) {
        return false;
    }
    return text.substr(parsed->body_begin, end - parsed->body_begin).find(kLegacyProcTypeMarker) != std::string_view::npos;
}

std::vector<std::byte> pemDecryptLegacy(std::u8string_view pem, std::u8string_view password)
{
    if (pem.empty()) {
        throw std::invalid_argument("vn::crypto: the PEM text is empty");
    }
    if (password.empty()) {
        throw std::invalid_argument("vn::crypto: the password must not be empty");
    }

    const auto text     = asChars(pem);
    const auto password_text = std::string(asChars(password));

    // The backend wants the required length first, then the buffer.
    const auto* pem_bytes = reinterpret_cast<const unsigned char*>(text.data());
    const int   needed    = wc_KeyPemToDer(pem_bytes, static_cast<int>(text.size()), nullptr, 0, password_text.c_str());
    if (needed <= 0) {
        throw std::invalid_argument(
            "vn::crypto: the encrypted private key was rejected (wrong password or unsupported legacy encoding)");
    }

    std::vector<std::byte> der(static_cast<std::size_t>(needed));
    const int              written = wc_KeyPemToDer(pem_bytes, static_cast<int>(text.size()),
                                                    reinterpret_cast<unsigned char*>(der.data()), needed,
                                                    password_text.c_str());
    if (written <= 0) {
        throw std::invalid_argument(
            "vn::crypto: the encrypted private key was rejected (wrong password or unsupported legacy encoding)");
    }

    der.resize(static_cast<std::size_t>(written));
    return der;
}

std::u8string derToPem(std::span<const std::byte> der, int type)
{
    if (der.empty()) {
        throw std::invalid_argument("vn::crypto: the DER bytes are empty");
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(der.data());

    // The backend answers the required length (terminator included) when asked with an empty buffer.
    const int needed = wc_DerToPem(bytes, static_cast<word32>(der.size()), nullptr, 0, type);
    if (needed <= 0) {
        throw std::runtime_error("vn::crypto: the DER bytes could not be encoded as PEM");
    }

    std::u8string pem(static_cast<std::size_t>(needed), u8'\0');
    const int   written =
        wc_DerToPem(bytes, static_cast<word32>(der.size()), reinterpret_cast<unsigned char*>(pem.data()),
                    static_cast<word32>(pem.size()), type);
    if (written <= 0) {
        throw std::runtime_error("vn::crypto: the DER bytes could not be encoded as PEM");
    }

    // The returned length counts the terminator; a C++ string carries its own, so drop it.
    pem.resize(static_cast<std::size_t>(written));
    while (!pem.empty() && pem.back() == u8'\0') {
        pem.pop_back();
    }
    return pem;
}

std::vector<std::byte> pemToDer(std::u8string_view pem)
{
    if (pem.empty()) {
        throw std::invalid_argument("vn::crypto: the PEM text is empty");
    }

    const std::string_view text = asChars(pem);

    // Locate the armour. The label itself is not interpreted here: the DER decoders that follow know what shape they
    // expect, so a `PUBLIC KEY` block fed to a private-key importer fails there with a precise message.
    const auto header = parsePemHeader(text);
    const auto end    = text.find(kEndMarker, header.body_begin);
    if (end == std::string_view::npos) {
        throw std::invalid_argument("vn::crypto: the text has no PEM END line");
    }
    if (end < header.body_begin) {
        throw std::invalid_argument("vn::crypto: the PEM block is empty");
    }

    // Collect the base64 digits, ignoring the line breaks the format inserts.
    std::string digits;
    digits.reserve(end - header.body_begin);
    for (const char ch : text.substr(header.body_begin, end - header.body_begin)) {
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') {
            continue;
        }
        if (ch == '=') {
            break;
        }
        if (base64Value(ch) < 0) {
            throw std::invalid_argument("vn::crypto: the PEM body is not base64");
        }
        digits.push_back(ch);
    }

    if (digits.size() % 4 == 1) {
        throw std::invalid_argument("vn::crypto: the PEM body has a truncated base64 group");
    }

    std::vector<std::byte> der;
    der.reserve(digits.size() / 4 * 3 + 2);

    // Decode four digits into three bytes at a time.
    std::uint32_t accumulator = 0;
    int           held        = 0;
    for (const char ch : digits) {
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(base64Value(ch));
        held += 6;
        if (held >= 8) {
            held -= 8;
            der.push_back(static_cast<std::byte>((accumulator >> held) & 0xFFu));
        }
    }

    if (der.empty()) {
        throw std::invalid_argument("vn::crypto: the PEM block carries no data");
    }

    return der;
}

} // namespace detail

VN_CRYPTO_NS_END
