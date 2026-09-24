#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <cstddef>
#include <utility>
#include <vector>

#include <vine/String.hpp>

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Up/down command history for the console input line.
 */
class CommandHistory
{
  public:
    /**
     * @brief Records a command; empty commands are ignored.
     *
     * @param command Command text to append.
     */
    void add(String command)
    {
        if (command.empty())
        {
            return;
        }
        items_.push_back(std::move(command));
        reset();
    }

    /**
     * @brief Returns the previous command, saving the current input on first entry.
     *
     * @param currentInput Text currently being edited.
     * @return The previous command, or an empty string when none exists.
     */
    String previous(const String& currentInput)
    {
        if (pos_ == items_.size())
        {
            saved_ = currentInput;
        }
        if (pos_ > 0)
        {
            --pos_;
        }
        return pos_ < items_.size() ? items_[pos_] : String();
    }

    /**
     * @brief Returns the next command; past the newest entry restores the saved input.
     *
     * @return The next command, or the restored input.
     */
    String next()
    {
        if (pos_ + 1 < items_.size())
        {
            ++pos_;
            return items_[pos_];
        }
        reset();
        return saved_;
    }

    /**
     * @brief Resets navigation to the newest entry.
     */
    void reset()
    {
        pos_ = items_.size();
        saved_.clear();
    }

  private:
    std::vector<String> items_;
    std::size_t         pos_{ 0 };
    String              saved_;
};

VN_APPFWGUI_NS_END
