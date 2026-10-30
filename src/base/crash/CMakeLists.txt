vn_add_library(CRASH_TARGET_NAME Crash)

target_link_libraries(${CRASH_TARGET_NAME} PUBLIC vn::Global vn::Core)

# The library only needs MiniDumpWriteDump; it links no GUI library on purpose - see the dependency boundary in
# .ai/research/base-crash-2026-10-30-crash-handler-module.md §6.1.
if(WIN32)
    target_link_libraries(${CRASH_TARGET_NAME} PRIVATE dbghelp)
endif()

# The visual handler lives in the same directory as the library: it is that library's process boundary (it reads the report
# and the dump). It is not a product, so it does not belong in src/apps - a handler that learned about appfw or Qt would stop
# being the thing that can still run when those have just died.
if(WIN32)
    add_executable(vnCrashHandler
        handler/main.cpp
        handler/CrashWindow.cpp
        handler/CrashWindow.hpp
        handler/Symbolize.cpp
        handler/Symbolize.hpp
        handler/Text.cpp
        handler/Text.hpp)

    # Windowed on purpose: the dialog is the handler's whole output.
    set_target_properties(vnCrashHandler PROPERTIES WIN32_EXECUTABLE TRUE FOLDER vine/base)

    # The same UTF-8 source options the libraries get (vn_add_library applies them to its own target only): this module's
    # strings are Chinese, and without them MSVC reads the sources in the machine's code page.
    if(MSVC)
        target_compile_options(vnCrashHandler PRIVATE /utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
    endif()

    # DbgHelp symbolizes, user32 draws the dialog, shell32 opens the folder and the mail draft, comctl32 is the dialog itself.
    target_link_libraries(vnCrashHandler PRIVATE vn::Crash)
    target_link_libraries(vnCrashHandler PRIVATE dbghelp user32 shell32 comctl32)

    install(TARGETS vnCrashHandler RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
