#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <vine/system/Hardware.hpp>
#include <vine/system/OperatingSystem.hpp>
#include <vine/system/Process.hpp>

using vn::String;
using vn::system::Hardware;
using vn::system::OperatingSystem;
using vn::system::Process;

namespace
{

/// The UTF-8 bytes of a String, for gtest messages and std::string comparisons only.
std::string narrow(const String& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

String exeName(const String& path)
{
    const std::size_t slash = path.find_last_of(u8"/\\");
    return slash == String::npos ? path : path.substr(slash + 1);
}

TEST(SystemTest, CurrentProcessIdIsPositive)
{
    EXPECT_GT(Process::currentProcessId(), 0);
}

TEST(SystemTest, CurrentExecutablePathIsNonEmpty)
{
    const String path = Process::currentExecutablePath();
    EXPECT_FALSE(path.empty());
    EXPECT_TRUE(path.find(u8'/') != String::npos || path.find(u8'\\') != String::npos);
}

TEST(SystemTest, ExistsForCurrentProcess)
{
    EXPECT_TRUE(Process::exists(Process::currentProcessId()));
    EXPECT_FALSE(Process::exists(0));
    EXPECT_FALSE(Process::exists(-1));
}

TEST(SystemTest, FindByNameFindsCurrentProcess)
{
    const String exe = exeName(Process::currentExecutablePath());
    ASSERT_FALSE(exe.empty());
    const auto pids = Process::findByName(exe);
    ASSERT_FALSE(pids.empty());
    EXPECT_NE(std::find(pids.begin(), pids.end(), Process::currentProcessId()), pids.end());
}

TEST(SystemTest, FindByNameWithAnEmptyOrUnknownNameFindsNothing)
{
    EXPECT_TRUE(Process::findByName(String()).empty());
    EXPECT_TRUE(Process::findByName(u8"vine-no-such-executable-7f3c").empty());
}

TEST(SystemTest, KillMissingProcessFails)
{
    EXPECT_FALSE(Process::kill(0));
    EXPECT_FALSE(Process::kill(-1));
    EXPECT_FALSE(Process::kill(std::numeric_limits<int>::max()));
}

TEST(SystemTest, OperatingSystemInfoIsFilled)
{
    const auto& info = OperatingSystem::info();
    EXPECT_FALSE(info.name.empty());
    EXPECT_FALSE(info.version.empty());
    EXPECT_FALSE(info.architecture.empty());
}

TEST(SystemTest, InfoIsCached)
{
    EXPECT_EQ(&OperatingSystem::info(), &OperatingSystem::info());
}

TEST(SystemTest, CpuInfoIsFilled)
{
    const auto& cpu = Hardware::cpu();
    EXPECT_GE(cpu.logicalCores, 1u);
    EXPECT_GE(cpu.physicalCores, 1u);
    EXPECT_LE(cpu.physicalCores, cpu.logicalCores);

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    // The two strings come from cpuid or from /proc/cpuinfo's vendor_id / model name; Hardware.hpp
    // documents that an ARM kernel may publish neither.
    EXPECT_FALSE(cpu.vendor.empty());
    EXPECT_FALSE(cpu.model.empty());
#endif
}

TEST(SystemTest, CpuInfoIsCached)
{
    EXPECT_EQ(&Hardware::cpu(), &Hardware::cpu());
}

TEST(SystemTest, MotherboardInfoIsFilled)
{
    // WSL and some containers do not expose /sys/class/dmi/id at all: the board facts are absent on
    // the platform, not unfilled by the implementation (Hardware::motherboard() reads this directory
    // and already falls back to sys_vendor / product_name).
    if (!std::filesystem::exists("/sys/class/dmi/id")) {
        GTEST_SKIP() << "/sys/class/dmi/id is not exposed on this platform (WSL/container)";
    }

    const auto& mb = Hardware::motherboard();
    EXPECT_FALSE(mb.manufacturer.empty());
    EXPECT_FALSE(mb.product.empty());
}

TEST(SystemTest, DisksAreEnumerated)
{
#if defined(__APPLE__)
    GTEST_SKIP() << "macOS has no disk enumeration (Hardware.hpp documents the empty answer)";
#else
    const auto disks = Hardware::disks();
    ASSERT_FALSE(disks.empty());
    for (const auto& disk : disks) {
        EXPECT_FALSE(disk.name.empty());
        EXPECT_FALSE(disk.kind.empty());
        // An optical drive reports 0 while it holds no media; every other entry has a capacity.
        if (disk.kind != String(u8"cdrom")) {
            EXPECT_GT(disk.capacity, 0ull) << narrow(disk.name);
        }
    }
#endif
}

#if defined(__linux__)
TEST(SystemTest, ReportedDisksAreWholeDevices)
{
    // A partition must never be reported: /sys/block lists whole devices only, so a reported node
    // that is not in that directory is a partition (or a name the platform invented).
    namespace fs = std::filesystem;
    for (const auto& disk : Hardware::disks()) {
        std::string node = narrow(disk.name);
        ASSERT_EQ(node.rfind("/dev/", 0), 0u) << node;
        node = node.substr(5);
        EXPECT_TRUE(fs::exists(fs::path("/sys/block") / node)) << node << " is not a whole device";
    }
}

TEST(SystemTest, EveryKnownWholeDeviceIsReported)
{
    // Regression this pins: a "the name contains a digit, so it is a partition" heuristic used to
    // drop every nvme0n1 and mmcblk0 - the only disk on a modern machine. The ground truth here is
    // "/sys/block holds whole devices", which is the same fact the implementation relies on.
    static constexpr const char* kDiskPrefixes[] = { "sd", "hd", "vd", "xvd", "nvme", "mmcblk", "md", "sr" };

    namespace fs = std::filesystem;
    std::error_code          ec;
    std::vector<std::string> expected;
    for (const auto& entry : fs::directory_iterator(fs::path("/sys/block"), ec)) {
        const std::string name = entry.path().filename().string();
        for (const char* prefix : kDiskPrefixes) {
            if (name.rfind(prefix, 0) == 0) {
                expected.push_back("/dev/" + name);
                break;
            }
        }
    }
    ASSERT_FALSE(expected.empty()) << "no known whole device under /sys/block to compare against";

    std::vector<std::string> reported;
    for (const auto& disk : Hardware::disks()) {
        reported.push_back(narrow(disk.name));
    }
    for (const auto& name : expected) {
        EXPECT_NE(std::find(reported.begin(), reported.end(), name), reported.end()) << name << " was dropped";
    }
}
#endif

TEST(SystemTest, DiskSpaceForCurrentPath)
{
    const String path  = Process::currentExecutablePath();
    const auto   total = Hardware::diskTotalSpace(path);
    const auto   free  = Hardware::diskFreeSpace(path);
    EXPECT_GT(total, 0ull);
    EXPECT_LE(free, total);
}

TEST(SystemTest, DiskSpaceOfAnUnknownPathIsZero)
{
    EXPECT_EQ(Hardware::diskTotalSpace(u8"vine-no-such-path-7f3c"), 0ull);
    EXPECT_EQ(Hardware::diskFreeSpace(u8"vine-no-such-path-7f3c"), 0ull);
}

TEST(SystemTest, HardwareDetails)
{
    const auto& cpu = Hardware::cpu();
    std::cout << "CPU vendor=" << narrow(cpu.vendor) << " model=" << narrow(cpu.model) << " cores=" << cpu.physicalCores << "P/" << cpu.logicalCores << "L\n";

    const auto& mb = Hardware::motherboard();
    std::cout << "MB manufacturer=" << narrow(mb.manufacturer) << " product=" << narrow(mb.product) << " version=" << narrow(mb.version)
              << " serial=" << narrow(mb.serial) << "\n";

    for (const auto& disk : Hardware::disks()) {
        std::cout << "Disk " << narrow(disk.name) << " model=" << narrow(disk.model) << " serial=" << narrow(disk.serial) << " cap=" << disk.capacity
                  << " kind=" << narrow(disk.kind) << "\n";
    }
    SUCCEED();
}

} // namespace
