vn_add_library(ROBOTICSIO_TARGET_NAME RoboticsIO)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
if(VINE_USE_FETCHCONTENT)
    # tinyxml2 is baked into the RoboticsIO DLL as a static library. Its
    # pkg-config block breaks under multi-config generators; see the patch.
    FetchContent_Declare(tinyxml2
        GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
        GIT_TAG        10.0.0
        GIT_SHALLOW    TRUE
        PATCH_COMMAND  "${CMAKE_COMMAND}"
                        -DTINYXML2_CMAKE_FILE=${CMAKE_BINARY_DIR}/_deps/tinyxml2-src/CMakeLists.txt
                        -P ${CMAKE_SOURCE_DIR}/cmake/patches/TinyXml2NoPkgConfig.cmake
    )
    FetchContent_MakeAvailable(tinyxml2)
else()
    # Consume tinyxml2 from vcpkg or a local install.
    find_package(tinyxml2 CONFIG REQUIRED)
endif()

target_link_libraries(${ROBOTICSIO_TARGET_NAME} PUBLIC vn::RoboticsCore vn::Geometry vn::IOBase)
target_link_libraries(${ROBOTICSIO_TARGET_NAME} PRIVATE tinyxml2)
