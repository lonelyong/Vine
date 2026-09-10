#pragma once

#include <atomic>

#include <vine/Events.hpp>
#include <vine/Signal.hpp>
#include <vine/async/AsyncEvent.hpp>
#include <vine/appfw/UserIO.hpp>

V_APPFW_NS_BEGIN
class CommandManager;
V_APPFW_NS_END

V_APPFWGUI_NS_BEGIN

class ConsolePanel;

class VisualUserIO : public UserIO {
    V_OBJECT_META_DECL;
    V_DISABLE_COPY_MOVE(VisualUserIO);

  public:
    VisualUserIO();
    virtual ~VisualUserIO();

  public:
    void setConsolePanel(ConsolePanel* console);
    void setCommandManager(vine::appfw::CommandManager* manager) override;

  public:
    virtual void putString(const String& str) override;
    virtual void clear() override;
    virtual void cancelPendingInput() override;

    virtual vine::async::Task<std::optional<String>>        getStringAsync(const String& prompt = {}) override;
    virtual vine::async::Task<std::optional<int>>           getIntAsync(const String& prompt = {}) override;
    virtual vine::async::Task<std::optional<double>>        getDoubleAsync(const String& prompt = {}) override;
    virtual vine::async::Task<std::optional<math::Point3d>> getPoint3dAsync(const String& prompt = {}) override;

  private:
    enum class PendingRead
    {
        None,
        String,
        Int,
        Double,
        Point
    };

    /// Releases the interaction slot when the read ends - however it ends,
    /// including a coroutine frame that is destroyed before it ever resumed.
    struct ReadScope {
        VisualUserIO* self;
        ~ReadScope() { self->endRead(); }
    };

    /// Claims the single interaction slot and shows the prompt; false when another
    /// read is already waiting.
    bool beginRead(PendingRead kind, const String& prompt);
    /// Releases the interaction slot.
    void endRead() noexcept;
    /// Waits for the user to answer the prompt; true when a value arrived, false
    /// when the interaction was cancelled.
    vine::async::Task<bool> waitForInput(PendingRead kind, const String& prompt);

    void completeString(const String& value);
    void completeInt(int value);
    void completeDouble(double value);
    void completePoint(const math::Point3d& value);
    void cancelInteraction();

    void onLineEntered(const String& text);
    void onEscape();
    void parseAndComplete(const String& text);
    void repromptError(const String& message);
    void refreshCompletion();

    /**
     * @brief Appends an error message to the console on the application thread.
     *
     * The command completion callback runs on the thread that finished the
     * command, which is not necessarily the application thread; the console panel
     * is a QWidget and may only be touched there.
     *
     * @param message Message to show.
     */
    void appendOnApplicationThread(const String& message);

  private:
    vine::async::AsyncEvent done_;
    std::atomic<bool>    cancelled_{ false };

    /// The interaction slot: only one read may wait for the user at a time, because
    /// the console shows a single prompt and two waiting reads would share done_
    /// and each other's result fields. Claimed and released with compare/exchange,
    /// so a second read can never slip in.
    std::atomic<PendingRead> pending_{ PendingRead::None };

    String        stringResult_;
    int           intResult_{ 0 };
    double        doubleResult_{ 0.0 };
    math::Point3d pointResult_;

    ConsolePanel* console_{ nullptr };
    /// Handlers registered on the bound console, so a rebind can drop them again.
    vine::Signal<const String&>::HandlerId line_handler_{ 0 };
    vine::Signal<>::HandlerId              escape_handler_{ 0 };
    /// Handler registered on the command manager's commandsChanged().
    vine::Signal<vine::appfw::CommandManager&, vine::EventArgs&>::HandlerId commands_handler_{ 0 };
    String                                                                 currentPrompt_;
};

V_APPFWGUI_NS_END
