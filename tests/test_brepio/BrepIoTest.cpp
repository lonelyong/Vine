#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <vine/brepio/BrepExporter.hpp>
#include <vine/brepio/BrepFormat.hpp>
#include <vine/brepio/BrepLoader.hpp>
#include <vine/geometry/BrepShape.hpp>
#include <vine/io/IoError.hpp>
#include <vine/intrusive_ptr.hpp>

#if defined(VN_BREPIO_HAS_OCCT)
#include <memory>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>
#endif

using vn::brepio::BrepExporter;
using vn::brepio::BrepFormat;
using vn::brepio::BrepLoader;
using vn::brepio::formatFromPath;
using vn::brepio::formatName;
using vn::io::IoError;

namespace
{

/// Whether this build has a backend. The tests below state the contract for both builds instead of assuming one.
bool hasBackend()
{
    return !BrepLoader::supportedExtensions().empty();
}

} // namespace

TEST(BrepIoTest, FormatVocabularyIsTheReadContract)
{
    // The extension lookup names the format whether or not a backend is linked: naming a format and being able to read
    // it are two different facts.
    EXPECT_EQ(formatFromPath("part.stp"), BrepFormat::Step);
    EXPECT_EQ(formatFromPath("part.step"), BrepFormat::Step);
    EXPECT_EQ(formatFromPath("part.igs"), BrepFormat::Iges);
    EXPECT_EQ(formatFromPath("part.iges"), BrepFormat::Iges);
    EXPECT_EQ(formatFromPath("part.IGES"), BrepFormat::Iges);
    EXPECT_EQ(formatFromPath("part.brep"), BrepFormat::Brep);
    EXPECT_EQ(formatFromPath("part.stl"), BrepFormat::Unknown);
    EXPECT_EQ(formatFromPath("part.txt"), BrepFormat::Unknown);
    EXPECT_EQ(formatFromPath("part"), BrepFormat::Unknown) << "no extension names no format";

    EXPECT_STREQ(formatName(BrepFormat::Step), "step");
    EXPECT_STREQ(formatName(BrepFormat::Unknown), "Unknown");
}

TEST(BrepIoTest, TheExtensionListNamesWhatTheLoaderCanOpen)
{
    // With a backend the list holds every spelling the table declares; without one it is empty, because offering a STEP
    // file this build would refuse is worse than offering none.
    if (hasBackend()) {
        EXPECT_EQ(BrepLoader::supportedExtensions().size(), 5u);
        for (const char* extension : { ".stp", ".step", ".igs", ".iges", ".brep" }) {
            EXPECT_TRUE(BrepLoader::supportedExtensions().contains(vn::String::fromUtf8(extension))) << extension;
        }
    }
    else {
        EXPECT_TRUE(BrepLoader::supportedExtensions().empty());
    }
}

TEST(BrepIoTest, LoadRefusesWithAValueNotAnException)
{
    // A failure is a classified value: no throw, no null without a reason.
    BrepLoader loader;

    auto unknown = loader.load("part.txt");
    ASSERT_FALSE(unknown) << "an extension the table does not name is refused before any read";
    EXPECT_EQ(unknown.error(), IoError::Unsupported);

    auto missing = loader.load("vn_no_such_part.step");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error(), hasBackend() ? IoError::NotFound : IoError::Unsupported);

    std::istringstream garbage("this is not a solid");
    auto               by_stream = loader.load(garbage, BrepFormat::Step);
    ASSERT_FALSE(by_stream);
    EXPECT_EQ(by_stream.error(), hasBackend() ? IoError::InvalidData : IoError::Unsupported);

    loader.clearCache(); // callable in both builds
}

TEST(BrepIoTest, AnUnknownStreamFormatIsUnsupported)
{
    BrepLoader         loader;
    std::istringstream in("anything");
    auto               result = loader.load(in, BrepFormat::Unknown);
    ASSERT_FALSE(result) << "a stream has no extension, so the format must be stated";
    EXPECT_EQ(result.error(), IoError::Unsupported);
}

TEST(BrepIoTest, ExportWithoutAShapeIsInvalidData)
{
    vn::geometry::BrepShape shape; // no shape: there is nothing to write, backend or not

    BrepExporter       exporter;
    std::ostringstream out;
    EXPECT_EQ(exporter.exportAs(shape, BrepFormat::Step, out), IoError::InvalidData);

    auto source = exporter.createSource(vn::intrusive_ptr<const vn::geometry::BrepShape>{}, BrepFormat::Step);
    ASSERT_NE(source, nullptr) << "the source contract is never-null; the refusal travels in error()";
    EXPECT_EQ(source->size(), 0u);
    EXPECT_EQ(source->error(), IoError::InvalidData);
}

TEST(BrepIoTest, AnEmptyShapeIsNotValid)
{
    vn::geometry::BrepShape shape;
    EXPECT_EQ(shape.shape(), nullptr);
    EXPECT_FALSE(shape.isValid());

    auto owned = vn::make_intrusive<vn::geometry::BrepShape>();
    owned->setShape({}); // clearing is allowed and leaves it invalid
    EXPECT_FALSE(owned->isValid());
}

#if defined(VN_BREPIO_HAS_OCCT)
TEST(BrepIoTest, ExportsABoxAndReadsItBack)
{
    // A real solid through every declared format: writer, reader, and the fingerprint cache that links them.
    auto solid = vn::make_intrusive<vn::geometry::BrepShape>();
    solid->setShape(std::make_shared<const TopoDS_Shape>(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape()));
    ASSERT_TRUE(solid->isValid());

    const auto dir = std::filesystem::temp_directory_path() / "vn_brepio_roundtrip";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    BrepExporter exporter;
    BrepLoader   loader;

    const struct Case
    {
        BrepFormat  format;
        const char* name;
    } cases[] = {
        { BrepFormat::Step, "box.step" },
        { BrepFormat::Iges, "box.igs" },
        { BrepFormat::Brep, "box.brep" },
    };

    for (const Case& item : cases) {
        const auto path = dir / item.name;
        EXPECT_EQ(exporter.exportAs(*solid, item.format, path), IoError::Ok) << item.name;
        ASSERT_TRUE(std::filesystem::exists(path)) << item.name;

        auto loaded = loader.load(path);
        ASSERT_TRUE(loaded) << item.name;
        EXPECT_TRUE(loaded.value()->isValid()) << item.name;

        // The same bytes through the stream entry hit the same cache entry.
        std::ifstream      file(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << file.rdbuf();
        std::istringstream image(bytes.str(), std::ios::binary);
        auto               by_stream = loader.load(image, item.format);
        ASSERT_TRUE(by_stream) << item.name;
        EXPECT_EQ(by_stream.value().get(), loaded.value().get()) << "content, not the path, keys the cache";
    }

    // The lazy source writes the same content the stream entry does.
    auto source = exporter.createSource(solid, BrepFormat::Step);
    ASSERT_NE(source, nullptr);
    EXPECT_GT(source->size(), 0u) << "asking for the size is the first pull";
    EXPECT_EQ(source->error(), IoError::Ok);

    std::filesystem::remove_all(dir);
}
#endif
