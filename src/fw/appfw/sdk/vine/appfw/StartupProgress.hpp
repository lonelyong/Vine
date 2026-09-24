#pragma once

#include "appfw_global.hpp"

#include <memory>
#include <string>

VN_APPFW_NS_BEGIN

/**
 * @brief Startup-phase progress sink: reports what a boot is doing to the startup frame and to any other presenter.
 *
 * A boot is a sequence of stages: the framework reports "initializing the user interface" and "loading plugin xxx",
 * and the application may add its own stages ("restoring the last session"), because only the application knows them.
 * The sink folds all of them into one progress: the label says what is happening right now, and the bar says how far
 * the current stage has come.
 *
 * Two kinds of stage:
 *
 * - a counted stage (stage(name, total)) knows its total, so the bar advances by done/total - the plugin load, which
 *   counts plugins, is the one that matters in practice;
 * - an indeterminate stage (stage(name)) knows what is happening but not how long it takes, so the bar is shown as
 *   busy.
 *
 * Every counted stage starts from zero, so the bar reads "how far this stage has come", not "how much of the whole
 * boot is left": no reporter can know the length of the stages it has not reached yet, and a bar that dips or jumps
 * would be lying.
 *
 * The sink owns a foreground ProgressHost, so the startup progress is not private to the startup frame: the status
 * bar's ProgressPresenter and the headless ConsoleProgressReporter show the same state without any extra wiring.
 *
 * Lifetime: at most one per process, owned by Application::startupProgress(), created when the boot starts and
 * destroyed by Application::finishStartup(). Without a sink (StartupProgress::current() == nullptr) every call is a
 * no-op, so boot code may report unconditionally.
 *
 * Thread contract: stage()/advance()/setLabel()/complete() are called by the reporting thread (the application thread
 * during a boot), while label()/isCounted()/fraction() are read from wherever a presenter runs.
 */
class VN_APPFW_API StartupProgress
{
  public:
    StartupProgress();
    ~StartupProgress();

    StartupProgress(const StartupProgress&)            = delete;
    StartupProgress& operator=(const StartupProgress&) = delete;

  public:
    /**
     * @brief Returns the process-wide startup progress sink, or nullptr when there is none.
     *
     * @return The sink of the running boot, or nullptr.
     */
    static StartupProgress* current();

  public:
    /**
     * @brief Begins a stage that cannot be counted.
     *
     * The bar is shown as busy: what is happening is known, how long it takes is not.
     *
     * @param name Status text, e.g. "正在查找插件".
     */
    void stage(const std::string& name);

    /**
     * @brief Begins a stage that reports how much of it is done.
     *
     * The bar returns to its start and follows advance(); total <= 0 is equivalent to stage(name).
     *
     * @param name Status text, e.g. "正在加载插件".
     * @param total Number of units the stage consists of.
     */
    void stage(const std::string& name, double total);

    /**
     * @brief Advances the current counted stage to the number of units done.
     *
     * @param done Units done so far (an absolute count, not an increment); a value below what was already reported is
     *        ignored.
     */
    void advance(double done);

    /**
     * @brief Updates the status text without changing how the current stage is counted.
     *
     * This is how "what is happening" is said down to the plugin ("正在加载插件 app_shell (2/3)") while the bar keeps
     * following the stage as a whole.
     *
     * @param text The new status text.
     */
    void setLabel(const std::string& text);

    /**
     * @brief Ends the current stage and fills the bar.
     *
     * Called by Application::finishStartup(); reporting a further stage afterwards starts over.
     */
    void complete();

  public:
    /**
     * @brief Returns the current status text.
     *
     * @return The status text, possibly empty.
     */
    std::string label() const;

    /**
     * @brief Returns whether the current stage can be counted.
     *
     * @return true when fraction() is meaningful, false when the bar is to be shown as busy.
     */
    bool isCounted() const;

    /**
     * @brief Returns how far the current counted stage has come.
     *
     * @return The units done as a fraction of the stage's total, in [0, 1]; 0 for an indeterminate stage.
     */
    double fraction() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_APPFW_NS_END
