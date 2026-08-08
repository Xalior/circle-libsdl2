# circle-diskcache

A read cache for a Circle block device, and the measurement tool that tells
you what size to make it.

It is one header and one source file. You compile the source file into your
own kernel; there is no library to build and nothing to link.

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

Writes always reach the card before the call returns. Nothing is ever held in
memory waiting to be written. This is deliberate: these machines are switched
off without warning, and there is no shutdown step in which delayed writes
could be completed.

It also counts every request that passes through it, in both directions, and
writes a summary to Circle's log at a fixed interval. The summary is how you
choose the pool size, so leave it on while you are deciding and read it on the
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

## Memory

One allocation when you call `Configure`, from the low heap, and none after
that. The read path never allocates.

The low heap is chosen on purpose. On a board with more than one gigabyte, the
memory above that point is a separate heap. Staying in the low heap gives the
same behaviour on every board.

## How to use it in a Circle project

**1. Put the two files where your build can see them.** Add the directory
holding `diskcache.h` to your compiler's include path, and add
`diskcache.cpp` to the sources your kernel compiles.

**Add it to `OBJS` before you include Circle's `Rules.mk`, not after.**
`Rules.mk` reads `OBJS` at the moment it is included and derives its
dependency list from it, so anything added afterwards is invisible to it. This
is the one ordering rule that matters here, and getting it wrong produces a
build that looks fine until a header changes and nothing rebuilds.

`diskcache.cpp` also needs a compile rule of its own, because it lives outside
your project and a pattern rule only matches sources beside the makefile that
declares it:

```make
DISKCACHE_DIR = /path/to/circle-diskcache
OBJS         += $(OBJDIR)/diskcache.o
INCLUDE      += -I $(DISKCACHE_DIR)

$(OBJDIR)/diskcache.o: $(DISKCACHE_DIR)/diskcache.cpp | $(OBJDIR)
	@$(CPP) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<
```

If your project leaves `CHECK_DEPS` at Circle's default of 1, Circle generates
dependency files itself with its own `%.d` pattern rules — and those are
relative too, so they will not match this source either. Give it a second rule
beside the first:

```make
$(OBJDIR)/diskcache.d: $(DISKCACHE_DIR)/diskcache.cpp | $(OBJDIR)
	@$(CPP) $(CPPFLAGS) -M -MG -MT $(OBJDIR)/diskcache.o -MT $@ -MF $@ $<
```

A project that sets `CHECK_DEPS = 0` and puts `-MD -MP` in its own compile
line needs only the first rule, since it never asks for a separate dependency
file.

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
m_DiskCache.Configure (nKilobytes);
```

Zero means no cache: every read reaches the card and only the counting
continues. That is the setting to compare every other size against.

Doing this as a separate step lets you take the size from wherever your
project reads its settings, which may be later in startup than `Install`. The
reads made by mounting the card happen before this point and are not cached.

**5. Call `Poll` regularly from core 0.**

```cpp
while (bRunning)
{
    m_DiskCache.Poll ();
    m_Scheduler.Yield ();
}
```

`Poll` writes the summary when the interval has passed, and otherwise reads
the clock and returns. Call it from a place that is allowed to write to the
log. Do not call it from inside a call another processor core is waiting for.

`Report` writes the summary immediately, which is useful once more before the
board stops.

## Choosing the size

Run your program at several sizes and read two figures from the summary.

`hits` is the share of read requests answered from memory.

`cache` includes a high-water mark: the largest amount of the pool that was
ever in use. **This is the figure that tells you when to stop.** If your
program never fills more than a small part of the pool, every larger size will
report the same hit rate, and the answer for that program is the smallest size
that still holds everything it re-reads.

The right size differs from one program to the next, which is why it is worth
setting per program rather than choosing one value for everything.

## Where it runs

Circle's devices belong to core 0, so every call into this class arrives on
core 0 already. That is why it may write to the log directly. If a project
ever performs disk access from another core, this must be looked at again
before anything else.
