#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#    include <windows.h>
#    include <dbghelp.h>
#    pragma comment(lib, "dbghelp.lib")
#endif

#include <vine/logging/Log.hpp>
#include <vine/logging/LogSink.hpp>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/gui/GuiAppBuilder.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

namespace fw    = vine::appfw;
namespace guifw = fw::gui;

namespace
{

#ifdef _WIN32

/**
 * @brief Dumps a symbolised stack of the crashing thread next to the exe.
 *
 * Dev aid: on an unhandled exception writes "vine_crash.log" in the executable
 * directory (function names + source lines from the Debug PDBs) so crashes in
 * third-party code can be diagnosed without a debugger attached.
 *
 * @param ep Exception information from the OS.
 * @return Handler result; terminates the process after logging.
 */
LONG WINAPI vineCrashFilter(EXCEPTION_POINTERS* ep)
{
    char exe_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    if (n > 0) {
        char* slash = strrchr(exe_path, '\\');
        if (slash != nullptr) {
            *slash = '\0';
        }
    }

    char path[MAX_PATH];
    strcpy_s(path, exe_path);
    strcat_s(path, "\\vine_crash.log");

    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    auto out = [&](const char* text) {
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
        }
    };

    char line[2048];
    wsprintfA(line, "crash code=0x%08lX at %p thread=%lu\r\n",
              static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
              ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    out(line);

    HANDLE process = GetCurrentProcess();
    DWORD  options = SymGetOptions();
    options |= SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES;
    SymSetOptions(options);
    SymInitialize(process, nullptr, TRUE);

    // Make sure DbgHelp can find the PDBs of every loaded module: default
    // auto-load often misses third-party DLLs (e.g. Qt) whose PDBs live next
    // to the DLL. Add the directory of the faulting module (and of this exe)
    // to the symbol search path, then refresh.
    {
        std::string search = exe_path;
        HMODULE     fault_module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(ep->ExceptionRecord->ExceptionAddress),
                           &fault_module);
        char module_path[MAX_PATH];
        if (fault_module != nullptr
            && GetModuleFileNameA(fault_module, module_path, MAX_PATH) > 0) {
            char* slash = strrchr(module_path, '\\');
            if (slash != nullptr) {
                *slash = '\0';
            }
            search += ";";
            search += module_path;
        }
        // Honour an explicitly configured symbol path (e.g. _NT_SYMBOL_PATH
        // pointing at the Qt bin directory so third-party PDBs resolve).
        const char* env_path = getenv("_NT_SYMBOL_PATH");
        if (env_path != nullptr && env_path[0] != '\0') {
            search += ";";
            search += env_path;
        }
        SymSetSearchPath(process, search.c_str());
        SymRefreshModuleList(process);
    }

    STACKFRAME64 frame{};
    frame.AddrPC.Offset    = static_cast<DWORD64>(ep->ContextRecord->Rip);
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = static_cast<DWORD64>(ep->ContextRecord->Rbp);
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = static_cast<DWORD64>(ep->ContextRecord->Rsp);
    frame.AddrStack.Mode   = AddrModeFlat;

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame,
                         ep->ContextRecord, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }

        union {
            SYMBOL_INFO info;
            char        pad[sizeof(SYMBOL_INFO) + 1024];
        } symbol;
        symbol.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol.info.MaxNameLen   = 1024;

        DWORD64 displacement = 0;
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, &symbol.info)) {
            wsprintfA(line, "  %2d  0x%p  %s", i,
                      reinterpret_cast<void*>(frame.AddrPC.Offset), symbol.info.Name);
            out(line);

            IMAGEHLP_LINE64 srcline{};
            srcline.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD  line_disp     = 0;
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_disp, &srcline)) {
                wsprintfA(line, "  [%s:%lu]\r\n", srcline.FileName, srcline.LineNumber);
                out(line);
            }
            else {
                out("\r\n");
            }
        }
        else {
            wsprintfA(line, "  %2d  0x%p  <no symbol>\r\n", i,
                      reinterpret_cast<void*>(frame.AddrPC.Offset));
            out(line);
        }
    }

    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }

    TerminateProcess(process, 1);
    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashLogger()
{
    SetUnhandledExceptionFilter(&vineCrashFilter);
}

#else

void installCrashLogger()
{
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv)
{
    installCrashLogger();

    // 通过 builder 构建并初始化 GUI 应用（内部会创建并显示主窗口）。
    // main 只声明应用身份（应用名/可选组织名），数据与配置目录由框架默认推导：
    // <用户数据>/<org>/<app>/{config,logs}，见 Application::dataDirectory()。
    fw::AppConfig config;
    config.name = "Vine";
    // config.organization 留空以使用框架默认组织名。
    // config.built_in_plugin_dir 留空以使用默认的自带插件目录。

    auto app = guifw::createGuiApplication(config, argc, argv);

    // 日志同时输出到控制台和数据目录下按日期滚动的文件。
    // main 只提供数据目录：日志落在 <data>/Vine/Vine/logs/vine.log（每日一文件）；
    // 配置文件由 builder 落在同级的 config/Vine.json。
    ::vine::logging::initDefault(::vine::logging::LogConfig{
        .level = ::vine::logging::LogLevel::Info,
        .sinks = {
            ::vine::logging::LogSink::console(),
            ::vine::logging::LogSink::dailyFile(app->dataDirectory() / "logs" / "vine.log"),
        },
    });

    // 加载插件：app_shell 会在主窗口上注册 Ribbon 标签与命令。
    if (!app->pluginManager()->loadAll()) {
        std::cerr << "Some plugins failed to load" << std::endl;
    }

    return app->run();
}
