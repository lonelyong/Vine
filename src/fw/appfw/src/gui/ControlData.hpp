#pragma once

#include <vine/appfw/gui/UIElementData.hpp>

V_APPFWGUI_NS_BEGIN

/**
 * @brief Data block of the Control level: what every control's Impl shares.
 *
 * Control keeps no state of its own - its properties (enabled, visible, tooltip, size) go straight to
 * the QWidget it wraps - so this carries nothing beyond what UIElementData already holds. It exists so
 * that the data hierarchy mirrors the widget hierarchy: a control declares its own Impl deriving from
 * this one, which keeps the dptr() downcast of each class well-founded, and a field added here reaches
 * every control without touching any of them.
 *
 * Same shape as ApplicationData/GuiApplicationData: a NAMED data block (not the nested Impl a leaf
 * class declares) at every level others derive from, holding data only and no behaviour.
 *
 * This is a private header: only the class implementations in this directory include it.
 */
struct ControlData : public UIElementData {};

V_APPFWGUI_NS_END
