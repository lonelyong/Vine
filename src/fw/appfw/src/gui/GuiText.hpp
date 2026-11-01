#pragma once

#include <QString>

#include <vine/String.hpp>

#include <vine/appfw/Translation.hpp>
#include <vine/appfw/gui/Gui.hpp>

#include "Convert.hpp"

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Translates UI text and hands it to a Qt API as a `QString`.
 *
 * @details
 * - Same lookup as `vn::appfw::translate()`, in the form the GUI needs: the context is explicit and the result is a
 *   `QString`, which is what `setText`, `setWindowTitle` and friends take.
 * - The name and the parameter order are `QCoreApplication::translate()`'s on purpose: the translation tool extracts
 *   call sites by that name, so a house name of our own would silently stop feeding the catalog (P9 in
 *   `.ai/design/appfw-translation.md`).
 * - Prefer this over a bare `tr()`: inside a `QObject` subclass `tr()` resolves to `QObject::tr`, whose context is the
 *   class name - the catalog has no such entry, and renaming the class would lose its translations (P7).
 *
 * @note Internal to the appfw GUI layer: `src/` is a private include directory, it is not installed and it is not
 * exported, so this cannot leak into the SDK's surface (C8).
 *
 * @param context Owner of the text: `"appfw"` for the framework's own UI, the plugin id for a plugin.
 * @param source  Chinese source text; must be a compile-time literal so the translation tool can extract it.
 * @return The translation, or @p source when no catalog is loaded.
 */
inline QString translate(const char* context, const char* source)
{
    return Convert::toQString(vn::appfw::translate(context, source));
}

VN_APPFWGUI_NS_END
