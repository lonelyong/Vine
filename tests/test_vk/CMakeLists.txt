set(TEST_NAME test_vk)

# The plugin is a MODULE library and cannot be linked, so this target compiles its implementation sources directly -
# the same arrangement tests/test_vsg uses for gfx_backend_vsg. No VulkanSceneGraph is linked: this backend reaches
# Vulkan through volk (tp::volk carries the Vulkan headers).
add_executable(${TEST_NAME}
        BackendTest.cpp
        BarrierPlanTest.cpp
        ContentDrawTest.cpp
        OffscreenClearTest.cpp

        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/BarrierPlan.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/Device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ShaderCompiler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/TargetImages.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/shell/BackendFactory.cpp)

target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/include)

# The device header derives from RenderBackend and lives under internal/, so the consumer opt-in is stated here too,
# exactly as the plugin states it.
target_compile_definitions(${TEST_NAME} PRIVATE VN_GRAPHICS_BACKEND_CONSUMER)

target_link_libraries(${TEST_NAME} PRIVATE vn::Appfw vn::Graphics tp::volk glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
