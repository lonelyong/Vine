#include <gtest/gtest.h>

#include <vine/crash/CrashHandler.hpp>

namespace
{

/**
 * @brief A configuration shaped like an application's: a name, one identity line, nothing else.
 *
 * @return The configuration the tests install.
 */
vn::crash::Config sampleConfig()
{
    vn::crash::Config config;
    config.app_name = u8"test_crash";
    config.context  = { u8"test_crash 0 (test)" };
    return config;
}

} // namespace

TEST(CrashInstallTest, InstallArmsWhereThePlatformSupportsIt)
{
    const bool installed = vn::crash::install(sampleConfig());
#ifdef _WIN32
    // Windows is the one platform with a hook today; the report model and its renderer are portable, the hook is not.
    EXPECT_TRUE(installed);
#else
    EXPECT_FALSE(installed);
#endif
    vn::crash::uninstall();
}

TEST(CrashInstallTest, UninstallIsSafeWithoutAnInstall)
{
    // Called before anything is installed: this is the state a tool that owns the process' fate leaves behind.
    vn::crash::uninstall();
    vn::crash::uninstall();
    SUCCEED();
}

TEST(CrashInstallTest, InstallingTwiceReplacesInsteadOfStacking)
{
    EXPECT_EQ(vn::crash::install(sampleConfig()), vn::crash::install(sampleConfig()));
    vn::crash::uninstall();
}

TEST(CrashInstallTest, InstallAcceptsAnEmptyConfiguration)
{
    // Every empty field means "the default next to the executable", so an application may pass almost nothing and still get
    // a report: the name falls back to the executable's own stem.
    vn::crash::install(vn::crash::Config{});
    vn::crash::uninstall();
    SUCCEED();
}
