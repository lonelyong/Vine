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
	DrawBlockTest.cpp
	GeometryFactsTest.cpp
	ContentSourcesTest.cpp
	ContentPassTest.cpp
	MrtTargetTest.cpp
	SharedDepthTest.cpp
	VariantCoreTest.cpp
	StreamUploadsTest.cpp
	BlockStorageTest.cpp
	BlockDescriptorsTest.cpp
	ContentPipelineTest.cpp
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
	GfxBackendVsgPluginTest.cpp
	RenderStateMapperTest.cpp
	CameraBridgeTest.cpp
	ContentSlotViewportTest.cpp
	GlslCompileTest.cpp
	SceneBridgePipelineSharingTest.cpp
	SceneBridgeCacheOwnershipTest.cpp
	DrawBlockPoolRetireTest.cpp
	DrawBlockPoolSlotsTest.cpp
	DeferredReleaseTest.cpp
	SceneBridgeDataRebuildTest.cpp
	GeometrySafetyTest.cpp
	CustomAttributeTest.cpp
	PassProtocolTest.cpp
	FrameCommitTest.cpp
	ReportOnceTest.cpp
	MaterialManagerTest.cpp
	LightDropReportTest.cpp
	PassObjectReleaseTest.cpp
	TargetBookkeepingTest.cpp
	GpuProfileTest.cpp
	PassRenderPassPlanTest.cpp
	PassClearValuesTest.cpp
	DeviceRequirementsTest.cpp
	DynamicStateTest.cpp
	ProgramSamplingTest.cpp
	GraphOrderTest.cpp
	DiagnosticsTest.cpp
	ReadbackTest.cpp
	SceneRulesTest.cpp
	TextureCacheTest.cpp
	MeshResourceCacheTest.cpp
	ChannelSliceTest.cpp
	LightsTest.cpp
	OverlayStagesTest.cpp
	ForwardShaderSetTest.cpp
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
# The plugin-load test boots an Application, whose data directory comes from
# QStandardPaths. Qt test mode keeps it under a per-user test root instead of the
# real <org>/<app> data directory; without it the suite's installPlugin() calls
# write registration files into the user's real data directory.
find_package(Qt6 CONFIG REQUIRED COMPONENTS Core)

target_link_libraries(${TEST_NAME} vi::Appfw vi::Graphics vi::Logging Qt6::Core tp::volk)

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
	# The rewritten backend's core layer: device-free by construction (no vsg:: include anywhere -
	# see the design's §2.3 boundary).
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/Protocol.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/FrameArena.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/FrameRecorder.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/FrameGraph.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/FrameCompiler.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/FrameTimeline.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/RetirementQueue.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/TargetPlan.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/ClearPlan.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/Readback.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/DepthProbe.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/Keys.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/Observe.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/PhaseTable.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/Diagnostics.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/PixelProbe.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/AllocationGate.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/DeviceRequirements.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/SlotProbe.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/SessionMove.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/Streams.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/MaterialArena.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/FrameRing.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/VariantPool.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/core/StateRegistry.cpp
	# The API side of the boundary: the only new code that touches the graphics API.
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/DeviceProbe.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/Session.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/StreamUploads.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/BlockStorage.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/BlockDescriptors.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/ContentPipeline.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/ContentDraw.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/StateCommands.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/DeviceFeatures.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/OffscreenTarget.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/VsgExecutor.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/ContentFacts.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/DrawBlock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/LightBlock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/ShadowBlock.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/GeometryFacts.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/ContentSources.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/ContentPass.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/WindowTarget.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/api/ViewBlock.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/SceneBridge.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/SceneBridgeGeometry.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgSceneRules.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/SceneBridgePipeline.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgMaterialManager.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgTextureCache.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgMeshResourceCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgDrawBlockPool.cpp
	# The pass protocol (the beginPass/endPass scope) lives in VsgRenderer, and
	# its validation must be testable WITHOUT a device: constructing the renderer
	# creates no window/viewer, and the scope calls only touch the retained
	# request. CameraBridge comes with it (content slots create one).
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgHostWindow.cpp	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgDiagnostics.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgRecordOrder.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgReadback.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgViewCompiler.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgContentSlot.cpp
	   ${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgTargetBookkeeping.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgPassMaterialiser.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgRetireRing.cpp
	# VsgRenderer is split over several TUs that share one free-function layer;
	# the test compiles the ones a device-free run reaches.
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgRendererPasses.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgProgramSlot.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgLights.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgBackendUtility.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgDynamicState.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgGpuProfile.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgVulkanEntryPoints.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/VsgPipelineFactory.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/src/CameraBridge.cpp)
target_include_directories(${TEST_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugins/gfx_backend_vsg/include)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

# 同上：渲染后端插件会建窗口/交换链，ctest 下一律走 offscreen，
# 避免在 WSLg / 远程桌面里弹窗口被中断后卡死。
set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
