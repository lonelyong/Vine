#include <vine/Uuid.hpp>

#include <cstdio>
#include <string>

#ifdef _WIN32
#    include <windows.h>
// RtlGenRandom (SystemFunction036) is exported by advapi32.
extern "C" BOOLEAN NTAPI SystemFunction036(PVOID pbBuffer, ULONG dwLength);
#endif // _WIN32

VN_CORE_NS_BEGIN

namespace
{

/// Fills 16 bytes with cryptographically random data.
bool fillRandom(std::array<std::uint8_t, 16>& out)
{
#if defined(_WIN32)
    return ::SystemFunction036(out.data(), static_cast<ULONG>(out.size())) != FALSE;
#else
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) {
        return false;
    }
    const std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
#endif
}

/// Converts one hex character to its value; returns false if invalid.
bool hexValue(char8_t c, std::uint8_t& out)
{
    if (c >= u8'0' && c <= u8'9') {
        out = static_cast<std::uint8_t>(c - u8'0');
        return true;
    }
    if (c >= u8'a' && c <= u8'f') {
        out = static_cast<std::uint8_t>(c - u8'a' + 10);
        return true;
    }
    if (c >= u8'A' && c <= u8'F') {
        out = static_cast<std::uint8_t>(c - u8'A' + 10);
        return true;
    }
    return false;
}

} // namespace

Uuid::Uuid(const std::array<std::uint8_t, 16>& bytes)
  : bytes_(bytes)
{}

Uuid Uuid::create()
{
    std::array<std::uint8_t, 16> bytes{};
    if (fillRandom(bytes)) {
        // Version 4: set version (0100) and variant (10xx) bits.
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
    }
    return Uuid(bytes);
}

Uuid Uuid::null()
{
    return Uuid();
}

Uuid Uuid::parse(const String& text)
{
    if (text.size() != 36) {
        return Uuid();
    }

    std::array<std::uint8_t, 16> bytes{};
    const char8_t* p = text.data();
    std::size_t byte_index = 0;
    int nibble = 0;

    for (std::size_t i = 0; i < 36; ++i) {
        const char8_t c = p[i];
        if (c == u8'-') {
            if (i != 8 && i != 13 && i != 18 && i != 23) {
                return Uuid();
            }
            continue;
        }
        if (byte_index >= 16) {
            return Uuid();
        }
        std::uint8_t v = 0;
        if (!hexValue(c, v)) {
            return Uuid();
        }
        if (nibble == 0) {
            bytes[byte_index] = static_cast<std::uint8_t>(v << 4);
            nibble = 1;
        } else {
            bytes[byte_index] |= v;
            ++byte_index;
            nibble = 0;
        }
    }

    if (byte_index != 16 || nibble != 0) {
        return Uuid();
    }
    return Uuid(bytes);
}

const std::array<std::uint8_t, 16>& Uuid::bytes() const
{
    return bytes_;
}

bool Uuid::isNull() const
{
    for (const auto b : bytes_) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

String Uuid::toString() const
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            s.push_back('-');
        }
        s.push_back(hex[bytes_[i] >> 4]);
        s.push_back(hex[bytes_[i] & 0x0F]);
    }
    return String::fromLocal8Bit(s.data(), s.size());
}

bool Uuid::operator==(const Uuid& other) const
{
    return bytes_ == other.bytes_;
}

bool Uuid::operator!=(const Uuid& other) const
{
    return !(*this == other);
}

bool Uuid::operator<(const Uuid& other) const
{
    for (std::size_t i = 0; i < 16; ++i) {
        if (bytes_[i] != other.bytes_[i]) {
            return bytes_[i] < other.bytes_[i];
        }
    }
    return false;
}

VN_CORE_NS_END
