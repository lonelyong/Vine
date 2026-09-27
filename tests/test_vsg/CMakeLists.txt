set(TEST_NAME test_vsg)
set(SRC_FILE_LIST
	# The rewritten backend's device-free core (see .ai/design/vsg-reimplementation.md): the protocol
	# state machine, the plan/timeline types, the key audit and the evidence fixtures. No device, no
	# vsg types.
	BackendCoreTest.cpp
	BackendEvidenceTest.cpp
	ContentCoreTest.cpp
	ClearPlanTest.cpp
	FrameRecorderTest.cpp
	FrameGraphTest.cpp
	FrameCompilerTest.cpp
	ExecutorTest.cpp
	ContentFactsTest.cpp
	ContentFactsIndexTest.cpp
	DrawBlockTest.cpp
	GeometryFactsTest.cpp
	ContentSourcesTest.cpp
	ProgramAbiTest.cpp
	ContentPassTest.cpp
	MrtTargetTest.cpp
	SharedDepthTest.cpp
	VariantCoreTest.cpp
	StreamUploadsTest.cpp
	BlockStorageTest.cpp
	BlockDescriptorsTest.cpp
	ContentPipelineTest.cpp
	ContentPushTest.cpp
	WhiteImageTest.cpp
	ContentImagesTest.cpp
	MaterialImagesTest.cpp
	ProgramVariantTest.cpp
	ContentStoreTest.cpp
	ContentSweepTest.cpp
	ExecutorSubmissionTest.cpp
	ContentHalvesTest.cpp
	ContentDrawTest.cpp
	DevicePhaseTest.cpp
	OffscreenTargetTest.cpp
	ContentStackTest.cpp
	SampledInputTest.cpp
	ScreenDrawTest.cpp
	WindowCompositionTest.cpp
	ViewBlockTest.cpp
	LightBlockTest.cpp
	ShadowBlockTest.cpp
	SessionContentTest.cpp
	DeviceProbeTest.cpp
	SessionTest.cpp
	SessionMoveTest.cpp
	VsgBackendTest.cpp
	HostWindowReadTest.cpp
	PassRegistryTest.cpp
	HostTargetsTest.cpp
	GfxBackendVsgPluginTest.cpp
	RenderStateMapperTest.cpp
	GlslCompileTest.cpp
	ReportOnceTest.cpp
	DeviceRequirementsTest.cpp
	SceneRulesTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
# The backend is an appfw plugin (MODULE DLL, cannot be linked). Its headers
# (VsgHostWindow.hpp etc.) annotate the classes VN_VSG_API (dllexport/dllimport on
# Windows). Because VsgHostWindow.cpp + VsgBackendUtility.cpp are compiled INTO
# this test executable (see target_sources below), the target must build them
# as the exporting side (VN_VSG_LIB); otherwise MSVC treats the classes as
# dllimport, their header-only accessors and vtables are never emitted locally,
# and the executable can neither import (no plugin .lib) nor resolve them ->
# LNK2019 / C4273. Harmless (empty) on Linux.
target_compile_definitions(${TEST_NAME} PRIVATE VN_VSG_LIB)
# The backend is an appfw plugin (MODULE, cannot be linked). The plugin-load
# test boots an Application, loads the gfx_backend_vsg plugin through
# PluginManager, then creates the backend by name via RenderBackendRegistry —
# exercising the real plugin load → register → create path.
#
# The RenderStateMapperTest exercises the device-free SDK-state → vsg-state
# mapping, so this target also links vsg and includes the plugin's public
# headers directly.
#
# The host-window and backend-utility sources are shared with the rewritten
# backend, so they are compiled into the test alongside its public headers.
find_package(vsg CONFIG QUIET)
# The plugin-load test boots an Application, whose data directory comes from
# QStandardPaths. Qt test mode keeps it under a per-user test root instead of the
# real <org>/<app> data directory; without it the suite's installPlugin() calls
# write registration files into the user's real data directory.
find_package(Qt6 CONFIG REQUIRED COMPONENTS Core)

target_link_libraries(${TEST_NAME} vn::Appfw vn::Graphics vn::Logging Qt6::Core tp::volk)

# VsgHostWindow creates the host surface itself; vsg builds the same code but links xcb PRIVATE, so its
# include directories do not reach this target.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    if(NOT TARGET PkgConfig::xcb)
        pkg_check_modules(xcb REQUIRED IMPORTED_TARGET xcb)
    endif()
    target_link_libraries(${TEST_NAME} PkgConfig::xcb)
endif()
if(vsg_FOUND)
	target_link_libraries(${TEST_NAME} vsg::vsg)
endif()
target_sources(${TEST_NAME} PRIVATE
	# The harness' own support: this binary replaces the global allocation functions so
	# AllocationGate's COUNTING half has an instrumenter (see the file's note). It must be part of this target
	# and not of the library: replacing the allocator is the host's choice to make, not the backend's.
	${CMAKE_CURRENT_SOURCE_DIR}/AllocationCounter.cpp
	# The backend half (frame description -> compiled frame -> executor) is compiled into
	# libGraphics now (sdk/vine/graphics/backend): device-free by construction - no vsg::
	# include anywhere - which is the design's §2.3 boundary.
	# The API side of the boundary: the only new code that touches the graphics API.
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/DeviceProbe.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/Session.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/PassRegistry.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/HostTargets.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/HostReadback.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/StreamUploads.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/BlockStorage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/BlockDescriptors.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentPipeline.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentDraw.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/StateCommands.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/DeviceFeatures.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/OffscreenTarget.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/VsgExecutor.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentFacts.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/DrawBlock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/LightBlock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ShadowBlock.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/GeometryFacts.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentSources.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ProgramAbi.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentPush.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/WhiteImage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/MaterialImages.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ProgramVariant.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentStore.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentSweep.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentSets.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentAssembly.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentHalves.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentImages.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ContentPass.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/WindowTarget.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/internal/ViewBlock.cpp
	# The flat translation units the rewritten backend still shares: the scene rules its pipeline layer
	# reads, and the host window the session creates.
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/support/VsgSceneRules.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/support/VsgHostWindow.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/support/VsgBackendUtility.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/support/VsgDynamicState.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/support/VsgVulkanEntryPoints.cpp)
target_include_directories(${TEST_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/include)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

# 同上：渲染后端插件会建窗口/交换链，ctest 下一律走 offscreen，
# 避免在 WSLg / 远程桌面里弹窗口被中断后卡死。
set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
