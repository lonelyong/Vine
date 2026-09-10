#pragma once

#include <vine/appfw/CommandManager.hpp>

#include <vine/appfw/gui/Window.hpp>

V_APPFWGUI_NS_BEGIN

/**
 * @brief Command manager dialog: lists registered commands (name, state, aliases,
 * source plugin, group, description) with a filter, and lets the user disable or
 * enable a command.
 *
 * Disabling is a flag, not a removal: the command stays registered and listed, it
 * only stops being executable, and enabling it restores execution right away. The
 * choice is persisted, so a command disabled here stays disabled after a restart
 * even though plugins register their commands again on every load.
 */
class V_APPFW_API CommandManagerDialog : public Window {
    V_OBJECT_META_DECL;

  public:
    explicit CommandManagerDialog(vine::appfw::CommandManager* manager);
    ~CommandManagerDialog() override;

  public:
    /**
     * @brief Rebuilds the command table from the manager, applying the current
     * filter.
     */
    void refresh();

  private:
    void applyFilter();
    void toggleSelectedEnabled();
    void updateActions();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
