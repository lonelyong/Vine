#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>

#include <vine/brepio/BrepExporter.hpp>
#include <vine/brepio/BrepLoader.hpp>
#include <vine/geometry/BrepShape.hpp>

using vn::brepio::BrepLoader;

namespace
{

/**
 * @brief Stand-in exporter that records which write entry it was called through.
 *
 * No OpenCASCADE backend is linked yet, so an abstract exporter has no real implementation to test; this one pins the
 * pair of write entries instead (a file target and a stream target, both part of the abstraction).
 */
class RecordingExporter final : public vn::brepio::BrepExporter
{
    VN_OBJECT_META_DECL;

  public:
    bool save(const std::filesystem::path& path, const vn::geometry::BrepShape& shape) override
    {
        static_cast<void>(shape);
        file_path_ = path;
        ++file_calls_;
        return true;
    }

    bool save(std::ostream& out, const vn::geometry::BrepShape& shape) override
    {
        static_cast<void>(shape);
        out << "brep";
        ++stream_calls_;
        return static_cast<bool>(out);
    }

    bool canExport(const vn::geometry::BrepShape& shape) const override
    {
        static_cast<void>(shape);
        return true;
    }

    /**
     * @brief Returns the path the file entry was given.
     *
     * @return The recorded path.
     */
    const std::filesystem::path& filePath() const noexcept { return file_path_; }

    /**
     * @brief Returns how often the file entry was called.
     *
     * @return The call count.
     */
    int fileCalls() const noexcept { return file_calls_; }

    /**
     * @brief Returns how often the stream entry was called.
     *
     * @return The call count.
     */
    int streamCalls() const noexcept { return stream_calls_; }

  private:
    std::filesystem::path file_path_;
    int                   file_calls_{ 0 };
    int                   stream_calls_{ 0 };
};

} // namespace

VN_OBJECT_META_IMPL(RecordingExporter, vn::brepio::BrepExporter)

TEST(BrepIoTest, BrepIsSupportedFormat)
{
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.stp"));
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.step"));
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.igs"));
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.iges"));
    EXPECT_TRUE(BrepLoader::isSupportedFormat("part.IGS"));
    EXPECT_FALSE(BrepLoader::isSupportedFormat("part.stl"));
    EXPECT_FALSE(BrepLoader::isSupportedFormat("part.obj"));
    EXPECT_FALSE(BrepLoader::isSupportedFormat("part.txt"));
    EXPECT_FALSE(BrepLoader::isSupportedFormat("part"));
}

TEST(BrepIoTest, BrepDefaultInstanceIsSingleton)
{
    EXPECT_EQ(&BrepLoader::defaultInstance(), &BrepLoader::defaultInstance());
}

TEST(BrepIoTest, BrepOptionsRoundTrip)
{
    // The option interface is usable even though loading is not wired in yet.
    BrepLoader          loader;
    BrepLoader::Options options;
    options.placeholder = 'x';
    loader.setOptions(options);
    EXPECT_EQ(loader.options().placeholder, 'x');
    EXPECT_TRUE(loader.options() == options);
}

TEST(BrepIoTest, BrepExporterAnswersBothTargets)
{
    RecordingExporter       exporter;
    vn::geometry::BrepShape shape;

    std::ostringstream out;
    EXPECT_TRUE(exporter.save(out, shape));
    EXPECT_EQ(out.str(), "brep");
    EXPECT_EQ(exporter.streamCalls(), 1);

    EXPECT_TRUE(exporter.save("part.step", shape));
    EXPECT_EQ(exporter.filePath(), std::filesystem::path("part.step"));
    EXPECT_EQ(exporter.fileCalls(), 1);

    EXPECT_TRUE(exporter.canExport(shape));
}

TEST(BrepIoTest, BrepLoaderTakesAStreamAndAFormatHint)
{
    // Both entries exist and agree today: without OpenCASCADE there is nothing to parse with.
    BrepLoader         loader;
    std::istringstream step("ISO-10303-21;\nHEADER;\nENDSEC;\n");
    EXPECT_EQ(loader.load(step, "step"), nullptr);
    EXPECT_EQ(loader.load("part.step"), nullptr);
}
