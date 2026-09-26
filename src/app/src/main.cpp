#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
// clang-format off
#    include <windows.h>  // Must precede dbghelp.h.
#    include <dbghelp.h>
// clang-format on
#    pragma comment(lib, "dbghelp.lib")
#endif

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <vine/logging/Log.hpp>
#include <vine/logging/LogSink.hpp>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/gui/GuiAppBuilder.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

namespace fw    = vn::appfw;
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

    // 通过 builder 构建应用：构造函数建身份、Qt 应用对象、UserIO、配置文件与窗口（不 show）；
    // main 只声明应用身份（应用名/可选组织名），数据与配置目录由框架默认推导：
    // <用户数据>/<org>/<app>/{config,logs}，见 Application::dataDirectory()。
    fw::AppConfig config;
    config.name = u8"Vine";
    // config.organization 留空以使用框架默认组织名。
    // config.built_in_plugin_dir 留空以使用默认的自带插件目录；config.load_plugins 默认开，
    // 插件由框架在启动阶段加载（下面的 run() 里），宿主不用自己调 loadAll()。
    // 启动框：显示到 startupEnd() 为止，期间报告正在启动/正在加载哪个插件。
    config.splash.enabled = true;

    auto app = guifw::createGuiApplication(config, argc, argv);

    // 日志落在哪里只有应用知道（控制台 + 数据目录下按日期滚动），所以它归 main —— 而且是**初始化**而不是
    // 启动阶段的一步：它不进启动框，也不必等界面上屏。数据目录要等应用建好才知道，所以这是最早能放的位置。
    // 这条规则只受一条约束：任何想进日志的启动动作（包括插件的 load()）都得排在它后面。
    ::vn::logging::initDefault(::vn::logging::LogConfig{
        .level = ::vn::logging::LogLevel::Info,
        .sinks = {
            ::vn::logging::LogSink::console(),
            ::vn::logging::LogSink::dailyFile(app->dataDirectory() / "logs" / "vine.log"),
        },
    });

    // 启动期应用线程最长连续占用（`VINE_BOOT_TIMING=1`；门禁读这一行）。这是"启动不会把界面卡住"的实测依据：
    // 现在的形状里，会话 attach（设备/管线）与 warm-up 那一帧都在池上跑，剩下的账是 功能栏+面板 / 取句柄+尺寸 /
    // 收尾 三段（2026-09-26 实测 74 ms；改成同步形状时它是 414 ms 一整段）。守着它的地方：
    // scripts/vsg_rewrite_gate.sh 的应用阶段（阈值 VINE_GATE_BOOT_OCCUPANCY_MS，默认 150）。
    if (qEnvironmentVariableIsSet("VINE_BOOT_TIMING")) {
        static QElapsedTimer since_start;
        static qint64        last_tick = 0;
        static qint64        worst_gap = 0;
        since_start.start();

        auto* watch = new QTimer(QCoreApplication::instance());
        watch->setInterval(10);
        QObject::connect(watch, &QTimer::timeout, QCoreApplication::instance(), [] {
            const qint64 now = since_start.elapsed();
            const qint64 gap = now - last_tick;
            last_tick        = now;
            if (gap > worst_gap) {
                worst_gap = gap;
            }
        });
        watch->start();

        QTimer::singleShot(1500, QCoreApplication::instance(), [] {
            std::fprintf(stderr, "[boot-timing] application thread max continuous occupancy: %lld ms\n", static_cast<long long>(worst_gap));
        });
    }

    // 剩下的全归框架：run() 先把事件循环跑起来，再由队列里的启动步上屏、等首帧、加载插件
    // （AppConfig::load_plugins 默认开），最后结束启动阶段（关掉启动框、把主窗口提到前面）。
    // 这个宿主没有自己的启动工作：有的话就重写 startup() 那一拍（重活再由它自己丢到池上）。
    return app->run();
}
