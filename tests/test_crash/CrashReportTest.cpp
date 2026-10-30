#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <vine/crash/CrashHandler.hpp>

using vn::crash::Report;
using vn::crash::ReportFrame;
using vn::crash::ReportImage;

namespace
{

/**
 * @brief Copies text into one of the report's fixed fields, the way the crashing process does.
 *
 * @param out Destination field.
 * @param capacity Capacity of @p out, including the terminator.
 * @param text Text to place there.
 */
void setField(char* out, std::size_t capacity, const char* text)
{
    std::strncpy(out, text, capacity - 1);
    out[capacity - 1] = '\0';
}

/**
 * @brief A report shaped like the one a crashing Vine would write: two images, three frames, one context line.
 *
 * @return The report, ready to render.
 */
Report sampleReport()
{
    Report report;
    setField(report.app, sizeof report.app, "Vine");
    setField(report.time, sizeof report.time, "2026-10-30T14:32:07");
    setField(report.dump, sizeof report.dump, "C:\\work\\bin\\Vine_crash.dmp");
    setField(report.context[0], Report::kContextChars, "Vine 1.0.0 (Debug)");
    report.context_count = 1;

    report.code          = 0xC0000005UL;
    report.address       = 0x00007FF6A1B2C3D4ULL;
    report.thread_id     = 19584;

    report.images[0].base = 0x00007FF6A0000000ULL;
    setField(report.images[0].name, ReportImage::kNameChars, "Vine.exe");
    setField(report.images[0].path, ReportImage::kPathChars, "C:\\work\\bin\\Vine.exe");
    report.images[1].base = 0x00007FFC12340000ULL;
    setField(report.images[1].name, ReportImage::kNameChars, "Qt6Core.dll");
    setField(report.images[1].path, ReportImage::kPathChars, "C:\\Qt\\6.9.0\\bin\\Qt6Core.dll");
    report.image_count = 2;

    report.frames[0].address = 0x00007FF6A1B2C3D4ULL;
    report.frames[0].offset  = 0x01B2C3D4ULL;
    report.frames[0].image   = 0;
    report.frames[1].address = 0x00007FFC12345678ULL;
    report.frames[1].offset  = 0x5678;
    report.frames[1].image   = 1;
    report.frames[2].address = 0x0000000012345678ULL;
    report.frames[2].image   = -1;
    report.frame_count       = 3;
    return report;
}

} // namespace

TEST(CrashReportTest, WritesTheHeaderOfAReportWithoutFrames)
{
    Report report;
    setField(report.app, sizeof report.app, "Vine");
    report.code      = 0xC0000005UL;
    report.address   = 0x1000;
    report.thread_id = 7;

    const std::string text = vn::crash::renderReport(report);

    EXPECT_EQ(text.rfind("vn-crash-report 1\r\n", 0), 0u);
    EXPECT_NE(text.find("app Vine\r\n"), std::string::npos);
    EXPECT_NE(text.find("exception 0xc0000005 at 0x0000000000001000 thread 7\r\n"), std::string::npos);
    // An empty dump field is not written at all, so a reader cannot mistake "" for a path.
    EXPECT_EQ(text.find("dump "), std::string::npos);
    EXPECT_EQ(text.find("frame "), std::string::npos);
}

TEST(CrashReportTest, NamesTheImageOfEveryFrame)
{
    const std::string text = vn::crash::renderReport(sampleReport());

    EXPECT_NE(text.find("image 0x00007ff6a0000000 Vine.exe C:\\work\\bin\\Vine.exe\r\n"), std::string::npos);
    EXPECT_NE(text.find("frame 0x00007ff6a1b2c3d4 0 0x1b2c3d4\r\n"), std::string::npos);
    EXPECT_NE(text.find("frame 0x00007ffc12345678 1 0x5678\r\n"), std::string::npos);
}

TEST(CrashReportTest, KeepsTheBareAddressOfAFrameOutsideEveryImage)
{
    const std::string text = vn::crash::renderReport(sampleReport());

    // Image index -1 with offset 0 is what "this frame belongs to no loaded image" looks like on the wire.
    EXPECT_NE(text.find("frame 0x0000000012345678 -1 0x0\r\n"), std::string::npos);
}

TEST(CrashReportTest, WritesContextLinesInOrder)
{
    Report report;
    setField(report.context[0], Report::kContextChars, "first");
    setField(report.context[1], Report::kContextChars, "second");
    report.context_count = 2;

    const std::string text  = vn::crash::renderReport(report);
    const std::size_t first = text.find("context first\r\n");

    ASSERT_NE(first, std::string::npos);
    EXPECT_NE(text.find("context second\r\n"), std::string::npos);
    EXPECT_LT(first, text.find("context second\r\n"));
}

TEST(CrashReportTest, RoundTripsThroughParse)
{
    const Report      original = sampleReport();
    const std::string text     = vn::crash::renderReport(original);

    Report parsed;
    ASSERT_TRUE(vn::crash::parseReport(text, parsed));

    EXPECT_STREQ(parsed.app, original.app);
    EXPECT_STREQ(parsed.time, original.time);
    EXPECT_STREQ(parsed.dump, original.dump);
    EXPECT_EQ(parsed.code, original.code);
    EXPECT_EQ(parsed.address, original.address);
    EXPECT_EQ(parsed.thread_id, original.thread_id);

    ASSERT_EQ(parsed.context_count, original.context_count);
    EXPECT_STREQ(parsed.context[0], original.context[0]);

    ASSERT_EQ(parsed.image_count, original.image_count);
    for (std::size_t i = 0; i < original.image_count; ++i) {
        EXPECT_EQ(parsed.images[i].base, original.images[i].base);
        EXPECT_STREQ(parsed.images[i].name, original.images[i].name);
        EXPECT_STREQ(parsed.images[i].path, original.images[i].path);
    }

    ASSERT_EQ(parsed.frame_count, original.frame_count);
    for (std::size_t i = 0; i < original.frame_count; ++i) {
        EXPECT_EQ(parsed.frames[i].address, original.frames[i].address);
        EXPECT_EQ(parsed.frames[i].offset, original.frames[i].offset);
        EXPECT_EQ(parsed.frames[i].image, original.frames[i].image);
    }
}

TEST(CrashReportTest, RejectsTextThatIsNotAReport)
{
    Report report;
    EXPECT_FALSE(vn::crash::parseReport("", report));
    EXPECT_FALSE(vn::crash::parseReport("random text\r\nwith lines\r\n", report));

    // A newer report version is refused rather than half-read: a wrong frame list is worse than no frame list.
    EXPECT_FALSE(vn::crash::parseReport("vn-crash-report 2\r\n", report));
}

TEST(CrashReportTest, IgnoresKeysItDoesNotKnow)
{
    const std::string text = "vn-crash-report 1\r\n"
                             "app Vine\r\n"
                             "future-field whatever\r\n"
                             "frame 0x0000000000001000 -1 0x0\r\n";

    Report report;
    ASSERT_TRUE(vn::crash::parseReport(text, report));
    EXPECT_STREQ(report.app, "Vine");
    ASSERT_EQ(report.frame_count, 1u);
    EXPECT_EQ(report.frames[0].address, 0x1000u);
}

TEST(CrashReportTest, DropsWholeLinesWhenTheBufferIsTooSmall)
{
    const Report original = sampleReport();

    char              full[4096]{};
    const std::size_t full_used = vn::crash::renderReport(original, std::span<char>(full, sizeof full));

    // A buffer that fits the magic line and the header, but not the frame list.
    char              buffer[130]{};
    const std::size_t used = vn::crash::renderReport(original, std::span<char>(buffer, sizeof buffer));

    ASSERT_GT(used, 0u);
    ASSERT_LE(used, sizeof buffer);
    ASSERT_LT(used, full_used);

    const std::string text{ buffer, used };
    const std::string complete{ full, full_used };

    // The result is a prefix of the full report: truncation happens between lines, never inside one.
    EXPECT_EQ(complete.rfind(text, 0), 0u);
    EXPECT_EQ(text.back(), '\n');
    EXPECT_EQ(text.find("frame "), std::string::npos);
}
