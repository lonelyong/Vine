#pragma once

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
    virtual vine::async::Task<std::optional<int8_t>>        getIntAsync(const String& prompt = {}) override;
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

    void completeString(const String& value);
    void completeInt(int8_t value);
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
    bool                 cancelled_{ false };

    String        stringResult_;
    int8_t        intResult_{ 0 };
    double        doubleResult_{ 0.0 };
    math::Point3d pointResult_;

    ConsolePanel*                     console_{ nullptr };
    PendingRead                       pending_{ PendingRead::None };
    String                            currentPrompt_;
};

V_APPFWGUI_NS_END
