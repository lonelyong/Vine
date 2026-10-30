#include <cstdio>
#include <string>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <vine/logging/Log.hpp>
#include <vine/logging/LogSink.hpp>

#include <vine/String.hpp>

#include <vine/crash/CrashHandler.hpp>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/gui/GuiAppBuilder.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

namespace fw    = vn::appfw;
namespace guifw = fw::gui;

using vn::String;

namespace
{

/**
 * @brief The compiler this build used, as one "about" detail row.
 *
 * @return Version text of the compiler that compiled this translation unit.
 */
String compilerInfo()
{
#if defined(_MSC_VER)
    return String::fromUtf8(("MSVC " + std::to_string(_MSC_VER)).c_str());
#elif defined(__clang__)
    return String::fromUtf8(__clang_version__);
#elif defined(__GNUC__)
    return String::fromUtf8(__VERSION__);
#else
    return u8"unknown";
#endif
}

/**
 * @brief Whether this build is a debug build, as one "about" detail row.
 *
 * @return "Debug" or "Release".
 */
String buildKindInfo()
{
#ifdef NDEBUG
    return u8"Release";
#else
    return u8"Debug";
#endif
}

/**
 * @brief One line naming the application and the build, written at the top of a crash report.
 *
 * @details
 * - Composed here, not in the crash module: an identity line is application knowledge, and the module stays product-agnostic
 *   (it knows nothing about Vine).
 *
 * @param info Application identity the host declared in its AppConfig.
 * @return "Vine 1.0.0 (Debug)", or just the name when no version was declared.
 */
String identityLine(const fw::AppInfo& info)
{
    std::string line = info.name.as_std_str();
    if (!info.version.empty()) {
        line += ' ';
        line += info.version.as_std_str();
    }
    line += " (";
    line += buildKindInfo().as_std_str();
    line += ')';
    return String::fromUtf8(line);
}

} // namespace

int main(int argc, char** argv)
{
    // 通过 builder 构建应用：构造函数建身份、Qt 应用对象、UserIO、配置文件与窗口（不 show）；
    // main 只声明应用身份（应用名/可选组织名），数据与配置目录由框架默认推导：
    // <用户数据>/<org>/<app>/{config,logs}，见 Application::dataDirectory()。
    fw::AppConfig config;
    config.name           = u8"Vine";
    // config.organization 留空以使用框架默认组织名。
    // config.built_in_plugin_dir 留空以使用默认的自带插件目录；config.load_plugins 默认开，
    // 插件由框架在启动阶段加载（下面的 run() 里），宿主不用自己调 loadAll()。
    // 启动框：显示到 startupEnd() 为止，期间报告正在启动/正在加载哪个插件。
    config.splash.enabled = true;

    // “关于”的内容属于应用，在这里声明一次：框架持有它（Application::appInfo()），关于页/关于对话框/标题都读它。
    // 版本与构建事实本应由构建系统生成（见 .ai/backlog/appfw-app-info.md 的 T1）。
    config.info.name        = config.name;
    config.info.version     = u8"1.0.0";
    config.info.description = u8"基于 Qt 与 C++20 的模块化应用框架";
    config.info.copyright   = u8"© 2026 Vine";
    config.info.license     = u8"MIT";
    config.info.homepage    = u8"https://github.com/zuijiuzhou/Vine";
    config.info.issues_url  = u8"https://github.com/zuijiuzhou/Vine/issues";
    config.info.logo        = ":/icons/about.svg"; // 由 app_shell 注册的资源；面板在装配时解析
    config.info.build_facts = {
        { u8"Qt", String::fromUtf8(qVersion()) },
        { u8"编译器", compilerInfo() },
        { u8"构建", buildKindInfo() },
    };
    config.info.credits = u8"Qt 6 \nEigen 3 \nwolfSSL \nvsg \nSARibbon \nDockingPanes \nspdlog \ngtest";

    // 崩溃报告属于进程，不属于界面，所以它在 createGuiApplication() 之前装好：此后无论崩在 Qt 初始化、插件加载、
    // 启动阶段还是渲染循环里，留下的都是同一份报告。
    // 路径全留空 = 都放在 exe 旁边（<app>_crash.log / <app>_crash.dmp / 同目录的 vnCrashHandler.exe），因此这里没有
    // 一行文件系统代码；报告头那一行只带应用身份，因为崩溃模块不认识本应用。
    {
        vn::crash::Config crash_config;
        crash_config.app_name = config.name;
        crash_config.context  = { identityLine(config.info) };
        vn::crash::install(crash_config);
    }

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

    // 启动期应用线程最长连续占用（`VN_BOOT_TIMING=1`；门禁读这一行）。这是"启动不会把界面卡住"的实测依据：
    // 现在的形状里，会话 attach（设备/管线）与 warm-up 那一帧都在池上跑，剩下的账是 功能栏+面板 / 取句柄+尺寸 /
    // 收尾 三段（2026-09-26 实测 74 ms；改成同步形状时它是 414 ms 一整段）。守着它的地方：
    // scripts/vsg_rewrite_gate.sh 的应用阶段（阈值 VN_GATE_BOOT_OCCUPANCY_MS）。
    //
    // 这一行同时报告那一段"什么时候"发生（`ending ... ms after start`）：门禁变红时，光看一个数分不清是"该上池的
    // 活干在了线程上"还是"这台机器/这个驱动整体慢"；日志时间戳对上扬弃点就知道它落在启动的哪一段（会话 attach /
    // 首次 present / 插件加载），诊断不用再重建。附带的偏差：10 ms 定时器本身会把 0–10 ms 算进空转。
    if (qEnvironmentVariableIsSet("VN_BOOT_TIMING")) {
        static QElapsedTimer since_start;
        static qint64        last_tick     = 0;
        static qint64        worst_gap     = 0;
        static qint64        worst_gap_end = 0;
        since_start.start();

        auto* watch = new QTimer(QCoreApplication::instance());
        watch->setInterval(10);
        QObject::connect(watch, &QTimer::timeout, QCoreApplication::instance(), [] {
            const qint64 now = since_start.elapsed();
            const qint64 gap = now - last_tick;
            last_tick        = now;
            if (gap > worst_gap) {
                worst_gap     = gap;
                worst_gap_end = now;
            }
        });
        watch->start();

        QTimer::singleShot(1500, QCoreApplication::instance(), [] {
            // The worst stretch ends at worst_gap_end: the reader lines it up against the boot's own log lines (the
            // session attach, the surface reaching Presenting, a plugin load) instead of guessing which stage it was.
            std::fprintf(stderr,
                         "[boot-timing] application thread max continuous occupancy: %lld ms ending %lld ms after start\n",
                         static_cast<long long>(worst_gap),
                         static_cast<long long>(worst_gap_end));
        });
    }

    // 剩下的全归框架：run() 先把事件循环跑起来，再由队列里的启动步上屏、等首帧、加载插件
    // （AppConfig::load_plugins 默认开），最后结束启动阶段（关掉启动框、把主窗口提到前面）。
    // 这个宿主没有自己的启动工作：有的话就重写 startup() 那一拍（重活再由它自己丢到池上）。
    return app->run();
}
