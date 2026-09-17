// Shared library loaded by DynamicLibraryLoaderTest.
//
// The tests used to load "/proc/self/exe", which cannot work on Linux: the test binary is a PIE,
// and glibc refuses to dlopen a position-independent executable ("cannot dynamically load
// position-independent executable"). Loading a system library by name would exercise the dynamic
// linker rather than the loader under test, so this fixture exports exactly one symbol and the
// tests load it by its build-time path.

#if defined(_WIN32)
#    define V_TEST_FIXTURE_API __declspec(dllexport)
#else
#    define V_TEST_FIXTURE_API __attribute__((visibility("default")))
#endif

extern "C" V_TEST_FIXTURE_API int vine_test_fixture_answer();

extern "C" V_TEST_FIXTURE_API int vine_test_fixture_answer()
{
    return 42;
}
