#pragma once

#include <vine/appfw/DocumentManager.hpp>

#include <vine/appfw/gui/Window.hpp>

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Document manager dialog: the registered document types, and the documents that are open right now.
 *
 * READ-ONLY ON PURPOSE, and that is the design rather than an unfinished state. The framework defines no "document
 * area", no single- versus multi-document policy and no view types (see appfw-document-model.md), so there is nothing
 * here to switch: what this dialog answers is "which types exist, who registered them, WHAT CAN THEY BE BUILT FROM,
 * and what is open" - which is exactly the question a user asks when a file will not open ("is anything registered for
 * this payload?") or a document cannot be found ("which source is it on?").
 *
 * A type cannot be switched off. Whether a type is available is decided by the plugin that registers it, and that
 * plugin already has a well-defined switch (disable or skip the plugin, see PluginManager); a second enable flag on the
 * type would add an axis whose semantics nobody can name - what happens to the documents already opened from it?
 *
 * The two tables are refreshed from the manager on open and on demand; the dialog keeps no state of its own.
 */
class VN_APPFW_API DocumentManagerDialog : public Window {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs the dialog over a document manager.
     *
     * @param manager Manager to list; nullptr lists nothing (every table stays empty).
     */
    explicit DocumentManagerDialog(vn::appfw::DocumentManager* manager);
    ~DocumentManagerDialog() override;

  public:
    /**
     * @brief Rebuilds both tables from the manager, applying the current filter.
     */
    void refresh();

  private:
    void applyFilter();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
