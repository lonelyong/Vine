#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

#include <vine/system/Hardware.hpp>
#include <vine/system/OperatingSystem.hpp>
#include <vine/system/Process.hpp>

using vn::String;
using vn::system::Hardware;
using vn::system::OperatingSystem;
using vn::system::Process;

namespace
{

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

TEST(SystemTest, KillMissingProcessFails)
{
    EXPECT_FALSE(Process::kill(0));
    EXPECT_FALSE(Process::kill(-1));
    EXPECT_FALSE(Process::kill(std::numeric_limits<int>::max()));
}

TEST(SystemTest, OperationSystemInfoIsFilled)
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
    EXPECT_FALSE(cpu.vendor.empty());
    EXPECT_FALSE(cpu.model.empty());
    EXPECT_GE(cpu.logicalCores, 1u);
    EXPECT_GE(cpu.physicalCores, 1u);
    EXPECT_LE(cpu.physicalCores, cpu.logicalCores);
}

TEST(SystemTest, CpuInfoIsCached)
{
    EXPECT_EQ(&Hardware::cpu(), &Hardware::cpu());
}

TEST(SystemTest, MotherboardInfoIsFilled)
{
    // WSL 与部分容器根本不暴露 /sys/class/dmi/id：主板信息在平台上取不到，不是实现没填
    // （Hardware::motherboard() 读的就是这个目录，并已对 sys_vendor/product_name 做了回退）。
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
        EXPECT_GT(disk.capacity, 0ull);
        EXPECT_FALSE(disk.kind.empty());
    }
}

TEST(SystemTest, DiskSpaceForCurrentPath)
{
    const String path  = Process::currentExecutablePath();
    const auto   total = Hardware::diskTotalSpace(path);
    const auto   free  = Hardware::diskFreeSpace(path);
    EXPECT_GT(total, 0ull);
    EXPECT_LE(free, total);
}

TEST(SystemTest, HardwareDetails)
{
    const auto print = [](const String& s) { return std::string(reinterpret_cast<const char*>(s.data()), s.size()); };

    const auto& cpu = Hardware::cpu();
    std::cout << "CPU vendor=" << print(cpu.vendor) << " model=" << print(cpu.model) << " cores=" << cpu.physicalCores << "P/" << cpu.logicalCores << "L\n";

    const auto& mb = Hardware::motherboard();
    std::cout << "MB manufacturer=" << print(mb.manufacturer) << " product=" << print(mb.product) << " version=" << print(mb.version)
              << " serial=" << print(mb.serial) << "\n";

    for (const auto& disk : Hardware::disks()) {
        std::cout << "Disk " << print(disk.name) << " model=" << print(disk.model) << " serial=" << print(disk.serial) << " cap=" << disk.capacity
                  << " kind=" << print(disk.kind) << "\n";
    }
    SUCCEED();
}

} // namespace
