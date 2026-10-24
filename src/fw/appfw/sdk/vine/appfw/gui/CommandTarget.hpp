#pragma once

#include <vine/appfw/Command.hpp>

#include <vine/appfw/gui/CentralDocumentHost.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/MainWindow.hpp>

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Resolves the target a command dispatched now should act on.
 *
 * @details
 * - The window's central document host knows which view is on screen, and that view is the presentation a command
 *   acts on: `target.document` is its document and `target.view` is the view itself. The document is taken FROM the
 *   view on purpose - a view knows its document, the reverse is not true (see ADR-2026-10-24-command-target.md).
 * - `target.view` is filled only when the view has a render control, i.e. when there is a 3D area to act on. A 2D
 *   view therefore resolves to a document-only target, and `CommandFlags::NeedsView` refuses uniformly instead of
 *   every 3D command discovering the missing control itself.
 * - Nothing shown, central area not ours, or no window at all (a console-only host) resolves to an EMPTY target -
 *   a legal state, not a failure: document-level commands still run, view-level ones get the uniform refusal.
 * - Resolution happens at DISPATCH time and travels with the execution, so a command never asks "what is current"
 *   while it runs: two windows or two panes can each carry their own answer.
 *
 * @param window Window the dispatch came from; may be null, and so may its dock manager and central host.
 * @return The target; every layer may be null.
 */
inline vn::appfw::CommandTarget resolveCommandTarget(MainWindow* window)
{
    vn::appfw::CommandTarget target;
    if (window == nullptr) {
        return target;
    }

    DockPanelManager* docks = window->dockPanelManager();
    if (docks == nullptr) {
        return target;
    }

    auto* host = dynamic_cast<CentralDocumentHost*>(docks->centralWidget());
    if (host == nullptr) {
        return target;
    }

    DocumentView* view = host->currentView();
    if (view == nullptr) {
        return target;
    }

    target.document = &view->document();
    if (view->renderControl() != nullptr) {
        target.view = view;
    }
    return target;
}

/**
 * @brief Reads a target's view as the `DocumentView` the command layer carries opaquely.
 *
 * @details
 * - This is the ONE place that casts: `CommandTarget::view` is an `Object` so that the command layer can name the
 *   target without importing GUI types (see CommandTarget in `vine/appfw/Command.hpp`).
 *
 * @param target Target handed to the execution.
 * @return The view, or null when the target has no presentation with a 3D area.
 */
inline DocumentView* targetView(const vn::appfw::CommandTarget& target)
{
    return obj_cast<DocumentView>(target.view);
}

VN_APPFWGUI_NS_END
