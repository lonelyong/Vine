#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#    include <sys/utsname.h>
#endif

#include <vine/system/Directories.hpp>
#include <vine/system/Environment.hpp>
#include <vine/system/Hardware.hpp>
#include <vine/system/Host.hpp>
#include <vine/system/OperatingSystem.hpp>
#include <vine/system/Process.hpp>

using vn::String;
using vn::system::Directories;
using vn::system::DiskKind;
using vn::system::Environment;
using vn::system::Hardware;
using vn::system::Host;
using vn::system::OperatingSystem;
using vn::system::Process;

namespace
{

/// The UTF-8 bytes of a String, for gtest messages and std::string comparisons only.
std::string narrow(const String& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// The spelling a log or a test message prints for a kind.
const char* kindText(DiskKind kind)
{
    switch (kind) {
    case DiskKind::Fixed: return "fixed";
    case DiskKind::Removable: return "removable";
    case DiskKind::Network: return "network";
    case DiskKind::CdRom: return "cdrom";
    case DiskKind::Unknown: break;
    }
    return "unknown";
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
    EXPECT_FALSE(info.osVersion.empty());
    EXPECT_FALSE(info.kernelVersion.empty());
    EXPECT_FALSE(info.architecture.empty());
}

#if defined(__linux__)
TEST(SystemTest, KernelVersionIsTheKernelRelease)
{
    // The split exists to keep two facts apart: the product version a user reads and the kernel this
    // process actually runs on. On Linux the second one is uname(2)'s answer, verifiable here.
    struct utsname uts{};
    ASSERT_EQ(::uname(&uts), 0);
    EXPECT_EQ(OperatingSystem::info().kernelVersion, String::fromLocal8Bit(uts.release));
}
#endif

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
    const auto disks = Hardware::disks();
    ASSERT_FALSE(disks.empty());
    for (const auto& disk : disks) {
        EXPECT_FALSE(disk.name.empty());
        // Unknown is a legitimate answer on a platform that reports a drive it cannot classify;
        // Linux classifies every whole device it reports.
        EXPECT_NE(disk.kind, DiskKind::Unknown) << narrow(disk.name);
        // An optical drive reports 0 while it holds no media; every other entry has a capacity.
        if (disk.kind != DiskKind::CdRom) {
            EXPECT_GT(disk.capacity, 0ull) << narrow(disk.name) << " kind=" << kindText(disk.kind);
        }
    }
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

TEST(SystemTest, EnvironmentAnswersPresenceAndValue)
{
    // PATH is the one variable every platform gives this process.
    EXPECT_TRUE(Environment::has(u8"PATH"));
    EXPECT_FALSE(Environment::value(u8"PATH").empty());

    EXPECT_FALSE(Environment::has(String()));
    EXPECT_TRUE(Environment::value(String()).empty());
    EXPECT_FALSE(Environment::has(u8"VINE_NO_SUCH_VARIABLE_7F3C"));
    EXPECT_TRUE(Environment::value(u8"VINE_NO_SUCH_VARIABLE_7F3C").empty());
}

#if !defined(_WIN32)
TEST(SystemTest, EnvironmentSeesAVariableSetByTheProcess)
{
    ASSERT_EQ(::setenv("VINE_SYSTEM_TEST_VARIABLE", "value-1", 1), 0);
    EXPECT_TRUE(Environment::has(u8"VINE_SYSTEM_TEST_VARIABLE"));
    EXPECT_EQ(Environment::value(u8"VINE_SYSTEM_TEST_VARIABLE"), String(u8"value-1"));

    ASSERT_EQ(::unsetenv("VINE_SYSTEM_TEST_VARIABLE"), 0);
    EXPECT_FALSE(Environment::has(u8"VINE_SYSTEM_TEST_VARIABLE"));
}
#endif

TEST(SystemTest, EnvironmentListsEveryVariable)
{
    const auto variables = Environment::variables();
    ASSERT_FALSE(variables.empty());
    const bool has_path =
        std::any_of(variables.begin(), variables.end(), [](const std::pair<String, String>& entry) { return entry.first == String(u8"PATH"); });
    EXPECT_TRUE(has_path);
}

TEST(SystemTest, DirectoryRootsHaveNoTrailingSeparator)
{
    const String roots[] = { Directories::temporary(), Directories::home(), Directories::data(), Directories::config(), Directories::cache() };
    for (const String& root : roots) {
        ASSERT_FALSE(root.empty());
        const char8_t last = root.back();
        EXPECT_NE(last, u8'/') << narrow(root);
        EXPECT_NE(last, u8'\\') << narrow(root);
    }
}

TEST(SystemTest, TemporaryAndHomeAreRealDirectories)
{
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path(narrow(Directories::temporary()))));
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path(narrow(Directories::home()))));
}

TEST(SystemTest, ExecutableDirectoryContainsTheExecutable)
{
    const String directory  = Directories::executableDirectory();
    const String executable = Process::currentExecutablePath();
    ASSERT_FALSE(directory.empty());
    ASSERT_FALSE(executable.empty());
    EXPECT_EQ(narrow(executable).rfind(narrow(directory), 0), 0u) << narrow(directory) << " vs " << narrow(executable);
}

TEST(SystemTest, MemoryInfoIsFilled)
{
    const auto memory = Hardware::memory();
    EXPECT_GT(memory.totalBytes, 0ull);
    EXPECT_LE(memory.availableBytes, memory.totalBytes);
    EXPECT_GT(memory.pageSize, 0u);
}

TEST(SystemTest, HostIsIdentified)
{
    EXPECT_FALSE(Host::hostName().empty());
    EXPECT_FALSE(Host::userName().empty());
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
                  << " kind=" << kindText(disk.kind) << "\n";
    }
    SUCCEED();
}

} // namespace
