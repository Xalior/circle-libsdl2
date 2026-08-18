//
// posix.h — the declarations that no header on this platform carries.
//
// Including posix.mk builds the component and puts this directory on the
// include path, and that is the whole of the build change (see posix.mk).
// Most of what this component defines is already declared by a header the
// C library ships: <unistd.h> declares access, execlp, execvp, getuid,
// geteuid and gethostname, <pwd.h> declares getpwuid and getpwnam,
// <libgen.h> declares dirname, <sys/stat.h> declares chmod and futimens,
// <stdio.h> declares popen, pclose, flockfile and funlockfile,
// <sys/wait.h> declares waitpid, and <sys/file.h> declares flock. Nothing
// has to be done to see any of those.
//
// What is left is the two groups below. The C library declares the thread
// functions only when it was built with POSIX threads, which this one was
// not, and it has no header for ioctl at all.
//
// The three headers beside this one — <sys/utsname.h>, <sys/termios.h> and
// <dlfcn.h> — are headers the C library does not have in any form. They are
// found the ordinary way, by this directory being on the include path, and
// they are deliberately not included from here: a consumer that forces this
// header into every translation unit would otherwise carry their macro
// names into every one as well, and names as common as ECHO collide.
//
#ifndef _circle_posix_h
#define _circle_posix_h

#include <sys/types.h>          // pthread_t, pthread_mutex_t
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The C library wraps its own declarations of these in a test for POSIX
// threads, which is off in this build, so the names are absent even though
// the types beside them are not.
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
pthread_t pthread_self(void);

// Device control. There is no <sys/ioctl.h> here and none is shipped: the
// whole interface is its request numbers, every one of them belongs to a
// device driver, and this platform has no descriptor that answers any of
// them. A consumer that names a request defines that name itself.
int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif // _circle_posix_h
