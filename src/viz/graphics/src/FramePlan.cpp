#include <vine/graphics/FramePlan.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

VN_GRAPHICS_NS_BEGIN

namespace
{

/** @brief A pointer as the plan prints it: identity, in the shape the log's own lines already use ("0x8f0ab2"). */
std::string pointerText(const void* pointer)
{
    if (pointer == nullptr) {
        return "-";
    }
    char buffer[2 + sizeof(std::uintptr_t) * 2 + 1]{};
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pointer)));
    return buffer;
}

/** @brief A pass' clear as the colour it clears to, or a dash when it does not clear at all. */
std::string clearText(const PassPlan& pass)
{
    if (!pass.clears) {
        return "-";
    }
    return "rgba(" + std::to_string(pass.clear_color.r) + "," + std::to_string(pass.clear_color.g) + "," +
           std::to_string(pass.clear_color.b) + "," + std::to_string(pass.clear_color.a) +
           (pass.clears_depth ? ",depth)" : ")");
}

/** @brief A pass' declared input names, or a dash when it declared none. */
std::string listText(const std::vector<String>& names)
{
    if (names.empty()) {
        return "-";
    }
    std::string text;
    for (const String& name : names) {
        if (!text.empty()) {
            text += ",";
        }
        text += name.empty() ? std::string("(unnamed)") : name.as_std_str();
    }
    return text;
}

/** @brief A layer mask as the plan prints it ("0x3"). */
std::string layerMaskText(std::uint32_t mask)
{
    char buffer[2 + sizeof(std::uint32_t) * 2 + 1]{};
    std::snprintf(buffer, sizeof(buffer), "0x%x", static_cast<unsigned>(mask));
    return buffer;
}

/** @brief A pass' viewport rectangle, or `full` when it draws the whole target. */
std::string viewportText(const PassPlan& pass)
{
    if (!pass.viewport.has_value()) {
        return "full";
    }
    const Viewport& rect = *pass.viewport;
    return "[" + std::to_string(rect.x) + "," + std::to_string(rect.y) + " " + std::to_string(rect.width) + "x" +
           std::to_string(rect.height) + "]";
}

}  // namespace

String FramePlan::describe() const
{
    std::size_t disabled = 0;
    for (const PassPlan& pass : passes) {
        if (!pass.enabled) {
            ++disabled;
        }
    }

    // The header states the count and, when it matters, how many of them will not run: "5 passes" of which four are
    // disabled is not the same frame as five running ones.
    std::string text = "frame plan: " + std::to_string(passes.size()) + " pass(es)";
    if (disabled != 0) {
        text += " (" + std::to_string(disabled) + " disabled)";
    }

    for (std::size_t index = 0; index < passes.size(); ++index) {
        const PassPlan& pass = passes[index];
        text += "\n  [" + std::to_string(index) + "] order=" + std::to_string(pass.order);
        text += " name='" + (pass.name.empty() ? std::string("(unnamed)") : pass.name.as_std_str()) + "'";
        text += pass.enabled ? "" : " DISABLED";
        text += " target=" + (pass.target != nullptr ? pointerText(pass.target) : std::string("window"));
        text += " camera=" + pointerText(pass.camera);
        text += " content=" + pointerText(pass.content);
        // Only when it is NOT the default: every line carrying the mask would make the common case noisier, and
        // "which layers" is a fact most passes never state (see .ai/design/graphics-layers.md).
        if (pass.layer_mask != kAllLayers) {
            text += " layers=" + layerMaskText(pass.layer_mask);
        }
        text += " clear=" + clearText(pass);
        text += " viewport=" + viewportText(pass);
        text += " inputs=" + listText(pass.inputs);
        text += " output='" + (pass.output.empty() ? std::string("-") : pass.output.as_std_str()) + "'";
        text += " last=" + std::to_string(pass.record_micros) + "us";
    }
    return String::fromUtf8(text);
}

VN_GRAPHICS_NS_END
