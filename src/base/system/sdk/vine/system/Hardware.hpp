#pragma once
#include "system_global.hpp"

#include <cstdint>
#include <vector>

#include <vine/String.hpp>

VN_SYSTEM_NS_BEGIN

/**
 * @brief Describes the CPU.
 */
struct VN_SYSTEM_API CpuInfo {
    String        vendor;            // CPU vendor, e.g. "GenuineIntel"; empty where the platform exposes no vendor string.
    String        model;             // CPU model name; empty where the platform exposes no model string.
    std::uint32_t physicalCores = 0; // Physical core count; never 0 once read through Hardware::cpu().
    std::uint32_t logicalCores  = 0; // Logical core count (includes hyper-threading); never 0 once read through Hardware::cpu().
};

/**
 * @brief Describes the system memory.
 *
 * @note
 * - The fields are a snapshot: availableBytes changes with every allocation made anywhere on the machine.
 */
struct VN_SYSTEM_API MemoryInfo {
    std::uint64_t totalBytes     = 0; // Physical memory in bytes; 0 when the platform cannot answer.
    std::uint64_t availableBytes = 0; // Memory the system can hand to a new allocation; 0 when unknown.
    std::uint32_t pageSize       = 0; // Smallest unit the platform maps in bytes; 0 when unknown.
};

/**
 * @brief Describes the motherboard.
 */
struct VN_SYSTEM_API MotherboardInfo {
    String manufacturer; // Board manufacturer; empty when unavailable.
    String product;      // Board model name; empty when unavailable.
    String version;      // Board version; may be empty.
    String serial;       // Board serial number; may be empty when unavailable.
};

/**
 * @brief Classifies a storage entry.
 *
 * @details
 * - The five values are the whole vocabulary: a caller compares, a log prints, and no platform invents a
 *   sixth word for a device that is really one of these.
 * - `Unknown` is the zero value, so an unclassified device never reads as `Fixed`.
 */
enum class DiskKind : std::uint8_t
{
    Unknown,   // The platform could not classify the entry.
    Fixed,     // A fixed (non-removable) volume or disk.
    Removable, // Removable media.
    Network,   // A network volume or a network-backed block device.
    CdRom,     // An optical drive; capacity is 0 while it holds no media.
};

/**
 * @brief Describes one storage entry as the platform reports it.
 *
 * @details
 * - Windows reports one entry per volume root (e.g. C:), with that volume's capacity and the model and serial of the
 *   physical disk backing it; a volume spanning several disks reports neither model nor serial.
 * - macOS reports one entry per mounted volume (e.g. "/"), with that volume's capacity; no model and no serial are
 *   available without querying the storage drivers directly.
 * - Linux reports one entry per whole block device ("/dev/sda"), with that device's capacity.
 *
 * @note
 * - An optical drive is `DiskKind::CdRom` and reports a capacity of 0 while it holds no media.
 */
struct VN_SYSTEM_API DiskInfo {
    String        name;                         // Volume root (e.g. C:) or device node ("/dev/sda"); never empty.
    String        model;                        // Disk model; empty when unavailable.
    String        serial;                       // Disk serial number; empty when unavailable.
    std::uint64_t capacity = 0;                 // Total capacity in bytes; 0 for an optical drive without media.
    DiskKind      kind     = DiskKind::Unknown; // What the platform reports the entry to be.
};

/**
 * @brief Provides read-only information about the host hardware.
 *
 * @details
 * - cpu() and motherboard() are built once on first call and then cached; the calls are thread-safe (the cache is a
 *   function-local static, initialised under the language's guard variable).
 * - disks() enumerates the current state on every call, because a volume can appear or disappear while the process runs.
 * - memory() answers a snapshot on every call; availableBytes is state, not a constant.
 * - The disk space helpers report the volume that contains the given path.
 *
 * @note
 * - No error is reported: an empty string, an empty list or 0 means "the platform could not answer", which includes a
 *   platform the module does not implement.
 * - disks() performs real I/O - on Windows one device query per volume - so it is not a cheap accessor.
 */
class VN_SYSTEM_API Hardware {

  public:
    /**
     * @brief Returns information about the CPU.
     *
     * @return The CPU information; borrowed, valid for the lifetime of the process.
     */
    static const CpuInfo& cpu();

    /**
     * @brief Returns information about the motherboard.
     *
     * @return The motherboard information; borrowed, valid for the lifetime of the process.
     */
    static const MotherboardInfo& motherboard();

    /**
     * @brief Returns the system memory facts.
     *
     * @details
     * - availableBytes is what the system reports as usable for a new allocation, not "total minus what this process
     *   asked for": it moves while the process runs, which is why the call is not cached.
     *
     * @note
     * - A field the platform cannot answer is 0, including on a platform the module does not implement.
     *
     * @return The memory facts; a snapshot, not state.
     */
    static MemoryInfo memory();

    /**
     * @brief Enumerates the storage the platform reports as a disk or volume.
     *
     * @note
     * - Not cached: every call queries the platform, and on Windows that is one device query per volume.
     * - macOS reports mounted volumes; a volume the platform does not expose there is simply absent.
     *
     * @return The devices, in the platform's enumeration order.
     */
    static std::vector<DiskInfo> disks();

    /**
     * @brief Returns the total capacity of the volume containing the given path.
     *
     * @note
     * - A relative path is resolved against the current directory on POSIX; the Windows half needs a directory part to
     *   fall back to, so a relative file name there answers 0.
     *
     * @param path Any path on the volume to query.
     * @return The total capacity in bytes, or 0 when it cannot be determined.
     */
    static std::uint64_t diskTotalSpace(const String& path);

    /**
     * @brief Returns the space of the volume containing the given path that is available to the calling user.
     *
     * @note
     * - Available-to-the-caller, not unused: a volume that reserves blocks for root reports less than its unused size.
     *
     * @param path Any path on the volume to query.
     * @return The available space in bytes, or 0 when it cannot be determined.
     */
    static std::uint64_t diskFreeSpace(const String& path);
};

VN_SYSTEM_NS_END
