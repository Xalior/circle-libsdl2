//
// stdinc.cpp - SDL_stdinc.h: SDL's own names for the C library.
//
// SDL offers its own name for most of the C standard library so that an
// application can be built against a C runtime SDL was not built against, and
// so that SDL's own allocator can be replaced. Almost every one of them is
// the C library function under a different name, and that is what they are
// here: the platform has a full C runtime (newlib), so wrapping it is both
// the smallest implementation and the most correct one.
//
// The handful that are not in the C library - the bounded string copies, the
// integer-to-string family, the UTF-8 length helpers, the CRCs, the character
// set converter - are written out, following SDL2's own definitions so that
// results match what an application would get on a desktop.
//
// SDL_malloc, SDL_calloc, SDL_realloc and SDL_free are not here. They live
// beside the stream code in rwops.cpp, because they are the same contract:
// what SDL allocates and hands back is released with SDL_free.
//
#include <SDL2/SDL.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <cmath>
#include <strings.h>

// ---------------------------------------------------------------------------
// The heap
// ---------------------------------------------------------------------------
//
// The allocator is fixed: SDL_malloc and its three companions call the C
// runtime and nothing else can be put in their place. SDL_SetMemoryFunctions
// refuses rather than accepting a replacement it would then ignore, since an
// application that hands SDL its own allocator and is quietly given the C
// one's memory back would have a corruption it cannot see coming.

extern "C" void SDL_GetMemoryFunctions(SDL_malloc_func *malloc_func,
                                       SDL_calloc_func *calloc_func,
                                       SDL_realloc_func *realloc_func,
                                       SDL_free_func *free_func)
{
    if (malloc_func)  *malloc_func  = SDL_malloc;
    if (calloc_func)  *calloc_func  = SDL_calloc;
    if (realloc_func) *realloc_func = SDL_realloc;
    if (free_func)    *free_func    = SDL_free;
}

// The originals and the current ones are the same set, because the set never
// changes.
extern "C" void SDL_GetOriginalMemoryFunctions(SDL_malloc_func *malloc_func,
                                               SDL_calloc_func *calloc_func,
                                               SDL_realloc_func *realloc_func,
                                               SDL_free_func *free_func)
{
    SDL_GetMemoryFunctions(malloc_func, calloc_func, realloc_func, free_func);
}

extern "C" int SDL_SetMemoryFunctions(SDL_malloc_func, SDL_calloc_func,
                                      SDL_realloc_func, SDL_free_func)
{
    return SDL_SetError("SDL_SetMemoryFunctions: the allocator cannot be "
                        "replaced on this platform");
}

// Allocation counting is not compiled in, and -1 is what SDL documents for
// that case.
extern "C" int SDL_GetNumAllocations(void)
{
    return -1;
}

// ---------------------------------------------------------------------------
// Environment, sorting, arithmetic
// ---------------------------------------------------------------------------

extern "C" char *SDL_getenv(const char *name)
{
    return getenv(name);
}

extern "C" int SDL_setenv(const char *name, const char *value, int overwrite)
{
    return setenv(name, value, overwrite);
}

extern "C" void SDL_qsort(void *base, size_t nmemb, size_t size,
                          SDL_CompareCallback compare)
{
    qsort(base, nmemb, size, compare);
}

extern "C" void *SDL_bsearch(const void *key, const void *base, size_t nmemb,
                             size_t size, SDL_CompareCallback compare)
{
    return bsearch(key, base, nmemb, size, compare);
}

extern "C" int SDL_abs(int x)
{
    return x < 0 ? -x : x;
}

// ---------------------------------------------------------------------------
// Character classes
// ---------------------------------------------------------------------------
//
// SDL's are the C locale's, always, whatever locale the C runtime is in. The
// tests are written out rather than handed to <cctype> for exactly that
// reason.

extern "C" int SDL_isalpha(int x) { return (x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z'); }
extern "C" int SDL_isdigit(int x) { return x >= '0' && x <= '9'; }
extern "C" int SDL_isalnum(int x) { return SDL_isalpha(x) || SDL_isdigit(x); }
extern "C" int SDL_isblank(int x) { return x == ' ' || x == '\t'; }
extern "C" int SDL_iscntrl(int x) { return (x >= 0x00 && x <= 0x1F) || x == 0x7F; }
extern "C" int SDL_isxdigit(int x)
{
    return SDL_isdigit(x) || (x >= 'a' && x <= 'f') || (x >= 'A' && x <= 'F');
}
extern "C" int SDL_isspace(int x)
{
    return x == ' ' || x == '\t' || x == '\n' || x == '\v' || x == '\f' || x == '\r';
}
extern "C" int SDL_isupper(int x) { return x >= 'A' && x <= 'Z'; }
extern "C" int SDL_islower(int x) { return x >= 'a' && x <= 'z'; }
extern "C" int SDL_isprint(int x) { return x >= ' ' && x < 0x7F; }
extern "C" int SDL_isgraph(int x) { return SDL_isprint(x) && x != ' '; }
extern "C" int SDL_ispunct(int x) { return SDL_isgraph(x) && !SDL_isalnum(x); }
extern "C" int SDL_toupper(int x) { return SDL_islower(x) ? x - 'a' + 'A' : x; }
extern "C" int SDL_tolower(int x) { return SDL_isupper(x) ? x - 'A' + 'a' : x; }

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------
//
// The reflected CRC-16/ARC and CRC-32 SDL uses, computed a bit at a time. No
// table: these run over device names and joystick descriptors a few times a
// session, not over data.

extern "C" Uint16 SDL_crc16(Uint16 crc, const void *data, size_t len)
{
    const Uint8 *p = (const Uint8 *)data;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? (Uint16)((crc >> 1) ^ 0xA001) : (Uint16)(crc >> 1);
    }
    return crc;
}

extern "C" Uint32 SDL_crc32(Uint32 crc, const void *data, size_t len)
{
    const Uint8 *p = (const Uint8 *)data;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Memory blocks
// ---------------------------------------------------------------------------

extern "C" void *SDL_memset(void *dst, int c, size_t len)  { return memset(dst, c, len); }
extern "C" void *SDL_memcpy(void *dst, const void *src, size_t len)  { return memcpy(dst, src, len); }
extern "C" void *SDL_memmove(void *dst, const void *src, size_t len) { return memmove(dst, src, len); }
extern "C" int   SDL_memcmp(const void *a, const void *b, size_t len) { return memcmp(a, b, len); }

// ---------------------------------------------------------------------------
// Wide strings
// ---------------------------------------------------------------------------

extern "C" size_t SDL_wcslen(const wchar_t *s) { return wcslen(s); }
extern "C" int SDL_wcscmp(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
extern "C" int SDL_wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) { return wcsncmp(a, b, n); }
extern "C" wchar_t *SDL_wcsstr(const wchar_t *haystack, const wchar_t *needle)
{
    return (wchar_t *)wcsstr(haystack, needle);
}

// The bounded wide copies are BSD's, which the C standard does not carry, so
// they are written out. Both return the length the result would have had, so
// the caller can tell truncation from a fit.
extern "C" size_t SDL_wcslcpy(wchar_t *dst, const wchar_t *src, size_t maxlen)
{
    const size_t srclen = wcslen(src);
    if (maxlen > 0)
    {
        const size_t len = srclen < maxlen - 1 ? srclen : maxlen - 1;
        memcpy(dst, src, len * sizeof(wchar_t));
        dst[len] = 0;
    }
    return srclen;
}

extern "C" size_t SDL_wcslcat(wchar_t *dst, const wchar_t *src, size_t maxlen)
{
    const size_t dstlen = wcslen(dst);
    const size_t srclen = wcslen(src);
    if (dstlen < maxlen)
        SDL_wcslcpy(dst + dstlen, src, maxlen - dstlen);
    return dstlen + srclen;
}

extern "C" wchar_t *SDL_wcsdup(const wchar_t *s)
{
    const size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *copy = (wchar_t *)SDL_malloc(bytes);
    if (copy)
        memcpy(copy, s, bytes);
    return copy;
}

extern "C" int SDL_wcscasecmp(const wchar_t *a, const wchar_t *b)
{
    for (;;)
    {
        const wint_t ca = towlower((wint_t)*a++);
        const wint_t cb = towlower((wint_t)*b++);
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (ca == 0)
            return 0;
    }
}

extern "C" int SDL_wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    while (n--)
    {
        const wint_t ca = towlower((wint_t)*a++);
        const wint_t cb = towlower((wint_t)*b++);
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (ca == 0)
            return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Byte strings
// ---------------------------------------------------------------------------

extern "C" size_t SDL_strlen(const char *s) { return strlen(s); }
extern "C" int SDL_strcmp(const char *a, const char *b) { return strcmp(a, b); }
extern "C" int SDL_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
extern "C" int SDL_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }
extern "C" int SDL_strncasecmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
extern "C" char *SDL_strchr(const char *s, int c) { return (char *)strchr(s, c); }
extern "C" char *SDL_strrchr(const char *s, int c) { return (char *)strrchr(s, c); }
extern "C" char *SDL_strstr(const char *haystack, const char *needle)
{
    return (char *)strstr(haystack, needle);
}
extern "C" char *SDL_strtokr(char *s, const char *delim, char **saveptr)
{
    return strtok_r(s, delim, saveptr);
}

// SDL hands the caller memory it frees with SDL_free, so this cannot be the C
// library's strdup even where one exists: the two heaps must be the same one.
extern "C" char *SDL_strdup(const char *s)
{
    const size_t bytes = strlen(s) + 1;
    char *copy = (char *)SDL_malloc(bytes);
    if (copy)
        memcpy(copy, s, bytes);
    return copy;
}

extern "C" char *SDL_strcasestr(const char *haystack, const char *needle)
{
    const size_t len = strlen(needle);
    if (len == 0)
        return (char *)haystack;
    while (*haystack)
    {
        if (strncasecmp(haystack, needle, len) == 0)
            return (char *)haystack;
        haystack++;
    }
    return nullptr;
}

// The bounded copies are BSD's. Both return the length the result would have
// had, which is how the caller detects truncation.
extern "C" size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen)
{
    const size_t srclen = strlen(src);
    if (maxlen > 0)
    {
        const size_t len = srclen < maxlen - 1 ? srclen : maxlen - 1;
        memcpy(dst, src, len);
        dst[len] = '\0';
    }
    return srclen;
}

extern "C" size_t SDL_strlcat(char *dst, const char *src, size_t maxlen)
{
    const size_t dstlen = strlen(dst);
    const size_t srclen = strlen(src);
    if (dstlen < maxlen)
        SDL_strlcpy(dst + dstlen, src, maxlen - dstlen);
    return dstlen + srclen;
}

extern "C" char *SDL_strrev(char *s)
{
    size_t len = strlen(s);
    if (len < 2)
        return s;
    char *a = s;
    char *b = s + len - 1;
    for (len /= 2; len > 0; len--)
    {
        const char c = *a;
        *a++ = *b;
        *b-- = c;
    }
    return s;
}

extern "C" char *SDL_strupr(char *s)
{
    for (char *p = s; *p; p++)
        *p = (char)SDL_toupper((unsigned char)*p);
    return s;
}

extern "C" char *SDL_strlwr(char *s)
{
    for (char *p = s; *p; p++)
        *p = (char)SDL_tolower((unsigned char)*p);
    return s;
}

// ---- UTF-8 aware lengths ----------------------------------------------------
//
// A UTF-8 sequence is one lead byte followed by trailing bytes in 0x80..0xBF,
// so counting the bytes that are NOT trailing bytes counts the characters.

namespace
{

inline bool Utf8IsLead(unsigned char c)     { return c >= 0xC0; }
inline bool Utf8IsTrailing(unsigned char c) { return c >= 0x80 && c <= 0xBF; }

inline unsigned Utf8TrailingCount(unsigned char c)
{
    if (c >= 0xF0 && c <= 0xF4) return 3;
    if (c >= 0xE0 && c <= 0xEF) return 2;
    if (c >= 0xC0 && c <= 0xDF) return 1;
    return 0;
}

} // namespace

extern "C" size_t SDL_utf8strlen(const char *s)
{
    size_t chars = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!Utf8IsTrailing(*p))
            chars++;
    return chars;
}

extern "C" size_t SDL_utf8strnlen(const char *s, size_t bytes)
{
    size_t chars = 0;
    const unsigned char *p = (const unsigned char *)s;
    for (; bytes > 0 && *p; bytes--, p++)
        if (!Utf8IsTrailing(*p))
            chars++;
    return chars;
}

// Like SDL_strlcpy, but the result is never cut through the middle of a
// character: a partial sequence at the end is dropped whole. Returns the
// bytes actually copied.
extern "C" size_t SDL_utf8strlcpy(char *dst, const char *src, size_t dst_bytes)
{
    if (dst_bytes == 0)
        return 0;

    const size_t srclen = strlen(src);
    size_t bytes = srclen < dst_bytes - 1 ? srclen : dst_bytes - 1;

    if (bytes > 0)
    {
        const unsigned char last = (unsigned char)src[bytes - 1];
        if (Utf8IsLead(last))
        {
            // A lead byte with nothing after it is half a character.
            bytes--;
        }
        else if (Utf8IsTrailing(last))
        {
            // Walk back to the lead byte and keep the sequence only if all
            // of it fits.
            for (size_t i = bytes - 1; i != 0; i--)
            {
                const unsigned trailing = Utf8TrailingCount((unsigned char)src[i]);
                if (trailing)
                {
                    if (bytes - i != trailing + 1)
                        bytes = i;
                    break;
                }
            }
        }
        memcpy(dst, src, bytes);
    }
    dst[bytes] = '\0';
    return bytes;
}

// ---------------------------------------------------------------------------
// Numbers to and from text
// ---------------------------------------------------------------------------

namespace
{
const char NtoaDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

char *UnsignedToString(Uint64 value, char *s, int radix)
{
    char *p = s;
    if (radix < 2 || radix > 36)
    {
        *p = '\0';
        return s;
    }
    if (value == 0)
    {
        *p++ = '0';
    }
    else
    {
        while (value > 0)
        {
            *p++ = NtoaDigits[value % (unsigned)radix];
            value /= (unsigned)radix;
        }
    }
    *p = '\0';
    return SDL_strrev(s);
}

char *SignedToString(Sint64 value, char *s, int radix)
{
    if (value < 0)
    {
        s[0] = '-';
        // Negated as an unsigned value, so the most negative number of the
        // type still converts instead of overflowing.
        UnsignedToString((Uint64)0 - (Uint64)value, s + 1, radix);
        return s;
    }
    return UnsignedToString((Uint64)value, s, radix);
}
} // namespace

extern "C" char *SDL_itoa(int value, char *s, int radix)   { return SignedToString(value, s, radix); }
extern "C" char *SDL_ltoa(long value, char *s, int radix)   { return SignedToString(value, s, radix); }
extern "C" char *SDL_lltoa(Sint64 value, char *s, int radix) { return SignedToString(value, s, radix); }
extern "C" char *SDL_uitoa(unsigned int value, char *s, int radix)  { return UnsignedToString(value, s, radix); }
extern "C" char *SDL_ultoa(unsigned long value, char *s, int radix) { return UnsignedToString(value, s, radix); }
extern "C" char *SDL_ulltoa(Uint64 value, char *s, int radix)       { return UnsignedToString(value, s, radix); }

extern "C" int    SDL_atoi(const char *s)  { return atoi(s); }
extern "C" double SDL_atof(const char *s)  { return atof(s); }
extern "C" long   SDL_strtol(const char *s, char **endp, int base)  { return strtol(s, endp, base); }
extern "C" unsigned long SDL_strtoul(const char *s, char **endp, int base) { return strtoul(s, endp, base); }
extern "C" Sint64 SDL_strtoll(const char *s, char **endp, int base) { return strtoll(s, endp, base); }
extern "C" Uint64 SDL_strtoull(const char *s, char **endp, int base) { return strtoull(s, endp, base); }
extern "C" double SDL_strtod(const char *s, char **endp) { return strtod(s, endp); }

// ---------------------------------------------------------------------------
// Formatted text
// ---------------------------------------------------------------------------

extern "C" int SDL_sscanf(const char *text, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsscanf(text, fmt, ap);
    va_end(ap);
    return n;
}

extern "C" int SDL_vsscanf(const char *text, const char *fmt, va_list ap)
{
    return vsscanf(text, fmt, ap);
}

extern "C" int SDL_snprintf(char *text, size_t maxlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return n;
}

extern "C" int SDL_vsnprintf(char *text, size_t maxlen, const char *fmt, va_list ap)
{
    return vsnprintf(text, maxlen, fmt, ap);
}

// The buffer comes off SDL's heap, so the caller releases it with SDL_free
// and not with the C library's free - which is why this measures and formats
// rather than handing the job to the C library's asprintf.
extern "C" int SDL_vasprintf(char **strp, const char *fmt, va_list ap)
{
    if (strp)
        *strp = nullptr;

    va_list measure;
    va_copy(measure, ap);
    const int len = vsnprintf(nullptr, 0, fmt, measure);
    va_end(measure);
    if (len < 0)
        return -1;

    char *text = (char *)SDL_malloc((size_t)len + 1);
    if (!text)
        return -1;

    const int n = vsnprintf(text, (size_t)len + 1, fmt, ap);
    if (n < 0)
    {
        SDL_free(text);
        return -1;
    }
    if (strp)
        *strp = text;
    else
        SDL_free(text);
    return n;
}

extern "C" int SDL_asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = SDL_vasprintf(strp, fmt, ap);
    va_end(ap);
    return n;
}

// ---------------------------------------------------------------------------
// Mathematics
// ---------------------------------------------------------------------------

extern "C" double SDL_acos(double x)   { return acos(x); }
extern "C" float  SDL_acosf(float x)   { return acosf(x); }
extern "C" double SDL_asin(double x)   { return asin(x); }
extern "C" float  SDL_asinf(float x)   { return asinf(x); }
extern "C" double SDL_atan(double x)   { return atan(x); }
extern "C" float  SDL_atanf(float x)   { return atanf(x); }
extern "C" double SDL_atan2(double y, double x) { return atan2(y, x); }
extern "C" float  SDL_atan2f(float y, float x)  { return atan2f(y, x); }
extern "C" double SDL_ceil(double x)   { return ceil(x); }
extern "C" float  SDL_ceilf(float x)   { return ceilf(x); }
extern "C" double SDL_copysign(double x, double y) { return copysign(x, y); }
extern "C" float  SDL_copysignf(float x, float y)  { return copysignf(x, y); }
extern "C" double SDL_cos(double x)    { return cos(x); }
extern "C" float  SDL_cosf(float x)    { return cosf(x); }
extern "C" double SDL_exp(double x)    { return exp(x); }
extern "C" float  SDL_expf(float x)    { return expf(x); }
extern "C" double SDL_fabs(double x)   { return fabs(x); }
extern "C" float  SDL_fabsf(float x)   { return fabsf(x); }
extern "C" double SDL_floor(double x)  { return floor(x); }
extern "C" float  SDL_floorf(float x)  { return floorf(x); }
extern "C" double SDL_trunc(double x)  { return trunc(x); }
extern "C" float  SDL_truncf(float x)  { return truncf(x); }
extern "C" double SDL_fmod(double x, double y) { return fmod(x, y); }
extern "C" float  SDL_fmodf(float x, float y)  { return fmodf(x, y); }
extern "C" double SDL_log(double x)    { return log(x); }
extern "C" float  SDL_logf(float x)    { return logf(x); }
extern "C" double SDL_log10(double x)  { return log10(x); }
extern "C" float  SDL_log10f(float x)  { return log10f(x); }
extern "C" double SDL_modf(double x, double *y) { return modf(x, y); }
extern "C" float  SDL_modff(float x, float *y)  { return modff(x, y); }
extern "C" double SDL_pow(double x, double y) { return pow(x, y); }
extern "C" float  SDL_powf(float x, float y)  { return powf(x, y); }
extern "C" double SDL_round(double x)  { return round(x); }
extern "C" float  SDL_roundf(float x)  { return roundf(x); }
extern "C" long   SDL_lround(double x) { return lround(x); }
extern "C" long   SDL_lroundf(float x) { return lroundf(x); }
extern "C" double SDL_scalbn(double x, int n) { return scalbn(x, n); }
extern "C" float  SDL_scalbnf(float x, int n) { return scalbnf(x, n); }
extern "C" double SDL_sin(double x)    { return sin(x); }
extern "C" float  SDL_sinf(float x)    { return sinf(x); }
extern "C" double SDL_sqrt(double x)   { return sqrt(x); }
extern "C" float  SDL_sqrtf(float x)   { return sqrtf(x); }
extern "C" double SDL_tan(double x)    { return tan(x); }
extern "C" float  SDL_tanf(float x)    { return tanf(x); }

// ---------------------------------------------------------------------------
// Character set conversion
// ---------------------------------------------------------------------------
//
// SDL_iconv converts text between character sets. There is no iconv on this
// platform, so this is a converter of its own, and it covers the Unicode
// encodings and the two single-byte sets that Unicode contains exactly:
//
//   UTF-8 (and the empty name, which SDL uses for "whatever the system talks";
//          this platform talks UTF-8), ASCII / US-ASCII, ISO-8859-1 /
//   LATIN1, UTF-16 / UTF-16LE / UTF-16BE, UCS-2 and its explicit-endian
//   forms, UTF-32 / UTF-32LE / UTF-32BE, UCS-4 and its explicit-endian
//   forms, and WCHAR_T (a 32-bit wide character here).
//
// Every one of those can be converted to and from any other without a
// mapping table, because each is a direct encoding of a Unicode code point.
// A legacy code page - Shift-JIS, KOI8-R, a Windows code page - cannot: it
// needs a table this library does not carry, so SDL_iconv_open refuses a
// name it does not know rather than passing bytes through unconverted.
//
// Every conversion goes through a code point: decode one character from the
// source encoding, encode it into the destination.

namespace
{

enum Encoding
{
    ENC_UNKNOWN = 0,
    ENC_UTF8,
    ENC_ASCII,
    ENC_LATIN1,
    ENC_UTF16LE,
    ENC_UTF16BE,
    ENC_UCS4LE,       // UTF-32LE / UCS-4LE / WCHAR_T
    ENC_UCS4BE
};

const Uint32 UNICODE_REPLACEMENT = 0xFFFD;

// The name a caller gives is matched without regard to case, as iconv does.
Encoding EncodingFromName(const char *name)
{
    // The empty name means "the system's own", and this system's own is
    // UTF-8. SDL's SDL_iconv_utf8_locale macro depends on that mapping.
    if (!name || !*name)
        return ENC_UTF8;

    static const struct { const char *name; Encoding enc; } Names[] = {
        {"UTF-8",     ENC_UTF8},
        {"UTF8",      ENC_UTF8},
        {"ASCII",     ENC_ASCII},
        {"US-ASCII",  ENC_ASCII},
        {"ISO-8859-1", ENC_LATIN1},
        {"ISO8859-1", ENC_LATIN1},
        {"LATIN1",    ENC_LATIN1},
        // The unqualified names take the machine's byte order, which is
        // little-endian on every board this runs on.
        {"UTF-16",    ENC_UTF16LE},
        {"UTF16",     ENC_UTF16LE},
        {"UTF-16LE",  ENC_UTF16LE},
        {"UTF-16BE",  ENC_UTF16BE},
        {"UCS-2",     ENC_UTF16LE},
        {"UCS2",      ENC_UTF16LE},
        {"UCS-2LE",   ENC_UTF16LE},
        {"UCS-2BE",   ENC_UTF16BE},
        {"UCS-2-INTERNAL", ENC_UTF16LE},
        {"UTF-32",    ENC_UCS4LE},
        {"UTF32",     ENC_UCS4LE},
        {"UTF-32LE",  ENC_UCS4LE},
        {"UTF-32BE",  ENC_UCS4BE},
        {"UCS-4",     ENC_UCS4LE},
        {"UCS4",      ENC_UCS4LE},
        {"UCS-4LE",   ENC_UCS4LE},
        {"UCS-4BE",   ENC_UCS4BE},
        {"UCS-4-INTERNAL", ENC_UCS4LE},
        // wchar_t is 32 bits wide and holds a code point on this toolchain.
        {"WCHAR_T",   ENC_UCS4LE},
    };

    for (const auto &entry : Names)
        if (strcasecmp(name, entry.name) == 0)
            return entry.enc;

    return ENC_UNKNOWN;
}

// UCS-2 in the strict sense has no surrogate pairs, but treating the 16-bit
// forms as UTF-16 throughout costs nothing and loses nothing: text that is
// really UCS-2 contains no surrogates to pair up.

// Decode one character. Returns SDL's error codes: EINVAL when the input
// ends in the middle of a character, EILSEQ when it is not valid in this
// encoding. Advances *src / *left only on success.
size_t DecodeOne(Encoding enc, const unsigned char **src, size_t *left, Uint32 *out)
{
    const unsigned char *p = *src;
    size_t n = *left;

    switch (enc)
    {
    case ENC_ASCII:
        if (n < 1) return SDL_ICONV_EINVAL;
        if (p[0] > 0x7F) return SDL_ICONV_EILSEQ;
        *out = p[0];
        *src += 1; *left -= 1;
        return 0;

    case ENC_LATIN1:
        if (n < 1) return SDL_ICONV_EINVAL;
        *out = p[0];
        *src += 1; *left -= 1;
        return 0;

    case ENC_UTF8:
    {
        if (n < 1) return SDL_ICONV_EINVAL;
        const unsigned char lead = p[0];
        if (lead < 0x80) { *out = lead; *src += 1; *left -= 1; return 0; }

        unsigned extra;
        Uint32 ch;
        if      (lead >= 0xC2 && lead <= 0xDF) { extra = 1; ch = lead & 0x1F; }
        else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; ch = lead & 0x0F; }
        else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; ch = lead & 0x07; }
        else return SDL_ICONV_EILSEQ;   // a trailing byte first, or an
                                        // overlong two-byte lead

        if (n < extra + 1) return SDL_ICONV_EINVAL;
        for (unsigned i = 1; i <= extra; i++)
        {
            if ((p[i] & 0xC0) != 0x80) return SDL_ICONV_EILSEQ;
            ch = (ch << 6) | (p[i] & 0x3F);
        }
        // Overlong forms, surrogates and values past the last code point are
        // all invalid, and every one of them is a way of smuggling a
        // character past a check that only looked at the encoded bytes.
        if ((extra == 2 && ch < 0x800) || (extra == 3 && ch < 0x10000))
            return SDL_ICONV_EILSEQ;
        if (ch > 0x10FFFF || (ch >= 0xD800 && ch <= 0xDFFF))
            return SDL_ICONV_EILSEQ;
        *out = ch;
        *src += extra + 1; *left -= extra + 1;
        return 0;
    }

    case ENC_UTF16LE:
    case ENC_UTF16BE:
    {
        if (n < 2) return SDL_ICONV_EINVAL;
        Uint32 unit = (enc == ENC_UTF16LE) ? (Uint32)(p[0] | (p[1] << 8))
                                           : (Uint32)((p[0] << 8) | p[1]);
        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            if (n < 4) return SDL_ICONV_EINVAL;
            Uint32 low = (enc == ENC_UTF16LE) ? (Uint32)(p[2] | (p[3] << 8))
                                              : (Uint32)((p[2] << 8) | p[3]);
            if (low < 0xDC00 || low > 0xDFFF) return SDL_ICONV_EILSEQ;
            *out = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            *src += 4; *left -= 4;
            return 0;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF)
            return SDL_ICONV_EILSEQ;    // a low surrogate with no high one
        *out = unit;
        *src += 2; *left -= 2;
        return 0;
    }

    case ENC_UCS4LE:
    case ENC_UCS4BE:
    {
        if (n < 4) return SDL_ICONV_EINVAL;
        Uint32 ch = (enc == ENC_UCS4LE)
            ? ((Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24))
            : (((Uint32)p[0] << 24) | ((Uint32)p[1] << 16) | ((Uint32)p[2] << 8) | (Uint32)p[3]);
        if (ch > 0x10FFFF || (ch >= 0xD800 && ch <= 0xDFFF))
            return SDL_ICONV_EILSEQ;
        *out = ch;
        *src += 4; *left -= 4;
        return 0;
    }

    default:
        return SDL_ICONV_ERROR;
    }
}

// Encode one character. E2BIG when it does not fit, so the caller can grow
// the buffer and ask again; nothing is written and nothing is advanced in
// that case. A character the destination cannot represent becomes '?' - the
// same substitution SDL makes - and is counted as a non-reversible
// conversion through *lossy.
size_t EncodeOne(Encoding enc, Uint32 ch, unsigned char **dst, size_t *left,
                 size_t *lossy)
{
    unsigned char *p = *dst;
    const size_t n = *left;

    switch (enc)
    {
    case ENC_ASCII:
    case ENC_LATIN1:
    {
        const Uint32 limit = (enc == ENC_ASCII) ? 0x7F : 0xFF;
        if (n < 1) return SDL_ICONV_E2BIG;
        if (ch > limit) { ch = '?'; (*lossy)++; }
        p[0] = (unsigned char)ch;
        *dst += 1; *left -= 1;
        return 0;
    }

    case ENC_UTF8:
        if (ch < 0x80)
        {
            if (n < 1) return SDL_ICONV_E2BIG;
            p[0] = (unsigned char)ch;
            *dst += 1; *left -= 1;
        }
        else if (ch < 0x800)
        {
            if (n < 2) return SDL_ICONV_E2BIG;
            p[0] = (unsigned char)(0xC0 | (ch >> 6));
            p[1] = (unsigned char)(0x80 | (ch & 0x3F));
            *dst += 2; *left -= 2;
        }
        else if (ch < 0x10000)
        {
            if (n < 3) return SDL_ICONV_E2BIG;
            p[0] = (unsigned char)(0xE0 | (ch >> 12));
            p[1] = (unsigned char)(0x80 | ((ch >> 6) & 0x3F));
            p[2] = (unsigned char)(0x80 | (ch & 0x3F));
            *dst += 3; *left -= 3;
        }
        else
        {
            if (n < 4) return SDL_ICONV_E2BIG;
            p[0] = (unsigned char)(0xF0 | (ch >> 18));
            p[1] = (unsigned char)(0x80 | ((ch >> 12) & 0x3F));
            p[2] = (unsigned char)(0x80 | ((ch >> 6) & 0x3F));
            p[3] = (unsigned char)(0x80 | (ch & 0x3F));
            *dst += 4; *left -= 4;
        }
        return 0;

    case ENC_UTF16LE:
    case ENC_UTF16BE:
    {
        const bool little = (enc == ENC_UTF16LE);
        if (ch < 0x10000)
        {
            if (n < 2) return SDL_ICONV_E2BIG;
            if (little) { p[0] = (unsigned char)ch; p[1] = (unsigned char)(ch >> 8); }
            else        { p[0] = (unsigned char)(ch >> 8); p[1] = (unsigned char)ch; }
            *dst += 2; *left -= 2;
        }
        else
        {
            if (n < 4) return SDL_ICONV_E2BIG;
            const Uint32 v = ch - 0x10000;
            const Uint32 hi = 0xD800 + (v >> 10);
            const Uint32 lo = 0xDC00 + (v & 0x3FF);
            if (little)
            {
                p[0] = (unsigned char)hi; p[1] = (unsigned char)(hi >> 8);
                p[2] = (unsigned char)lo; p[3] = (unsigned char)(lo >> 8);
            }
            else
            {
                p[0] = (unsigned char)(hi >> 8); p[1] = (unsigned char)hi;
                p[2] = (unsigned char)(lo >> 8); p[3] = (unsigned char)lo;
            }
            *dst += 4; *left -= 4;
        }
        return 0;
    }

    case ENC_UCS4LE:
    case ENC_UCS4BE:
        if (n < 4) return SDL_ICONV_E2BIG;
        if (enc == ENC_UCS4LE)
        {
            p[0] = (unsigned char)ch;         p[1] = (unsigned char)(ch >> 8);
            p[2] = (unsigned char)(ch >> 16); p[3] = (unsigned char)(ch >> 24);
        }
        else
        {
            p[0] = (unsigned char)(ch >> 24); p[1] = (unsigned char)(ch >> 16);
            p[2] = (unsigned char)(ch >> 8);  p[3] = (unsigned char)ch;
        }
        *dst += 4; *left -= 4;
        return 0;

    default:
        return SDL_ICONV_ERROR;
    }
}

} // namespace

// The conversion handle. SDL declares it as a pointer to an incomplete
// struct, so the type is named here and nowhere else. The two encodings are
// held as plain integers because the enum naming them is local to this file.
struct _SDL_iconv_t
{
    int from;
    int to;
};

extern "C" SDL_iconv_t SDL_iconv_open(const char *tocode, const char *fromcode)
{
    const Encoding to = EncodingFromName(tocode);
    const Encoding from = EncodingFromName(fromcode);
    if (to == ENC_UNKNOWN || from == ENC_UNKNOWN)
        return (SDL_iconv_t)-1;

    SDL_iconv_t cd = (SDL_iconv_t)SDL_malloc(sizeof(struct _SDL_iconv_t));
    if (!cd)
        return (SDL_iconv_t)-1;
    cd->from = (int)from;
    cd->to = (int)to;
    return cd;
}

extern "C" int SDL_iconv_close(SDL_iconv_t cd)
{
    if (cd == (SDL_iconv_t)-1 || cd == nullptr)
        return -1;
    SDL_free(cd);
    return 0;
}

extern "C" size_t SDL_iconv(SDL_iconv_t cd, const char **inbuf,
                            size_t *inbytesleft, char **outbuf,
                            size_t *outbytesleft)
{
    if (cd == (SDL_iconv_t)-1 || cd == nullptr)
        return SDL_ICONV_ERROR;

    // No input buffer means "reset the conversion state". This converter
    // carries none between characters, so there is nothing to reset.
    if (!inbuf || !*inbuf)
        return 0;
    if (!inbytesleft || !outbuf || !*outbuf || !outbytesleft)
        return SDL_ICONV_ERROR;

    size_t lossy = 0;
    while (*inbytesleft > 0)
    {
        const unsigned char *src = (const unsigned char *)*inbuf;
        size_t srcleft = *inbytesleft;
        Uint32 ch = 0;

        const size_t decoded = DecodeOne((Encoding)cd->from, &src, &srcleft, &ch);
        if (decoded != 0)
            return decoded;

        unsigned char *dst = (unsigned char *)*outbuf;
        size_t dstleft = *outbytesleft;
        const size_t encoded = EncodeOne((Encoding)cd->to, ch, &dst, &dstleft, &lossy);
        if (encoded != 0)
            return encoded;      // E2BIG: the input character is untouched

        *inbuf = (const char *)src;
        *inbytesleft = srcleft;
        *outbuf = (char *)dst;
        *outbytesleft = dstleft;
    }
    return lossy;
}

extern "C" char *SDL_iconv_string(const char *tocode, const char *fromcode,
                                  const char *inbuf, size_t inbytesleft)
{
    SDL_iconv_t cd = SDL_iconv_open(tocode, fromcode);
    if (cd == (SDL_iconv_t)-1)
        return nullptr;

    // At least four bytes, so the terminator below always has room whatever
    // the destination encoding's character width is.
    size_t stringsize = inbytesleft > 4 ? inbytesleft : 4;
    char *string = (char *)SDL_malloc(stringsize);
    if (!string)
    {
        SDL_iconv_close(cd);
        return nullptr;
    }

    char *outbuf = string;
    size_t outbytesleft = stringsize;
    memset(outbuf, 0, 4);

    while (inbytesleft > 0)
    {
        const size_t before = inbytesleft;
        const size_t result = SDL_iconv(cd, &inbuf, &inbytesleft,
                                        &outbuf, &outbytesleft);
        if (result == SDL_ICONV_E2BIG)
        {
            char *old = string;
            stringsize *= 2;
            char *grown = (char *)SDL_realloc(string, stringsize);
            if (!grown)
            {
                SDL_free(string);
                SDL_iconv_close(cd);
                return nullptr;
            }
            string = grown;
            outbuf = string + (outbuf - old);
            outbytesleft = stringsize - (size_t)(outbuf - string);
            memset(outbuf, 0, 4);
        }
        else if (result == SDL_ICONV_EILSEQ)
        {
            // Skip the byte that could not be read and carry on, so one bad
            // byte does not cost the whole string.
            inbuf++;
            inbytesleft--;
        }
        else if (result == SDL_ICONV_EINVAL || result == SDL_ICONV_ERROR)
        {
            break;
        }

        if (before == inbytesleft && result != SDL_ICONV_E2BIG)
            break;      // nothing was consumed and nothing will be
    }

    SDL_iconv_close(cd);
    return string;
}
