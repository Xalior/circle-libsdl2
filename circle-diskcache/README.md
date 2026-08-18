# circle-diskcache

A read cache for a Circle block device, and the measurement tool that tells
you what size to make it.

It is one header and one source file, with a makefile fragment that builds
them. You compile the source into your own kernel; there is no library to
build and nothing to link.

## This is not part of SDL

It is stored in this repository because that is a convenient place to keep it
today. It has nothing to do with SDL and it does not belong to this library.

It uses Circle and only Circle. It does not include an SDL header, it does not
call anything in circle-libsdl2, and it does not know that either exists. A
Circle project that has never used circle-libsdl2 can copy these two files and
use them exactly as described below.

## What it does

`CDiskCacheDevice` is a `CDevice` that wraps another `CDevice` — normally the
SD card — and registers itself under the wrapped device's name.

Circle's FatFs support does not hold a pointer to the card. It looks the card
up by name and then calls `Seek` and `Read` on whatever it is given. So when
this class takes the name over, everything above it uses the cache without
being changed: FatFs, the C library's `fopen` and `fread`, and any other code
that reads files.

A read it can answer from memory returns the same bytes the card would have
returned, without a card transaction. A read it cannot answer is passed to the
card unchanged.

It does two separate things, and they help different kinds of reading.

The **pool** holds blocks that have been asked for more than once, so the
second and later times cost nothing. It cannot help a file that is read
straight through and never touched again, because nothing in it is ever asked
for twice.

**Read-ahead** is what helps that file. The card charges almost the same for a
large request as a small one, so the cost of reading a file one sector at a
time is the number of requests, not the amount of data. When a read carries on
exactly where the previous one ended, this class asks the card for a whole run
of sectors at once and keeps it in a small window; the reads that follow come
out of the window.

The window is not the pool, and read-ahead never puts anything in the pool. A
sector served from the window faces the same admission test as any other, so a
file streamed once still never takes up pool space, while a file streamed round
a loop is admitted the second time round like any other repeat.

Both are sized separately at runtime, and either can be turned off.

Writes always reach the card before the call returns. Nothing is ever held in
memory waiting to be written. This is deliberate: these machines are switched
off without warning, and there is no shutdown step in which delayed writes
could be completed.

It also counts every request that passes through it, in both directions, and
writes a summary to Circle's log at a fixed interval. The summary is how you
choose both sizes, so leave it on while you are deciding and read it on the
serial console.

## Three decisions inside it

**A block enters the pool only when it is asked for a second time.** A file
that is read once from beginning to end — a level, a video, a package of
recorded speech — is never admitted, so it cannot push out anything useful. A
sound track played in a loop is admitted on its second pass and then stays.
One rule covers both cases, and the program does not have to describe itself.

**A block is removed by looking at a few slots chosen at random and dropping
the least recently used of those.** This is not true least-recently-used, and
the difference matters. Reading in a repeating cycle is common in games. Under
true least-recently-used, a cycle slightly larger than the pool produces no
hits at all, because every block is removed shortly before it is needed again.
Choosing candidates at random prevents that. Keeping recency within the sample
means a block that survives is used again, becomes recent, and is likely to
survive again, so a stable set of useful blocks builds up.

**Finding a block, and choosing which to remove, both cost the same however
large the pool is.** There is a hash index for lookup and a fixed-size random
sample for removal, and the pool itself is never scanned. This is why a larger
pool costs memory and nothing else.

**Read-ahead fetches on a continuation, and only on a continuation.** A run is
fetched when a read begins exactly where the previous one ended, which is what
reading a file through looks like. A read that lands somewhere new fetches
nothing extra, so scattered access never pays for data it will not use.

## Memory

Allocations happen only when you call `Configure` — the pool, its bookkeeping,
and the read-ahead window — and never again. The read path never allocates.

The low heap is chosen on purpose. On a board with more than one gigabyte, the
memory above that point is a separate heap. Staying in the low heap gives the
same behaviour on every board.

## How to use it in a Circle project

**1. Build it, with one line.** Put the directory anywhere your project can
reach, and include its makefile fragment on the line **before** Circle's
`Rules.mk`:

```make
include /wherever/circle-diskcache/diskcache.mk
include $(CIRCLEHOME)/Rules.mk
```

That is the whole of the build change. The fragment works out where it is, so
you do not have to tell it, and it adds its own object to `OBJS`, its own
directory to the include path, and the rules to build both the object and — if
your project wants one — its dependency file.

**The line before `Rules.mk` is not a style preference, and after it does not
work.** `Rules.mk` reads `OBJS` at the moment it is included and works out its
dependency list from it there and then, so anything added afterwards is
something make never checks. That build is not loud about it: everything
compiles, and then you change a header and nothing rebuilds. The same position
also settles `CHECK_DEPS`, which your project can only have set before
`Rules.mk`, so the one instruction covers both.

If your project builds objects into a directory of its own, set `OBJDIR`
before the include and this object goes there with the rest. If it does not,
the object is built beside the source, as Circle's own rules do.

**2. Give your kernel a cache object.** Add the header and one member:

```cpp
#include <diskcache.h>

class CKernel
{
    // ...
    CEMMCDevice      m_EMMC;
    CDiskCacheDevice m_DiskCache;
    FATFS            m_FileSystem;
};
```

The member is a plain object, not a pointer. It allocates nothing until you
call `Configure`.

**3. Install it after the card is up and before anything mounts it.** The card
registers its name when it initialises, so the cache cannot take that name
over before then. Mounting reads sectors, so it must happen after.

```cpp
if (bOK) bOK = m_EMMC.Initialize ();

if (bOK && !m_DiskCache.Install ())
    m_Logger.Write (FromKernel, LogWarning,
                    "disk cache did not install — running without it");

if (bOK) bOK = (f_mount (&m_FileSystem, "SD:", 1) == FR_OK);
```

`Install` only puts the object in the path. It does not give it any memory.

**4. Give it its memory, once, before your program starts.**

```cpp
m_DiskCache.Configure (nPoolKB, nReadAheadKB);
```

Both are in kilobytes and both may be zero. A pool of zero means no read is
ever answered from held data. A read-ahead of zero means the card is asked for
exactly what was requested and nothing more. Both zero is the run you compare
every other setting against: the card on its own, with the counting still
going.

Read-ahead is held to a fraction of the pool, so a window can never be set out
of all proportion to what it feeds. You do not have to get that right at the
call site.

Doing this as a separate step from `Install` lets you take both sizes from
wherever your project reads its settings, which may be later in startup. The
reads made by mounting the card happen before this point and are not cached.

**5. Call `Poll` regularly from core 0.**

```cpp
while (bRunning)
{
    m_DiskCache.Poll ();
    m_Scheduler.Yield ();
}
```

**Nothing is reported unless you ask for it.** `DISKCACHE_REPORT_SECONDS` is
zero, and at zero `Poll` returns without writing anything - the summary is an
instrument, and a cache that is working has nothing to say worth a console the
rest of the machine is competing for. Build with a number of seconds to watch
one, and put it back to zero afterwards.

With an interval set, `Poll` writes the summary when that interval has passed
and otherwise reads the clock and returns. Call it from a place that is
allowed to write to the log. Do not call it from inside a call another
processor core is waiting for.

`Report` writes the summary immediately, which is useful once more before the
board stops.

## Choosing the sizes

Run your program at several settings and read the summary. Both sizes are
worth measuring, and they are independent.

**For the pool**, read `hits` and the high-water mark on the `cache` line.
`hits` is the share of read requests answered from held data. The high-water
mark is the largest amount of the pool ever in use, and **it is the figure that
tells you when to stop**: if your program never fills more than a small part of
the pool, every larger size will report the same hit rate, and the answer is
the smallest size that still holds everything it re-reads. If instead the pool
fills completely and blocks are being evicted, the pool is too small and the
measurement has not found the answer yet — go up until it stops filling.

**For read-ahead**, read the `ahead` line. It gives the share of each fetched
run that something went on to ask for. A share near the whole of it means the
depth could go further; a small share means the card is being asked for
sectors nobody wants, which costs time and gains nothing. Also watch the
`card` line: fewer, larger requests is the point, and the total time is what
should fall.

The right values differ from one program to the next, which is why they are
worth setting per program rather than choosing one pair for everything.

## Where it runs

Circle's devices belong to core 0, so every call into this class arrives on
core 0 already. That is why it may write to the log directly. If a project
ever performs disk access from another core, this must be looked at again
before anything else.
