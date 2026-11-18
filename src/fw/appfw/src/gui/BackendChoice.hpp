#pragma once
#include <span>
#include <vector>

#include <vine/String.hpp>
#include <vine/appfw/appfw_global.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>

VN_APPFWGUI_NS_BEGIN

/**
 * @brief What one attach brings up: a registered backend name, or the reason nothing can be brought up.
 *
 * @details
 * - Exactly one of the two fields is set, so a caller reads one branch instead of testing a name and then guessing
 *   why it is empty.
 * - This is the DECISION, not the creation: the registry says what is installed, the application says what it wants
 *   (see RenderControl::setDefaultBackendName), and chooseBackend answers what an attach does with the two.
 */
struct BackendChoice {
    String name;    /// Registered backend to create; empty when @c refusal carries the reason.
    String refusal; /// Host-facing reason no backend can be brought up; empty when @c name is set.
};

/**
 * @brief Decides which backend an attach creates, from what is registered and what the application named.
 *
 * @details
 * - THE NAME WINS: a named backend that is registered is the answer, whatever else is installed.
 * - NO NAME, ONE REGISTERED: that one, because there is nothing to choose between.
 * - NO NAME, NONE REGISTERED: refused, with "no render backend is registered".
 * - NO NAME, SEVERAL REGISTERED: refused, naming them and how to choose one. This replaces "the first entry", which
 *   was a choice made by the ALPHABET (`entries()` is ordered by name) and by whichever plugin happened to add a
 *   name - a default no line of code stated, so a reader could not find it and a new plugin changed it silently
 *   (`G4`).
 * - A NAMED BACKEND THAT IS NOT REGISTERED is refused, naming it and what IS registered: falling back would turn a
 *   misspelt name into a silent switch of renderer.
 *
 * @param entries What the registry reports as installed; its order is never used as a decision.
 * @param named The backend name the application stated; empty when it stated none.
 * @return The name to create, or the refusal reason (see BackendChoice).
 */
inline BackendChoice chooseBackend(std::span<const vn::graphics::RenderBackendRegistry::Entry> entries, const String& named)
{
    /// The registered names as one "a, b" sentence, for a refusal that has to say what the caller may choose from.
    const auto registered_names = [entries] {
        String joined;
        for (const auto& entry : entries) {
            if (!joined.empty()) {
                joined += u8", ";
            }
            joined += entry.info.name;
        }
        return joined;
    };

    if (!named.empty()) {
        for (const auto& entry : entries) {
            if (entry.info.name == named) {
                return BackendChoice{ named, {} };
            }
        }
        return BackendChoice{ {}, String(u8"the render backend \"") + named + u8"\" is not registered" +
                                   (entries.empty() ? String(u8"(no render backend is registered)")
                                                    : String(u8" (registered: ") + registered_names() + String(u8")")) };
    }

    if (entries.empty()) {
        return BackendChoice{ {}, String(u8"no render backend is registered") };
    }
    if (entries.size() > 1) {
        return BackendChoice{ {}, String(u8"several render backends are registered (") + registered_names() +
                                   String(u8"); name one with RenderControl::setDefaultBackendName() before the first attach") };
    }

    // ONE registered backend: taking it decides nothing (there is no other), so it needs no name from the application.
    return BackendChoice{ entries[0].info.name, {} };
}

VN_APPFWGUI_NS_END
