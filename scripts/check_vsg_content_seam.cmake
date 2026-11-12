# 内容层的设备无关半边：事实表、键、半边、来源、清扫这些 TU 不许出现 vsg 类型。
#
# 为什么用 CMake 而不是 scripts/check_*.py：这条规则必须在 `ctest` 里跑（跟构建一起跑，不靠人记得），
# 而这条流水线里 python 解释器不一定在。CMake 脚本是这里唯一保证可用的解释器。
#
# 规则与理由见 .ai/design/vsg-backend.md 2.7。判据是"名字里出现 ::vsg::"——这半边只需要字符串扫描，
# 不需要解析 C++。
#
# 用法：cmake -DSOURCE_ROOT=<repo> -P scripts/check_vsg_content_seam.cmake
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "check_vsg_content_seam: pass -DSOURCE_ROOT=<repository root>")
endif()

set(_guarded
    src/plugins/gfx_backend_vsg/include/vine/vsg/internal/ContentFacts.hpp
    src/plugins/gfx_backend_vsg/include/vine/vsg/internal/ContentHalves.hpp
    src/plugins/gfx_backend_vsg/include/vine/vsg/internal/ContentPush.hpp
    src/plugins/gfx_backend_vsg/include/vine/vsg/internal/ContentSources.hpp
    src/plugins/gfx_backend_vsg/include/vine/vsg/internal/ContentStore.hpp
    src/plugins/gfx_backend_vsg/src/internal/ContentFacts.cpp
    src/plugins/gfx_backend_vsg/src/internal/ContentHalves.cpp
    src/plugins/gfx_backend_vsg/src/internal/ContentPush.cpp
    src/plugins/gfx_backend_vsg/src/internal/ContentSources.cpp
    src/plugins/gfx_backend_vsg/src/internal/ContentStore.cpp)

set(_checked 0)
foreach(_file IN LISTS _guarded)
    set(_path "${SOURCE_ROOT}/${_file}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "check_vsg_content_seam: '${_file}' is gone - update this list with it (a rename must not silence the guard)")
    endif()
    file(READ "${_path}" _text)
    string(FIND "${_text}" "::vsg::" _hit)
    if(NOT _hit EQUAL -1)
        message(FATAL_ERROR
            "check_vsg_content_seam: '${_file}' names a vsg type.\n"
            "The content BOOKKEEPING half must stay device-agnostic so a second backend can reuse it (vsg-backend.md 2.7):\n"
            "move the device work into the adapter that owns it (StreamUploads / StateCommands / ContentPipeline /\n"
            "ContentDraw), or pass a plain value the adapter converts.")
    endif()
    math(EXPR _checked "${_checked} + 1")
endforeach()

message(STATUS "check_vsg_content_seam: ${_checked} file(s) are device-agnostic")
