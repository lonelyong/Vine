#pragma once

#include "ControlData.hpp"

V_APPFWGUI_NS_BEGIN

/**
 * @brief Data block of the Window level: what every top-level window's Impl shares.
 *
 * Window keeps no state of its own either - the window state, the startup position and the active
 * flags all live on the QWidget (its own flags and a dynamic property) - so this carries nothing
 * beyond ControlData. It is the block Window itself passes up and the base a window class's Impl
 * derives from; see ControlData for why each level is a named data block instead of one Impl.
 *
 * This is a private header: only the class implementations in this directory include it.
 */
struct WindowData : public ControlData {};

V_APPFWGUI_NS_END
