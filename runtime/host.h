/* What differs between the hosts we build for: Linux (and WSL) and native
 * Windows (cross-compiled from WSL with clang --target=x86_64-w64-mingw32,
 * for the Vulkan renderer). Everything else in runtime/, hle/ and gpu/ is
 * plain C11 + POSIX threads, which both provide (winpthreads on Windows). */
#ifndef RECOMP_HOST_H
#define RECOMP_HOST_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>

/* Files are binary: Windows would otherwise translate line endings. */
#define HOST_O_BINARY O_BINARY

/* The 4 GiB guest address space, committed up front (Windows only takes
   physical pages when they are touched). */
static inline void *host_reserve(uint64_t size)
{
    return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
static inline void host_release(void *p, uint64_t size) { (void)size; VirtualFree(p, 0, MEM_RELEASE); }

/* A read-only view of a whole file (a RomFS is often over a gigabyte). */
static inline void *host_map_file(int fd, uint64_t size)
{
    HANDLE m = CreateFileMappingA((HANDLE)_get_osfhandle(fd), NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m)
        return NULL;
    void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, (SIZE_T)size);
    CloseHandle(m);
    return p;
}
static inline void host_unmap_file(void *p, uint64_t size) { (void)size; UnmapViewOfFile(p); }

/* Positioned reads and writes: a seek, then the transfer. Safe here: only
   one guest thread runs at a time, so no one else moves the file position. */
static inline long long pread(int fd, void *buf, size_t n, long long offset)
{
    if (_lseeki64(fd, offset, SEEK_SET) < 0)
        return -1;
    return _read(fd, buf, (unsigned)n);
}
static inline long long pwrite(int fd, const void *buf, size_t n, long long offset)
{
    if (_lseeki64(fd, offset, SEEK_SET) < 0)
        return -1;
    return _write(fd, buf, (unsigned)n);
}

static inline int host_mkdir(const char *path) { return _mkdir(path); }
static inline int host_truncate(int fd, uint64_t size) { return _chsize_s(fd, (__int64)size) == 0 ? 0 : -1; }

/* Rename, replacing `to` if it exists (Windows' rename() refuses). */
static inline int host_replace(const char *from, const char *to)
{
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

static inline int host_cpus(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#else
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOST_O_BINARY 0

static inline void *host_reserve(uint64_t size)
{
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static inline void host_release(void *p, uint64_t size) { munmap(p, (size_t)size); }

static inline void *host_map_file(int fd, uint64_t size)
{
    void *p = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    return p == MAP_FAILED ? NULL : p;
}
static inline void host_unmap_file(void *p, uint64_t size) { munmap(p, (size_t)size); }

static inline int host_mkdir(const char *path) { return mkdir(path, 0755); }
static inline int host_truncate(int fd, uint64_t size) { return ftruncate(fd, (off_t)size); }
static inline int host_replace(const char *from, const char *to) { return rename(from, to); }
static inline int host_cpus(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }
#endif

#endif
