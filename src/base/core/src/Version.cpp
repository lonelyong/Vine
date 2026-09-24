#include <vine/Version.hpp>

#include <string>

VN_CORE_NS_BEGIN

Version::Version(int major, int minor, int patch)
  : major_(major)
  , minor_(minor)
  , patch_(patch)
  , valid_(true)
{}

Version Version::parse(const String& text)
{
    const char8_t* p = text.data();
    const std::size_t n = text.size();
    std::size_t i = 0;

    auto parseInt = [&](int& out) -> bool {
        if (i >= n || p[i] < u8'0' || p[i] > u8'9') {
            return false;
        }
        int v = 0;
        while (i < n && p[i] >= u8'0' && p[i] <= u8'9') {
            v = v * 10 + static_cast<int>(p[i] - u8'0');
            ++i;
        }
        out = v;
        return true;
    };

    int major = 0;
    int minor = 0;
    int patch = 0;

    if (!parseInt(major)) {
        return Version();
    }
    if (i >= n || p[i] != u8'.') {
        return Version();
    }
    ++i;
    if (!parseInt(minor)) {
        return Version();
    }
    if (i >= n || p[i] != u8'.') {
        return Version();
    }
    ++i;
    if (!parseInt(patch) || i != n) {
        return Version();
    }

    return Version(major, minor, patch);
}

int Version::major() const
{
    return major_;
}

int Version::minor() const
{
    return minor_;
}

int Version::patch() const
{
    return patch_;
}

bool Version::isValid() const
{
    return valid_;
}

String Version::toString() const
{
    if (!valid_) {
        return String();
    }

    std::string s = std::to_string(major_);
    s.push_back('.');
    s += std::to_string(minor_);
    s.push_back('.');
    s += std::to_string(patch_);
    return String::fromLocal8Bit(s.data(), s.size());
}

int Version::compare(const Version& other) const
{
    if (major_ != other.major_) {
        return major_ < other.major_ ? -1 : 1;
    }
    if (minor_ != other.minor_) {
        return minor_ < other.minor_ ? -1 : 1;
    }
    if (patch_ != other.patch_) {
        return patch_ < other.patch_ ? -1 : 1;
    }
    return 0;
}

bool Version::operator==(const Version& other) const
{
    return major_ == other.major_ && minor_ == other.minor_ && patch_ == other.patch_;
}

bool Version::operator!=(const Version& other) const
{
    return !(*this == other);
}

bool Version::operator<(const Version& other) const
{
    return compare(other) < 0;
}

bool Version::operator<=(const Version& other) const
{
    return compare(other) <= 0;
}

bool Version::operator>(const Version& other) const
{
    return compare(other) > 0;
}

bool Version::operator>=(const Version& other) const
{
    return compare(other) >= 0;
}

VN_CORE_NS_END
