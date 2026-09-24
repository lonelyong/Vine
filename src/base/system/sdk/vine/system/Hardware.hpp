#pragma once
#include "system_global.hpp"

#include <vector>

#include <vine/String.hpp>

VN_SYSTEM_NS_BEGIN

/**
 * @brief Describes the CPU.
 */
struct VN_SYSTEM_API CpuInfo {
    String   vendor;            // CPU vendor, e.g. "GenuineIntel" or "AuthenticAMD".
    String   model;             // CPU model name.
    unsigned physicalCores = 0; // Physical core count.
    unsigned logicalCores  = 0; // Logical core count (includes hyper-threading).
};

/**
 * @brief Describes the motherboard.
 */
struct VN_SYSTEM_API MotherboardInfo {
    String manufacturer; // Board manufacturer.
    String product;      // Board model name.
    String version;      // Board version.
    String serial;       // Board serial number; may be empty when unavailable.
};

/**
 * @brief Describes a disk volume or block device.
 */
struct VN_SYSTEM_API DiskInfo {
    String             name;         // Drive/device name, e.g. "C:" or "/dev/sda".
    String             model;        // Disk model, may be empty when unavailable.
    String             serial;       // Disk serial number; may be empty when unavailable.
    unsigned long long capacity = 0; // Total capacity in bytes.
    String             kind;         // "fixed", "removable", "network" or "cdrom".
};

/**
 * @brief Provides read-only information about the host hardware.
 *
 * cpu() and motherboard() are built once on first call and then cached;
 * disks() enumerates the current volumes or block devices. The disk space
 * helpers return byte counts for the volume containing the given path.
 */
class VN_SYSTEM_API Hardware {

  public:
    /**
     * @brief Returns information about the CPU.
     *
     * @return The CPU information.
     */
    static const CpuInfo& cpu();

    /**
     * @brief Returns information about the motherboard.
     *
     * @return The motherboard information.
     */
    static const MotherboardInfo& motherboard();

    /**
     * @brief Enumerates the disk volumes or block devices.
     *
     * @return The list of disks.
     */
    static std::vector<DiskInfo> disks();

    /**
     * @brief Returns the total capacity of the volume containing the given path.
     *
     * @param path Any path on the volume to query.
     * @return The total capacity in bytes, or 0 when it cannot be determined.
     */
    static unsigned long long diskTotalSpace(const String& path);

    /**
     * @brief Returns the free space of the volume containing the given path.
     *
     * @param path Any path on the volume to query.
     * @return The free space in bytes, or 0 when it cannot be determined.
     */
    static unsigned long long diskFreeSpace(const String& path);
};

VN_SYSTEM_NS_END
