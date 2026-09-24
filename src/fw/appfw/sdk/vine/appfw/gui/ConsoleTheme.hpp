#pragma once

#include <vine/appfw/appfw_global.hpp>
#include <vine/Color.hpp>

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Semantic message type used by the console.
 */
enum class ConsoleMessageType
{
    Normal,   ///< Plain output.
    Command,  ///< Command echo.
    Prompt,   ///< Prompt for user input.
    Warning,  ///< Warning output.
    Error     ///< Error output.
};

/**
 * @brief Console color scheme, keyed by message type.
 *
 * Provides light and dark presets; the active preset follows the application
 * theme and can be overridden through ConsolePanel::setTheme().
 */
struct ConsoleTheme
{
    Color normal;
    Color command;
    Color prompt;
    Color warning;
    Color error;

    /**
     * @brief Returns the default light color scheme.
     */
    static ConsoleTheme light()
    {
        return { Color(0x1a1a1affu), Color(0x1e5aa8ffu), Color(0x2e7d32ffu),
                 Color(0xc88700ffu), Color(0xd32f2fffu) };
    }

    /**
     * @brief Returns the default dark color scheme.
     */
    static ConsoleTheme dark()
    {
        return { Color(0xe0e0e0ffu), Color(0x7fa8e0ffu), Color(0x8fd18fffu),
                 Color(0xe0b04cffu), Color(0xf28b82ffu) };
    }
};

VN_APPFWGUI_NS_END
