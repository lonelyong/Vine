#pragma once

#include <memory>
#include <optional>

#include <vine/appfw/appfw_global.hpp>
#include <vine/appfw/UserIO.hpp>

V_APPFWGUI_NS_BEGIN

class ConsolePanel;

/**
 * @brief UserIO that shows its output and prompts in a ConsolePanel.
 *
 * The panel is not owned here: whoever builds the user interface creates and docks it and binds it through
 * setConsolePanel(), and may unbind it again. Without a panel the output is dropped and a waiting read cannot be answered,
 * so a host that wants its input prompts to work has to bind one - GuiApplication::createUserIO() returns this class by
 * default, and the host that builds the console (the application shell) binds it itself.
 *
 * Every touch of the panel is marshalled to the application thread, because the entry points may be called from any
 * thread (see UserIO). The panel may also be destroyed before a marshalled call runs, which is why the calls are guarded
 * and a write into a dead panel is a no-op instead of a crash.
 *
 * The state and the helpers behind it live in the implementation, so the layout of this class does not change when they
 * do.
 */
class V_APPFW_API VisualUserIO : public UserIO {
    V_OBJECT_META_DECL;
    V_DISABLE_COPY_MOVE(VisualUserIO);

  public:
    VisualUserIO();
    ~VisualUserIO() override;

  public:
    /**
     * @brief Binds the panel the output and the prompts go to.
     *
     * Binding a panel drops the handlers of the panel that was bound before, so binding the same panel twice does not
     * dispatch an entered line twice.
     *
     * @param console Panel to show the output and the prompts in, or nullptr to unbind.
     */
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
    struct Impl;
    std::unique_ptr<Impl> d;
};

V_APPFWGUI_NS_END
