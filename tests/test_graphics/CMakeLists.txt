set(TEST_NAME test_graphics)
set(SRC_FILE_LIST
	# The SDK backend seam's device-free facts: ONE source compiled into every binary that links the seam (see
	# tests/BackendSeamTest.cpp for why it is not repeated per backend).
	${CMAKE_CURRENT_SOURCE_DIR}/../BackendSeamTest.cpp
	ChainRegistrationTest.cpp
	FrameScheduleTest.cpp
	GraphicsTest.cpp
	HeadlessBackendTest.cpp
	PlanTest.cpp
	RenderAreaTest.cpp
	ChannelSliceTest.cpp
	EmbeddedShadersTest.cpp
	MaterialInstanceTest.cpp
	ShaderAbiTest.cpp
	LightBlockTest.cpp
	DrawBlockTest.cpp
	MaterialBlockTest.cpp
	SceneBuildTest.cpp
	MeshShapeTest.cpp
	ResidencyTest.cpp
	ContentRegistryTest.cpp
	RenderWorldTest.cpp
	ReportOnceTest.cpp
	SelectTest.cpp
	SnapshotShadowTest.cpp
	PublicationStoreTest.cpp
	FrameExpandTest.cpp
	FrameRunnerTest.cpp
        ValueChannelTest.cpp        # The module guide's first example, compiled: the snippet a host copies must not be able to
        # rot between builds (see the file's header).
        GuideExample.cpp
        # The guide's three hand-built pass examples (3.3 / 3.4 / 3.5), compiled for the same reason.
        GuideExamplePasses.cpp)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::Graphics vn::Geom vn::Models)
# The render-world test drives the library's internal draw-list table (src/, not the SDK): that table is where both
# producers of a draw list meet, so the test has to see it.
target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src/viz/graphics/src)
# The table, the publication and the frame mechanisms live in libGraphics' private half. On Linux the shared object
# exports them, so the cases can link them from there; on Windows a DLL exports only what the SDK declares, so the
# private translation units that answer those calls are compiled INTO this binary - the same treatment test_vsg gives
# the backend's device-free half. Only translation units that define private types belong here: a public symbol would
# have to come from the library instead (see check_export_annotations.py).
target_sources(${TEST_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/DrawListBake.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/DrawListDiff.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/DrawListMemo.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/FrameExpand.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/FrameRunner.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/PublicationStore.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/ResidencyTable.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/ScenePublisher.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/SceneRows.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/SceneSnapshot.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/viz/graphics/src/SnapshotSource.cpp)
# The embedded-shader test includes the generated SDK shader header; wire the
# rule so the header exists before this target compiles (cmake/VineShaders.cmake).
vn_use_embedded_shaders(${TEST_NAME} OUTPUT vine/graphics/EmbeddedShaders.hpp SOURCES)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
