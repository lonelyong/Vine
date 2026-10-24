# VineEmbedShaders.cmake — generator run in script mode (cmake -P) by
# vn_declare_embedded_shaders() (see VineShaderHelper.cmake).
#
# Turns .vert/.frag/... GLSL files into one C++ header of string constants, so
# the shaders live as real files (syntax highlighting, diffable, validated) yet
# ship inside the binary (no runtime resource copying, no loose files next to
# the DLL).
#
# A shader may name one of the SDK's shared GLSL fragments with
# `#include "vine_params.glsl"` (see that file). The line is REPLACED by the
# fragment's text here, so what ships is one self-contained source: no include
# resolution, no file on disk and no compiler extension at run time. The
# fragments are declared with INCLUDES in the inventory (cmake/VineShaders.cmake)
# so the generation rule depends on them - a custom command can only watch files
# known at configure time, and a fragment edit that left every embedded copy
# stale would be invisible.
#
# Inputs (all passed with -D):
#   OUT       Absolute path of the header to write.
#   SOURCES   Shader files, ','-separated (a ',' is never valid in a path, and a
#             ';'-separated list would be re-split by CMake's list expansion).
#   NAMESPACE C++ namespace for the generated constants (e.g. vn::vsg::shaders).
#
# Mode used by the offline gate (scripts/vine_shader_check.sh), which must compile
# and hash exactly the text the build embeds:
#   VINE_EXPAND_ONLY=ON  Expand the single SOURCE's includes into OUT and return.
#
# The header is only rewritten when its content changes, so an unchanged shader
# does not make every includer recompile.

cmake_minimum_required(VERSION 3.21)

if(NOT OUT)
    message(FATAL_ERROR "VineEmbedShaders: OUT is required")
endif()
if(NOT SOURCES)
    message(FATAL_ERROR "VineEmbedShaders: SOURCES is required")
endif()

# ---- #include expansion ------------------------------------------------------
#
# Replaces every `#include "name"` LINE of @p content_var with the text of the file of that name sitting
# next to @p source_dir. One implementation serves both the build and the offline gate, so the two cannot
# disagree about what a program's text is. A fragment may include another; the nesting bound turns a cycle
# into a configure-time error instead of a hang.
#
# @param content_var Variable holding the text to expand.
# @param source_dir  Directory the include names are resolved against (the including file's).
# @param depth       Current nesting depth (0 for a shader source).
# @param out_var     Variable that receives the expanded text (set in the caller's scope).
function(vine_expand_includes content_var source_dir depth out_var)
    set(_content "${${content_var}}")
    if(depth GREATER 8)
        message(FATAL_ERROR
            "VineEmbedShaders: #include nesting is deeper than 8 next to ${source_dir} (a cycle?)")
    endif()
    while(TRUE)
        # The directive must START a line (the leading newline is part of the match and put back with the
        # fragment's text): `vine_params.glsl` documents its own use with an `#include "vine_params.glsl"`
        # line inside a comment, and an unanchored match would make the fragment include itself.
        string(REGEX MATCH "\n#include[ \t]+\"([^\"]+)\"\n" _include_line "${_content}")
        if(NOT _include_line)
            break()
        endif()
        string(REGEX REPLACE "\n#include[ \t]+\"([^\"]+)\"\n" "\\1" _fragment_name "${_include_line}")
        set(_fragment "${source_dir}/${_fragment_name}")
        if(NOT EXISTS "${_fragment}")
            message(FATAL_ERROR
                "VineEmbedShaders: #include \"${_fragment_name}\" does not resolve next to the including source "
                "(${source_dir}); fragments live beside the shaders that include them")
        endif()
        # The CR guard applies to an included file too: file(READ) normalises CRLF, so only the raw bytes can
        # tell - the same guard, for the same reason, as the one on a shader source below.
        file(READ "${_fragment}" _fragment_hex HEX)
        string(FIND "${_fragment_hex}" "0d" _fragment_carriage_return)
        if(NOT _fragment_carriage_return EQUAL -1)
            message(FATAL_ERROR
                "VineEmbedShaders: ${_fragment_name} contains CR bytes; shader sources must use LF line endings")
        endif()
        file(READ "${_fragment}" _fragment_text)
        math(EXPR _nested_depth "${depth} + 1")
        vine_expand_includes(_fragment_text "${source_dir}" "${_nested_depth}" _fragment_text)
        string(REPLACE "${_include_line}" "\n${_fragment_text}" _content "${_content}")
    endwhile()
    set(${out_var} "${_content}" PARENT_SCOPE)
endfunction()

# Expand-only mode: one source in, its expanded text out, nothing else touched.
if(VINE_EXPAND_ONLY)
    string(REPLACE "," ";" _expand_sources "${SOURCES}")
    list(LENGTH _expand_sources _expand_count)
    if(NOT _expand_count EQUAL 1)
        message(FATAL_ERROR "VineEmbedShaders: VINE_EXPAND_ONLY takes exactly one source, got ${SOURCES}")
    endif()
    set(_expand_source "${_expand_sources}")
    get_filename_component(_expand_dir "${_expand_source}" DIRECTORY)
    file(READ "${_expand_source}" _expand_text)
    vine_expand_includes(_expand_text "${_expand_dir}" 0 _expand_text)
    file(WRITE "${OUT}" "${_expand_text}")
    return()
endif()

if(NOT NAMESPACE)
    message(FATAL_ERROR "VineEmbedShaders: NAMESPACE is required")
endif()

# The generated text is wrapped in a raw string literal with this delimiter.
set(_delimiter "VINE_GLSL")
set(_opener "R\"${_delimiter}(")
set(_closer ")${_delimiter}\"")

# Stage suffix per file extension. .glsl files need an explicit stage when
# validating, so every embedded shader names its stage in its extension.
set(_suffix_vert "Vert")
set(_suffix_frag "Frag")
set(_suffix_comp "Comp")
set(_suffix_geom "Geom")
set(_suffix_tesc "Tesc")
set(_suffix_tese "Tese")

set(_names "")
set(_body "")
string(REPLACE "," ";" _sources "${SOURCES}")
set(_count 0)

foreach(_source IN LISTS _sources)
    if(NOT EXISTS "${_source}")
        message(FATAL_ERROR "VineEmbedShaders: shader source not found: ${_source}")
    endif()

    get_filename_component(_file_name "${_source}" NAME)
    get_filename_component(_stem "${_source}" NAME_WE)
    get_filename_component(_extension "${_source}" EXT)
    string(TOLOWER "${_extension}" _extension_lower)
    # Drop the leading dot so the extension can index the _suffix_<stage> table.
    string(SUBSTRING "${_extension_lower}" 1 -1 _stage_key)

    if(NOT DEFINED _suffix_${_stage_key})
        message(FATAL_ERROR
            "VineEmbedShaders: ${_file_name}: unsupported shader extension '${_extension}' "
            "(use .vert/.frag/.comp/.geom/.tesc/.tese)")
    endif()
    set(_suffix "${_suffix_${_stage_key}}")

    file(READ "${_source}" _content)
    if(NOT _content)
        message(FATAL_ERROR "VineEmbedShaders: ${_file_name} is empty")
    endif()
    # The fragment lines are replaced BEFORE the guards and the hash below: what is embedded - and what a
    # backend compiles - is the expanded text, so its size and hash are the expanded text's.
    get_filename_component(_source_dir "${_source}" DIRECTORY)
    vine_expand_includes(_content "${_source_dir}" 0 _content)

    # Guard 1: file(READ) converts CRLF to LF, and the compiler would do the same
    # again, so a CR would make the embedded text differ from the file on disk.
    # Read the raw bytes (HEX) to see them.
    file(READ "${_source}" _raw_hex HEX)
    string(FIND "${_raw_hex}" "0d" _carriage_return)
    if(NOT _carriage_return EQUAL -1)
        message(FATAL_ERROR
            "VineEmbedShaders: ${_file_name} contains CR bytes; shader sources must use LF line endings")
    endif()

    # Guard 2: the delimiter must not occur in the text, else the raw string
    # literal would be terminated early and the header would not compile.
    string(FIND "${_content}" "${_closer}" _closer_at)
    if(NOT _closer_at EQUAL -1)
        message(FATAL_ERROR
            "VineEmbedShaders: ${_file_name} contains '${_closer}'; pick another delimiter in VineEmbedShaders.cmake")
    endif()
    string(FIND "${_content}" "${_opener}" _opener_at)
    if(NOT _opener_at EQUAL -1)
        message(FATAL_ERROR
            "VineEmbedShaders: ${_file_name} contains '${_opener}'; pick another delimiter in VineEmbedShaders.cmake")
    endif()

    # Constant name: builtin_forward.vert -> kBuiltinForwardVert.
    string(REPLACE "_" ";" _words "${_stem}")
    set(_camel "")
    foreach(_word IN LISTS _words)
        string(SUBSTRING "${_word}" 0 1 _first)
        string(TOUPPER "${_first}" _first_upper)
        string(SUBSTRING "${_word}" 1 -1 _rest)
        string(APPEND _camel "${_first_upper}${_rest}")
    endforeach()
    set(_constant "k${_camel}${_suffix}")
    if(NOT _constant MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
            "VineEmbedShaders: ${_file_name} yields the invalid C++ identifier '${_constant}'")
    endif()
    if(_constant IN_LIST _names)
        message(FATAL_ERROR
            "VineEmbedShaders: ${_file_name} yields the duplicate constant '${_constant}' (another shader shares its stem)")
    endif()
    list(APPEND _names "${_constant}")
    if(_count GREATER 0)
        string(APPEND _body "\n")
    endif()

    # Hashing by path: `string(<algo> <string> <out>)` returns an empty string on
    # some CMake builds (observed with 4.2.3), while `file(<algo> <path> <out>)` is
    # reliable. The expanded text is hashed through a scratch file next to OUT, then
    # removed; the file cannot change between the write and the hash: this script runs
    # as one build step.
    set(_hash_input "${OUT}.hash")
    file(WRITE "${_hash_input}" "${_content}")
    file(SHA256 "${_hash_input}" _sha)
    file(REMOVE "${_hash_input}")
    string(SUBSTRING "${_sha}" 0 16 _hash)
    string(LENGTH "${_content}" _bytes)

    # The source is appended as a variable (never as an inline CMake literal), so
    # quotes/backslashes in it cannot be re-parsed by CMake.
    string(APPEND _body "/** @brief Embedded from \"${_file_name}\". */\n")
    string(APPEND _body "inline constexpr std::u8string_view ${_constant} = u8${_opener}")
    string(APPEND _body "${_content}")
    string(APPEND _body "${_closer};\n")

    string(APPEND _entries "    { \"${_file_name}\", ${_constant}, \"${_hash}\", ${_bytes} },\n")
    math(EXPR _count "${_count} + 1")
endforeach()

set(_text "")
string(APPEND _text "// Generated by cmake/VineEmbedShaders.cmake - DO NOT EDIT.\n")
string(APPEND _text "//\n")
string(APPEND _text "// Every entry is one shader file with its `#include` fragments expanded\n")
string(APPEND _text "// (see vine_params.glsl): the text a backend compiles, byte for byte.\n")
string(APPEND _text "//\n")
string(APPEND _text "// The hash and byte count are the EXPANDED text's.\n")
string(APPEND _text "//\n")
foreach(_source IN LISTS _sources)
    get_filename_component(_file_name "${_source}" NAME)
    string(APPEND _text "//   ${_file_name}\n")
endforeach()
string(APPEND _text "\n")
string(APPEND _text "#pragma once\n")
string(APPEND _text "\n")
string(APPEND _text "#include <cstddef>\n")
string(APPEND _text "#include <iterator>\n")
string(APPEND _text "#include <string_view>\n")
string(APPEND _text "\n")
string(APPEND _text "namespace ${NAMESPACE}\n")
string(APPEND _text "{\n")
string(APPEND _text "\n")
string(APPEND _text "/**\n")
string(APPEND _text " * @brief One embedded shader stage.\n")
string(APPEND _text " */\n")
string(APPEND _text "struct Entry\n")
string(APPEND _text "{\n")
string(APPEND _text "    std::string_view name;  ///< Source file name, e.g. \"screen_texture.frag\".\n")
string(APPEND _text "    std::u8string_view source;  ///< GLSL source, byte-for-byte the file content.\n")
string(APPEND _text "    std::string_view hash;  ///< First 16 hex digits of the source's SHA-256.\n")
string(APPEND _text "    std::size_t bytes{};  ///< Length of @ref source in bytes.\n")
string(APPEND _text "};\n")
string(APPEND _text "\n")
string(APPEND _text "${_body}")
string(APPEND _text "\n")
string(APPEND _text "/** @brief Every embedded shader stage, in declaration order. */\n")
string(APPEND _text "inline constexpr Entry kAll[] = {\n")
string(APPEND _text "${_entries}")
string(APPEND _text "};\n")
string(APPEND _text "\n")
string(APPEND _text "/** @brief Number of entries in @ref kAll. */\n")
string(APPEND _text "inline constexpr std::size_t kCount = std::size(kAll);\n")
string(APPEND _text "\n")
string(APPEND _text "}  // namespace ${NAMESPACE}\n")

if(EXISTS "${OUT}")
    file(READ "${OUT}" _previous)
    if(_previous STREQUAL _text)
        return()
    endif()
endif()
get_filename_component(_out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")
file(WRITE "${OUT}" "${_text}")
message(STATUS "VineEmbedShaders: wrote ${OUT} (${_count} shader(s))")
