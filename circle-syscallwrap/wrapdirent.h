//
// wrapdirent.h — struct dirent, extended with d_type.
//
// The C library's own <sys/dirent.h> (FatFs's glue, behind
// CIRCLE_STDLIB_INCLUDES) declares struct dirent with only d_ino and
// d_name. A port whose file-listing code reads d->d_type — a desktop C
// library always carries the field, so code written against one takes it
// for granted — has nowhere for that read to land.
//
// A struct's shape cannot be extended from outside, so this header replaces
// the original outright: it defines _SYS_DIRENT_H itself, ahead of the real
// one, so the real header's own `#ifndef _SYS_DIRENT_H` guard finds the name
// already taken and contributes nothing. That preemption is what matters,
// not where this file sits: reached by resolving first on the include
// search path under the name sys/dirent.h, or by a plain #include of this
// file before the translation unit's first #include <dirent.h>, either way
// leaves _SYS_DIRENT_H defined before the real header is read, and its own
// guard does the rest. syscallwrap.cpp uses the second form.
//
// DIR stays exactly what it was — an opaque handle onto Circle's own
// _CIRCLE_DIR — so opendir/readdir/closedir/rewinddir need no change to
// keep compiling against it.
//
// d_type goes last, and that placement is load-bearing rather than a matter
// of taste. The C library in the archive was built from its own
// <sys/dirent.h> (d_ino then d_name[FF_LFN_BUF + 1], which is 256), and
// syscallwrap.cpp's readdir wrapper has to read entries that library
// returns, so every field shared with it must sit at the same offset.
// Appending is the only way to add one without moving d_name.
//
// A consumer that does not put this header where the real <sys/dirent.h>
// would otherwise be found uses the C library's own two-field struct
// instead, and reads d_ino and d_name at the same offsets either way.
//
#ifndef _SYS_DIRENT_H
#define _SYS_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

// Circle's own FatFs directory handle. Forward-declared, never defined
// here: every use on this side is through a DIR *, and the real struct
// belongs to the shim's own FatFs glue, which nothing here has a reason to
// include just to name a pointer type.
typedef struct _CIRCLE_DIR DIR;

// Standard POSIX values (Linux/BSD numbering; nothing else on this target
// reads them, so only self-consistency with syscallwrap.cpp matters).
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

struct dirent
{
    ino_t         d_ino;
    char          d_name[256];
    unsigned char d_type;
};

DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int readdir_r(DIR *__restrict, struct dirent *__restrict,
              struct dirent **__restrict);
void rewinddir(DIR *);
int closedir(DIR *);

#ifdef __cplusplus
}
#endif

#endif // _SYS_DIRENT_H
