//
// bmp.cpp - SDL_LoadBMP_RW and SDL_SaveBMP_RW
//
// BMP is the one image format that is part of core SDL2 rather than of
// SDL_image, and it is the format a great many games keep their fonts,
// their menus and their title screens in, because it needs no decoder.
//
// What is read here: the Windows BITMAPINFOHEADER family (and the older
// BITMAPCOREHEADER), uncompressed at 1, 4, 8, 16, 24 and 32 bits per pixel,
// plus BI_BITFIELDS where the file states its own channel masks. Run-length
// encoded BMPs (BI_RLE4, BI_RLE8) are rejected with a clear error rather
// than half-decoded: they are rare in game data and a wrong guess at one
// produces a plausible-looking corrupt image, which is worse than a refusal.
//
// Rows in a BMP are stored bottom-up unless the height is negative, and
// padded to a four-byte boundary. Both are handled here, so a surface comes
// back the right way up whichever way the file was written.
//
#include "pixels.h"
#include "sdl2circle.h"

#include <cstdlib>
#include <cstring>

namespace
{
constexpr Uint32 BI_RGB       = 0;
constexpr Uint32 BI_RLE8      = 1;
constexpr Uint32 BI_RLE4      = 2;
constexpr Uint32 BI_BITFIELDS = 3;

// The whole file is little-endian, whatever the machine is.
Uint16 read_le16(SDL_RWops *src) { return SDL_ReadLE16(src); }
Uint32 read_le32(SDL_RWops *src) { return SDL_ReadLE32(src); }
} // namespace

extern "C" SDL_Surface *SDL_LoadBMP_RW(SDL_RWops *src, int freesrc)
{
    SDL_Surface *surface = nullptr;
    Uint8       *palette_bytes = nullptr;

    auto finish = [&](SDL_Surface *result) -> SDL_Surface * {
        free(palette_bytes);
        if (freesrc && src != nullptr)
            SDL_RWclose(src);
        if (result == nullptr && surface != nullptr)
            SDL_FreeSurface(surface);
        return result;
    };

    if (src == nullptr)
    {
        SDL_InvalidParamError("src");
        return finish(nullptr);
    }

    const Sint64 start = SDL_RWtell(src);

    // -- file header -------------------------------------------------------
    char magic[2];
    if (SDL_RWread(src, magic, 1, 2) != 2 || magic[0] != 'B' || magic[1] != 'M')
    {
        SDL_SetError("not a BMP file");
        return finish(nullptr);
    }
    read_le32(src);                       // file size, not trusted
    read_le16(src);                       // reserved
    read_le16(src);                       // reserved
    const Uint32 data_offset = read_le32(src);

    // -- info header -------------------------------------------------------
    const Uint32 header_size = read_le32(src);
    Sint32 width, height;
    Uint16 planes, bpp;
    Uint32 compression = BI_RGB;
    Uint32 palette_entries = 0;

    if (header_size == 12)
    {
        // BITMAPCOREHEADER: 16-bit dimensions, no compression field.
        width  = (Sint16)read_le16(src);
        height = (Sint16)read_le16(src);
        planes = read_le16(src);
        bpp    = read_le16(src);
    }
    else if (header_size >= 40)
    {
        width       = (Sint32)read_le32(src);
        height      = (Sint32)read_le32(src);
        planes      = read_le16(src);
        bpp         = read_le16(src);
        compression = read_le32(src);
        read_le32(src);                   // image size
        read_le32(src);                   // x pixels per metre
        read_le32(src);                   // y pixels per metre
        palette_entries = read_le32(src);
        read_le32(src);                   // important colours
    }
    else
    {
        SDL_SetError("unsupported BMP header size %u", (unsigned)header_size);
        return finish(nullptr);
    }

    if (planes != 1)
    {
        SDL_SetError("BMP with %u planes is not supported", (unsigned)planes);
        return finish(nullptr);
    }
    if (compression == BI_RLE4 || compression == BI_RLE8)
    {
        SDL_SetError("run-length encoded BMP files are not supported");
        return finish(nullptr);
    }
    if (compression != BI_RGB && compression != BI_BITFIELDS)
    {
        SDL_SetError("BMP compression %u is not supported", (unsigned)compression);
        return finish(nullptr);
    }
    if (width <= 0)
    {
        SDL_SetError("BMP width must be positive");
        return finish(nullptr);
    }

    const bool bottom_up = (height > 0);
    if (height < 0)
        height = -height;
    if (height == 0)
    {
        SDL_SetError("BMP height must not be zero");
        return finish(nullptr);
    }

    // -- channel masks -----------------------------------------------------
    Uint32 rmask = 0, gmask = 0, bmask = 0, amask = 0;
    if (compression == BI_BITFIELDS)
    {
        rmask = read_le32(src);
        gmask = read_le32(src);
        bmask = read_le32(src);
        // A BITMAPV4HEADER and later carries an alpha mask as well; an
        // ordinary BITMAPINFOHEADER with BI_BITFIELDS does not.
        if (header_size >= 56)
            amask = read_le32(src);
    }
    else
    {
        switch (bpp)
        {
        case 16: rmask = 0x7C00; gmask = 0x03E0; bmask = 0x001F; break;
        case 24: rmask = 0xFF0000; gmask = 0x00FF00; bmask = 0x0000FF; break;
        case 32: rmask = 0x00FF0000; gmask = 0x0000FF00; bmask = 0x000000FF; break;
        default: break;   // indexed: masks stay zero
        }
    }

    // -- destination surface ----------------------------------------------
    const bool indexed = (bpp <= 8);
    Uint32 format;
    if (indexed)
    {
        format = SDL_PIXELFORMAT_INDEX8;   // 1 and 4 bpp are expanded on read
    }
    else
    {
        format = SDL_MasksToPixelFormatEnum(bpp == 32 ? 32 : bpp,
                                            rmask, gmask, bmask, amask);
        if (format == SDL_PIXELFORMAT_UNKNOWN)
        {
            SDL_SetError("BMP with %u bits and masks %08x/%08x/%08x is not "
                         "a format this library can describe",
                         (unsigned)bpp, (unsigned)rmask, (unsigned)gmask,
                         (unsigned)bmask);
            return finish(nullptr);
        }
    }

    surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 0, format);
    if (surface == nullptr)
        return finish(nullptr);

    // -- palette -----------------------------------------------------------
    if (indexed)
    {
        const int entry_size = (header_size == 12) ? 3 : 4;
        if (palette_entries == 0)
            palette_entries = 1u << bpp;
        if (palette_entries > 256)
            palette_entries = 256;

        palette_bytes = (Uint8 *)malloc((size_t)palette_entries * entry_size);
        if (palette_bytes == nullptr)
        {
            SDL_SetError("out of memory reading BMP palette");
            return finish(nullptr);
        }
        SDL_RWseek(src, start + 14 + header_size, RW_SEEK_SET);
        if (SDL_RWread(src, palette_bytes, entry_size,
                       (size_t)palette_entries) != (size_t)palette_entries)
        {
            SDL_SetError("truncated BMP palette");
            return finish(nullptr);
        }

        SDL_Palette *palette = surface->format->palette;
        for (Uint32 i = 0; i < palette_entries && (int)i < palette->ncolors; i++)
        {
            const Uint8 *e = palette_bytes + (size_t)i * entry_size;
            palette->colors[i].b = e[0];   // BMP stores BGR, not RGB
            palette->colors[i].g = e[1];
            palette->colors[i].r = e[2];
            palette->colors[i].a = SDL_ALPHA_OPAQUE;
        }
    }

    // -- pixels ------------------------------------------------------------
    if (SDL_RWseek(src, start + data_offset, RW_SEEK_SET) < 0)
    {
        SDL_SetError("cannot seek to BMP pixel data");
        return finish(nullptr);
    }

    const size_t src_row_bytes = (((size_t)width * bpp + 31) / 32) * 4;
    Uint8 *row = (Uint8 *)malloc(src_row_bytes);
    if (row == nullptr)
    {
        SDL_SetError("out of memory reading BMP rows");
        return finish(nullptr);
    }

    const int dst_bpp = surface->format->BytesPerPixel;
    for (int y = 0; y < height; y++)
    {
        if (SDL_RWread(src, row, 1, src_row_bytes) != src_row_bytes)
        {
            free(row);
            SDL_SetError("truncated BMP pixel data");
            return finish(nullptr);
        }
        const int dst_y = bottom_up ? (height - 1 - y) : y;
        Uint8 *dst = (Uint8 *)surface->pixels + (size_t)dst_y * surface->pitch;

        switch (bpp)
        {
        case 1:
            for (int x = 0; x < width; x++)
                dst[x] = (Uint8)((row[x >> 3] >> (7 - (x & 7))) & 1);
            break;
        case 4:
            for (int x = 0; x < width; x++)
                dst[x] = (Uint8)((x & 1) ? (row[x >> 1] & 0x0F)
                                         : (row[x >> 1] >> 4));
            break;
        case 8:
            memcpy(dst, row, (size_t)width);
            break;
        default:
            memcpy(dst, row, (size_t)width * dst_bpp);
            break;
        }
    }
    free(row);

    return finish(surface);
}

// SDL_LoadBMP and SDL_SaveBMP are macros over these in SDL_surface.h; there
// is no separate function to provide.

extern "C" int SDL_SaveBMP_RW(SDL_Surface *surface, SDL_RWops *dst, int freedst)
{
    int          rc      = -1;
    SDL_Surface *convert = nullptr;

    auto finish = [&](int result) -> int {
        if (convert != nullptr)
            SDL_FreeSurface(convert);
        if (freedst && dst != nullptr)
            SDL_RWclose(dst);
        return result;
    };

    if (surface == nullptr)
        return finish(SDL_InvalidParamError("surface"));
    if (dst == nullptr)
        return finish(SDL_InvalidParamError("dst"));

    // Written as 24-bit BGR, which every BMP reader in existence accepts.
    // An 8-bit surface keeps its palette instead, because a paletted image
    // written as truecolour stops being the thing the caller saved.
    SDL_Surface *save = surface;
    const bool   indexed = (surface->format->palette != nullptr &&
                            surface->format->BitsPerPixel == 8);
    if (!indexed && surface->format->format != SDL_PIXELFORMAT_BGR24)
    {
        convert = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_BGR24, 0);
        if (convert == nullptr)
            return finish(-1);
        save = convert;
    }

    const Uint16 bpp             = indexed ? 8 : 24;
    const Uint32 palette_entries = indexed ? 256 : 0;
    const size_t row_bytes       = (((size_t)save->w * bpp + 31) / 32) * 4;
    const Uint32 pixel_offset    = (Uint32)(14 + 40 + palette_entries * 4);
    const Uint32 file_size       = (Uint32)(pixel_offset + row_bytes * save->h);

    SDL_RWwrite(dst, "BM", 1, 2);
    SDL_WriteLE32(dst, file_size);
    SDL_WriteLE16(dst, 0);
    SDL_WriteLE16(dst, 0);
    SDL_WriteLE32(dst, pixel_offset);

    SDL_WriteLE32(dst, 40);
    SDL_WriteLE32(dst, (Uint32)save->w);
    SDL_WriteLE32(dst, (Uint32)save->h);      // positive: bottom-up
    SDL_WriteLE16(dst, 1);
    SDL_WriteLE16(dst, bpp);
    SDL_WriteLE32(dst, BI_RGB);
    SDL_WriteLE32(dst, (Uint32)(row_bytes * save->h));
    SDL_WriteLE32(dst, 2835);                 // 72 dpi, in pixels per metre
    SDL_WriteLE32(dst, 2835);
    SDL_WriteLE32(dst, palette_entries);
    SDL_WriteLE32(dst, 0);

    if (indexed)
    {
        const SDL_Palette *palette = save->format->palette;
        for (Uint32 i = 0; i < palette_entries; i++)
        {
            Uint8 entry[4] = { 0, 0, 0, 0 };
            if ((int)i < palette->ncolors)
            {
                entry[0] = palette->colors[i].b;
                entry[1] = palette->colors[i].g;
                entry[2] = palette->colors[i].r;
            }
            SDL_RWwrite(dst, entry, 1, 4);
        }
    }

    Uint8 *row = (Uint8 *)calloc(1, row_bytes);
    if (row == nullptr)
    {
        SDL_SetError("out of memory writing BMP rows");
        return finish(-1);
    }
    const size_t used = (size_t)save->w * (indexed ? 1 : 3);
    for (int y = save->h - 1; y >= 0; y--)
    {
        memcpy(row, (const Uint8 *)save->pixels + (size_t)y * save->pitch, used);
        if (SDL_RWwrite(dst, row, 1, row_bytes) != row_bytes)
        {
            free(row);
            SDL_SetError("short write saving BMP");
            return finish(-1);
        }
    }
    free(row);
    rc = 0;
    return finish(rc);
}
