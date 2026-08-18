//
// sys/mman.h — memory mapping, as far as this platform can honour it.
//
// The C library on this target has no version of this header at all, so
// this is the whole of the declaration a consumer sees.
//
// What can be honoured is a mapping that is private to the process: there
// is one program on the board and its memory is its own, so an anonymous
// mapping is an allocation and a MAP_PRIVATE file mapping is that
// allocation with the file's bytes read into it. What cannot be honoured
// is MAP_SHARED, which promises that a write through the mapping reaches
// the file and every other mapping of it. Nothing here can carry that
// promise, so mmap refuses it rather than serving a private copy in its
// place - a caller that writes through a shared mapping and finds the file
// unchanged has no way to discover why.
//
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_FILE        0x0000
#define MAP_SHARED      0x0001
#define MAP_PRIVATE     0x0002
#define MAP_FIXED       0x0010
#define MAP_ANON        0x0020
#define MAP_ANONYMOUS   MAP_ANON

#define MAP_FAILED      ((void *) -1)

#define MS_ASYNC        0x1
#define MS_INVALIDATE   0x2
#define MS_SYNC         0x4

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t length, int prot);
int   msync(void *addr, size_t length, int flags);

#ifdef __cplusplus
}
#endif

#endif // _SYS_MMAN_H
