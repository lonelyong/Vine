vn_add_library(IOBASE_TARGET_NAME IOBase)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
if(VINE_USE_FETCHCONTENT)
    # madler/zlib always defines both a shared and a static target; the patch
    # removes the shared one so only zlibstatic is built and listed.
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG        v1.3.1
        PATCH_COMMAND  "${CMAKE_COMMAND}"
                        -DZLIB_CMAKE_FILE=${CMAKE_BINARY_DIR}/_deps/zlib-src/CMakeLists.txt
                        -P ${CMAKE_SOURCE_DIR}/cmake/patches/ZlibStaticOnly.cmake
    )
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE) # No example executables.
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)     # zlib is baked into IOBase; do not install it.
    FetchContent_MakeAvailable(zlib)

    # libzip's find_package(ZLIB) must resolve to the zlib built above. FindZLIB
    # skips its search when ZLIB_LIBRARY is preset and keeps an existing ZLIB::ZLIB
    # target, so point both at zlibstatic.
    # zlib.h lives in the source dir but its generated zconf.h lives in the build
    # dir; consumers such as assimp do INCLUDE_DIRECTORIES(${ZLIB_INCLUDE_DIR}),
    # so both directories must be on the include path or `#include "zconf.h"`
    # fails to compile.
    set(ZLIB_INCLUDE_DIR "${zlib_BINARY_DIR};${zlib_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(ZLIB_LIBRARY zlibstatic CACHE FILEPATH "" FORCE)
    # Mark ZLIB as found to prevent find_package(ZLIB) from searching for system zlib
    set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()

    # libzip-ng: static zip library only (no tools, tests, fuzzers, examples,
    # documentation or install rules).
    FetchContent_Declare(libzip
        GIT_REPOSITORY https://github.com/nih-at/libzip.git
        GIT_TAG        v1.11.4
    )
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) # Static lib, baked into the DLL.
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)       # No zipcmp/zipmerge/ziptool.
    set(BUILD_REGRESS OFF CACHE BOOL "" FORCE)     # No regression tests.
    set(BUILD_OSSFUZZ OFF CACHE BOOL "" FORCE)     # No fuzzers.
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)    # No examples.
    set(BUILD_DOC OFF CACHE BOOL "" FORCE)         # No documentation.
    set(LIBZIP_DO_INSTALL OFF CACHE BOOL "" FORCE) # No install rules.
    set(ENABLE_BZIP2 OFF CACHE BOOL "" FORCE)      # No extra codecs.
    set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
    set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)    # Windows crypto covers AES.
    set(ENABLE_GNUTLS OFF CACHE BOOL "" FORCE)
    set(ENABLE_COMMONCRYPTO OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libzip)

    # Group every third-party target under the third_party solution folder.
    vn_group_targets_folder(${zlib_SOURCE_DIR} third_party)
    vn_group_targets_folder(${libzip_SOURCE_DIR} third_party)
else()
    # Consume zlib and libzip from vcpkg or a local install.
    find_package(ZLIB REQUIRED)
    find_package(LibZip CONFIG REQUIRED)
    # Normalize to the FetchContent target names used below.
    if(NOT TARGET zlibstatic AND TARGET ZLIB::ZLIB)
        add_library(zlibstatic ALIAS ZLIB::ZLIB)
    endif()
    if(NOT TARGET zip AND TARGET LibZip::zip)
        add_library(zip ALIAS LibZip::zip)
    endif()
endif()

target_link_libraries(${IOBASE_TARGET_NAME} PUBLIC vn::Global vn::Core)
# Static zlib and libzip are baked into the IOBase DLL.
target_link_libraries(${IOBASE_TARGET_NAME} PRIVATE zlibstatic zip)
