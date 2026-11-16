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
	RenderSurfaceTest.cpp
	ChannelSliceTest.cpp
	EmbeddedShadersTest.cpp
	MaterialInstanceTest.cpp
	ShaderAbiTest.cpp
	LightBlockTest.cpp
	DrawBlockTest.cpp
	MaterialBlockTest.cpp
	ModelSceneTest.cpp
	MeshShapeTest.cpp
	ResidencyTest.cpp
	ContentRegistryTest.cpp
	RenderWorldTest.cpp
	SelectTest.cpp
	SnapshotShadowTest.cpp
	PublicationStoreTest.cpp
	FrameExpandTest.cpp
	FrameRunnerTest.cpp
        ValueChannelTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::Graphics vn::Geom vn::Models)
# The seam's own cases (BackendSeamTest.cpp) read vine/graphics/backend/ directly, so this binary is a backend-half
# consumer for the same reason a backend and its tests are (see BackendHalfGuard.hpp): the opt-in states that it is
# deliberate, and no host code in this binary may take it.
target_compile_definitions(${TEST_NAME} PRIVATE VN_GRAPHICS_BACKEND_CONSUMER)
# The render-world test drives the library's internal draw-list table (src/, not the SDK): that table is where both
# producers of a draw list meet, so the test has to see it.
target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src/viz/graphics/src)
# The embedded-shader test includes the generated SDK shader header; wire the
# rule so the header exists before this target compiles (cmake/VineShaders.cmake).
vn_use_embedded_shaders(${TEST_NAME} OUTPUT vine/graphics/EmbeddedShaders.hpp SOURCES)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
