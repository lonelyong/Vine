# v_embed_shaders.cmake — generator run in script mode (cmake -P) by
# v_declare_embedded_shaders() (see VineShaderHelper.cmake).
#
# Turns .vert/.frag/... GLSL files into one C++ header of string constants, so
# the shaders live as real files (syntax highlighting, diffable, validated) yet
# ship inside the binary (no runtime resource copying, no loose files next to
# the DLL).
#
# Inputs (all passed with -D):
#   OUT       Absolute path of the header to write.
#   SOURCES   Shader files, ','-separated (a ',' is never valid in a path, and a
#             ';'-separated list would be re-split by CMake's list expansion).
#   NAMESPACE C++ namespace for the generated constants (e.g. vine::vsg::shaders).
#
# The header is only rewritten when its content changes, so an unchanged shader
# does not make every includer recompile.

cmake_minimum_required(VERSION 3.21)

if(NOT OUT)
    message(FATAL_ERROR "v_embed_shaders: OUT is required")
endif()
if(NOT SOURCES)
    message(FATAL_ERROR "v_embed_shaders: SOURCES is required")
endif()
if(NOT NAMESPACE)
    message(FATAL_ERROR "v_embed_shaders: NAMESPACE is required")
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
        message(FATAL_ERROR "v_embed_shaders: shader source not found: ${_source}")
    endif()

    get_filename_component(_file_name "${_source}" NAME)
    get_filename_component(_stem "${_source}" NAME_WE)
    get_filename_component(_extension "${_source}" EXT)
    string(TOLOWER "${_extension}" _extension_lower)
    # Drop the leading dot so the extension can index the _suffix_<stage> table.
    string(SUBSTRING "${_extension_lower}" 1 -1 _stage_key)

    if(NOT DEFINED _suffix_${_stage_key})
        message(FATAL_ERROR
            "v_embed_shaders: ${_file_name}: unsupported shader extension '${_extension}' "
            "(use .vert/.frag/.comp/.geom/.tesc/.tese)")
    endif()
    set(_suffix "${_suffix_${_stage_key}}")

    file(READ "${_source}" _content)
    if(NOT _content)
        message(FATAL_ERROR "v_embed_shaders: ${_file_name} is empty")
    endif()

    # Guard 1: file(READ) converts CRLF to LF, and the compiler would do the same
    # again, so a CR would make the embedded text differ from the file on disk.
    # Read the raw bytes (HEX) to see them.
    file(READ "${_source}" _raw_hex HEX)
    string(FIND "${_raw_hex}" "0d" _carriage_return)
    if(NOT _carriage_return EQUAL -1)
        message(FATAL_ERROR
            "v_embed_shaders: ${_file_name} contains CR bytes; shader sources must use LF line endings")
    endif()

    # Guard 2: the delimiter must not occur in the text, else the raw string
    # literal would be terminated early and the header would not compile.
    string(FIND "${_content}" "${_closer}" _closer_at)
    if(NOT _closer_at EQUAL -1)
        message(FATAL_ERROR
            "v_embed_shaders: ${_file_name} contains '${_closer}'; pick another delimiter in v_embed_shaders.cmake")
    endif()
    string(FIND "${_content}" "${_opener}" _opener_at)
    if(NOT _opener_at EQUAL -1)
        message(FATAL_ERROR
            "v_embed_shaders: ${_file_name} contains '${_opener}'; pick another delimiter in v_embed_shaders.cmake")
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
            "v_embed_shaders: ${_file_name} yields the invalid C++ identifier '${_constant}'")
    endif()
    if(_constant IN_LIST _names)
        message(FATAL_ERROR
            "v_embed_shaders: ${_file_name} yields the duplicate constant '${_constant}' (another shader shares its stem)")
    endif()
    list(APPEND _names "${_constant}")
    if(_count GREATER 0)
        string(APPEND _body "\n")
    endif()

    # Hashing by path: `string(<algo> <string> <out>)` returns an empty string on
    # some CMake builds (observed with 4.2.3), while `file(<algo> <path> <out>)` is
    # reliable. The file cannot change between the read and the hash: this script
    # runs as a build step that depends on it.
    file(SHA256 "${_source}" _sha)
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
string(APPEND _text "// Generated by cmake/v_embed_shaders.cmake - DO NOT EDIT.\n")
string(APPEND _text "//\n")
string(APPEND _text "// Every entry is the byte-for-byte content of one shader file:\n")
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
message(STATUS "v_embed_shaders: wrote ${OUT} (${_count} shader(s))")
