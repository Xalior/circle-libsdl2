//
// video.cpp — window / software renderer / streaming texture over
// Circle's CBcmFrameBuffer (double-buffered, vsync page flip).
//
// Scope matches MAME's drawsdl.cpp software path: one fullscreen window,
// an SDL_Renderer, streaming ARGB8888 textures.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_circle.h>
#include "sdl2circle.h"
#include <circle/bcmframebuffer.h>
#include <cstring>
#include <cstdlib>

struct SDL_Window
{
    CBcmFrameBuffer *fb;
    int w, h;
    Uint32 flags;
};

struct SDL_Renderer
{
    SDL_Window *window;
    u8 *base;          // start of the (2x height) virtual framebuffer
    unsigned pitch;
    unsigned back;     // half we're drawing into: 0 = top, 1 = bottom
    bool vsync;        // present blocks for vertical sync
    Uint8 r, g, b, a;  // draw color
};

struct SDL_Texture
{
    int w, h;
    Uint32 format;
    u8 *pixels;
    int pitch;
    SDL_BlendMode blend;
    Uint8 alphamod;
};

// The one fullscreen window (ID 1). Display-mode queries answer with its
// size once it exists, and with the panel default before that.
static SDL_Window *s_window = nullptr;

// The display size is fixed before SDL comes up: the hosting kernel picks
// it (SDL2Circle_SetDisplaySize) and the VideoCore scaler stretches the
// framebuffer to the panel. 1080p when the host doesn't say otherwise.
static int s_display_w = 1920, s_display_h = 1080;
static const int DEFAULT_HZ = 60;

extern "C" void SDL2Circle_SetDisplaySize(int w, int h)
{
    if (w > 0 && h > 0 && !s_window)
    {
        s_display_w = w;
        s_display_h = h;
    }
}

static u8 *back_buffer(SDL_Renderer *ren)
{
    return ren->base + (size_t)ren->back * ren->window->h * ren->pitch;
}

static void fill_mode(SDL_DisplayMode *mode)
{
    mode->format = SDL_PIXELFORMAT_ARGB8888;
    mode->w = s_window ? s_window->w : s_display_w;
    mode->h = s_window ? s_window->h : s_display_h;
    mode->refresh_rate = DEFAULT_HZ;
    mode->driverdata = nullptr;
}

// ---- display information ---------------------------------------------------

extern "C" int SDL_GetNumVideoDisplays(void) { return 1; }

extern "C" const char *SDL_GetDisplayName(int) { return "HDMI0"; }

extern "C" int SDL_GetDisplayBounds(int, SDL_Rect *rect)
{
    rect->x = 0;
    rect->y = 0;
    rect->w = s_window ? s_window->w : s_display_w;
    rect->h = s_window ? s_window->h : s_display_h;
    return 0;
}

extern "C" int SDL_GetNumDisplayModes(int) { return 1; }

extern "C" int SDL_GetDisplayMode(int, int, SDL_DisplayMode *mode)
{
    fill_mode(mode);
    return 0;
}

extern "C" int SDL_GetCurrentDisplayMode(int, SDL_DisplayMode *mode)
{
    fill_mode(mode);
    return 0;
}

extern "C" int SDL_GetDesktopDisplayMode(int, SDL_DisplayMode *mode)
{
    fill_mode(mode);
    return 0;
}

extern "C" int SDL_GetNumVideoDrivers(void) { return 1; }
extern "C" const char *SDL_GetVideoDriver(int) { return "circle"; }
extern "C" const char *SDL_GetCurrentVideoDriver(void) { return "circle"; }

extern "C" SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp,
                                               Uint32 *Rmask, Uint32 *Gmask,
                                               Uint32 *Bmask, Uint32 *Amask)
{
    switch (format)
    {
    case SDL_PIXELFORMAT_ARGB8888:
        *bpp = 32;
        *Rmask = 0x00FF0000;
        *Gmask = 0x0000FF00;
        *Bmask = 0x000000FF;
        *Amask = 0xFF000000;
        return SDL_TRUE;
    case SDL_PIXELFORMAT_RGB888:   // XRGB, no alpha
        *bpp = 32;
        *Rmask = 0x00FF0000;
        *Gmask = 0x0000FF00;
        *Bmask = 0x000000FF;
        *Amask = 0;
        return SDL_TRUE;
    default:
        SDL_SetError("unsupported pixel format");
        return SDL_FALSE;
    }
}

extern "C" SDL_Window *SDL_CreateWindow(const char *, int, int, int w, int h,
                                        Uint32 flags)
{
    CBcmFrameBuffer *fb =
        new CBcmFrameBuffer(w, h, 32, 0, 0, 0, TRUE /*double buffered*/);
    if (!fb->Initialize())
    {
        delete fb;
        SDL_SetError("CBcmFrameBuffer::Initialize failed (%dx%d)", w, h);
        return nullptr;
    }

    SDL_Window *win = new SDL_Window;
    win->fb = fb;
    win->w = (int)fb->GetWidth();
    win->h = (int)fb->GetHeight();
    win->flags = flags | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN;
    s_window = win;

    // The window is the whole display: it is shown and focused from birth.
    // Consumers (MAME's OSD among them) gate keyboard input on having seen
    // a focus event, so announce it.
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_WINDOWEVENT;
    ev.window.timestamp = SDL_GetTicks();
    ev.window.windowID = 1;
    ev.window.event = SDL_WINDOWEVENT_SHOWN;
    SDL_PushEvent(&ev);
    ev.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    SDL_PushEvent(&ev);

    return win;
}

extern "C" Uint32 SDL_GetWindowID(SDL_Window *win)
{
    return (win && win == s_window) ? 1 : 0;
}

extern "C" SDL_Window *SDL_GetWindowFromID(Uint32 id)
{
    return (id == 1) ? s_window : nullptr;
}

extern "C" int SDL_GetWindowDisplayIndex(SDL_Window *) { return 0; }

extern "C" void SDL_DestroyWindow(SDL_Window *win)
{
    if (!win)
        return;
    if (win == s_window)
        s_window = nullptr;
    delete win->fb;
    delete win;
}

extern "C" void SDL_GetWindowSize(SDL_Window *win, int *w, int *h)
{
    if (w) *w = win ? win->w : 0;
    if (h) *h = win ? win->h : 0;
}

extern "C" Uint32 SDL_GetWindowFlags(SDL_Window *win)
{
    return win ? win->flags : 0;
}

extern "C" void SDL_SetWindowTitle(SDL_Window *, const char *) {}
extern "C" void SDL_ShowWindow(SDL_Window *) {}

extern "C" SDL_Renderer *SDL_CreateRenderer(SDL_Window *win, int, Uint32 flags)
{
    if (!win)
    {
        SDL_SetError("SDL_CreateRenderer: no window");
        return nullptr;
    }
    SDL_Renderer *ren = new SDL_Renderer;
    ren->window = win;
    ren->base = (u8 *)(uintptr)win->fb->GetBuffer();
    ren->pitch = win->fb->GetPitch();
    ren->back = 1;   // half 0 is visible after init; draw into half 1 first
    ren->vsync = (flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    ren->r = ren->g = ren->b = 0;
    ren->a = 255;
    return ren;
}

extern "C" void SDL_DestroyRenderer(SDL_Renderer *ren)
{
    delete ren;
}

extern "C" int SDL_GetRendererOutputSize(SDL_Renderer *ren, int *w, int *h)
{
    SDL_GetWindowSize(ren ? ren->window : nullptr, w, h);
    return 0;
}

extern "C" int SDL_SetRenderDrawColor(SDL_Renderer *ren, Uint8 r, Uint8 g,
                                      Uint8 b, Uint8 a)
{
    ren->r = r; ren->g = g; ren->b = b; ren->a = a;
    return 0;
}

extern "C" int SDL_RenderClear(SDL_Renderer *ren)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    // Pi firmware 32bpp framebuffer layout: XRGB little-endian
    u32 color = ((u32)ren->a << 24) | ((u32)ren->r << 16) |
                ((u32)ren->g << 8) | ren->b;
    u8 *dst = back_buffer(ren);
    for (int y = 0; y < ren->window->h; y++, dst += ren->pitch)
    {
        u32 *row = (u32 *)dst;
        for (int x = 0; x < ren->window->w; x++)
            row[x] = color;
    }
    return 0;
}

extern "C" SDL_Texture *SDL_CreateTexture(SDL_Renderer *, Uint32 format,
                                          int access, int w, int h)
{
    if (format != SDL_PIXELFORMAT_ARGB8888 || access != SDL_TEXTUREACCESS_STREAMING)
    {
        SDL_SetError("only streaming ARGB8888 textures are implemented");
        return nullptr;
    }
    SDL_Texture *tex = new SDL_Texture;
    tex->w = w;
    tex->h = h;
    tex->format = format;
    tex->pitch = w * 4;
    tex->pixels = (u8 *)malloc((size_t)tex->pitch * h);
    tex->blend = SDL_BLENDMODE_NONE;
    tex->alphamod = 255;
    return tex;
}

extern "C" int SDL_QueryTexture(SDL_Texture *tex, Uint32 *format, int *access,
                                int *w, int *h)
{
    if (format) *format = tex->format;
    if (access) *access = SDL_TEXTUREACCESS_STREAMING;
    if (w) *w = tex->w;
    if (h) *h = tex->h;
    return 0;
}

extern "C" int SDL_UpdateTexture(SDL_Texture *tex, const SDL_Rect *rect,
                                 const void *pixels, int pitch)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    int x = rect ? rect->x : 0;
    int y = rect ? rect->y : 0;
    int w = rect ? rect->w : tex->w;
    int h = rect ? rect->h : tex->h;
    const u8 *src = (const u8 *)pixels;
    u8 *dst = tex->pixels + (size_t)y * tex->pitch + (size_t)x * 4;
    for (int row = 0; row < h; row++)
    {
        memcpy(dst, src, (size_t)w * 4);
        src += pitch;
        dst += tex->pitch;
    }
    return 0;
}

extern "C" int SDL_SetTextureBlendMode(SDL_Texture *tex, SDL_BlendMode blend)
{
    tex->blend = blend;
    return 0;
}

extern "C" int SDL_GetTextureBlendMode(SDL_Texture *tex, SDL_BlendMode *blend)
{
    *blend = tex->blend;
    return 0;
}

extern "C" int SDL_SetTextureAlphaMod(SDL_Texture *tex, Uint8 alpha)
{
    tex->alphamod = alpha;
    return 0;
}

extern "C" int SDL_SetTextureColorMod(SDL_Texture *, Uint8, Uint8, Uint8)
{
    return 0;   // tinting is not applied; MAME uses it only for effects
}

extern "C" void SDL_DestroyTexture(SDL_Texture *tex)
{
    if (!tex)
        return;
    free(tex->pixels);
    delete tex;
}

extern "C" int SDL_LockTexture(SDL_Texture *tex, const SDL_Rect *rect,
                               void **pixels, int *pitch)
{
    if (rect)
        *pixels = tex->pixels + (size_t)rect->y * tex->pitch + (size_t)rect->x * 4;
    else
        *pixels = tex->pixels;
    *pitch = tex->pitch;
    return 0;
}

extern "C" void SDL_UnlockTexture(SDL_Texture *) {}

extern "C" int SDL_RenderCopy(SDL_Renderer *ren, SDL_Texture *tex,
                              const SDL_Rect *srcrect, const SDL_Rect *dstrect)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    // Unscaled blit (MAME's drawsdl renders at output size already), with
    // straight-alpha blending when the texture asks for it.
    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;
    int sw = srcrect ? srcrect->w : tex->w;
    int sh = srcrect ? srcrect->h : tex->h;
    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;

    int w = sw, h = sh;
    if (dx + w > ren->window->w) w = ren->window->w - dx;
    if (dy + h > ren->window->h) h = ren->window->h - dy;
    if (w <= 0 || h <= 0)
        return 0;

    const u8 *src = tex->pixels + (size_t)sy * tex->pitch + (size_t)sx * 4;
    u8 *dst = back_buffer(ren) + (size_t)dy * ren->pitch + (size_t)dx * 4;

    if (tex->blend != SDL_BLENDMODE_BLEND && tex->alphamod == 255)
    {
        for (int y = 0; y < h; y++)
        {
            memcpy(dst, src, (size_t)w * 4);
            src += tex->pitch;
            dst += ren->pitch;
        }
        return 0;
    }

    for (int y = 0; y < h; y++)
    {
        const u32 *s = (const u32 *)src;
        u32 *d = (u32 *)dst;
        for (int x = 0; x < w; x++)
        {
            u32 sp = s[x];
            unsigned a = ((sp >> 24) * tex->alphamod) / 255;
            if (a == 255)
            {
                d[x] = sp;
            }
            else if (a != 0)
            {
                u32 dp = d[x];
                u32 srb = sp & 0x00FF00FF, sg = sp & 0x0000FF00;
                u32 drb = dp & 0x00FF00FF, dg = dp & 0x0000FF00;
                u32 rb = ((srb * a + drb * (255 - a)) >> 8) & 0x00FF00FF;
                u32 g = ((sg * a + dg * (255 - a)) >> 8) & 0x0000FF00;
                d[x] = 0xFF000000u | rb | g;
            }
        }
        src += tex->pitch;
        dst += ren->pitch;
    }
    return 0;
}

extern "C" int SDL_GetRendererInfo(SDL_Renderer *, SDL_RendererInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->name = "circle";
    info->flags = SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC;
    info->num_texture_formats = 1;
    info->texture_formats[0] = SDL_PIXELFORMAT_ARGB8888;
    info->max_texture_width = 4096;
    info->max_texture_height = 4096;
    return 0;
}

extern "C" int SDL_GetNumRenderDrivers(void) { return 1; }

extern "C" int SDL_GetRenderDriverInfo(int, SDL_RendererInfo *info)
{
    return SDL_GetRendererInfo(nullptr, info);
}

extern "C" int SDL_RenderSetViewport(SDL_Renderer *, const SDL_Rect *)
{
    return 0;   // the target is always the whole framebuffer
}

extern "C" int SDL_SetRenderDrawBlendMode(SDL_Renderer *, SDL_BlendMode)
{
    return 0;   // draw ops (clear/fill) are opaque
}

extern "C" int SDL_RenderFillRect(SDL_Renderer *ren, const SDL_Rect *rect)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    int x = rect ? rect->x : 0;
    int y = rect ? rect->y : 0;
    int w = rect ? rect->w : ren->window->w;
    int h = rect ? rect->h : ren->window->h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ren->window->w) w = ren->window->w - x;
    if (y + h > ren->window->h) h = ren->window->h - y;
    if (w <= 0 || h <= 0)
        return 0;

    u32 color = ((u32)ren->a << 24) | ((u32)ren->r << 16) |
                ((u32)ren->g << 8) | ren->b;
    u8 *dst = back_buffer(ren) + (size_t)y * ren->pitch + (size_t)x * 4;
    for (int row = 0; row < h; row++, dst += ren->pitch)
    {
        u32 *d = (u32 *)dst;
        for (int i = 0; i < w; i++)
            d[i] = color;
    }
    return 0;
}

extern "C" int SDL_RenderDrawLine(SDL_Renderer *ren, int x1, int y1,
                                  int x2, int y2)
{
    // horizontal/vertical only (MAME's UI uses axis-aligned lines)
    SDL_Rect r;
    if (y1 == y2)
    {
        r = { x1 < x2 ? x1 : x2, y1, (x1 < x2 ? x2 - x1 : x1 - x2) + 1, 1 };
        return SDL_RenderFillRect(ren, &r);
    }
    if (x1 == x2)
    {
        r = { x1, y1 < y2 ? y1 : y2, 1, (y1 < y2 ? y2 - y1 : y1 - y2) + 1 };
        return SDL_RenderFillRect(ren, &r);
    }
    return 0;
}

extern "C" void SDL_RenderPresent(SDL_Renderer *ren)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    CBcmFrameBuffer *fb = ren->window->fb;
    fb->SetVirtualOffset(0, ren->back * ren->window->h);
    if (ren->vsync)
        fb->WaitForVerticalSync();   // only when the app asked for vsync:
                                     // throttled apps pace themselves, and
                                     // blocking here would double-throttle
    ren->back ^= 1;
}
