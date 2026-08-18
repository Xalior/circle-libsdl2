# circle-posix

The POSIX functions a bare-metal Circle program is missing, in one place.

It is one source file, a header, three headers the C library does not have,
and a makefile fragment that builds them. You compile the source into your
own kernel; there is no library to build and nothing extra to link.

## What it is for

Circle and its C library give a program a filesystem, a clock, a heap and a
console. They do not give it the rest of an operating system. A program
written for a desktop calls the rest anyway — it asks who the user is, runs
a helper program, opens a shared library, puts the terminal into raw mode —
and on this platform none of those functions exists, so the program does not
link.

Every port of an existing program hits this, and every port has answered it
the same way: a file of its own full of small functions. This is that file,
written once. A port that includes it keeps only the functions that are
genuinely its own — the ones that are about the program being ported rather
than about the missing operating system.

## The rule every answer follows

**A function that cannot do its job says so, in the way POSIX defines for
that function.** It does not report a success it did not achieve.

This is the opposite of what it looks like at first. Reporting success is
the answer that breaks programs, quietly and much later: a program told that
the terminal is now in raw mode reads as though it were and gets something
else. A program told the call failed takes the path it already has for a
system where the thing is missing — because a desktop system missing the
same helper program, or the same shared library, gives the same failure —
and that path works.

**Where success is the truth, success is what it returns.** Some of these
requests really are met here, for a reason particular to the platform: an
advisory file lock excludes other processes and there can never be another
process, so the exclusion the caller asked for genuinely holds. Every such
case has its reason written beside it in the source.

## What it answers

**This machine.** `uname` fills every field from what the platform actually
is — Circle, its version, the instruction set, the board. `gethostname`
gives the same name `uname` reports, which is what POSIX requires of the
pair.

**The user database.** There is none. `getuid` and `geteuid` answer 0: one
program runs and nothing on the board is withheld from it, which is what
that number means. `getpwuid` and `getpwnam` find nothing, and leave `errno`
alone — POSIX distinguishes "no such entry" from an error while searching,
and this is the first.

**Files.** `access` asks the filesystem whether the path is there. Read and
write are answered by its being there, since FAT carries no permission bits.
Execute is refused for anything that is not a directory, because nothing on
this platform can be executed; a caller looking for a program to run finds
none, which is true. `chmod` succeeds and changes nothing: there are no
permission bits to change and nothing that reads them. `flock` grants the
lock. `futimens` fails: the filesystem records a modification time but
nothing here can write one.

**Other processes.** There are none, and no arranging by the caller would
produce one. `execlp`, `execvp` and `popen` fail with `ENOSYS`, `waitpid`
and `pclose` with `ECHILD`.

**Dynamic loading.** One statically linked image, no loader. `dlopen` and
`dlsym` fail, `dlerror` says why and clears itself as POSIX requires, and
`dlclose` succeeds because nothing was opened.

**Terminals.** There are none. Standard input is the library's keyboard and
standard output is its log; neither is a tty with attributes. `tcgetattr`,
`tcsetattr` and `ioctl` fail with `ENOTTY`.

**Threads.** The C library here was built without POSIX threads, so it
declares the types and defines none of the functions. `pthread_mutex_*` is a
real mutex built on an atomic exchange, not a no-op — a program on this
platform can have more than one core running its code. `pthread_self`
returns a stable identifier for the one thread. `flockfile` and `funlockfile`
do nothing, because the C library does no locking inside a stream either and
nothing added out here could change that.

**`dirname`** is pure string work, with the standard semantics.

## What it deliberately does not have

Memory mapping, signal delivery, network interface enumeration, backtraces,
and setting a file's timestamp through `utime`. Each of those has more than
one defensible answer, and the answer changes what a calling program does,
so it belongs to the program that needs it until somebody decides which
answer is right for everybody.

## How to use it in a Circle project

**1. Build it, with one line.** Put the directory anywhere your project can
reach, and include its makefile fragment on the line **before** Circle's
`Rules.mk`:

```make
include /wherever/circle-posix/posix.mk
include $(CIRCLEHOME)/Rules.mk
```

That is the whole of the build change. The fragment works out where it is,
and adds its own object to `OBJS`, its own directory to the include path,
and the rules to build both the object and — if your project wants one — its
dependency file.

**The line before `Rules.mk` is not a style preference, and after it does
not work.** `Rules.mk` reads `OBJS` at the moment it is included and works
out its dependency list from it there and then, so anything added afterwards
is something make never checks. That build is not loud about it: everything
compiles, and then you change a header and nothing rebuilds.

**2. Include a header only where you need one.** Most of these functions are
already declared by a header the C library ships, and those need nothing.
Three are headers the C library does not have at all — `<sys/utsname.h>`,
`<sys/termios.h>` and `<dlfcn.h>` — and they are found the ordinary way,
because the fragment puts this directory on the include path.

The remaining two groups, the thread functions and `ioctl`, are declared in
`posix.h`. Include it where you call them. If your project forces a header
into every translation unit, this is the one to force; the other three are
deliberately not pulled in by it, because their macro names are common words
that collide.

## Where it runs

Almost every answer here comes from a constant or from memory the caller
already owns, so any core may call it.

`access` is the exception: it has to ask the filesystem, and the filesystem
belongs to core 0. It asks through the library's I/O service, which is valid
from any core and becomes a direct call when there is only one core in play.
That is also why this component, unlike some of its neighbours, is written
against the library and not against Circle alone.

## Adding to it

A program that needs a function this does not have gets it added here, once,
for everybody — with the fact about the platform that decides its answer
written beside it, the way every function in the file has one. A function
whose honest answer would change what a calling program does, in a way
nobody has yet worked out, stays with that program instead.
