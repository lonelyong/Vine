#pragma once

#include "ControlData.hpp"

class QObject;

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Data block of the Window level: what every top-level window's Impl shares.
 *
 * Window keeps almost no state of its own - the window state, the startup position and the active flags all live on
 * the QWidget (its own flags and a dynamic property) - so the only thing this carries is the watcher that answers
 * Window::hasPainted().
 *
 * It is the block Window itself passes up and the base a window class's Impl derives from; see ControlData for why
 * each level is a named data block instead of one Impl.
 *
 * This is a private header: only the class implementations in this directory include it.
 */
struct WindowData : public ControlData
{
    /// Watches the window's widget tree for its first paint; see Window::hasPainted().
    /// Held as a QObject* because the watcher is a Window.cpp detail: this header carries the pointer, not the type.
    QObject* paint_watcher = nullptr;
};

VN_APPFWGUI_NS_END
