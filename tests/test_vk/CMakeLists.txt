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
        ConcurrentFrameTest.cpp
        DeferredTest.cpp
        HandlesTest.cpp
        MemoryTest.cpp
        MsaaTest.cpp
        MultiSessionTest.cpp
        OffscreenClearTest.cpp
        PassTimingTest.cpp
        PipelineCacheTest.cpp
        PrepareUploadTest.cpp
        ProbeTest.cpp
        ResidencyTest.cpp
        ResidencyClaimsTest.cpp
        ResidenceWireTest.cpp
        RingTest.cpp
        SessionShapeTest.cpp
        ShadowTest.cpp
        StateMappingTest.cpp
        TargetClaimFrameTest.cpp
        TargetTableTest.cpp
        VariantTest.cpp
        VertexInputTest.cpp
        WindowPresentTest.cpp

        # The SDK backend seam's device-free facts (the frame gate among them): ONE source compiled into every
        # binary that links the seam, not a copy per backend.
        ${CMAKE_CURRENT_SOURCE_DIR}/../BackendSeamTest.cpp

        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/BarrierPlan.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ContentCaches.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ContentImages.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ContentRefusal.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/Device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/DynamicRendering.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/FrameScratch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/MemoryAllocator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/PipelineCacheFile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ProgramVariant.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ReadbackStaging.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ResidencyClaims.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/ShaderCompiler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/TargetObjects.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/TargetTable.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/WindowTarget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/internal/WindowSystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/support/Handles.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/src/shell/BackendFactory.cpp)

target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vk/include)

# THE MODULE'S OWN EXPORT MACRO. This target compiles the plugin's sources instead of linking the MODULE, so it HAS to
# define them the way the module does: `VN_BACKEND_VK_API` is dllexport under `VN_BACKEND_VK_LIB` and dllimport without
# it, and a target that declares dllimport for symbols it compiles itself fails to link on Windows (on Linux the
# attribute is visibility and the omission goes unnoticed). test_vsg states `VN_BACKEND_VSG_LIB` for the same reason.
target_compile_definitions(${TEST_NAME} PRIVATE VN_BACKEND_VK_LIB)

target_link_libraries(${TEST_NAME} PRIVATE vn::Appfw vn::Graphics tp::volk glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)

# THE WINDOW INSTRUMENT. WindowPresentTest reads a presented window back through tests/test_vsg/TestHostWindow.hpp,
# the fixture T12 gave both halves: an XGetImage through xcb on X11 and a PrintWindow compositor read on Win32. The
# Win32 half waits for a composition pass, so it needs dwmapi (the same link test_vsg makes). On Linux the xcb block
# below is what makes the fixture compilable at all; without it the window cases compile out and skip.
if(WIN32)
    target_link_libraries(${TEST_NAME} PRIVATE dwmapi)
endif()

# THE WINDOW TEST creates a real X11 window with xcb when the development package is present (the shared
# TestHostWindow fixture includes <xcb/xcb.h> on that platform). The PLUGIN never links xcb (it resolves
# xcb_connect/xcb_disconnect with dlopen); this is the TEST's own measuring instrument, and a machine without xcb
# simply runs the other cases (the window cases compile out).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(VK_TEST_XCB QUIET IMPORTED_TARGET xcb)
endif()
if(VK_TEST_XCB_FOUND)
    target_link_libraries(${TEST_NAME} PRIVATE PkgConfig::VK_TEST_XCB)
    target_compile_definitions(${TEST_NAME} PRIVATE VN_TEST_HAVE_XCB)
endif()

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

# 暴露边界：SDK 的两个面不许提 Vulkan，插件 shell 只露 RenderBackend，vk 自己的帧路径不许走逃生门，
# 低层单元（池/缓存文件/窗口/读回/Handles）不许 include vine/graphics
# （见 .ai/research/graphics-2026-11-13-vk-exposure-boundary.md 5.1 / 6.1 的 V2、V4 与 5.2 的纪律；
#   E2 见 .ai/design/vk-backend.md C10）。
# 跟着 ctest 跑，所以"下一次有人把 vulkan 头写进 backend/"会在测试里红，而不是在下一次移植时才发现。
# 脚本用仓库相对路径、工作目录设在仓库根：find_package(Python3) 在这台机器上找到的是 Cygwin 的 python，
# 它把 "I:/..." 当作相对路径打不开（同一个原因让 check_translations 在这里也是红的）；--root 仍给绝对路径，
# 由脚本自己翻译成它能用的形式。
add_test(NAME vk_exposure
	COMMAND ${Python3_EXECUTABLE} scripts/check_vk_exposure.py --root ${CMAKE_SOURCE_DIR})
set_tests_properties(vk_exposure PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
