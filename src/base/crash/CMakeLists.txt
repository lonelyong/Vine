vn_add_library(CRASH_TARGET_NAME Crash)

target_link_libraries(${CRASH_TARGET_NAME} PUBLIC vn::Global vn::Core)

# The library only needs MiniDumpWriteDump; it links no GUI library on purpose - see the dependency boundary in
# .ai/design/base-crash.md §2.1.
if(WIN32)
    target_link_libraries(${CRASH_TARGET_NAME} PRIVATE dbghelp)
    # vn_add_library compiles every source under src/; CrashCaptureLinux.cpp is the Linux half of installPlatform().
    set_source_files_properties(src/CrashCaptureLinux.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
else()
    # vn_add_library compiles every source under src/, and src/CrashCaptureWindows.cpp is the Windows half of
    # installPlatform(): off Windows it can only fail on <windows.h>. HEADER_FILE_ONLY keeps a foreign-platform file visible
    # in the IDE while keeping it out of the build.
    set_source_files_properties(src/CrashCaptureWindows.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

# The visual handler lives in the same directory as the library: it is that library's process boundary (it reads the report
# and the dump). It is not a product, so it does not belong in src/apps - a handler that learned about appfw or a UI toolkit
# would stop being the thing that can still run when those have just died.
#
# Linux draws its window with Xlib and its text with Xft: both are system libraries, so the handler keeps the dependency
# surface of a base-layer process (ADR-2026-10-30-crash-handler-x11-on-linux.md).
set(VN_CRASH_HANDLER_SOURCES)
set(VN_CRASH_HANDLER_LIBRARIES)
set(VN_CRASH_HANDLER_INCLUDES)

if(WIN32)
    set(VN_CRASH_HANDLER_SOURCES
        handler/mainWin.cpp
        handler/CrashWindowWin.cpp
        handler/CrashWindowWin.hpp
        handler/SymbolizeWin.cpp
        handler/Symbolize.hpp
        handler/Text.cpp
        handler/Text.hpp)
    # DbgHelp symbolizes, user32 draws the dialog, shell32 opens the folder and the mail draft, comctl32 is the dialog itself.
    set(VN_CRASH_HANDLER_LIBRARIES dbghelp user32 shell32 comctl32)
elseif(UNIX AND NOT APPLE)
    # Xft's own headers pull in <ft2build.h>, so the freetype include directory is part of "can we build the window".
    find_package(X11)
    find_package(Freetype)
    if(X11_FOUND AND X11_Xft_FOUND AND FREETYPE_FOUND)
        set(VN_CRASH_HANDLER_SOURCES
            handler/mainLinux.cpp
            handler/CrashWindowX11.cpp
            handler/CrashWindowX11.hpp
            handler/SymbolizeLinux.cpp
            handler/Symbolize.hpp
            handler/Text.cpp
            handler/Text.hpp)
        set(VN_CRASH_HANDLER_LIBRARIES ${X11_X11_LIB} ${X11_Xft_LIB})
        set(VN_CRASH_HANDLER_INCLUDES ${X11_INCLUDE_DIR} ${X11_Xft_INCLUDE_PATH} ${FREETYPE_INCLUDE_DIRS})
    else()
        # No X11/Xft development files: there is no window to build. The library still captures, writes the report and the
        # `.txt`; only the dialog is missing, so this is a warning and not a configure failure.
        message(
            WARNING
                "X11/Xft development files not found (install libx11-dev + libxft-dev); CrashHandler will not be built")
    endif()
endif()

if(VN_CRASH_HANDLER_SOURCES)
    add_executable(CrashHandler
        ${VN_CRASH_HANDLER_SOURCES}
        handler/ReportText.cpp
        handler/ReportText.hpp)

    set_target_properties(CrashHandler PROPERTIES FOLDER vine/base)
    target_include_directories(CrashHandler PRIVATE ${VN_CRASH_HANDLER_INCLUDES})

    if(WIN32)
        # Windowed on purpose: the dialog is the handler's whole output.
        set_target_properties(CrashHandler PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()

    # The same UTF-8 source options the libraries get (vn_add_library applies them to its own target only): this module's
    # strings are Chinese, and without them MSVC reads the sources in the machine's code page.
    if(MSVC)
        target_compile_options(CrashHandler PRIVATE /utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
    endif()

    target_link_libraries(CrashHandler PRIVATE vn::Crash ${VN_CRASH_HANDLER_LIBRARIES})

    install(TARGETS CrashHandler RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
