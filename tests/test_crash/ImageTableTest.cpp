#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "CrashDetail.hpp"

#if defined(__linux__)

namespace {

/// @brief A report whose frames are the addresses given, in order.
vn::crash::Report reportWithFrames(const std::initializer_list<std::uintptr_t>& addresses)
{
    vn::crash::Report report;
    for (const std::uintptr_t address : addresses) {
        vn::crash::ReportFrame& frame = report.frames[report.frame_count++];
        frame.address                 = address;
        frame.image                   = -1;
        frame.offset                  = 0;
    }
    return report;
}

/// @brief One representative `/proc/self/maps`: two segments of the executable, one library, two anonymous ranges.
constexpr const char* kMaps =
    "55d4c0e0d000-55d4c0e0e000 r--p 00000000 08:02 1234567                    /opt/vine/bin/Vine\n"
    "55d4c0e0e000-55d4c1a4f000 r-xp 00001000 08:02 1234567                    /opt/vine/bin/Vine\n"
    "7f2b4c000000-7f2b4c021000 r-xp 00000000 08:02 7654321                    /usr/lib/x86_64-linux-gnu/libc.so.6\n"
    "7f2b4c200000-7f2b4c204000 rw-p 00000000 00:00 0                          [heap]\n"
    "7ffd3b0a3000-7ffd3b0c4000 rw-p 00000000 00:00 0                          [stack]\n";

} // namespace

TEST(ImageTableTest, FramesResolveToTheirImageAndOffset)
{
    vn::crash::Report report = reportWithFrames({ 0x55d4c0e0e100ULL, 0x7f2b4c001200ULL });

    const std::size_t count = vn::crash::detail::resolveImageTable(kMaps, report);

    ASSERT_EQ(count, 2u);
    ASSERT_EQ(report.image_count, 2u);
    EXPECT_EQ(report.frames[0].image, 0);
    EXPECT_EQ(report.frames[0].offset, 0x1100ULL);
    EXPECT_EQ(report.frames[1].image, 1);
    EXPECT_EQ(report.frames[1].offset, 0x1200ULL);
}

TEST(ImageTableTest, OneImagePerFileBasedAtItsLowestMapping)
{
    // The second segment of the executable is 0x1000 higher; a frame there must still resolve against the file's base, or the
    // offset would not be the one a debugger (and addr2line) expects.
    vn::crash::Report report = reportWithFrames({ 0x55d4c0e0e010ULL, 0x55d4c1a4e000ULL });

    ASSERT_EQ(vn::crash::detail::resolveImageTable(kMaps, report), 2u);

    EXPECT_EQ(report.images[0].base, 0x55d4c0e0d000ULL);
    EXPECT_EQ(report.frames[0].image, 0);
    EXPECT_EQ(report.frames[0].offset, 0x1010ULL);
    EXPECT_EQ(report.frames[1].image, 0);
    EXPECT_EQ(report.frames[1].offset, 0x55d4c1a4e000ULL - 0x55d4c0e0d000ULL);
}

TEST(ImageTableTest, ImagesCarryTheFileNameAndTheFullPath)
{
    vn::crash::Report report;

    ASSERT_EQ(vn::crash::detail::resolveImageTable(kMaps, report), 2u);

    EXPECT_STREQ(report.images[0].name, "Vine");
    EXPECT_STREQ(report.images[0].path, "/opt/vine/bin/Vine");
    EXPECT_STREQ(report.images[1].name, "libc.so.6");
    EXPECT_STREQ(report.images[1].path, "/usr/lib/x86_64-linux-gnu/libc.so.6");
}

TEST(ImageTableTest, AnonymousRangesAreNotImages)
{
    // [heap], [stack], [vdso] have no file: a frame there keeps a bare address instead of a wrong module.
    vn::crash::Report report = reportWithFrames({ 0x7f2b4c201000ULL, 0x7ffd3b0b0000ULL });

    ASSERT_EQ(vn::crash::detail::resolveImageTable(kMaps, report), 2u);

    EXPECT_EQ(report.frames[0].image, -1);
    EXPECT_EQ(report.frames[1].image, -1);
    EXPECT_EQ(report.frames[0].offset, 0u);
}

TEST(ImageTableTest, TruncatedLastLineIsIgnored)
{
    // A buffer that filled mid-line must not produce half an image with a made-up base.
    const std::string maps = std::string{ kMaps } + "7f2b4c800000-7f2b4c801000 r-xp 00000000 08:02 9 /opt/vine/bin/tru";

    vn::crash::Report report;
    ASSERT_EQ(vn::crash::detail::resolveImageTable(maps, report), 2u);
    EXPECT_EQ(report.image_count, 2u);
}

TEST(ImageTableTest, ImagesAreCappedAndFramesBeyondStayUnresolved)
{
    std::string                 maps;
    std::vector<std::uintptr_t> bases;
    std::uintptr_t              base = 0x7f0000000000ULL;
    for (int i = 0; i < 30; ++i) {
        char line[256]{};
        std::snprintf(line,
                      sizeof line,
                      "%llx-%llx r-xp 00000000 08:02 1 /opt/lib%d.so\n",
                      static_cast<unsigned long long>(base),
                      static_cast<unsigned long long>(base + 0x1000),
                      i);
        maps += line;
        bases.push_back(base);
        base += 0x100000;
    }

    vn::crash::Report report = reportWithFrames({ bases.front() + 0x10, bases.back() + 0x200 });

    const std::size_t count = vn::crash::detail::resolveImageTable(maps, report);

    EXPECT_EQ(count, vn::crash::Report::kMaxImages);
    EXPECT_EQ(report.frames[0].image, 0);
    EXPECT_EQ(report.frames[1].image, -1); // The thirtieth image did not fit, so its frames stay bare addresses.
}

#endif // __linux__
