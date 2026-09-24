#include <vine/appfw/ConfigStandard.hpp>

VN_APPFW_NS_BEGIN

String standardCategoryName(StandardCategory id)
{
    switch (id) {
    case StandardCategory::General:    return String(u8"general");
    case StandardCategory::Appearance: return String(u8"appearance");
    case StandardCategory::Logging:    return String(u8"logging");
    case StandardCategory::Files:      return String(u8"files");
    case StandardCategory::Editor:     return String(u8"editor");
    case StandardCategory::Rendering:  return String(u8"rendering");
    case StandardCategory::Plugins:    return String(u8"plugins");
    }
    return String();
}

String standardCategoryLabel(StandardCategory id)
{
    switch (id) {
    case StandardCategory::General:    return String(u8"通用");
    case StandardCategory::Appearance: return String(u8"外观");
    case StandardCategory::Logging:    return String(u8"日志");
    case StandardCategory::Files:      return String(u8"文件");
    case StandardCategory::Editor:     return String(u8"编辑器");
    case StandardCategory::Rendering:  return String(u8"渲染");
    case StandardCategory::Plugins:    return String(u8"插件");
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
    case StandardGroup::Startup:  return String(u8"startup");
    case StandardGroup::Behavior: return String(u8"behavior");
    case StandardGroup::Theme:    return String(u8"theme");
    case StandardGroup::Language: return String(u8"language");
    case StandardGroup::Console:  return String(u8"console");
    case StandardGroup::File:     return String(u8"file");
    case StandardGroup::Autosave: return String(u8"autosave");
    case StandardGroup::Paths:    return String(u8"paths");
    }
    return String();
}

String standardGroupLabel(StandardGroup id)
{
    switch (id) {
    case StandardGroup::Startup:  return String(u8"启动");
    case StandardGroup::Behavior: return String(u8"行为");
    case StandardGroup::Theme:    return String(u8"主题");
    case StandardGroup::Language: return String(u8"语言");
    case StandardGroup::Console:  return String(u8"控制台");
    case StandardGroup::File:     return String(u8"文件");
    case StandardGroup::Autosave: return String(u8"自动保存");
    case StandardGroup::Paths:    return String(u8"路径");
    }
    return String();
}

VN_APPFW_NS_END
