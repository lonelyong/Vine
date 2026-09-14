#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include <vine/appfw/gui/ConsolePanel.hpp>

V_APPFWGUI_NS_BEGIN

/**
 * @brief Case-insensitive prefix completion over command entries.
 *
 * Matches a candidate when its name or any alias starts with the prefix.
 */
class CommandCompleter
{
  public:
    /**
     * @brief Replaces the candidate set and sorts it by name for stable order.
     *
     * @param entries Candidate commands.
     */
    void setEntries(std::vector<ConsoleCommandEntry> entries)
    {
        entries_ = std::move(entries);
        std::sort(entries_.begin(), entries_.end(),
            [](const ConsoleCommandEntry& a, const ConsoleCommandEntry& b) {
                if (a.source != b.source) {
                    return a.source < b.source;
                }
                return a.name < b.name;
            });
    }

    /**
     * @brief Returns all entries whose name or alias starts with the prefix.
     *
     * @param prefix Text to match against the start of each name/alias.
     * @return Matching entries, sorted by name.
     */
    std::vector<ConsoleCommandEntry> complete(const String& prefix) const
    {
        std::vector<ConsoleCommandEntry> result;
        for (const auto& entry : entries_)
        {
            if (entry.name.startsWith(prefix, true))
            {
                result.push_back(entry);
                continue;
            }
            for (const auto& alias : entry.aliases)
            {
                if (alias.startsWith(prefix, true))
                {
                    result.push_back(entry);
                    break;
                }
            }
        }
        return result;
    }

  private:
    std::vector<ConsoleCommandEntry> entries_;
};

V_APPFWGUI_NS_END
