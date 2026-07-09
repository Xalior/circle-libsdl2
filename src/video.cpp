//
// video.cpp — window / software renderer / streaming texture over
// Circle's CBcmFrameBuffer (double-buffered, vsync page flip).
//
// Scope matches MAME's drawsdl.cpp software path: one fullscreen window,
// an SDL_Renderer, streaming ARGB8888 textures.
//
#include <SDL2/SDL.h>
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
    Uint8 r, g, b, a;  // draw color
};

struct SDL_Texture
{
    int w, h;
    Uint32 format;
    u8 *pixels;
    int pitch;
};

static u8 *back_buffer(SDL_Renderer *ren)
{
    return ren->base + (size_t)ren->back * ren->window->h * ren->pitch;
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
    return win;
}

extern "C" void SDL_DestroyWindow(SDL_Window *win)
{
    if (!win)
        return;
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

extern "C" SDL_Renderer *SDL_CreateRenderer(SDL_Window *win, int, Uint32)
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
    return tex;
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
    // Milestone scope: full-texture, unscaled blit to the top-left of the
    // target (or dstrect position). MAME renders its own scaling, so this
    // covers the drawsdl software path; srcrect subsetting comes later.
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
    for (int y = 0; y < h; y++)
    {
        memcpy(dst, src, (size_t)w * 4);
        src += tex->pitch;
        dst += ren->pitch;
    }
    return 0;
}

extern "C" void SDL_RenderPresent(SDL_Renderer *ren)
{
    CBcmFrameBuffer *fb = ren->window->fb;
    fb->SetVirtualOffset(0, ren->back * ren->window->h);
    fb->WaitForVerticalSync();
    ren->back ^= 1;
}
