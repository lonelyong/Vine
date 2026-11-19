#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

#include <vine/runtime/DynamicLibraryLoader.hpp>

using vn::String;
using vn::runtime::DynamicLibraryLoader;

namespace
{

/** Absolute path of the fixture shared library, handed over by the build (see CMakeLists.txt).
 *  The native path is ASCII on every platform this test runs on (forward slashes on Windows too),
 *  so this is the same reinterpretation the plugin layer uses for native text, not a transcode.
 */
String fixturePath()
{
    return String(std::u8string_view(reinterpret_cast<const char8_t*>(VN_RUNTIME_FIXTURE)));
}

/** The filesystem spelling of a String; ASCII paths on every platform this test runs on. */
std::filesystem::path pathOf(const String& text)
{
#ifdef _WIN32
    return std::filesystem::path(text.toUtf16());
#else
    return std::filesystem::path(std::string(reinterpret_cast<const char*>(text.c_str())));
#endif
}

TEST(DynamicLibraryLoaderTest, LoadMissingReturnsNull)
{
    DynamicLibraryLoader loader;
    EXPECT_EQ(loader.load(u8"vn_no_such_library_xyz"), nullptr);
}

TEST(DynamicLibraryLoaderTest, ReusesAlreadyLoadedLibrary)
{
    DynamicLibraryLoader loader;
    const String         path = fixturePath();

    auto* first  = loader.load(path);
    auto* second = loader.load(path);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    EXPECT_EQ(loader.count(), 1u);
    EXPECT_NE(first->handle(), nullptr);
    EXPECT_TRUE(first->fileName() == path);
    EXPECT_EQ(loader.find(path), first);
}

TEST(DynamicLibraryLoaderTest, FindReturnsNullWhenNotLoaded)
{
    DynamicLibraryLoader loader;
    EXPECT_EQ(loader.find(u8"vn_no_such_library_xyz"), nullptr);
}

TEST(DynamicLibraryLoaderTest, SharedInstanceIsSingle)
{
    auto& a = DynamicLibraryLoader::instance();
    auto& b = DynamicLibraryLoader::instance();
    EXPECT_EQ(&a, &b);
    EXPECT_EQ(a.load(u8"vn_no_such_library_xyz"), nullptr);
}

TEST(DynamicLibraryLoaderTest, ResolvesKnownSymbol)
{
    DynamicLibraryLoader loader;
    auto*                lib = loader.load(fixturePath());
    ASSERT_NE(lib, nullptr);

    // The fixture exports this one symbol; calling it proves resolveSymbol() hands back the real
    // function rather than an address somewhere inside the library.
    auto* answer = lib->resolveSymbol<int (*)()>(u8"vn_test_fixture_answer");
    ASSERT_NE(answer, nullptr);
    EXPECT_EQ(answer(), 42);

    // A name the fixture does not export resolves to nothing.
    EXPECT_EQ(lib->resolveSymbol<int (*)()>(u8"vn_test_fixture_missing"), nullptr);
}

TEST(DynamicLibraryLoaderTest, SearchPathResolvesAndDeduplicates)
{
    DynamicLibraryLoader loader;

    const String path  = fixturePath();
    const auto   slash = path.rfind(u8'/');
    ASSERT_NE(slash, String::npos);
    const String directory = path.substr(0, slash);
    const String name      = path.substr(slash + 1);

    auto* direct = loader.load(path);
    ASSERT_NE(direct, nullptr);

    loader.addSearchPath(directory);
    EXPECT_EQ(loader.searchPaths().size(), 1u);
    auto* via_search = loader.load(name);
    ASSERT_NE(via_search, nullptr);
    // Both names resolve to the same path, so one cached instance is reused.
    EXPECT_EQ(via_search, direct);
    EXPECT_EQ(loader.count(), 1u);
    EXPECT_EQ(loader.find(name), direct);
}

TEST(DynamicLibraryLoaderTest, ManageSearchPaths)
{
    DynamicLibraryLoader loader;
    loader.addSearchPath(u8"/a");
    loader.addSearchPath(u8"/b");
    EXPECT_EQ(loader.searchPaths().size(), 2u);
    loader.removeSearchPath(u8"/a");
    EXPECT_EQ(loader.searchPaths().size(), 1u);
    loader.clearSearchPaths();
    EXPECT_TRUE(loader.searchPaths().empty());
}

TEST(DynamicLibraryLoaderTest, ManageDependencyPaths)
{
    DynamicLibraryLoader loader;
    loader.addDependencyPath(u8"/a");
    loader.addDependencyPath(u8"/b");
    EXPECT_EQ(loader.dependencyPaths().size(), 2u);
    loader.removeDependencyPath(u8"/a");
    EXPECT_EQ(loader.dependencyPaths().size(), 1u);
    loader.clearDependencyPaths();
    EXPECT_TRUE(loader.dependencyPaths().empty());
}

TEST(DynamicLibraryLoaderTest, SameFileThroughADotSegmentIsOneLibrary)
{
    // Two spellings of one path: `dir/./name` is a different cache key from
    // `dir/name` unless identity comes from the file, not the text.
    DynamicLibraryLoader loader;
    const String         path = fixturePath();
    const auto           file = pathOf(path);
    const String         dotted((file.parent_path() / "." / file.filename()).u8string());

    auto* direct     = loader.load(path);
    auto* dotted_lib = loader.load(dotted);
    ASSERT_NE(direct, nullptr);
    ASSERT_NE(dotted_lib, nullptr);
    EXPECT_EQ(direct, dotted_lib);
    EXPECT_EQ(loader.count(), 1u);
}

TEST(DynamicLibraryLoaderTest, SameFileThroughASymlinkIsOneLibrary)
{
#ifdef _WIN32
    GTEST_SKIP() << "creating a symlink needs a privilege the test process may not have";
#else
    namespace fs = std::filesystem;

    const fs::path fixture = pathOf(fixturePath());
    const fs::path alias   = fs::temp_directory_path() / "vn_runtime_alias_fixture.so";
    fs::remove(alias);
    fs::create_symlink(fixture, alias);

    DynamicLibraryLoader loader;
    auto*                direct = loader.load(fixturePath());
    auto*                linked = loader.load(String(alias.u8string()));
    ASSERT_NE(direct, nullptr);
    ASSERT_NE(linked, nullptr);
    EXPECT_EQ(direct, linked);
    EXPECT_EQ(loader.count(), 1u);
    EXPECT_EQ(direct->handle(), linked->handle());

    fs::remove(alias);
#endif
}

TEST(DynamicLibraryLoaderTest, LastErrorExplainsAFailedLoad)
{
    DynamicLibraryLoader loader;
    EXPECT_TRUE(loader.lastError().empty());

    EXPECT_EQ(loader.load(u8"vn_no_such_library_xyz"), nullptr);
    EXPECT_FALSE(loader.lastError().empty());

    ASSERT_NE(loader.load(fixturePath()), nullptr);
    EXPECT_TRUE(loader.lastError().empty()); // a success clears the previous reason
}

TEST(DynamicLibraryLoaderTest, KeepMappedLoaderOutlivesItsOwnDestruction)
{
    vn::runtime::DynamicLibrary* lib = nullptr;
    {
        DynamicLibraryLoader loader(DynamicLibraryLoader::Retention::KeepMapped);
        lib = loader.load(fixturePath());
        ASSERT_NE(lib, nullptr);
    }

    // The loader is gone and the library was requested to stay mapped, so its code
    // is still usable. A Retention::UnloadOnDestruction loader must not be probed
    // this way; that is the distinction the policy exists for.
    auto* answer = lib->resolveSymbol<int (*)()>(u8"vn_test_fixture_answer");
    ASSERT_NE(answer, nullptr);
    EXPECT_EQ(answer(), 42);
}

} // namespace
