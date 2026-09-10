set(TEST_NAME test_vsg)
set(SRC_FILE_LIST
	GfxBackendVsgPluginTest.cpp
	RenderStateMapperTest.cpp
	GlslCompileTest.cpp
	SceneBridgePipelineSharingTest.cpp
	GeometrySafetyTest.cpp
	CustomAttributeTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
# The backend is an appfw plugin (MODULE DLL, cannot be linked). Its headers
# (SceneBridge.hpp etc.) annotate the classes V_VSG_API (dllexport/dllimport on
# Windows). Because SceneBridge.cpp + VsgMaterialManager.cpp are compiled INTO
# this test executable (see target_sources below), the target must build them
# as the exporting side (V_VSG_LIB); otherwise MSVC treats the classes as
# dllimport, the header-only accessors (pipelineVariantCount / variantReuseCount)
# and the vtable are never emitted locally, and the executable can neither
# import (no plugin .lib) nor resolve them -> LNK2019 / C4273. Harmless (empty)
# on Linux.
target_compile_definitions(${TEST_NAME} PRIVATE V_VSG_LIB)
# The backend is an appfw plugin (MODULE, cannot be linked). The plugin-load
# test boots an Application, loads the gfx_backend_vsg plugin through
# PluginManager, then creates the backend by name via RenderBackendRegistry —
# exercising the real plugin load → register → create path.
#
# The RenderStateMapperTest exercises the device-free SDK-state → vsg-state
# mapping, so this target also links vsg and includes the plugin's public
# headers directly.
#
# SceneBridgePipelineSharingTest instantiates SceneBridge itself, so the two
# device-free bridge sources (SceneBridge + its material manager) are compiled
# into the test alongside its public headers.
find_package(vsg CONFIG QUIET)
target_link_libraries(${TEST_NAME} vi::Appfw vi::Graphics)
if(vsg_FOUND)
	target_link_libraries(${TEST_NAME} vsg::vsg)
endif()
target_sources(${TEST_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/SceneBridge.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgMaterialManager.cpp)
target_include_directories(${TEST_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/include
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

# 同上：渲染后端插件会建窗口/交换链，ctest 下一律走 offscreen，
# 避免在 WSLg / 远程桌面里弹窗口被中断后卡死。
set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
