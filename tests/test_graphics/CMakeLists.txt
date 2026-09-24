set(TEST_NAME test_graphics)
set(SRC_FILE_LIST
	GraphicsTest.cpp
	ChannelSliceTest.cpp
	EmbeddedShadersTest.cpp
	ShaderAbiTest.cpp
)
add_executable(${TEST_NAME} ${SRC_FILE_LIST})
set_target_properties(${TEST_NAME} PROPERTIES FOLDER test)
target_link_libraries(${TEST_NAME} vn::Graphics vn::Geometry)
# The embedded-shader test includes the generated SDK shader header; wire the
# rule so the header exists before this target compiles (cmake/VineShaders.cmake).
vn_use_embedded_shaders(${TEST_NAME} OUTPUT vine/graphics/EmbeddedShaders.hpp SOURCES)
add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
