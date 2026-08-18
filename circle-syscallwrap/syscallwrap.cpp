//
// syscallwrap.cpp — file syscalls routed to core 0.
//
// Only core 0 may touch a device. A port that runs its own code on the
// application core and opens files with plain C — fopen, opendir, and the
// rest — needs newlib's file syscalls redirected onto circle-libsdl2's I/O
// service (SDL2Circle_IO*, SDL2/SDL_circle.h) instead, which is valid from
// any core. See docs/CORE-SPLIT.md, "What the application needs".
//
// Redirection is by --wrap, added to LDFLAGS by syscallwrap.mk from the
// same symbol list this file implements. Every wrapper calls __real_* on
// core 0 and the I/O service everywhere else; the I/O service itself
// reaches the card through those same __real_* calls once it has reached
// core 0, which is why every wrapper needs its own core-0 branch — without
// one, the service's own call back into the C library would land in the
// wrapper again instead of reaching the card.
//
// Descriptors 0, 1 and 2 are the console rather than files, and have no
// route through the I/O service. They need none: the library binds them
// itself and each half already crosses cores on its own, so a wrapper hands
// them to __real_* and is done. The two exceptions are named where they
// occur, below. See docs/CORE-SPLIT.md, "What the application needs", and
// docs/LOGGING.md.
//
// The service names an offset on every read and write; the C library
// expects a file to remember its position. The position is held here, one
// slot per open descriptor.
//
#include "wrapdirent.h"
#include "syscallwrap.h"

#include <SDL2/SDL_circle.h>

#include <circle/multicore.h>

#include <cerrno>
#include <cstring>
#include <utility>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {

// The genuine newlib glue, still reachable under these names because of
// --wrap. Called on core 0 only.
int __real__open(const char *path, int flags, ...);
int __real__close(int fd);
long __real__read(int fd, void *buf, size_t len);
long __real__write(int fd, const void *buf, size_t len);
off_t __real__lseek(int fd, off_t off, int whence);
int __real__fstat(int fd, struct stat *st);
int __real__stat(const char *path, struct stat *st);
int __real_lstat(const char *path, struct stat *st);
int __real__unlink(const char *path);
int __real__rename(const char *from, const char *to);
int __real__fcntl(int fd, int cmd, ...);
int __real_mkdir(const char *path, mode_t mode);
int __real_chdir(const char *path);
char *__real_getcwd(char *buf, size_t size);
int __real_dup(int fd);
int __real_dup2(int a, int b);
DIR *__real_opendir(const char *path);
struct dirent *__real_readdir(DIR *dir);
int __real_closedir(DIR *dir);
void __real_rewinddir(DIR *dir);
int __real__gettimeofday(struct timeval *tv, void *tz);
int __real__isatty(int fd);

// fstat answers by stat-ing the file's own name, the way the C library does.
int __wrap__stat(const char *path, struct stat *st);
int __real_ftruncate(int fd, off_t length);

} // extern "C"

namespace
{

// Are we the core that owns the hardware? Before the split is armed there
// is only one core and the answer is always yes, which is what makes this
// file inert on a single-core boot.
inline bool OnHardwareCore(void)
{
    if (!SDL2Circle_SplitActive())
        return true;
#ifdef ARM_ALLOW_MULTI_CORE
    return CMultiCoreSupport::ThisCore() == 0;
#else
    return true;
#endif
}

// Run a call on core 0 and bring back both its result and its errno. Used
// for the handful of things the I/O service does not cover; the marshalled
// body runs on core 0, so the wrappers it re-enters take their direct path.
template <typename F, typename R = decltype(std::declval<F &>()())>
R OnHardwareCoreDo(F fn)
{
    struct Box
    {
        F *fn;
        R result;
        int err;
    } box{&fn, R(), 0};

    SDL2Circle_CallOn0([](void *p)
    {
        Box *b = static_cast<Box *>(p);
        errno = 0;
        b->result = (*b->fn)();
        b->err = errno;
    }, &box);

    errno = box.err;
    return box.result;
}

// Descriptors 0, 1 and 2 are the serial console, not files.
inline bool IsConsole(int fd) { return fd >= 0 && fd <= 2; }

// ---------------------------------------------------------------------------
// Open-file positions.
//
// Sized past Circle's own descriptor table (20 entries), because the
// descriptors here are that table's — the service opens the file on core 0
// and hands the real descriptor back. A descriptor outside this range would
// mean the glue grew, so it is refused rather than silently unpositioned.
// ---------------------------------------------------------------------------

struct OpenFile
{
    bool used;
    long long pos;      // where the next read or write starts
    long long size;     // last known length, for seeks from the end
    int  oflags;        // the flags this descriptor was opened with, for F_GETFL

    // The name it was opened with. The C library answers fstat by stat-ing
    // the file's own name - it keeps the name for exactly this - so
    // answering one off core 0 needs the same name and the same route.
    char name[SYSCALLWRAP_MAX_PATH];
};

OpenFile s_files[SYSCALLWRAP_MAX_TRACKED_FD];

OpenFile *Track(int fd)
{
    if (fd < 0 || fd >= SYSCALLWRAP_MAX_TRACKED_FD || !s_files[fd].used)
        return nullptr;
    return &s_files[fd];
}

// The service reports failures as a negated errno and never touches the
// caller's. Put it back where C expects it.
int Fail(int negErrno)
{
    errno = negErrno < 0 ? -negErrno : EIO;
    return -1;
}

// A duplicated descriptor is the same open file, so it inherits this
// file's position and length. Without this the copy is untracked and the
// first call on it fails with EBADF - which the C library reports as the
// file being unopenable, not as a bad descriptor.
void TrackDup(int oldfd, int newfd)
{
    if (newfd < 0 || newfd >= SYSCALLWRAP_MAX_TRACKED_FD)
        return;
    OpenFile *f = Track(oldfd);
    if (!f)
        return;
    s_files[newfd] = *f;
}

} // namespace

extern "C" {

// ---- files -----------------------------------------------------------------

int __wrap__open(const char *path, int flags, ...)
{
    // The mode argument only matters when creating, and the service always
    // creates with the same permissions, so it is not forwarded.
    if (OnHardwareCore())
        return __real__open(path, flags, 0666);

    unsigned io = 0;
    switch (flags & O_ACCMODE)
    {
    case O_WRONLY: io = SDL2CIRCLE_IO_WRITE; break;
    case O_RDWR:   io = SDL2CIRCLE_IO_READ | SDL2CIRCLE_IO_WRITE; break;
    default:       io = SDL2CIRCLE_IO_READ; break;
    }
    if (flags & O_CREAT)
        io |= SDL2CIRCLE_IO_CREATE;

    uint64_t size = 0;
    int fd = SDL2Circle_IOOpen(path, io, &size);
    if (fd < 0)
        return Fail(fd);
    if (fd >= SYSCALLWRAP_MAX_TRACKED_FD)
    {
        SDL2Circle_IOClose(fd);
        errno = EMFILE;
        return -1;
    }

    s_files[fd].used = true;
    s_files[fd].size = (long long)size;
    s_files[fd].oflags = flags;
    strncpy(s_files[fd].name, path, sizeof(s_files[fd].name) - 1);
    s_files[fd].name[sizeof(s_files[fd].name) - 1] = '\0';
    // O_APPEND starts at the end; everything else at the beginning. A
    // create truncated the file, so its length is zero whatever was there.
    s_files[fd].pos = (flags & O_APPEND) ? (long long)size : 0;
    if (io & SDL2CIRCLE_IO_CREATE)
        s_files[fd].size = 0;
    return fd;
}

int __wrap__close(int fd)
{
    if (OnHardwareCore())
        return __real__close(fd);
    // The one console descriptor call that has to be marshalled. The C
    // library's _close ends by yielding to Circle's scheduler, which exists
    // on core 0 and nowhere else, so this descriptor cannot be closed from
    // the calling core the way the other console calls are served.
    if (IsConsole(fd))
        return OnHardwareCoreDo([&] { return __real__close(fd); });

    OpenFile *f = Track(fd);
    if (f)
        f->used = false;
    int r = SDL2Circle_IOClose(fd);
    return r < 0 ? Fail(r) : 0;
}

long __wrap__read(int fd, void *buf, size_t len)
{
    if (OnHardwareCore())
        return __real__read(fd, buf, len);
    // Standard input is the console's keyboard half, and the library has an
    // entry point for it that crosses to core 0 on a task of its own and
    // keeps the watchdog's heartbeat going while it waits. It is the only
    // route: a read here waits on a human, so the bounded call mailbox the
    // helper above uses must never carry one — the servo drains that mailbox
    // inline, and a servo that waits stops the board.
    if (fd == 0)
    {
        long n = SDL2Circle_ReadStdin(buf, (uint32_t)len);
        return n < 0 ? Fail((int)n) : n;
    }
    // 1 and 2 are the write half, which answers a read with an error and
    // touches nothing to do it.
    if (IsConsole(fd))
        return __real__read(fd, buf, len);

    OpenFile *f = Track(fd);
    if (!f)
    {
        errno = EBADF;
        return -1;
    }
    long n = SDL2Circle_IORead(fd, buf, (uint64_t)f->pos, (uint32_t)len);
    if (n < 0)
        return Fail((int)n);
    f->pos += n;
    return n;
}

long __wrap__write(int fd, const void *buf, size_t len)
{
    // A console descriptor goes straight to the C library from any core. The
    // console the library bound behind it hands the bytes to
    // SDL2Circle_WriteBytes, which puts them in the calling core's own ring
    // for core 0's servo to drain: no mailbox, no waiting, no device
    // touched. It is also the only route that leaves a program's own output
    // byte for byte, which is what standard output and standard error are
    // for — the labelled, timestamped channel is SDL2Circle_Log and nothing
    // reaches it through here.
    if (OnHardwareCore() || IsConsole(fd))
        return __real__write(fd, buf, len);

    OpenFile *f = Track(fd);
    if (!f)
    {
        errno = EBADF;
        return -1;
    }
    long n = SDL2Circle_IOWrite(fd, buf, (uint64_t)f->pos, (uint32_t)len);
    if (n < 0)
        return Fail((int)n);
    f->pos += n;
    if (f->pos > f->size)
        f->size = f->pos;
    return n;
}

off_t __wrap__lseek(int fd, off_t off, int whence)
{
    // A console has no position, and the C library says so — ESPIPE, from a
    // constant, without reaching a device.
    if (OnHardwareCore() || IsConsole(fd))
        return __real__lseek(fd, off, whence);

    OpenFile *f = Track(fd);
    if (!f)
    {
        errno = EBADF;
        return -1;
    }
    long long target;
    switch (whence)
    {
    case SEEK_SET: target = off; break;
    case SEEK_CUR: target = f->pos + off; break;
    // The length recorded when the file was opened, advanced by anything
    // written since. Nothing else on this board can be changing the file
    // underneath, so it is exact.
    case SEEK_END: target = f->size + off; break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (target < 0)
    {
        errno = EINVAL;
        return -1;
    }
    f->pos = target;
    return (off_t)target;
}

// Called by the C++ library on every file it opens, to size its buffer.
int __wrap__fstat(int fd, struct stat *st)
{
    // The C library answers for a console descriptor out of fixed values —
    // the character-device flag newlib looks for — and reaches nothing.
    if (OnHardwareCore() || IsConsole(fd))
        return __real__fstat(fd, st);

    OpenFile *f = Track(fd);
    if (!f)
    {
        errno = EBADF;
        return -1;
    }

    // The same answer the C library would give, by the same means: it stats
    // the name the descriptor was opened with. The card belongs to core 0,
    // so the stat goes through the service rather than straight into the
    // filesystem.
    return __wrap__stat(f->name, st);
}

static int stat_through_service(const char *path, struct stat *st)
{
    SDL2Circle_IOStat s;
    int r = SDL2Circle_IOStatPath(path, &s);
    if (r < 0)
        return Fail(r);
    memset(st, 0, sizeof(*st));
    st->st_mode = (s.isdir ? S_IFDIR : S_IFREG) | 0666;
    st->st_size = (off_t)s.size;
    st->st_mtime = (time_t)s.mtime;
    st->st_nlink = 1;
    st->st_blksize = 512;
    st->st_blocks = (s.size + 511) / 512;
    return 0;
}

int __wrap__stat(const char *path, struct stat *st)
{
    if (OnHardwareCore())
        return __real__stat(path, st);
    return stat_through_service(path, st);
}

// No symbolic links exist on this filesystem, so this is stat.
int __wrap_lstat(const char *path, struct stat *st)
{
    if (OnHardwareCore())
        return __real_lstat(path, st);
    return stat_through_service(path, st);
}

int __wrap__unlink(const char *path)
{
    if (OnHardwareCore())
        return __real__unlink(path);
    int r = SDL2Circle_IOUnlink(path);
    return r < 0 ? Fail(r) : 0;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    if (OnHardwareCore())
        return __real_mkdir(path, mode);
    int r = SDL2Circle_IOMkdir(path);
    return r < 0 ? Fail(r) : 0;
}

int __wrap__rename(const char *from, const char *to)
{
    if (OnHardwareCore())
        return __real__rename(from, to);
    int r = SDL2Circle_IORename(from, to);
    return r < 0 ? Fail(r) : 0;
}

// The working directory is one setting for the whole board, held on core 0,
// so these reach it rather than keeping a second copy here.
int __wrap_chdir(const char *path)
{
    if (OnHardwareCore())
        return __real_chdir(path);
    int r = SDL2Circle_IOChdir(path);
    return r < 0 ? Fail(r) : 0;
}

char *__wrap_getcwd(char *buf, size_t size)
{
    if (OnHardwareCore())
        return __real_getcwd(buf, size);
    int r = SDL2Circle_IOGetCwd(buf, (uint32_t)size);
    if (r < 0)
    {
        Fail(r);
        return nullptr;
    }
    return buf;
}

// ---- directories -----------------------------------------------------------
//
// The service opens the directory on core 0 and hands back the very handle
// the C library would have given, so it travels as an opaque DIR * here.

DIR *__wrap_opendir(const char *path)
{
    if (OnHardwareCore())
        return __real_opendir(path);
    return (DIR *)SDL2Circle_IOOpenDir(path);
}

struct dirent *__wrap_readdir(DIR *dir)
{
    // A real readdir returns storage owned by the directory handle, which
    // this side cannot reach into. One buffer serves instead, on the same
    // terms the C library sets: the entry is only valid until the next
    // call, and the application core is the single reader.
    static struct dirent s_entry;

    if (OnHardwareCore())
    {
        // The I/O service reads the directory with readdir once it has
        // reached core 0, and --wrap redirects that call here too, so this
        // branch is the service's own read, not the application's. It must
        // still reach the real one, exactly as every other wrapper here
        // does — returning without doing so would call this wrapper again
        // instead of the C library, forever.
        //
        // The entry comes back in the C library's own layout. This side's
        // struct dirent shares that layout up to d_name and adds d_type
        // after it (see wrapdirent.h), so d_ino and d_name are read
        // straight from it and d_type is the one field the C library has
        // no answer for. The service fills isdir from its own stat, so
        // reporting DT_UNKNOWN here is the truth rather than a gap.
        struct dirent *d = __real_readdir(dir);
        if (!d)
            return nullptr;

        // Read from its entry, never written to: the storage behind it is
        // the C library's, and it is one field shorter than this side's
        // struct, so setting d_type there would write into whatever
        // follows it inside the directory handle.
        memset(&s_entry, 0, sizeof(s_entry));
        s_entry.d_ino = d->d_ino;
        strncpy(s_entry.d_name, d->d_name, sizeof(s_entry.d_name) - 1);
        s_entry.d_type = DT_UNKNOWN;
        return &s_entry;
    }

    SDL2Circle_IODirEntry e;
    int r = SDL2Circle_IOReadDir((intptr_t)dir, &e);
    if (r <= 0)
    {
        if (r < 0)
            errno = -r;
        return nullptr;
    }
    memset(&s_entry, 0, sizeof(s_entry));
    s_entry.d_type = e.isdir ? DT_DIR : DT_REG;
    strncpy(s_entry.d_name, e.name, sizeof(s_entry.d_name) - 1);
    return &s_entry;
}

int __wrap_closedir(DIR *dir)
{
    if (OnHardwareCore())
        return __real_closedir(dir);
    SDL2Circle_IOCloseDir((intptr_t)dir);
    return 0;
}

// ---- the residue -------------------------------------------------------------
//
// Descriptor-table, directory-handle and clock calls, none of which the I/O
// service carries. Nothing a port typically calls reaches most of them, but
// each one reads or writes state that belongs to core 0, so they are
// marshalled rather than left to run on the wrong core.

void __wrap_rewinddir(DIR *dir)
{
    if (OnHardwareCore())
    {
        __real_rewinddir(dir);
        return;
    }
    OnHardwareCoreDo([&] { __real_rewinddir(dir); return 0; });
}

// The C library reads the clock through CTimer, which is a device and so is
// core 0's alone. A port calling time() or gettimeofday() directly from the
// application core reaches the clock through here.
int __wrap__gettimeofday(struct timeval *tv, void *tz)
{
    if (OnHardwareCore())
        return __real__gettimeofday(tv, tz);
    return OnHardwareCoreDo([&] { return __real__gettimeofday(tv, tz); });
}

// newlib's fopen asks this while sizing the stream's buffer, on the
// descriptor it has just opened. Unwrapped it reaches the glue on whichever
// core called it; a file opened through the service is never a terminal, and
// the console descriptors are.
int __wrap_ftruncate(int fd, off_t length)
{
    // A console descriptor has no length to set, and the C library already
    // says so: its own glue answers one from a base implementation that sets
    // errno and returns, reaching no device. Forwarding keeps that answer the
    // C library's rather than restating it here.
    if (OnHardwareCore() || IsConsole(fd))
        return __real_ftruncate(fd, length);

    OpenFile *f = Track(fd);
    if (!f)
    {
        errno = EBADF;
        return -1;
    }

    const int r = SDL2Circle_IOTruncate(fd, (uint64_t)length);
    if (r < 0)
        return Fail(r);

    // The recorded size follows the file, and a position past the new end
    // stays where it is: the C library allows a seek beyond the end, and a
    // write there is what extends the file again.
    f->size = (uint64_t)length;
    return 0;
}

int __wrap__isatty(int fd)
{
    // Neither implementation behind this reaches a device: a console
    // descriptor is answered from a constant, and a file descriptor sets
    // ENOTTY and returns zero. So it forwards from either core, and the
    // answer - errno included - stays the C library's.
    return __real__isatty(fd);
}

int __wrap__fcntl(int fd, int cmd, int arg)
{
    if (OnHardwareCore())
        return __real__fcntl(fd, cmd, arg);
    // F_GETFL and F_SETFL are answered here rather than passed on. The C
    // library's fdopen asks for the access mode before it will build a
    // stream, and the glue underneath this layer refuses the request - so
    // every fdopen on this platform fails, and a port that opens a file
    // that way cannot read it at all. This layer knows the answer: it
    // recorded the flags when the descriptor was opened.
    if (OpenFile *f = Track(fd))
    {
        if (cmd == F_GETFL)
            return f->oflags;
        if (cmd == F_SETFL)
        {
            // Only the status bits may change, and none of the ones this
            // layer acts on. Accept and record, so a later F_GETFL agrees.
            f->oflags = (f->oflags & O_ACCMODE) | (arg & ~O_ACCMODE);
            return 0;
        }
    }

    const int r = OnHardwareCoreDo([&] { return __real__fcntl(fd, cmd, arg); });
    // F_DUPFD hands back a second descriptor for the same open file. The C
    // library's fopen uses one, so it has to carry the position with it.
    if (r >= 0 && (cmd == F_DUPFD
#ifdef F_DUPFD_CLOEXEC
                   || cmd == F_DUPFD_CLOEXEC
#endif
                  ))
        TrackDup(fd, r);
    return r;
}

int __wrap_dup(int fd)
{
    if (OnHardwareCore())
        return __real_dup(fd);
    const int r = OnHardwareCoreDo([&] { return __real_dup(fd); });
    if (r >= 0)
        TrackDup(fd, r);
    return r;
}

int __wrap_dup2(int a, int b)
{
    if (OnHardwareCore())
        return __real_dup2(a, b);
    const int r = OnHardwareCoreDo([&] { return __real_dup2(a, b); });
    if (r >= 0)
        TrackDup(a, r);
    return r;
}

} // extern "C"
