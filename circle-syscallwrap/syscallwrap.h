//
// syscallwrap.h — the one setting this component exposes.
//
// Including syscallwrap.mk is enough to get the wrappers and the --wrap
// flags that route to them (see syscallwrap.mk). Nothing here needs to be
// called: every symbol this component defines is reached by the linker's
// --wrap redirection, never by name from application code. This header
// exists for the one thing that is not settled by the .mk — how many open
// descriptors the wrappers can track at once.
//
#ifndef _syscallwrap_h
#define _syscallwrap_h

// Sized past Circle's own descriptor table (20 entries), because the
// descriptors the wrappers track are that table's: the I/O service opens
// each file on core 0 and hands the real descriptor back, so this only
// ever needs to be as large as that table could ever be. A consumer whose
// own build adds a larger value to CPPFLAGS before syscallwrap.o is
// compiled gets that value instead.
#ifndef SYSCALLWRAP_MAX_TRACKED_FD
#define SYSCALLWRAP_MAX_TRACKED_FD 64
#endif

#endif // _syscallwrap_h

// How much of a path a tracked descriptor remembers. It is remembered because
// the C library answers fstat by stat-ing the file's own name, and answering
// one off core 0 has to do the same.
#ifndef SYSCALLWRAP_MAX_PATH
#define SYSCALLWRAP_MAX_PATH 256
#endif
