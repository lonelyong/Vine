#include <vine/appfw/ConfigStandard.hpp>

#include <vine/appfw/Translation.hpp>

VN_APPFW_NS_BEGIN

String standardCategoryName(StandardCategory id)
{
    switch (id) {
    case StandardCategory::General: return String(u8"general");
    case StandardCategory::Appearance: return String(u8"appearance");
    case StandardCategory::Logging: return String(u8"logging");
    case StandardCategory::Files: return String(u8"files");
    case StandardCategory::Editor: return String(u8"editor");
    case StandardCategory::Rendering: return String(u8"rendering");
    case StandardCategory::Plugins: return String(u8"plugins");
    }
    return String();
}

String standardCategoryLabel(StandardCategory id)
{
    // Display text, resolved in the language installed when this is called; standardCategoryName() is the identity.
    // The context is a literal on purpose: lupdate only extracts a translate() call it can read (P9).
    switch (id) {
    case StandardCategory::General: return translate("appfw", "General");
    case StandardCategory::Appearance: return translate("appfw", "Appearance");
    case StandardCategory::Logging: return translate("appfw", "Logging");
    case StandardCategory::Files: return translate("appfw", "File");
    case StandardCategory::Editor: return translate("appfw", "Editor");
    case StandardCategory::Rendering: return translate("appfw", "Rendering");
    case StandardCategory::Plugins: return translate("appfw", "Plugins");
    }
    return String();
}

int standardCategoryOrder(StandardCategory id)
{
    return static_cast<int>(id);
}

String standardGroupName(StandardGroup id)
{
    switch (id) {
    case StandardGroup::Startup: return String(u8"startup");
    case StandardGroup::Behavior: return String(u8"behavior");
    case StandardGroup::Theme: return String(u8"theme");
    case StandardGroup::Language: return String(u8"language");
    case StandardGroup::Console: return String(u8"console");
    case StandardGroup::File: return String(u8"file");
    case StandardGroup::Autosave: return String(u8"autosave");
    case StandardGroup::Paths: return String(u8"paths");
    }
    return String();
}

String standardGroupLabel(StandardGroup id)
{
    switch (id) {
    case StandardGroup::Startup: return translate("appfw", "Startup");
    case StandardGroup::Behavior: return translate("appfw", "Behavior");
    case StandardGroup::Theme: return translate("appfw", "Theme");
    case StandardGroup::Language: return translate("appfw", "Language");
    case StandardGroup::Console: return translate("appfw", "Console");
    case StandardGroup::File: return translate("appfw", "File");
    case StandardGroup::Autosave: return translate("appfw", "Autosave");
    case StandardGroup::Paths: return translate("appfw", "Paths");
    }
    return String();
}

VN_APPFW_NS_END
