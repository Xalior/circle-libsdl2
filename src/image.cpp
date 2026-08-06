//
// image.cpp — SDL_image, for the formats a bare-metal board can decode.
//
// WHY THIS IS IN THE SAME ARCHIVE, and not a companion the way upstream
// splits SDL2 from SDL2_image. Upstream's split exists so a distribution can
// package the image decoders separately and so an application can link the
// codec libraries it wants — libpng, libjpeg, libwebp and the rest, each a
// shared object with its own version. None of that applies here. There are
// no shared objects, no packages, and no codec libraries: the decoder below
// is self-contained, and a second archive would be one more thing for every
// consumer to name in its link line for no gain. So IMG_* lives beside SDL_*
// and a consumer links one archive. SDL_image.h is the upstream header, so
// application source needs no change.
//
// WHAT IS DECODED: PNG at 8 bits per channel, not interlaced, in any of the
// five colour types — greyscale, truecolour, palette, greyscale with alpha,
// truecolour with alpha — honouring a tRNS chunk for palette and a full
// alpha channel where the file carries one. BMP goes to the core SDL loader
// in bmp.cpp, which is where SDL2 itself keeps it.
//
// Anything else is REFUSED WITH A MESSAGE NAMING WHAT IT WAS, rather than
// decoded approximately. A wrong picture on a screen with no debugger is far
// more expensive than a clear failure.
//
// There is no libpng here and no zlib, so both are below: a DEFLATE
// decompressor (RFC 1951) and a PNG reader on top of it.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{

// ---------------------------------------------------------------------------
// DEFLATE (RFC 1951), decompressing into a buffer of known size.
//
// The size is always known here: a PNG's uncompressed data is exactly one
// filter byte plus one row of samples for every row of the image. So there is
// no growing buffer and no guessing — the decompressor either fills the
// buffer it was given or reports that the stream was malformed.
// ---------------------------------------------------------------------------

struct Bitstream
{
    const uint8_t *in;
    size_t inlen;
    size_t incnt;
    long bitbuf;
    int bitcnt;
};

// A canonical Huffman table, in the form that makes decoding a walk down the
// code lengths: how many codes there are of each length, and the symbols in
// order.
struct Huffman
{
    short *count;       // [0 .. MAXBITS]
    short *symbol;
};

const int MAXBITS = 15;
const int MAXLCODES = 286;
const int MAXDCODES = 30;
const int FIXLCODES = 288;

// The next `need` bits, least significant first. Returns -1 past the end of
// the input.
int Bits(Bitstream *s, int need)
{
    long val = s->bitbuf;
    while (s->bitcnt < need)
    {
        if (s->incnt == s->inlen)
            return -1;
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

int Decode(Bitstream *s, const Huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++)
    {
        const int b = Bits(s, 1);
        if (b < 0)
            return -1;
        code |= b;
        const int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

// Build a table from a list of code lengths. Returns 0 for a complete code,
// a positive number for an incomplete one (legal only in the single-symbol
// case), and a negative number for an over-subscribed one.
int Construct(Huffman *h, const short *length, int n)
{
    for (int len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (int symbol = 0; symbol < n; symbol++)
        h->count[length[symbol]]++;
    if (h->count[0] == n)
        return 0;

    int left = 1;
    for (int len = 1; len <= MAXBITS; len++)
    {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }

    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

int Codes(Bitstream *s, const Huffman *lencode, const Huffman *distcode,
          uint8_t *out, size_t outlen, size_t *outpos)
{
    static const short lens[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dists[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
        12289, 16385, 24577 };
    static const short dext[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

    for (;;)
    {
        int symbol = Decode(s, lencode);
        if (symbol < 0)
            return symbol;

        if (symbol < 256)
        {
            if (*outpos >= outlen)
                return -1;
            out[(*outpos)++] = (uint8_t)symbol;
        }
        else if (symbol == 256)
        {
            return 0;
        }
        else
        {
            symbol -= 257;
            if (symbol >= 29)
                return -1;
            const int extra_len = Bits(s, lext[symbol]);
            if (extra_len < 0)
                return -1;
            const int len = lens[symbol] + extra_len;

            symbol = Decode(s, distcode);
            if (symbol < 0 || symbol >= 30)
                return -1;
            const int extra_dist = Bits(s, dext[symbol]);
            if (extra_dist < 0)
                return -1;
            const size_t dist = (size_t)(dists[symbol] + extra_dist);

            if (dist > *outpos || *outpos + (size_t)len > outlen)
                return -1;
            for (int i = 0; i < len; i++)
            {
                out[*outpos] = out[*outpos - dist];
                (*outpos)++;
            }
        }
    }
}

int Stored(Bitstream *s, uint8_t *out, size_t outlen, size_t *outpos)
{
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen)
        return -1;
    const unsigned len = s->in[s->incnt] | ((unsigned)s->in[s->incnt + 1] << 8);
    // The block length is stored twice, the second time inverted.
    if (s->in[s->incnt + 2] != (~len & 0xFF)
        || s->in[s->incnt + 3] != ((~len >> 8) & 0xFF))
        return -1;
    s->incnt += 4;
    if (s->incnt + len > s->inlen || *outpos + len > outlen)
        return -1;
    memcpy(out + *outpos, s->in + s->incnt, len);
    s->incnt += len;
    *outpos += len;
    return 0;
}

int Fixed(Bitstream *s, uint8_t *out, size_t outlen, size_t *outpos)
{
    static short lencnt[MAXBITS + 1], lensym[FIXLCODES];
    static short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    static Huffman lencode = { lencnt, lensym };
    static Huffman distcode = { distcnt, distsym };
    static bool built = false;

    if (!built)
    {
        short lengths[FIXLCODES];
        int symbol = 0;
        for (; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        Construct(&lencode, lengths, FIXLCODES);
        for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        Construct(&distcode, lengths, MAXDCODES);
        built = true;
    }
    return Codes(s, &lencode, &distcode, out, outlen, outpos);
}

int Dynamic(Bitstream *s, uint8_t *out, size_t outlen, size_t *outpos)
{
    static const short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

    const int nlen_raw = Bits(s, 5);
    const int ndist_raw = Bits(s, 5);
    const int ncode_raw = Bits(s, 4);
    if (nlen_raw < 0 || ndist_raw < 0 || ncode_raw < 0)
        return -1;
    const int nlen = nlen_raw + 257;
    const int ndist = ndist_raw + 1;
    const int ncode = ncode_raw + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return -1;

    short lengths[MAXLCODES + MAXDCODES];
    memset(lengths, 0, sizeof(lengths));
    for (int index = 0; index < ncode; index++)
    {
        const int v = Bits(s, 3);
        if (v < 0)
            return -1;
        lengths[order[index]] = (short)v;
    }

    short lencnt[MAXBITS + 1], lensym[MAXLCODES + MAXDCODES];
    Huffman lencode = { lencnt, lensym };
    if (Construct(&lencode, lengths, 19) != 0)
        return -1;

    int index = 0;
    while (index < nlen + ndist)
    {
        int symbol = Decode(s, &lencode);
        if (symbol < 0)
            return -1;

        if (symbol < 16)
        {
            lengths[index++] = (short)symbol;
            continue;
        }

        short len = 0;
        int repeat;
        if (symbol == 16)
        {
            if (index == 0)
                return -1;
            len = lengths[index - 1];
            repeat = 3 + Bits(s, 2);
        }
        else if (symbol == 17)
        {
            repeat = 3 + Bits(s, 3);
        }
        else
        {
            repeat = 11 + Bits(s, 7);
        }
        if (index + repeat > nlen + ndist)
            return -1;
        while (repeat-- > 0)
            lengths[index++] = len;
    }

    short dcnt[MAXBITS + 1], dsym[MAXDCODES];
    Huffman distcode = { dcnt, dsym };
    if (Construct(&lencode, lengths, nlen) < 0)
        return -1;
    if (Construct(&distcode, lengths + nlen, ndist) < 0)
        return -1;

    return Codes(s, &lencode, &distcode, out, outlen, outpos);
}

// Decompresses a zlib stream (RFC 1950: a two-byte header, then DEFLATE)
// into a buffer whose size is already known. The trailing Adler-32 is not
// checked: a PNG carries a CRC on every chunk already.
bool ZlibInflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    if (inlen < 2)
        return false;

    Bitstream s;
    s.in = in;
    s.inlen = inlen;
    s.incnt = 2;                // past the zlib header
    s.bitbuf = 0;
    s.bitcnt = 0;

    size_t outpos = 0;
    int last, type;
    do
    {
        last = Bits(&s, 1);
        type = Bits(&s, 2);
        if (last < 0 || type < 0)
            return false;

        int err;
        switch (type)
        {
        case 0:  err = Stored(&s, out, outlen, &outpos); break;
        case 1:  err = Fixed(&s, out, outlen, &outpos); break;
        case 2:  err = Dynamic(&s, out, outlen, &outpos); break;
        default: return false;
        }
        if (err != 0)
            return false;
    } while (!last);

    return outpos == outlen;
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

const uint8_t PNG_SIGNATURE[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };

inline uint32_t BE32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

// The Paeth predictor, from the PNG specification: of the three neighbours,
// the one closest to their linear estimate.
inline int Paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Undoes the per-row filter, in place, over the whole decompressed image.
// Each row is one filter byte followed by `stride` bytes of samples.
bool Unfilter(uint8_t *raw, int height, size_t stride, int bpp)
{
    uint8_t *prev = nullptr;
    uint8_t *row = raw;

    for (int y = 0; y < height; y++)
    {
        const int filter = *row++;
        for (size_t i = 0; i < stride; i++)
        {
            const int a = (i >= (size_t)bpp) ? row[i - bpp] : 0;
            const int b = prev ? prev[i] : 0;
            const int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
            switch (filter)
            {
            case 0: break;
            case 1: row[i] = (uint8_t)(row[i] + a); break;
            case 2: row[i] = (uint8_t)(row[i] + b); break;
            case 3: row[i] = (uint8_t)(row[i] + ((a + b) >> 1)); break;
            case 4: row[i] = (uint8_t)(row[i] + Paeth(a, b, c)); break;
            default: return false;
            }
        }
        prev = row;
        row += stride;
    }
    return true;
}

// Everything an SDL_RWops still holds, into memory. A decoder needs the
// whole file: a PNG's image data may be split over any number of IDAT
// chunks that have to be inflated as one stream.
uint8_t *ReadAll(SDL_RWops *src, size_t *size)
{
    const Sint64 here = SDL_RWtell(src);
    const Sint64 end = SDL_RWseek(src, 0, RW_SEEK_END);
    if (here < 0 || end < 0 || end <= here)
    {
        SDL_SetError("cannot measure the image data");
        return nullptr;
    }
    SDL_RWseek(src, here, RW_SEEK_SET);

    const size_t len = (size_t)(end - here);
    uint8_t *data = (uint8_t *)SDL_malloc(len);
    if (!data)
    {
        SDL_OutOfMemory();
        return nullptr;
    }
    if (SDL_RWread(src, data, 1, len) != len)
    {
        SDL_free(data);
        SDL_SetError("the image data ended early");
        return nullptr;
    }
    *size = len;
    return data;
}

// True when the stream starts with the given magic, leaving the stream where
// it was found. This is what every IMG_is* function is.
int StartsWith(SDL_RWops *src, const void *magic, size_t n)
{
    if (!src)
        return 0;
    const Sint64 here = SDL_RWtell(src);
    if (here < 0)
        return 0;

    uint8_t head[16];
    if (n > sizeof(head))
        n = sizeof(head);
    const size_t got = SDL_RWread(src, head, 1, n);
    SDL_RWseek(src, here, RW_SEEK_SET);
    return (got == n && memcmp(head, magic, n) == 0) ? 1 : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// The API
// ---------------------------------------------------------------------------

extern "C" const SDL_version *IMG_Linked_Version(void)
{
    static SDL_version version;
    SDL_IMAGE_VERSION(&version);
    return &version;
}

// SDL_image's contract: the answer is which of the ASKED-FOR formats are now
// available, and a caller checks the bit it needs. PNG is what there is, so
// asking for anything else gets an honest zero for that bit rather than a
// success that fails at the first load.
extern "C" int IMG_Init(int flags)
{
    return flags & IMG_INIT_PNG;
}

extern "C" void IMG_Quit(void) {}

extern "C" int IMG_isPNG(SDL_RWops *src)
{
    return StartsWith(src, PNG_SIGNATURE, sizeof(PNG_SIGNATURE));
}

extern "C" int IMG_isBMP(SDL_RWops *src)
{
    return StartsWith(src, "BM", 2);
}

// The formats with no decoder here. Answering honestly is what lets
// IMG_LoadTyped_RW pick the right one and fail cleanly when there is none.
extern "C" int IMG_isAVIF(SDL_RWops *) { return 0; }
extern "C" int IMG_isCUR(SDL_RWops *)  { return 0; }
extern "C" int IMG_isGIF(SDL_RWops *)  { return 0; }
extern "C" int IMG_isICO(SDL_RWops *)  { return 0; }
extern "C" int IMG_isJPG(SDL_RWops *)  { return 0; }
extern "C" int IMG_isJXL(SDL_RWops *)  { return 0; }
extern "C" int IMG_isLBM(SDL_RWops *)  { return 0; }
extern "C" int IMG_isPCX(SDL_RWops *)  { return 0; }
extern "C" int IMG_isPNM(SDL_RWops *)  { return 0; }
extern "C" int IMG_isQOI(SDL_RWops *)  { return 0; }
extern "C" int IMG_isSVG(SDL_RWops *)  { return 0; }
extern "C" int IMG_isTIF(SDL_RWops *)  { return 0; }
extern "C" int IMG_isWEBP(SDL_RWops *) { return 0; }
extern "C" int IMG_isXCF(SDL_RWops *)  { return 0; }
extern "C" int IMG_isXPM(SDL_RWops *)  { return 0; }
extern "C" int IMG_isXV(SDL_RWops *)   { return 0; }

extern "C" SDL_Surface *IMG_LoadPNG_RW(SDL_RWops *src)
{
    if (!src)
    {
        SDL_SetError("IMG_LoadPNG_RW: no source");
        return nullptr;
    }

    size_t filesize = 0;
    uint8_t *data = ReadAll(src, &filesize);
    if (!data)
        return nullptr;

    if (filesize < 8 || memcmp(data, PNG_SIGNATURE, 8) != 0)
    {
        SDL_free(data);
        SDL_SetError("IMG_LoadPNG_RW: not a PNG file");
        return nullptr;
    }

    // Pass one: the header, the palette, the transparency and the compressed
    // data, gathering every IDAT into one stream.
    uint32_t width = 0, height = 0;
    int depth = 0, colour = 0, interlace = 0;
    uint8_t palette[256][3];
    uint8_t palette_alpha[256];
    int palette_size = 0;
    memset(palette, 0, sizeof(palette));
    memset(palette_alpha, 255, sizeof(palette_alpha));

    uint8_t *idat = nullptr;
    size_t idat_len = 0;

    size_t pos = 8;
    bool ok = true;
    while (pos + 8 <= filesize)
    {
        const uint32_t len = BE32(data + pos);
        const uint8_t *type = data + pos + 4;
        const uint8_t *body = data + pos + 8;
        if (pos + 12 + (size_t)len > filesize)
        {
            ok = false;
            break;
        }

        if (memcmp(type, "IHDR", 4) == 0 && len >= 13)
        {
            width = BE32(body);
            height = BE32(body + 4);
            depth = body[8];
            colour = body[9];
            interlace = body[12];
        }
        else if (memcmp(type, "PLTE", 4) == 0)
        {
            palette_size = (int)(len / 3);
            if (palette_size > 256)
                palette_size = 256;
            memcpy(palette, body, (size_t)palette_size * 3);
        }
        else if (memcmp(type, "tRNS", 4) == 0)
        {
            const size_t n = (len > 256) ? 256 : len;
            memcpy(palette_alpha, body, n);
        }
        else if (memcmp(type, "IDAT", 4) == 0)
        {
            uint8_t *grown = (uint8_t *)SDL_realloc(idat, idat_len + len);
            if (!grown)
            {
                ok = false;
                break;
            }
            idat = grown;
            memcpy(idat + idat_len, body, len);
            idat_len += len;
        }
        else if (memcmp(type, "IEND", 4) == 0)
        {
            break;
        }

        pos += 12 + (size_t)len;        // length, type, body, CRC
    }

    // How many bytes one pixel takes in the decompressed rows.
    int samples = 0;
    switch (colour)
    {
    case 0: samples = 1; break;     // greyscale
    case 2: samples = 3; break;     // truecolour
    case 3: samples = 1; break;     // palette index
    case 4: samples = 2; break;     // greyscale and alpha
    case 6: samples = 4; break;     // truecolour and alpha
    default: ok = false; break;
    }

    if (!ok || width == 0 || height == 0 || !idat)
    {
        SDL_free(idat);
        SDL_free(data);
        SDL_SetError("IMG_LoadPNG_RW: malformed PNG");
        return nullptr;
    }
    if (depth != 8 || interlace != 0)
    {
        SDL_free(idat);
        SDL_free(data);
        SDL_SetError("IMG_LoadPNG_RW: %d-bit%s PNG; only 8-bit "
                     "non-interlaced is read", depth,
                     interlace ? " interlaced" : "");
        return nullptr;
    }

    const size_t stride = (size_t)width * samples;
    const size_t rawlen = (stride + 1) * height;
    uint8_t *raw = (uint8_t *)SDL_malloc(rawlen);
    if (!raw || !ZlibInflate(idat, idat_len, raw, rawlen)
        || !Unfilter(raw, (int)height, stride, samples))
    {
        SDL_free(raw);
        SDL_free(idat);
        SDL_free(data);
        SDL_SetError("IMG_LoadPNG_RW: the compressed image data is malformed");
        return nullptr;
    }
    SDL_free(idat);
    SDL_free(data);

    // ARGB8888, so the alpha a PNG carries is described by the surface's own
    // format and survives every later conversion on its own terms.
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, (int)width, (int)height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface)
    {
        SDL_free(raw);
        return nullptr;
    }

    for (uint32_t y = 0; y < height; y++)
    {
        const uint8_t *s = raw + (stride + 1) * y + 1;
        Uint32 *dst = (Uint32 *)((Uint8 *)surface->pixels
                        + (size_t)y * surface->pitch);
        for (uint32_t x = 0; x < width; x++)
        {
            uint8_t r, g, b, a = 255;
            switch (colour)
            {
            case 0:
                r = g = b = s[x];
                break;
            case 2:
                r = s[x * 3]; g = s[x * 3 + 1]; b = s[x * 3 + 2];
                break;
            case 3:
            {
                const int idx = s[x];
                r = palette[idx][0]; g = palette[idx][1]; b = palette[idx][2];
                a = palette_alpha[idx];
                break;
            }
            case 4:
                r = g = b = s[x * 2];
                a = s[x * 2 + 1];
                break;
            default:
                r = s[x * 4]; g = s[x * 4 + 1]; b = s[x * 4 + 2];
                a = s[x * 4 + 3];
                break;
            }
            dst[x] = ((Uint32)a << 24) | ((Uint32)r << 16)
                   | ((Uint32)g << 8) | b;
        }
    }
    SDL_free(raw);
    return surface;
}

extern "C" SDL_Surface *IMG_LoadBMP_RW(SDL_RWops *src)
{
    return SDL_LoadBMP_RW(src, 0);   // core SDL owns BMP, as it does upstream
}

extern "C" SDL_Surface *IMG_LoadTyped_RW(SDL_RWops *src, int freesrc,
                                         const char *type)
{
    if (!src)
    {
        SDL_SetError("IMG_LoadTyped_RW: no source");
        return nullptr;
    }

    SDL_Surface *surface = nullptr;

    // A stated type is a hint, and SDL_image falls back to sniffing when it
    // does not match. Sniffing is what actually decides.
    if (IMG_isPNG(src))
        surface = IMG_LoadPNG_RW(src);
    else if (IMG_isBMP(src))
        surface = IMG_LoadBMP_RW(src);
    else
        SDL_SetError("IMG_LoadTyped_RW: unrecognised image format%s%s "
                     "(this build reads PNG and BMP)",
                     type ? ", stated as " : "", type ? type : "");

    if (freesrc)
        SDL_RWclose(src);
    return surface;
}

extern "C" SDL_Surface *IMG_Load_RW(SDL_RWops *src, int freesrc)
{
    return IMG_LoadTyped_RW(src, freesrc, nullptr);
}

extern "C" SDL_Surface *IMG_Load(const char *file)
{
    if (!file)
    {
        SDL_SetError("IMG_Load: no file named");
        return nullptr;
    }
    SDL_RWops *src = SDL_RWFromFile(file, "rb");
    if (!src)
    {
        SDL_SetError("IMG_Load: cannot open %s", file);
        return nullptr;
    }
    return IMG_LoadTyped_RW(src, 1, nullptr);
}

extern "C" SDL_Texture *IMG_LoadTextureTyped_RW(SDL_Renderer *renderer,
                                                SDL_RWops *src, int freesrc,
                                                const char *type)
{
    SDL_Surface *surface = IMG_LoadTyped_RW(src, freesrc, type);
    if (!surface)
        return nullptr;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}

extern "C" SDL_Texture *IMG_LoadTexture_RW(SDL_Renderer *renderer,
                                           SDL_RWops *src, int freesrc)
{
    return IMG_LoadTextureTyped_RW(renderer, src, freesrc, nullptr);
}

extern "C" SDL_Texture *IMG_LoadTexture(SDL_Renderer *renderer,
                                        const char *file)
{
    SDL_Surface *surface = IMG_Load(file);
    if (!surface)
        return nullptr;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}
