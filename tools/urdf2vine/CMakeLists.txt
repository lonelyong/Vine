add_executable(urdf2vine main.cpp)
set_target_properties(urdf2vine PROPERTIES FOLDER tools)
target_link_libraries(urdf2vine PRIVATE vn::RoboticsIO vn::RoboticsCore vn::Geometry vn::IOBase vn::MeshIO tinyxml2)

if(MSVC)
    # Same options the project's own code uses (see VineLibraryHelper.cmake).
    target_compile_options(urdf2vine PRIVATE /utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
endif()
