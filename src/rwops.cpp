//
// rwops.cpp - SDL_RWops streams over files and over memory.
//
// SDL_RWops is how SDL hands an application a stream it can read without
// caring where the bytes are. Two of them exist here: one over a block of
// memory, and one over a file. The file one goes through the shim's own I/O
// service rather than stdio, because that service is valid from any core -
// FatFs and the card belong to core 0, and the service marshals - so an
// application running off core 0 can open a file with the same call.
//
// This file also carries SDL's heap entry points. They belong beside the
// stream code because they are the same contract: anything SDL allocates and
// hands back - a mapping string, an RWops - is released with SDL_free.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"

#include <cstring>
#include <cstdlib>

// ---- SDL's heap -------------------------------------------------------------

extern "C" void *SDL_malloc(size_t size)              { return malloc(size); }
extern "C" void *SDL_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
extern "C" void *SDL_realloc(void *mem, size_t size)   { return realloc(mem, size); }
extern "C" void  SDL_free(void *mem)                   { free(mem); }

// ---- allocation of the stream object itself ---------------------------------

extern "C" SDL_RWops *SDL_AllocRW(void)
{
    SDL_RWops *rw = (SDL_RWops *)malloc(sizeof(SDL_RWops));
    if (!rw)
    {
        SDL_SetError("out of memory");
        return nullptr;
    }
    memset(rw, 0, sizeof *rw);
    rw->type = SDL_RWOPS_UNKNOWN;
    return rw;
}

extern "C" void SDL_FreeRW(SDL_RWops *area)
{
    free(area);
}

// ---- the generic call wrappers ----------------------------------------------

extern "C" Sint64 SDL_RWsize(SDL_RWops *context)
{
    if (!context || !context->size)
        return SDL_SetError("stream is not readable");
    return context->size(context);
}

extern "C" Sint64 SDL_RWseek(SDL_RWops *context, Sint64 offset, int whence)
{
    if (!context || !context->seek)
        return SDL_SetError("stream is not seekable");
    return context->seek(context, offset, whence);
}

extern "C" Sint64 SDL_RWtell(SDL_RWops *context)
{
    if (!context || !context->seek)
        return SDL_SetError("stream is not seekable");
    return context->seek(context, 0, RW_SEEK_CUR);
}

extern "C" size_t SDL_RWread(SDL_RWops *context, void *ptr, size_t size, size_t maxnum)
{
    if (!context || !context->read)
        return 0;
    return context->read(context, ptr, size, maxnum);
}

extern "C" size_t SDL_RWwrite(SDL_RWops *context, const void *ptr, size_t size, size_t num)
{
    if (!context || !context->write)
        return 0;
    return context->write(context, ptr, size, num);
}

extern "C" int SDL_RWclose(SDL_RWops *context)
{
    if (!context)
        return SDL_SetError("no stream to close");
    return context->close ? context->close(context) : (SDL_FreeRW(context), 0);
}

// ---- a stream over memory ---------------------------------------------------

namespace
{

Sint64 SDLCALL mem_size(SDL_RWops *ctx)
{
    return (Sint64)(ctx->hidden.mem.stop - ctx->hidden.mem.base);
}

Sint64 SDLCALL mem_seek(SDL_RWops *ctx, Sint64 offset, int whence)
{
    Uint8 *newpos;
    switch (whence)
    {
    case RW_SEEK_SET: newpos = ctx->hidden.mem.base + offset; break;
    case RW_SEEK_CUR: newpos = ctx->hidden.mem.here + offset; break;
    case RW_SEEK_END: newpos = ctx->hidden.mem.stop + offset; break;
    default:          return SDL_SetError("unknown seek origin");
    }
    if (newpos < ctx->hidden.mem.base) newpos = ctx->hidden.mem.base;
    if (newpos > ctx->hidden.mem.stop) newpos = ctx->hidden.mem.stop;
    ctx->hidden.mem.here = newpos;
    return (Sint64)(newpos - ctx->hidden.mem.base);
}

size_t SDLCALL mem_read(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum)
{
    if (size == 0 || maxnum == 0)
        return 0;
    size_t avail = (size_t)(ctx->hidden.mem.stop - ctx->hidden.mem.here) / size;
    if (avail < maxnum)
        maxnum = avail;
    memcpy(ptr, ctx->hidden.mem.here, maxnum * size);
    ctx->hidden.mem.here += maxnum * size;
    return maxnum;
}

size_t SDLCALL mem_write(SDL_RWops *ctx, const void *ptr, size_t size, size_t num)
{
    if (size == 0 || num == 0)
        return 0;
    size_t room = (size_t)(ctx->hidden.mem.stop - ctx->hidden.mem.here) / size;
    if (room < num)
        num = room;
    memcpy(ctx->hidden.mem.here, ptr, num * size);
    ctx->hidden.mem.here += num * size;
    return num;
}

size_t SDLCALL mem_write_ro(SDL_RWops *, const void *, size_t, size_t)
{
    SDL_SetError("stream is read-only");
    return 0;
}

int SDLCALL mem_close(SDL_RWops *ctx)
{
    SDL_FreeRW(ctx);
    return 0;
}

// ---- a stream over a file ---------------------------------------------------
//
// The I/O service is offset-addressed, so the stream's own position lives
// here rather than in the handle.

struct FileStream
{
    int      handle;
    Uint64   size;
    Uint64   pos;
    bool     writable;
};

Sint64 SDLCALL file_size(SDL_RWops *ctx)
{
    FileStream *f = (FileStream *)ctx->hidden.unknown.data1;
    return (Sint64)f->size;
}

Sint64 SDLCALL file_seek(SDL_RWops *ctx, Sint64 offset, int whence)
{
    FileStream *f = (FileStream *)ctx->hidden.unknown.data1;
    Sint64 pos;
    switch (whence)
    {
    case RW_SEEK_SET: pos = offset; break;
    case RW_SEEK_CUR: pos = (Sint64)f->pos + offset; break;
    case RW_SEEK_END: pos = (Sint64)f->size + offset; break;
    default:          return SDL_SetError("unknown seek origin");
    }
    if (pos < 0)
        pos = 0;
    f->pos = (Uint64)pos;
    return pos;
}

size_t SDLCALL file_read(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum)
{
    FileStream *f = (FileStream *)ctx->hidden.unknown.data1;
    if (size == 0 || maxnum == 0)
        return 0;

    Uint64 left = f->pos < f->size ? f->size - f->pos : 0;
    size_t want = maxnum * size;
    if ((Uint64)want > left)
        want = (size_t)left;
    if (want == 0)
        return 0;

    long got = SDL2Circle_IORead(f->handle, ptr, f->pos, (uint32_t)want);
    if (got <= 0)
        return 0;
    f->pos += (Uint64)got;
    return (size_t)got / size;
}

size_t SDLCALL file_write(SDL_RWops *ctx, const void *ptr, size_t size, size_t num)
{
    FileStream *f = (FileStream *)ctx->hidden.unknown.data1;
    if (!f->writable || size == 0 || num == 0)
        return 0;

    long put = SDL2Circle_IOWrite(f->handle, ptr, f->pos, (uint32_t)(size * num));
    if (put <= 0)
        return 0;
    f->pos += (Uint64)put;
    if (f->pos > f->size)
        f->size = f->pos;
    return (size_t)put / size;
}

int SDLCALL file_close(SDL_RWops *ctx)
{
    FileStream *f = (FileStream *)ctx->hidden.unknown.data1;
    if (f)
    {
        SDL2Circle_IOClose(f->handle);
        free(f);
    }
    SDL_FreeRW(ctx);
    return 0;
}

} // namespace

extern "C" SDL_RWops *SDL_RWFromMem(void *mem, int size)
{
    if (!mem || size < 0)
    {
        SDL_SetError("no memory to read");
        return nullptr;
    }
    SDL_RWops *rw = SDL_AllocRW();
    if (!rw)
        return nullptr;
    rw->size  = mem_size;
    rw->seek  = mem_seek;
    rw->read  = mem_read;
    rw->write = mem_write;
    rw->close = mem_close;
    rw->type  = SDL_RWOPS_MEMORY;
    rw->hidden.mem.base = (Uint8 *)mem;
    rw->hidden.mem.here = rw->hidden.mem.base;
    rw->hidden.mem.stop = rw->hidden.mem.base + size;
    return rw;
}

extern "C" SDL_RWops *SDL_RWFromConstMem(const void *mem, int size)
{
    SDL_RWops *rw = SDL_RWFromMem((void *)mem, size);
    if (rw)
    {
        rw->write = mem_write_ro;
        rw->type  = SDL_RWOPS_MEMORY_RO;
    }
    return rw;
}

extern "C" SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
    if (!file || !mode)
    {
        SDL_SetError("no file to open");
        return nullptr;
    }

    // Only the access the underlying service offers: read, write, and write
    // with truncation. The 'b' every caller writes is meaningless here - a
    // bare-metal file has no text mode to be distinguished from.
    bool wants_write = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    unsigned flags = SDL2CIRCLE_IO_READ;
    if (wants_write)
    {
        flags |= SDL2CIRCLE_IO_WRITE;
        if (strchr(mode, 'w'))
            flags |= SDL2CIRCLE_IO_CREATE;
    }

    uint64_t size = 0;
    int handle = SDL2Circle_IOOpen(file, flags, &size);
    if (handle < 0)
    {
        SDL_SetError("cannot open %s", file);
        return nullptr;
    }

    FileStream *f = (FileStream *)malloc(sizeof(FileStream));
    SDL_RWops *rw = f ? SDL_AllocRW() : nullptr;
    if (!rw)
    {
        free(f);
        SDL2Circle_IOClose(handle);
        SDL_SetError("out of memory");
        return nullptr;
    }

    f->handle   = handle;
    f->size     = size;
    f->pos      = strchr(mode, 'a') ? size : 0;
    f->writable = wants_write;

    rw->size  = file_size;
    rw->seek  = file_seek;
    rw->read  = file_read;
    rw->write = file_write;
    rw->close = file_close;
    rw->type  = SDL_RWOPS_UNKNOWN;
    rw->hidden.unknown.data1 = f;
    rw->hidden.unknown.data2 = nullptr;
    return rw;
}

// ---------------------------------------------------------------------------
// Sized, endian-aware reads and writes
//
// SDL2 offers these so that a file format's byte order can be stated at the
// call site instead of being fought with per platform. On an AArch64 Pi the
// machine is little-endian, so the LE forms are plain reads and the BE forms
// swap; the code below says which is which explicitly rather than relying on
// that, because the statement is the point.
//
// A short read returns 0, which is SDL2's documented behaviour and the
// reason a loader must check its lengths rather than its values.
// ---------------------------------------------------------------------------

extern "C" Uint8 SDL_ReadU8(SDL_RWops *src)
{
    Uint8 value = 0;
    if (SDL_RWread(src, &value, sizeof(value), 1) != 1)
        return 0;
    return value;
}

extern "C" Uint16 SDL_ReadLE16(SDL_RWops *src)
{
    Uint16 value = 0;
    if (SDL_RWread(src, &value, sizeof(value), 1) != 1)
        return 0;
    return SDL_SwapLE16(value);
}

extern "C" Uint16 SDL_ReadBE16(SDL_RWops *src)
{
    Uint16 value = 0;
    if (SDL_RWread(src, &value, sizeof(value), 1) != 1)
        return 0;
    return SDL_SwapBE16(value);
}

extern "C" Uint32 SDL_ReadLE32(SDL_RWops *src)
{
    Uint32 value = 0;
    if (SDL_RWread(src, &value, sizeof(value), 1) != 1)
        return 0;
    return SDL_SwapLE32(value);
}

extern "C" Uint32 SDL_ReadBE32(SDL_RWops *src)
{
    Uint32 value = 0;
    if (SDL_RWread(src, &value, sizeof(value), 1) != 1)
        return 0;
    return SDL_SwapBE32(value);
}

extern "C" Uint64 SDL_ReadLE64(SDL_RWops *src)
{
    Uint64 value = 0;
    if (SDL_RWread(src, &value, sizeof(value), 1) != 1)
        return 0;
    return SDL_SwapLE64(value);
}

extern "C" Uint64 SDL_ReadBE64(SDL_RWops *src)
{
    Uint64 value = 0;
    if (SDL_RWread(src, &value, sizeof(value), 1) != 1)
        return 0;
    return SDL_SwapBE64(value);
}

extern "C" size_t SDL_WriteU8(SDL_RWops *dst, Uint8 value)
{
    return SDL_RWwrite(dst, &value, sizeof(value), 1);
}

extern "C" size_t SDL_WriteLE16(SDL_RWops *dst, Uint16 value)
{
    const Uint16 swapped = SDL_SwapLE16(value);
    return SDL_RWwrite(dst, &swapped, sizeof(swapped), 1);
}

extern "C" size_t SDL_WriteBE16(SDL_RWops *dst, Uint16 value)
{
    const Uint16 swapped = SDL_SwapBE16(value);
    return SDL_RWwrite(dst, &swapped, sizeof(swapped), 1);
}

extern "C" size_t SDL_WriteLE32(SDL_RWops *dst, Uint32 value)
{
    const Uint32 swapped = SDL_SwapLE32(value);
    return SDL_RWwrite(dst, &swapped, sizeof(swapped), 1);
}

extern "C" size_t SDL_WriteBE32(SDL_RWops *dst, Uint32 value)
{
    const Uint32 swapped = SDL_SwapBE32(value);
    return SDL_RWwrite(dst, &swapped, sizeof(swapped), 1);
}

extern "C" size_t SDL_WriteLE64(SDL_RWops *dst, Uint64 value)
{
    const Uint64 swapped = SDL_SwapLE64(value);
    return SDL_RWwrite(dst, &swapped, sizeof(swapped), 1);
}

extern "C" size_t SDL_WriteBE64(SDL_RWops *dst, Uint64 value)
{
    const Uint64 swapped = SDL_SwapBE64(value);
    return SDL_RWwrite(dst, &swapped, sizeof(swapped), 1);
}

// A whole file in memory, terminated with a zero byte that is not counted in
// the size - SDL's contract, and what lets the result be used directly as a
// string when the file is text. The caller releases it with SDL_free.
extern "C" void *SDL_LoadFile_RW(SDL_RWops *src, size_t *datasize, int freesrc)
{
    void *result = nullptr;

    if (!src)
    {
        SDL_InvalidParamError("src");
        goto done;
    }

    {
        const Sint64 here = SDL_RWtell(src);
        const Sint64 end = SDL_RWseek(src, 0, RW_SEEK_END);
        if (here < 0 || end < 0 || end < here)
        {
            SDL_SetError("SDL_LoadFile_RW: cannot measure the stream");
            goto done;
        }
        SDL_RWseek(src, here, RW_SEEK_SET);

        const size_t len = (size_t)(end - here);
        Uint8 *buf = (Uint8 *)SDL_malloc(len + 1);
        if (!buf)
        {
            SDL_OutOfMemory();
            goto done;
        }
        if (len && SDL_RWread(src, buf, 1, len) != len)
        {
            SDL_free(buf);
            SDL_SetError("SDL_LoadFile_RW: the stream ended early");
            goto done;
        }
        buf[len] = '\0';

        if (datasize)
            *datasize = len;
        result = buf;
    }

done:
    if (src && freesrc)
        SDL_RWclose(src);
    if (!result && datasize)
        *datasize = 0;
    return result;
}

extern "C" void *SDL_LoadFile(const char *file, size_t *datasize)
{
    SDL_RWops *src = SDL_RWFromFile(file, "rb");
    if (!src)
    {
        if (datasize)
            *datasize = 0;
        return nullptr;
    }
    return SDL_LoadFile_RW(src, datasize, 1);
}
