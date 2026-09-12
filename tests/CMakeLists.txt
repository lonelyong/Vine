include(GoogleTest)
include(FetchContent)

if(VINE_USE_FETCHCONTENT)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(BUILD_GTEST ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE) # Tests are not installed.

    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.17.0
    )
    FetchContent_MakeAvailable(googletest)

    set_target_properties(gtest PROPERTIES FOLDER third_party)
    set_target_properties(gtest_main PROPERTIES FOLDER third_party)
else()
    # Consume googletest from vcpkg or a local install.
    find_package(GTest CONFIG REQUIRED)
    # Normalize to the FetchContent target names used below.
    if(NOT TARGET gtest AND TARGET GTest::gtest)
        add_library(gtest ALIAS GTest::gtest)
        add_library(gtest_main ALIAS GTest::gtest_main)
    endif()
endif()
include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>)
link_libraries(gtest gtest_main)

# MSVC options for the project's own test targets. Placed after the gtest
# FetchContent so the fetched gtest does not inherit them (see v_add_library).
if(MSVC)
    add_compile_options(/utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
endif()

add_subdirectory(test_math)
add_subdirectory(test_core)
add_subdirectory(test_async)
add_subdirectory(test_cppstd)
add_subdirectory(test_imaging)
add_subdirectory(test_graphics)
add_subdirectory(test_vsg)
add_subdirectory(test_robotics_core)
add_subdirectory(test_robotics_io)
add_subdirectory(test_proximity)
add_subdirectory(test_di)
add_subdirectory(test_runtime)
add_subdirectory(test_system)
add_subdirectory(test_window)
add_subdirectory(test_crypto)
add_subdirectory(test_meshio)
add_subdirectory(test_brepio)
add_subdirectory(test_imageio)
add_subdirectory(test_iobase)
add_subdirectory(test_progress)
add_subdirectory(test_gui)
add_subdirectory(test_asyncqt)
add_subdirectory(test_logging)
