set(TEST_NAME test_vk)

# The plugin is a MODULE library and cannot be linked, so this target compiles its implementation sources directly -
# the same arrangement tests/test_vsg uses for gfx_backend_vsg. No VulkanSceneGraph is linked: this backend reaches
# Vulkan through volk (tp::volk carries the Vulkan headers).
add_executable(${TEST_NAME}
        BackendTest.cpp
        BarrierPlanTest.cpp
        BuiltinProgramTest.cpp
        ContentDrawTest.cpp
        ContentTargetTest.cpp
        DeferredTest.cpp
        MsaaTest.cpp
        OffscreenClearTest.cpp
        ProbeTest.cpp
        ResidencyTest.cpp
        ShadowTest.cpp
        StateMappingTest.cpp
        VariantTest.cpp
        WindowPresentTest.cpp

        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/BarrierPlan.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ContentImages.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/Device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/LightBlock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ProbeWindow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ProgramVariant.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ShaderCompiler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/TargetImages.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/WindowTarget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/XcbSurface.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/shell/BackendFactory.cpp)

target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/include)

# The device header derives from RenderBackend and lives under internal/, so the consumer opt-in is stated here too,
# exactly as the plugin states it.
target_compile_definitions(${TEST_NAME} PRIVATE VN_GRAPHICS_BACKEND_CONSUMER)

# THE MODULE'S OWN EXPORT MACRO. This target compiles the plugin's sources instead of linking the MODULE, so it HAS to
# define them the way the module does: `VN_BACKEND_VK_API` is dllexport under `VN_BACKEND_VK_LIB` and dllimport without
# it, and a target that declares dllimport for symbols it compiles itself fails to link on Windows (on Linux the
# attribute is visibility and the omission goes unnoticed). test_vsg states `VN_BACKEND_VSG_LIB` for the same reason.
target_compile_definitions(${TEST_NAME} PRIVATE VN_BACKEND_VK_LIB)

target_link_libraries(${TEST_NAME} PRIVATE vn::Appfw vn::Graphics tp::volk glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)

# THE WINDOW TEST creates a real X11 window with xcb when the development package is present. The PLUGIN never links
# xcb (it resolves xcb_connect/xcb_disconnect with dlopen); this is the TEST's own measuring instrument, and a machine
# without xcb simply runs the other cases (the test is compiled out).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(VK_TEST_XCB QUIET IMPORTED_TARGET xcb)
endif()
if(VK_TEST_XCB_FOUND)
    target_link_libraries(${TEST_NAME} PRIVATE PkgConfig::VK_TEST_XCB)
    target_compile_definitions(${TEST_NAME} PRIVATE VN_TEST_HAVE_XCB)
endif()

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
