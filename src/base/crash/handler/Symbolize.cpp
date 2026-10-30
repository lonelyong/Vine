#include "Symbolize.hpp"

#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

// clang-format off
#include <windows.h>  // Must precede dbghelp.h.
#include <dbghelp.h>
// clang-format on

#pragma comment(lib, "dbghelp.lib")

#include "Text.hpp"

namespace handler {
namespace {

constexpr std::size_t kMaxSymbolNameChars{ 512 };

constexpr long          kDosHeaderLfanew{ 0x3C }; ///< offset of the PE header pointer inside the DOS header
constexpr long          kPEHeaderToOptionalHeader{ 4 + 20 }; ///< PE signature plus the COFF header
constexpr long          kImageBaseOffset{ 24 }; ///< offset of ImageBase inside the optional header (both PE32 and PE32+)
constexpr long          kImageSizeOffset{ 56 }; ///< offset of SizeOfImage inside the optional header (both PE32 and PE32+)
constexpr std::uint16_t kPEMagic32{ 0x10B };
constexpr std::uint16_t kPEMagic64{ 0x20B };

/// @brief Distance added to a preferred base that another image already occupies, so two images never share a slot.
constexpr std::uint64_t kRelocationStride{ 0x10000000000ULL };

/**
 * @brief Formats an address the way the report file does.
 *
 * @param value Address to format.
 * @return `"0x"` followed by sixteen hex digits.
 */
std::string hex(std::uintptr_t value)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof buffer, "0x%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

/**
 * @brief Builds the directories DbgHelp should search for PDBs.
 *
 * @details
 * - This executable's directory first (a PDB deployed next to the handler), then every directory the report's images came
 *   from (where a build's PDBs live), then `_NT_SYMBOL_PATH` as the operator's own override. The handler inherits the
 *   crashed process' environment, so that last part needs no configuration of its own.
 *
 * @param report Report whose images are searched.
 * @return Semicolon-separated search path.
 */
std::wstring symbolSearchPath(const vn::crash::Report& report)
{
    std::wstring path;

    const auto append = [&path](const std::wstring& directory) {
        if (directory.empty()) {
            return;
        }
        std::wstring value = directory;
        if (value.back() != L'\\' && value.back() != L'/') {
            value += L'\\';
        }
        if (!path.empty()) {
            path += L';';
        }
        path += value;
    };

    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self))) > 0) {
        const std::wstring self_path = self;
        append(self_path.substr(0, self_path.find_last_of(L"\\/") + 1));
    }

    for (std::size_t i = 0; i < report.image_count && i < vn::crash::Report::kMaxImages; ++i) {
        const std::wstring image_path = toWide(report.images[i].path);
        if (!image_path.empty()) {
            append(image_path.substr(0, image_path.find_last_of(L"\\/") + 1));
        }
    }

    const DWORD needed = GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", nullptr, 0);
    if (needed > 0) {
        std::wstring value(needed, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", value.data(), needed);
        if (copied > 0 && copied < needed) {
            value.resize(copied);
            if (!path.empty()) {
                path += L';';
            }
            path += value;
        }
    }
    return path;
}

/**
 * @brief Whether one of the slots already handed to DbgHelp is @p base.
 *
 * @param bases Slots taken so far, zero meaning free.
 * @param base Base address to test.
 * @return true when another image already occupies that base.
 */
bool baseInUse(const std::vector<std::uint64_t>& bases, std::uint64_t base)
{
    for (const std::uint64_t used : bases) {
        if (used == base) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Reads the preferred base address and the mapped size out of the image's own PE header.
 *
 * @details
 * - DbgHelp resolves a symbol as `base + RVA`, where the base must be the one the PDB was built against. The address the
 *   crashed process happened to map the image at is *not* that base (ASLR moves it on every run), so the handler reads the
 *   preferred base from the file and loads the module there.
 * - The size is read for the same reason the base is: a module DbgHelp believes is zero bytes long owns no address, so
 *   every lookup inside it fails before the PDB is ever consulted.
 *
 * @param path Image file.
 * @param base Receives the preferred base address.
 * @param size Receives the image size in bytes.
 * @return true when @p path is a readable PE image.
 */
bool readImageHeader(const std::wstring& path, std::uint64_t& base, std::uint32_t& size)
{
    base = 0;
    size = 0;

    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        return false;
    }

    bool          ok = false;
    std::uint8_t  magic_bytes[2]{};
    std::fseek(file, kDosHeaderLfanew, SEEK_SET);
    std::uint32_t lfanew = 0;
    if (std::fread(&lfanew, sizeof lfanew, 1, file) == 1 && lfanew > 0) {
        const long optional_header = static_cast<long>(lfanew) + kPEHeaderToOptionalHeader;
        std::fseek(file, optional_header, SEEK_SET);
        if (std::fread(magic_bytes, 1, sizeof magic_bytes, file) == sizeof magic_bytes) {
            const auto magic = static_cast<std::uint16_t>(magic_bytes[0] | (magic_bytes[1] << 8));
            if (magic == kPEMagic64 || magic == kPEMagic32) {
                std::fseek(file, optional_header + kImageBaseOffset, SEEK_SET);
                if (magic == kPEMagic64) {
                    ok = std::fread(&base, sizeof base, 1, file) == 1;
                }
                else {
                    std::uint32_t base32 = 0;
                    ok = std::fread(&base32, sizeof base32, 1, file) == 1;
                    base = base32;
                }
                std::fseek(file, optional_header + kImageSizeOffset, SEEK_SET);
                ok = ok && std::fread(&size, sizeof size, 1, file) == 1;
            }
        }
    }
    std::fclose(file);
    return ok;
}


/**
 * @brief Formats one frame, symbolized when its image's symbols could be loaded.
 *
 * @param process Handle DbgHelp was initialized with.
 * @param report Report the frame came from; supplies the image name and offset fallback.
 * @param index Position of the frame in the report, shown in front of it.
 * @param lookup Address to ask DbgHelp about; this is the image's preferred base plus the offset, not the address the dead
 *        process ran at.
 * @param symbolized Whether symbol loading succeeded at all.
 * @param symbol Scratch symbol buffer reused across frames.
 * @return The frame's line, without a line ending.
 */
std::string frameLine(HANDLE                                     process,
                      const vn::crash::Report&                   report,
                      std::size_t                                index,
                      DWORD64                                    lookup,
                      bool                                       symbolized,
                      SYMBOL_INFOW*                              symbol)
{
    const vn::crash::ReportFrame& frame = report.frames[index];

    std::string name;
    std::string source;
    if (symbolized) {
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen   = static_cast<ULONG>(kMaxSymbolNameChars);

        DWORD64 displacement = 0;
        if (SymFromAddrW(process, lookup, &displacement, symbol) != FALSE) {
            name = toUtf8(std::wstring{ symbol->Name });
            if (displacement != 0) {
                char buffer[32]{};
                std::snprintf(buffer, sizeof buffer, "+0x%llx", static_cast<unsigned long long>(displacement));
                name += buffer;
            }

            IMAGEHLP_LINEW64 line{};
            line.SizeOfStruct = sizeof line;
            DWORD line_displacement = 0;
            if (SymGetLineFromAddrW64(process, lookup, &line_displacement, &line) != FALSE) {
                char buffer[32]{};
                std::snprintf(buffer, sizeof buffer, ":%lu", line.LineNumber);
                source = toUtf8(std::wstring{ line.FileName }) + buffer;
            }
        }
    }

    if (name.empty()) {
        // No symbols: the image and the offset are still exact, and the offset is what a match with a rebuilt binary needs.
        if (frame.image >= 0 && static_cast<std::size_t>(frame.image) < report.image_count) {
            const vn::crash::ReportImage& image = report.images[frame.image];
            char                          offset[32]{};
            std::snprintf(offset, sizeof offset, "+0x%llx", static_cast<unsigned long long>(frame.offset));
            name = std::string{ image.name[0] != '\0' ? image.name : "?" } + offset;
        }
        else {
            name = "<未知模块>";
        }
    }

    std::string text = "  " + std::to_string(index) + "  " + hex(frame.address) + "  " + name;
    if (!source.empty()) {
        text += "  [" + source + "]";
    }
    return text;
}

} // namespace

std::string symbolize(const vn::crash::Report& report)
{
    std::string text;
    text += "崩溃报告\r\n";
    text += "========\r\n\r\n";

    if (report.app[0] != '\0') {
        text += "应用: ";
        text += report.app;
        text += "\r\n";
    }
    if (report.time[0] != '\0') {
        text += "时间: ";
        text += report.time;
        text += "\r\n";
    }
    {
        char buffer[160]{};
        std::snprintf(buffer,
                      sizeof buffer,
                      "异常: 0x%08lx 于 %s 线程 %lu\r\n",
                      report.code,
                      hex(report.address).c_str(),
                      report.thread_id);
        text += buffer;
    }
    if (report.dump[0] != '\0') {
        text += "转储: ";
        text += report.dump;
        text += "\r\n";
    }

    if (report.context_count > 0) {
        text += "\r\n上下文:\r\n";
        for (std::size_t i = 0; i < report.context_count && i < vn::crash::Report::kMaxContext; ++i) {
            text += "  ";
            text += report.context[i];
            text += "\r\n";
        }
    }

    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);

    // Anything that stops symbols from being found lands here and is printed at the end: a reader must be able to tell
    // "this frame has no symbol" from "no symbols were loaded at all", which are different problems.
    std::string diagnostics;

    // The base each image's symbols were built for, per report image index; 0 = unknown, symbols unusable for that image.
    std::vector<std::uint64_t> bases(report.image_count, 0);

    const std::wstring search     = symbolSearchPath(report);
    const bool         symbolized = SymInitializeW(process, search.c_str(), FALSE) != FALSE;
    if (!symbolized) {
        char buffer[160]{};
        std::snprintf(buffer, sizeof buffer, "  SymInitialize 失败，GetLastError=%lu\r\n", static_cast<unsigned long>(GetLastError()));
        diagnostics += buffer;
    }
    else {
        // Each image is loaded at its own preferred base: DbgHelp resolves a symbol as image-base + RVA, so the base it knows
        // must be the one the PDB was built against, which is not the address the crash happened to run the image at.
        for (std::size_t i = 0; i < report.image_count && i < vn::crash::Report::kMaxImages; ++i) {
            const vn::crash::ReportImage& image = report.images[i];
            const std::wstring            path  = toWide(image.path);
            const std::wstring            name  = toWide(image.name);
            if (path.empty()) {
                continue;
            }
            std::uint64_t preferred_base = 0;
            std::uint32_t image_size     = 0;
            if (!readImageHeader(path, preferred_base, image_size)) {
                char buffer[400]{};
                std::snprintf(buffer,
                              sizeof buffer,
                              "  image %zu (%s) 不是可读的 PE 文件，未加载符号\r\n",
                              i,
                              image.name[0] != '\0' ? image.name : "?");
                diagnostics += buffer;
                continue;
            }
            // Two images can prefer the same base (KERNELBASE and ntdll both use 0x180000000) and DbgHelp keys its module
            // list by base, so the later one would take the earlier one's slot. Give each collision a slot of its own.
            std::uint64_t base = preferred_base;
            for (std::size_t bump = 1; baseInUse(bases, base); ++bump) {
                base = preferred_base + (kRelocationStride * bump);
            }
            if (SymLoadModuleExW(process, nullptr, path.c_str(), name.empty() ? nullptr : name.c_str(), base, image_size, nullptr, 0)
                == 0)
            {
                char buffer[400]{};
                std::snprintf(buffer,
                              sizeof buffer,
                              "  image %zu (%s) 符号未加载，GetLastError=%lu\r\n",
                              i,
                              image.name[0] != '\0' ? image.name : "?",
                              static_cast<unsigned long>(GetLastError()));
                diagnostics += buffer;
                continue;
            }
            bases[i] = base;
        }
    }

    std::vector<char> storage(sizeof(SYMBOL_INFOW) + kMaxSymbolNameChars * sizeof(wchar_t), '\0');
    auto*             symbol = reinterpret_cast<SYMBOL_INFOW*>(storage.data());

    text += "\r\n调用栈:\r\n";
    for (std::size_t i = 0; i < report.frame_count && i < vn::crash::Report::kMaxFrames; ++i) {
        const vn::crash::ReportFrame& frame  = report.frames[i];
        DWORD64                       lookup = static_cast<DWORD64>(frame.address);
        if (frame.image >= 0 && static_cast<std::size_t>(frame.image) < bases.size() && bases[frame.image] != 0) {
            lookup = bases[frame.image] + static_cast<DWORD64>(frame.offset);
        }
        text += frameLine(process, report, i, lookup, symbolized, symbol);
        text += "\r\n";
    }

    if (symbolized) {
        // Deferred symbol loading means "loaded" is only true once something asked for a symbol: ask now, then report the
        // images that ended up without a PDB. "no symbol for this frame" and "no PDB for this module" are different problems
        // and a reader must be able to tell them apart.
        for (std::size_t i = 0; i < report.image_count && i < bases.size(); ++i) {
            if (bases[i] == 0) {
                continue;
            }
            IMAGEHLP_MODULEW64 info{};
            info.SizeOfStruct = sizeof info;
            if (SymGetModuleInfoW64(process, bases[i], &info) != FALSE && info.LoadedPdbName[0] == '\0') {
                char buffer[600]{};
                std::snprintf(buffer,
                              sizeof buffer,
                              "  image %zu (%s) 找不到 PDB（已打开镜像: %ls）\r\n",
                              i,
                              report.images[i].name[0] != '\0' ? report.images[i].name : "?",
                              info.LoadedImageName[0] != '\0' ? info.LoadedImageName : L"未打开");
                diagnostics += buffer;
            }
        }
        SymCleanup(process);
    }
    if (!diagnostics.empty()) {
        text += "\r\n符号:\r\n";
        text += "（本段说明为什么有些帧只有模块+偏移而没有函数名）\r\n";
        text += diagnostics;
    }
    return text;
}

} // namespace handler
