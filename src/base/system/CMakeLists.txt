vn_add_library(SYSTEM_TARGET_NAME System)

target_link_libraries(${SYSTEM_TARGET_NAME} PUBLIC vn::Global vn::Core)

# vn_add_library globs every source under src/, so exactly one platform half of the private seam
# (SystemPlatform.hpp) is built: the other halves are marked HEADER_FILE_ONLY, which keeps them
# visible in the IDE and out of this build - the same arrangement base/crash uses for its
# CrashCaptureWindows/Linux.cpp. SystemUnsupported.cpp answers a platform the module does not
# implement: it is the only half built on such a platform and is never built here.
if(WIN32)
    set(VN_SYSTEM_FOREIGN_IMPLEMENTATIONS src/SystemLinux.cpp src/SystemApple.cpp src/SystemUnsupported.cpp)
elseif(APPLE)
    set(VN_SYSTEM_FOREIGN_IMPLEMENTATIONS src/SystemWindows.cpp src/SystemLinux.cpp src/SystemUnsupported.cpp)
elseif(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android)$")
    set(VN_SYSTEM_FOREIGN_IMPLEMENTATIONS src/SystemWindows.cpp src/SystemApple.cpp src/SystemUnsupported.cpp)
else()
    set(VN_SYSTEM_FOREIGN_IMPLEMENTATIONS src/SystemWindows.cpp src/SystemLinux.cpp src/SystemApple.cpp)
endif()

set_source_files_properties(${VN_SYSTEM_FOREIGN_IMPLEMENTATIONS} PROPERTIES HEADER_FILE_ONLY TRUE)