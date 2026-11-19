#pragma once

#include <vine/system/system_global.hpp>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <vine/String.hpp>

#include <vine/system/Directories.hpp>
#include <vine/system/Environment.hpp>
#include <vine/system/Hardware.hpp>
#include <vine/system/Host.hpp>
#include <vine/system/OperatingSystem.hpp>
#include <vine/system/Process.hpp>

VN_SYSTEM_NS_BEGIN

namespace detail
{

/**
 * @brief Platform half of the system module: one implementation file per platform family.
 *
 * @details
 * - `SystemWindows.cpp`, `SystemLinux.cpp` and `SystemApple.cpp` each answer every function below for their platform;
 *   `SystemUnsupported.cpp` answers all of them with the empty value their contract documents, so an unsupported platform
 *   gets a defined answer instead of a function that never returns.
 * - The public classes hold what is the same on every platform: the pid guard, "query once and cache", and "never report
 *   zero cores". These functions hold only the mechanism, which is why no `#ifdef` for a platform appears in a shared file.
 * - A platform file is compiled only by its own platform (the other halves are marked `HEADER_FILE_ONLY` in CMake, the
 *   same arrangement `base/crash` uses for `CrashCapture*.cpp`): every platform half must therefore be read in its own
 *   file, and a missing function is a link error on that platform rather than a silent no-op.
 * - Nothing here is exported from the library; `SystemPlatform` is the module's private seam.
 */

/**
 * @brief Fills @p info with the host operating system facts.
 *
 * @details
 * - A field the platform cannot determine is left empty; `OperatingSystem::info()` turns an empty field into `unknown`.
 * - `name` is the platform family (`Windows`, `Linux`, `macOS`), `version` is the platform's own version spelling, and
 *   `architecture` is `x64`, `x86` or `arm64` where the platform reports a machine the module has a spelling for.
 *
 * @param info Structure to fill; `name` is set even when nothing else can be.
 */
void fillOsInfo(OperatingSystemInfo& info);

/**
 * @brief Fills @p info with the CPU facts the platform exposes.
 *
 * @details
 * - A count left at 0 is filled by the shared caller (`logicalCores` 1, `physicalCores` = `logicalCores`), which is what
 *   keeps a cpuid-less platform from reporting a machine with no cores.
 * - `vendor` and `model` are left empty where the platform has no such string (ARM64 Windows has no `__cpuid`; some ARM
 *   Linux kernels publish no `vendor_id`).
 *
 * @param info Structure to fill; may be left untouched on a platform with no CPU query.
 */
void fillCpuInfo(CpuInfo& info);

/**
 * @brief Fills @p info with the motherboard facts.
 *
 * @param info Structure to fill; a field the platform cannot read is left empty.
 */
void fillMotherboardInfo(MotherboardInfo& info);

/**
 * @brief Enumerates the storage the platform reports as a disk or volume.
 *
 * @details
 * - Windows reports one entry per volume root (`C:\`), Linux one entry per whole block device (`/dev/sda`), which is the
 *   difference `Hardware.hpp` documents on `DiskInfo`.
 * - Never cached by the caller: the list changes while the process runs.
 *
 * @return The devices, in the platform's enumeration order; empty where the platform has no implementation.
 */
std::vector<DiskInfo> enumerateDisks();

/**
 * @brief Returns the total capacity of the volume that holds @p path.
 *
 * @param path Any path on the volume; an empty or unresolvable path answers 0.
 * @return Bytes, or 0 when the query has no answer.
 */
std::uint64_t volumeTotalSpace(const String& path);

/**
 * @brief Returns the space on the volume that holds @p path that is available to the calling user.
 *
 * @details
 * - "Available to the caller", not "free": a volume reserved for root can report less than its unused size.
 *
 * @param path Any path on the volume; an empty or unresolvable path answers 0.
 * @return Bytes, or 0 when the query has no answer.
 */
std::uint64_t volumeFreeSpace(const String& path);

/**
 * @brief Returns the current process identifier.
 *
 * @return The identifier, or 0 on a platform with no implementation.
 */
int currentPid();

/**
 * @brief Returns the path of the running executable.
 *
 * @return The path, or an empty string when the platform cannot report it.
 */
String executablePath();

/**
 * @brief Whether a process with @p pid exists.
 *
 * @details
 * - `@p pid` is positive: `Process::exists()` rejects 0 and negative ids once, for every platform.
 *
 * @param pid Process identifier; always positive when called from the public API.
 * @return true when the process exists, including when it exists but cannot be opened.
 */
bool pidExists(int pid);

/**
 * @brief Returns the identifiers of the processes whose executable file name equals @p name.
 *
 * @details
 * - `@p name` is non-empty: `Process::findByName()` rejects the empty name once, for every platform.
 * - Each platform enforces the name comparison it documents on `Process::findByName()`.
 *
 * @param name Executable file name, without a directory part.
 * @return The matching identifiers; empty where the platform has no enumeration.
 */
std::vector<int> pidsByName(const String& name);

/**
 * @brief Terminates the process @p pid.
 *
 * @details
 * - `@p pid` is positive; the shared caller performs that check.
 *
 * @param pid Process identifier; always positive when called from the public API.
 * @return true when the platform reports the process was terminated.
 */
bool terminatePid(int pid);

/**
 * @brief Maps a `uname(2)` machine spelling to the module's architecture vocabulary.
 *
 * @details
 * - Shared by the two POSIX halves, which read the same field and must report the same spelling (`x64`, `x86`, `arm64`).
 * - A machine the module has no spelling for is returned unchanged, so a caller still sees the platform's own name.
 *
 * @param machine Null-terminated machine string from `struct utsname`; must not be null.
 * @return The module's spelling, or the input when it has none.
 */
inline String architectureName(const char* machine)
{
    if (std::strcmp(machine, "x86_64") == 0 || std::strcmp(machine, "amd64") == 0) {
        return u8"x64";
    }
    if (std::strcmp(machine, "aarch64") == 0 || std::strcmp(machine, "arm64") == 0) {
        return u8"arm64";
    }
    if (std::strcmp(machine, "i386") == 0 || std::strcmp(machine, "i686") == 0 || std::strcmp(machine, "x86") == 0) {
        return u8"x86";
    }
    return String::fromLocal8Bit(machine);
}

/**
 * @brief Writes the value of environment variable @p name into @p value.
 *
 * @details
 * - Answers the "set but empty" case: a variable that exists with an empty value returns true and writes an empty
 *   string.
 * - The per-drive pseudo entries Windows exposes (`=C:=C:\dir`) are not variables and never match.
 *
 * @param name Variable name; never empty when called from Environment.
 * @param value Receives the value; untouched when the variable is not set.
 * @return true when the variable exists.
 */
bool environmentVariable(const String& name, String& value);

/**
 * @brief Returns every inherited variable as a name/value pair, in the platform's order.
 *
 * @details
 * - Split at the first '=', so a value may contain the character.
 * - The Windows per-drive pseudo entries are excluded: they are working directories, not variables.
 *
 * @return The pairs; empty where the platform cannot enumerate its environment.
 */
std::vector<std::pair<String, String>> environmentVariables();

/**
 * @brief Returns the system temporary root.
 *
 * @return The directory, or an empty string where the platform has none; a trailing separator may be present and is
 *         removed by the shared caller.
 */
String temporaryDirectory();

/**
 * @brief Returns the current user's home directory.
 *
 * @return The directory, or an empty string when the process has no home.
 */
String homeDirectory();

/**
 * @brief Returns the root for per-user application data.
 *
 * @return The directory, or an empty string when it cannot be determined.
 */
String dataDirectory();

/**
 * @brief Returns the root for per-user configuration.
 *
 * @return The directory, or an empty string when it cannot be determined.
 */
String configDirectory();

/**
 * @brief Returns the root for per-user cache data.
 *
 * @return The directory, or an empty string when it cannot be determined (or the platform has no separate root).
 */
String cacheDirectory();

/**
 * @brief Fills @p info with the memory facts.
 *
 * @details
 * - A field the platform cannot answer is left at 0; the shared caller does not substitute a guess.
 *
 * @param info Structure to fill; may be left all-zero where the platform has no query.
 */
void fillMemoryInfo(MemoryInfo& info);

/**
 * @brief Returns the machine name.
 *
 * @return The name, or an empty string when the platform does not expose one.
 */
String hostName();

/**
 * @brief Returns the account name of the user owning this process.
 *
 * @return The account name, or an empty string when the platform cannot answer.
 */
String userName();

} // namespace detail

VN_SYSTEM_NS_END
