/*
 * alloc_trace.c — an investigation instrument, not a gate.
 *
 * Build and use (see .ai/memory/graphics-perf-backlog.md, item H7):
 *
 *   clang -shared -fPIC -O1 -o /tmp/alloc_trace.so scripts/alloc_trace.c -ldl
 *   LD_PRELOAD=/tmp/alloc_trace.so ./bin/vsg_backend_selftest 2> trace.log
 *
 * It answers three questions that a wall clock cannot:
 *   - how many BYTES a run maps (not how many calls -- a thousand small
 *     mappings and one huge one cost very different amounts of kernel time),
 *   - what the big ones are (size, prot, flags, and the file behind the fd),
 *   - how much code was JIT-compiled (PROT_EXEC at mmap, or added later by
 *     mprotect).
 *
 * Mappings of at least ALLOC_TRACE_MIN bytes are printed with a millisecond
 * timestamp relative to the first intercepted call, so the trace can be lined
 * up against the program's own log timestamps. A SUMMARY line is printed at
 * exit.
 *
 * Measured with it (2026-09-16, lavapipe): two revisions that print byte-for-byte
 * identical logs differ by 39 extra 16 MB device-memory mappings, named
 * `/memfd:allocation fd` -- a string that lives in libvulkan_lvp.so. The extra
 * mappings sit in exactly the windows where the slower revision also spends
 * 0.6 s of extra main-thread CPU, which is what made "the same work costs more"
 * untenable as an explanation.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef ALLOC_TRACE_MIN
#define ALLOC_TRACE_MIN (256 * 1024)
#endif

static struct timespec t0;
static int               have_t0;
static long              mmap_calls;
static long              anon_bytes;
static long              file_bytes;
static long              exec_mmap_calls;
static long              exec_bytes;
static long              exec_mprotect_count;
static long              exec_mprotect_bytes;

static long elapsedMs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!have_t0) {
        t0      = t;
        have_t0 = 1;
    }
    return (t.tv_sec - t0.tv_sec) * 1000L + (t.tv_nsec - t0.tv_nsec) / 1000000L;
}

static void trace(const char* what, long len, int prot, int flags, int fd)
{
    if (len < ALLOC_TRACE_MIN) {
        return;
    }
    char target[160] = "-";
    if (fd >= 0 && (flags & MAP_ANONYMOUS) == 0) {
        char    link[64];
        ssize_t n = snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t got = readlink(link, target, sizeof target - 1);
        target[got > 0 ? (size_t)got : 0] = '\0';
    }
    char line[512];
    int  n = snprintf(line, sizeof line,
                      "[alloc_trace] t=%ldms %s len=%ld prot=0x%x flags=0x%x fd=%d file=%s\n",
                      elapsedMs(), what, len, prot, flags, fd, target);
    write(STDERR_FILENO, line, (size_t)n);
}

static void account(long len, int prot, int flags, int fd)
{
    ++mmap_calls;
    if ((flags & MAP_ANONYMOUS) != 0 || fd < 0) {
        anon_bytes += len;
    }
    else {
        file_bytes += len;
    }
    if ((prot & PROT_EXEC) != 0) {
        ++exec_mmap_calls;
        exec_bytes += len;
    }
    trace("mmap", len, prot, flags, fd);
}

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off)
{
    static void* (*real)(void*, size_t, int, int, int, off_t);
    if (real == NULL) {
        real = dlsym(RTLD_NEXT, "mmap");
    }
    account((long)len, prot, flags, fd);
    return real(addr, len, prot, flags, fd, off);
}

void* mmap64(void* addr, size_t len, int prot, int flags, int fd, off64_t off)
{
    static void* (*real)(void*, size_t, int, int, int, off64_t);
    if (real == NULL) {
        real = dlsym(RTLD_NEXT, "mmap64");
    }
    account((long)len, prot, flags, fd);
    return real(addr, len, prot, flags, fd, off);
}

int mprotect(void* addr, size_t len, int prot)
{
    static int (*real)(void*, size_t, int);
    if (real == NULL) {
        real = dlsym(RTLD_NEXT, "mprotect");
    }
    if ((prot & PROT_EXEC) != 0) {
        ++exec_mprotect_count;
        exec_mprotect_bytes += (long)len;
    }
    return real(addr, len, prot);
}

__attribute__((destructor)) static void alloc_trace_fini(void)
{
    char line[320];
    int  n = snprintf(line, sizeof line,
                      "[alloc_trace] SUMMARY mmap_calls=%ld anon_bytes=%ld file_bytes=%ld "
                      "exec_mmap_calls=%ld exec_bytes=%ld exec_mprotect=%ld exec_mprotect_bytes=%ld\n",
                      mmap_calls, anon_bytes, file_bytes, exec_mmap_calls, exec_bytes,
                      exec_mprotect_count, exec_mprotect_bytes);
    write(STDERR_FILENO, line, (size_t)n);
}
