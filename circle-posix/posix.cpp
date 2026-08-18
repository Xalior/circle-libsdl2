//
// posix.cpp — the POSIX functions this platform does not have.
//
// Circle and its C library give a program a filesystem, a clock, a heap and
// a console. They do not give it the rest of an operating system. There is
// no user database, no second process to start or wait for, no terminal
// line discipline, no dynamic loader, and the FAT filesystem underneath
// carries neither permission bits nor file locks. A program written for a
// desktop calls these functions anyway, and without them it does not link.
//
// THE RULE EVERY ANSWER HERE FOLLOWS. Each function gives the answer this
// platform can honestly give. One that cannot do its job says so, in the
// way POSIX defines for that function, rather than reporting a success it
// did not achieve. That is not a loss: a caller that checks the result then
// takes the path it already has for a system where the thing is missing,
// and that path works. Where the truthful answer is success — because the
// state the caller asked for is the state that already exists, or because
// the guarantee it asked for holds here for a different reason — that is
// what it returns, and the reason is written beside it.
//
// WHERE THIS RUNS. Almost nothing here reaches hardware: the answers come
// from constants and from memory the caller already owns, so any core may
// call them. The one exception is access(), which has to ask the filesystem
// whether a path exists, and the filesystem belongs to core 0. It asks
// through the library's I/O service, which is valid from any core and is a
// direct call when there is only one core in play. It is the I/O service
// rather than SDL2Circle_CallOn0 because the service already carries this
// exact question, and because a card transaction taken through the call
// mailbox occupies the servo for its whole duration. See
// docs/CORE-SPLIT.md.
//
// GROWING IT. A consumer that needs a function this file does not have adds
// it here, once, for everybody — with the platform fact that decides its
// answer written beside it, the way every function below has one.
//
#include "posix.h"

#include <SDL2/SDL_circle.h>

#include <circle/version.h>

#include <dlfcn.h>
#include <sys/termios.h>
#include <sys/utsname.h>

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace
{

// The name of this machine, used for both uname's nodename and
// gethostname, which POSIX says are the same value. No name is configured
// anywhere on this platform and there is nothing to configure one from, and
// localhost is the name a machine with no other name has.
const char NODE_NAME[] = "localhost";

// Copy into one of uname's fixed-length fields, always terminated.
void CopyField(char *field, const char *value)
{
    strncpy(field, value, _UTSNAME_LENGTH - 1);
    field[_UTSNAME_LENGTH - 1] = '\0';
}

} // namespace

extern "C" {

// ---- what this machine is --------------------------------------------------

// Every field is filled from what the platform actually is: the operating
// system is Circle, the release is Circle's own version string, and the
// machine is the instruction set this was built for. A program that reads
// any of them gets something true, and one that compares sysname against a
// desktop system's name gets a straight answer that it is not that.
int uname(struct utsname *buf)
{
    if (buf == nullptr)
    {
        errno = EFAULT;
        return -1;
    }

    CopyField(buf->sysname,  CIRCLE_NAME);
    CopyField(buf->nodename, NODE_NAME);
    CopyField(buf->release,  CIRCLE_VERSION_STRING);
#ifdef __aarch64__
    CopyField(buf->machine,  "aarch64");
#else
    CopyField(buf->machine,  "arm");
#endif

#ifdef RASPPI
    snprintf(buf->version, _UTSNAME_LENGTH, "bare metal, Raspberry Pi %d",
             (int)RASPPI);
#else
    CopyField(buf->version, "bare metal");
#endif
    return 0;
}

// The same name uname reports as its nodename, for the same reason.
int gethostname(char *name, size_t len)
{
    if (name == nullptr)
    {
        errno = EFAULT;
        return -1;
    }
    if (len <= sizeof(NODE_NAME) - 1)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(name, NODE_NAME);
    return 0;
}

// ---- the user database, which does not exist -------------------------------

// There is no user database and no login: one program runs, and nothing on
// the board is withheld from it. That is what user 0 means, so that is the
// answer, and the effective user is the same one.
uid_t getuid(void)
{
    return 0;
}

uid_t geteuid(void)
{
    return 0;
}

// Every lookup finds nothing, because there is nothing to look in. POSIX
// distinguishes "no such entry", which returns a null pointer and leaves
// errno alone, from an error while searching, which sets it — so errno is
// deliberately not touched here. A caller that follows POSIX and tests
// errno to tell the two apart is told the entry is absent rather than that
// the search failed, which is exactly what happened.
struct passwd *getpwuid(uid_t uid)
{
    (void)uid;
    return nullptr;
}

struct passwd *getpwnam(const char *name)
{
    (void)name;
    return nullptr;
}

// ---- files -----------------------------------------------------------------

// Existence is the only thing this filesystem can be asked, and for read
// and write it is also the whole answer: FAT carries no permission bits,
// and every file that is here can be read and written by the one program
// that is running.
//
// Execute is the exception, and it is not a technicality. Nothing on this
// platform can be executed — there is no loader and no process to start, as
// the exec family below reports — so no file is an executable file. A
// directory is a different question: for a directory, execute permission
// means the right to look inside it, and that is granted. A caller looking
// for a program to run therefore finds none, which is the truth and is the
// same answer it would get on a desktop where the program is not installed.
int access(const char *path, int amode)
{
    SDL2Circle_IOStat st;
    const int r = SDL2Circle_IOStatPath(path, &st);
    if (r < 0)
    {
        errno = -r;
        return -1;
    }

    if ((amode & X_OK) != 0 && !st.isdir)
    {
        errno = EACCES;
        return -1;
    }
    return 0;
}

// The filesystem has no permission bits to change and nothing that reads
// them: every file is reported as readable and writable by everybody
// whatever happens here. So there is no state a caller could set and later
// find unset, and success says the request has been met as far as anything
// on this platform can tell. Reporting failure instead would stop callers
// that treat it as fatal from doing work the platform is perfectly able to
// do — copying a file, or changing an attribute it keeps its own record of.
int chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return 0;
}

// The modification time cannot be set. The filesystem records one and the
// I/O service reports it, but nothing here can write one, so a caller
// asking for a particular timestamp is asking for something that will not
// happen. Every caller of this is stamping a file it has just written, and
// every one of them carries on when it fails.
int futimens(int fd, const struct timespec times[2])
{
    (void)fd;
    (void)times;
    errno = ENOSYS;
    return -1;
}

// An advisory lock keeps two processes out of each other's way. There is
// one program on this board and there can never be another, so the
// exclusion the caller asked for genuinely holds, and granting the lock is
// the true answer rather than a polite one. Callers commonly refuse to open
// a file at all when a lock is denied, so refusing here would lose them
// work for a conflict that cannot occur.
int flock(int fd, int operation)
{
    (void)fd;
    (void)operation;
    return 0;
}

// ---- paths -----------------------------------------------------------------

// Pure string work, and the platform has no part in it: the standard
// semantics, answered out of static storage the way every C library
// answers it.
char *dirname(char *path)
{
    static char buf[PATH_MAX];

    if (path == nullptr || path[0] == '\0')
    {
        strcpy(buf, ".");
        return buf;
    }

    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = '\0';

    char *slash = strrchr(buf, '/');
    if (slash == nullptr)
    {
        strcpy(buf, ".");
        return buf;
    }
    if (slash == buf)
    {
        buf[1] = '\0';          // the root directory
        return buf;
    }
    *slash = '\0';
    return buf;
}

// ---- other processes, of which there are none ------------------------------
//
// This image is the whole of what runs. There is no shell, no program to
// load and no child to wait for, and no amount of arranging by the caller
// would produce one — so these do not fail for want of a file, they fail
// because the service does not exist. That is ENOSYS.

int execlp(const char *file, const char *arg0, ...)
{
    (void)file;
    (void)arg0;
    errno = ENOSYS;
    return -1;
}

int execvp(const char *file, char *const argv[])
{
    (void)file;
    (void)argv;
    errno = ENOSYS;
    return -1;
}

// A pipe to a command needs the command, so this can only fail. Callers
// that reach for a helper program this way already handle its absence.
FILE *popen(const char *command, const char *type)
{
    (void)command;
    (void)type;
    errno = ENOSYS;
    return nullptr;
}

// popen never returned a stream, so any stream reaching this one did not
// come from it and has no child behind it. ECHILD is what POSIX gives
// pclose for a status it cannot obtain.
int pclose(FILE *stream)
{
    (void)stream;
    errno = ECHILD;
    return -1;
}

// Nothing ever started a child, so there is never one to wait for. The
// caller's status is left alone: POSIX does not define what it holds when
// the call fails.
pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid;
    (void)status;
    (void)options;
    errno = ECHILD;
    return -1;
}

// ---- dynamic loading, which does not happen --------------------------------
//
// One statically linked image, no loader, no symbol table to search at run
// time. dlopen and dlsym report failure and dlerror explains it, which is
// what a caller sees on a desktop when the library it wanted is not
// installed.

static const char DL_MESSAGE[] =
    "dynamic loading is not available on this platform";
static bool s_dl_failed = false;

void *dlopen(const char *filename, int flags)
{
    (void)filename;
    (void)flags;
    s_dl_failed = true;
    return nullptr;
}

void *dlsym(void *handle, const char *symbol)
{
    (void)handle;
    (void)symbol;
    s_dl_failed = true;
    return nullptr;
}

// Nothing was opened, so there is nothing that can fail to close.
int dlclose(void *handle)
{
    (void)handle;
    return 0;
}

// POSIX: the message stands for the most recent failure and is cleared by
// being read, so a caller that asks when nothing has failed is told
// nothing has failed rather than being handed a message about a call it
// never made.
char *dlerror(void)
{
    if (!s_dl_failed)
        return nullptr;
    s_dl_failed = false;
    return const_cast<char *>(DL_MESSAGE);
}

// ---- terminals, of which there are none ------------------------------------

// There is no terminal and no line discipline. Standard input here is the
// library's own keyboard, and standard output and error are its log; none
// of them is a tty with attributes to report or change, and no descriptor
// this platform can open is one either. ENOTTY is exactly that statement,
// and it is the answer POSIX defines for a descriptor that is not a
// terminal.
//
// Reporting success instead would be the more dangerous answer, not the
// kinder one: a program that puts a terminal into non-canonical mode and is
// told the change was made goes on to read as though it were, and gets
// something else. Every caller of this pair fetches attributes, changes a
// flag and sets them back, and does the same thing whether the calls worked
// or not.
int tcgetattr(int fd, struct termios *t)
{
    (void)fd;
    (void)t;
    errno = ENOTTY;
    return -1;
}

int tcsetattr(int fd, int actions, const struct termios *t)
{
    (void)fd;
    (void)actions;
    (void)t;
    errno = ENOTTY;
    return -1;
}

// Device control by request number. Every request belongs to a driver, and
// no descriptor on this platform — file, directory or console — answers
// any of them. ENOTTY is what a descriptor that does not support the
// request returns.
int ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    (void)request;
    errno = ENOTTY;
    return -1;
}

// ---- threads ---------------------------------------------------------------

// The C library was built without POSIX threads, so it defines none of
// this. What it does define is the type: a mutex is one unsigned word.
//
// The mutex below is a real one — an atomic exchange, not a no-op. A
// program on this platform can have more than one core running its code,
// and even where it does not, this file is in no position to promise on the
// caller's behalf that its own mutexes are uncontended.
//
// Locked is the one value that matters, and any other value counts as
// unlocked. That is deliberate: the C library's static initialiser for a
// mutex is all-ones rather than zero, so a mutex declared with it and never
// passed to pthread_mutex_init would otherwise look permanently locked and
// the first lock would spin forever.

static const pthread_mutex_t MUTEX_LOCKED = 1;

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    (void)attr;
    if (m == nullptr)
        return EINVAL;
    __atomic_store_n(m, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    if (m == nullptr)
        return EINVAL;
    while (__atomic_exchange_n(m, MUTEX_LOCKED, __ATOMIC_ACQUIRE) == MUTEX_LOCKED)
    {
        // Held by somebody else. Wait for them.
        __asm volatile ("yield");
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m == nullptr)
        return EINVAL;
    __atomic_store_n(m, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

// A thread identifier has to be stable and has to compare equal to itself.
// Nothing here creates a thread, so there is one of them and it is this
// constant. Zero is avoided because callers store an identifier and test it
// against zero to mean "not set yet".
pthread_t pthread_self(void)
{
    return 1;
}

// ---- stream locking --------------------------------------------------------

// The C library was built without thread locking, so it does no locking of
// its own inside a FILE either. There is therefore nothing for these to
// take or release, and no locking they could add that would make a stream
// safe when the library beneath is not doing any. They exist so that a
// program written to hold a stream lock across several calls still builds.
void flockfile(FILE *file)
{
    (void)file;
}

void funlockfile(FILE *file)
{
    (void)file;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Memory mapping
//
// A mapping here is memory this component allocates, and for a file it is
// that memory with the file's bytes read into it. That is exactly
// MAP_PRIVATE: the pages are the caller's own, a write to them changes
// nothing on the card, and the mapping ends when it is unmapped.
//
// MAP_SHARED is refused. It promises that a write through the mapping
// reaches the file and every other mapping of it, and nothing under this
// carries that promise - there is no page table walking a file's blocks, no
// write-back, and no way to notice a page was touched. Serving it as a
// private copy would leave a caller writing into memory and finding the file
// unchanged, with nothing anywhere to say why. Refusing puts the failure at
// the call that cannot be honoured.
//
// Every mapping is recorded, because munmap is given an address and a length
// and has to free the allocation that address came from. A caller that
// unmaps something this never handed out is told so rather than having a
// stray pointer passed to free.
// ---------------------------------------------------------------------------

namespace
{

struct Mapping
{
    void   *base;
    size_t  length;
    Mapping *next;
};

Mapping *s_mappings = nullptr;

} // namespace

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)prot;

    // A fixed address means "put it exactly here", and this allocates
    // wherever the heap has room.
    if (length == 0 || (flags & MAP_FIXED) != 0)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    (void)addr;

    const bool anonymous = (flags & MAP_ANONYMOUS) != 0;

    if ((flags & MAP_SHARED) != 0)
    {
        // See the note above: this platform cannot carry what MAP_SHARED
        // promises, so it says so instead of pretending.
        errno = ENOTSUP;
        return MAP_FAILED;
    }

    void *base = calloc(1, length);
    if (base == nullptr)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    if (!anonymous)
    {
        if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
        {
            free(base);
            return MAP_FAILED;
        }
        size_t done = 0;
        while (done < length)
        {
            long n = read(fd, (char *)base + done, length - done);
            if (n < 0)
            {
                free(base);
                return MAP_FAILED;
            }
            if (n == 0)
                break;      // shorter than the mapping; the rest stays zero
            done += (size_t)n;
        }
    }

    Mapping *rec = (Mapping *)calloc(1, sizeof(Mapping));
    if (rec == nullptr)
    {
        free(base);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    rec->base = base;
    rec->length = length;
    rec->next = s_mappings;
    s_mappings = rec;
    return base;
}

int munmap(void *addr, size_t length)
{
    (void)length;
    for (Mapping **link = &s_mappings; *link != nullptr; link = &(*link)->next)
    {
        if ((*link)->base == addr)
        {
            Mapping *rec = *link;
            *link = rec->next;
            free(rec->base);
            free(rec);
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

int mprotect(void *addr, size_t length, int prot)
{
    (void)addr;
    (void)length;

    // The memory a mapping is made of is ordinary heap: readable and
    // writable, and nothing here can make it less. Asking for what it
    // already is succeeds; asking for anything narrower cannot be honoured
    // and is refused rather than silently ignored.
    if ((prot & (PROT_READ | PROT_WRITE)) == (PROT_READ | PROT_WRITE))
        return 0;
    errno = ENOTSUP;
    return -1;
}

int msync(void *addr, size_t length, int flags)
{
    (void)addr;
    (void)length;
    (void)flags;

    // Only a shared mapping has anything to write back, and there are none.
    return 0;
}
