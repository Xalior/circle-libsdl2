//
// video.cpp — window / software renderer / streaming texture over
// Circle's CBcmFrameBuffer (double-buffered, vsync page flip).
//
// Scope matches MAME's drawsdl.cpp software path: one fullscreen window,
// an SDL_Renderer, streaming ARGB8888 textures.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include <circle/bcmframebuffer.h>
#include <circle/bcmpropertytags.h>
#include <circle/koptions.h>
#include <circle/logger.h>
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

    // Draw calls become present commands. Single-core they execute
    // immediately (the degenerate case of the same design); under the core
    // split they are recorded here and RenderPresent posts the frame to the
    // presentation worker, which blits and flips off-core.
    SDL2CirclePresentCmd cmds[SDL2CIRCLE_PRESENT_MAX_CMDS];
    unsigned ncmds;
};

struct SDL_Texture
{
    int w, h;
    Uint32 format;
    u8 *pixels[2];     // [1] exists only under the core split: the app
                       // renders into one buffer while the presentation
                       // worker still reads the frame in flight
    u8 widx;           // buffer the app writes next
    u8 posted;         // buffer referenced by the last recorded COPY (0xFF none)
    int pitch;
    SDL_BlendMode blend;
    Uint8 alphamod;
};

// The one fullscreen window (ID 1). Display-mode queries answer with its
// size once it exists, and with the panel default before that.
static SDL_Window *s_window = nullptr;

// The display size is platform boot configuration, consumed the way
// Circle's own samples consume it: `width=`/`height=` in the FAT-root
// cmdline.txt (circle/doc/cmdline.txt) through CKernelOptions. Without
// boot config the panel's own size is used — the same probe (and the
// same sanity clamp) CBcmFrameBuffer performs when constructed with 0x0.
// The VideoCore scaler stretches the framebuffer to the panel.
static int s_display_w = 0, s_display_h = 0;
static const int DEFAULT_HZ = 60;

static void resolve_display_size(void)
{
    if (s_display_w > 0 && s_display_h > 0)
        return;

    CKernelOptions *opts = CKernelOptions::Get();
    if (opts && opts->GetWidth() > 0 && opts->GetHeight() > 0)
    {
        s_display_w = (int)opts->GetWidth();
        s_display_h = (int)opts->GetHeight();
        return;
    }

    CBcmPropertyTags Tags;
    TPropertyTagDisplayDimensions Dim;
    if (Tags.GetTag(PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim)
        && Dim.nWidth >= 640 && Dim.nWidth <= 4096
        && Dim.nHeight >= 480 && Dim.nHeight <= 2160)
    {
        s_display_w = (int)Dim.nWidth;
        s_display_h = (int)Dim.nHeight;
        return;
    }

    s_display_w = 640;
    s_display_h = 480;
}

// Presentation geometry, published when the window exists: the worker core
// executes commands against these (it must never touch SDL structs that the
// app core mutates).
static u8 *s_fb_base = nullptr;
static unsigned s_fb_pitch = 0;
static int s_fb_w = 0, s_fb_h = 0;
// 2 = page-flip between stacked halves; 1 = the firmware's grant cannot hold
// two halves, draw and scan half 0 only (single-buffered, tears instead of
// corrupting memory past the buffer).
static unsigned s_fb_halves = 2;

// Execute one present command into a framebuffer half. Runs on the caller
// single-core, and on the presentation worker under the core split.
void SDL2Circle_VideoExecCmd(const SDL2CirclePresentCmd *cmd, unsigned half)
{
    if (!s_fb_base)
        return;
    u8 *dst0 = s_fb_base + (size_t)half * s_fb_h * s_fb_pitch;

    if (cmd->op == SDL2CirclePresentCmd::FILL)
    {
        u8 *dst = dst0 + (size_t)cmd->dy * s_fb_pitch + (size_t)cmd->dx * 4;
        for (int row = 0; row < cmd->h; row++, dst += s_fb_pitch)
        {
            u32 *d = (u32 *)dst;
            for (int i = 0; i < cmd->w; i++)
                d[i] = cmd->color;
        }
        return;
    }

    // COPY, with straight-alpha blending when the texture asked for it.
    const u8 *src = cmd->src;
    u8 *dst = dst0 + (size_t)cmd->dy * s_fb_pitch + (size_t)cmd->dx * 4;

    if (!cmd->blend && cmd->alphamod == 255)
    {
        for (int y = 0; y < cmd->h; y++)
        {
            memcpy(dst, src, (size_t)cmd->w * 4);
            src += cmd->srcpitch;
            dst += s_fb_pitch;
        }
        return;
    }

    for (int y = 0; y < cmd->h; y++)
    {
        const u32 *s = (const u32 *)src;
        u32 *d = (u32 *)dst;
        for (int x = 0; x < cmd->w; x++)
        {
            u32 sp = s[x];
            unsigned a = ((sp >> 24) * cmd->alphamod) / 255;
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
        src += cmd->srcpitch;
        dst += s_fb_pitch;
    }
}

// Page-flip to a framebuffer half. The firmware mailbox tolerates off-core
// callers (single presentation owner at a time).
void SDL2Circle_VideoFlip(unsigned half)
{
    if (!s_window)
        return;
    boolean ok = s_window->fb->SetVirtualOffset(0, half * (unsigned)s_fb_h);
    // One-shot diagnostic: a firmware that refuses the pan (it reports the
    // granted offset back) silently breaks the page flip — the visible
    // half then only ever receives alternate frames.
    static bool s_flip_logged = false;
    if (!s_flip_logged)
    {
        s_flip_logged = true;
        CLogger::Get()->Write("sdl2video", LogNotice,
                              "first flip to half %u: SetVirtualOffset %s",
                              half, ok ? "ok" : "REFUSED");
    }
}

// Record a command (core split) or execute it into the back half now.
static void emit_cmd(SDL_Renderer *ren, const SDL2CirclePresentCmd &cmd)
{
    if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
    {
        if (ren->ncmds < SDL2CIRCLE_PRESENT_MAX_CMDS)
            ren->cmds[ren->ncmds++] = cmd;
        return;
    }
    SDL2Circle_VideoExecCmd(&cmd, ren->back);
}

static void fill_mode(SDL_DisplayMode *mode)
{
    resolve_display_size();
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
    resolve_display_size();
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

namespace
{
struct CreateWindowArgs
{
    int w, h;
    Uint32 flags;
    SDL_Window *result;
};
}

// The framebuffer allocation (a firmware mailbox transaction plus Circle
// device bookkeeping) runs on core 0; under the core split the window
// creation marshals there through the call mailbox.
static void create_window_on0(void *p)
{
    auto *a = (CreateWindowArgs *)p;
    a->result = nullptr;

    CBcmFrameBuffer *fb =
        new CBcmFrameBuffer(a->w, a->h, 32, 0, 0, 0, TRUE /*double buffered*/);
    if (!fb->Initialize())
    {
        delete fb;
        SDL_SetError("CBcmFrameBuffer::Initialize failed (%dx%d)", a->w, a->h);
        return;
    }

    SDL_Window *win = new SDL_Window;
    win->fb = fb;
    win->w = (int)fb->GetWidth();
    win->h = (int)fb->GetHeight();
    win->flags = a->flags | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN;

    // Publish the presentation geometry before the window becomes visible
    // to the app core or the worker.
    s_fb_base = (u8 *)(uintptr)fb->GetBuffer();
    s_fb_pitch = fb->GetPitch();
    s_fb_w = win->w;
    s_fb_h = win->h;

    // Believe the GRANT, not the request: double buffering draws and pans
    // across 2*h rows, and a firmware that grants fewer rows than that (the
    // Pi 5 grants the native mode's row count regardless of the virtual
    // height it acknowledges) would have every second frame written partly
    // past the buffer and scanned out of it. Fall back to a single half.
    unsigned nRowsGranted = s_fb_pitch != 0 ? fb->GetSize() / s_fb_pitch : 0;
    s_fb_halves = nRowsGranted >= 2u * (unsigned)win->h ? 2 : 1;
    if (s_fb_halves == 1)
        CLogger::Get()->Write("sdl2video", LogWarning,
                              "granted %u rows < %u: single-buffered, no page flip",
                              nRowsGranted, 2u * (unsigned)win->h);

    s_window = win;

    // The one line that proves the geometry chain: boot config (or panel)
    // -> display mode -> window -> this allocation. Virtual height and
    // pitch expose what the firmware really granted: the double-buffer
    // flip needs virt == 2*h, and a pitch wider than the width means the
    // buffer lives inside a native-mode surface (observed on the Pi 5,
    // whose firmware ignores mode requests).
    CLogger::Get()->Write("sdl2video", LogNotice,
                          "framebuffer %ux%u virt %ux%u pitch %u size %u",
                          fb->GetWidth(), fb->GetHeight(),
                          fb->GetVirtWidth(), fb->GetVirtHeight(),
                          fb->GetPitch(), fb->GetSize());

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

    a->result = win;
}

extern "C" SDL_Window *SDL_CreateWindow(const char *, int, int, int w, int h,
                                        Uint32 flags)
{
    CreateWindowArgs a{w, h, flags, nullptr};
    SDL2Circle_CallOn0(create_window_on0, &a);
    return a.result;
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
    {
        s_window = nullptr;
        s_fb_base = nullptr;
    }
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
    // Half 0 is visible after init, so draw into half 1 first -- unless the
    // grant forced single-buffering, where half 0 is all there is.
    ren->back = s_fb_halves == 2 ? 1 : 0;
    ren->vsync = (flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    ren->r = ren->g = ren->b = 0;
    ren->a = 255;
    ren->ncmds = 0;
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
    SDL2CirclePresentCmd cmd;
    cmd.op = SDL2CirclePresentCmd::FILL;
    cmd.dx = 0;
    cmd.dy = 0;
    cmd.w = ren->window->w;
    cmd.h = ren->window->h;
    cmd.color = ((u32)ren->a << 24) | ((u32)ren->r << 16) |
                ((u32)ren->g << 8) | ren->b;
    emit_cmd(ren, cmd);
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
    tex->pixels[0] = (u8 *)malloc((size_t)tex->pitch * h);
    tex->pixels[1] = nullptr;   // allocated on first split-mode reuse
    tex->widx = 0;
    tex->posted = 0xFF;
    tex->blend = SDL_BLENDMODE_NONE;
    tex->alphamod = 255;
    return tex;
}

// Core split: a texture referenced by the frame in flight must not be
// written; hand the app the other buffer. One frame is in flight at most
// (SDL2Circle_PresentPost waits for the previous ACK), so two buffers are
// provably enough. MAME's software path redraws the full texture each
// frame; the partial-update path still copies the stable content across
// first.
static u8 *texture_write_buffer(SDL_Texture *tex, bool preserve)
{
    if (SDL2Circle_SplitActive() && tex->posted == tex->widx)
    {
        u8 next = tex->widx ^ 1;
        if (!tex->pixels[next])
            tex->pixels[next] = (u8 *)malloc((size_t)tex->pitch * tex->h);
        if (preserve)
            memcpy(tex->pixels[next], tex->pixels[tex->widx],
                   (size_t)tex->pitch * tex->h);
        tex->widx = next;
    }
    return tex->pixels[tex->widx];
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
    bool partial = (x != 0) || (y != 0) || (w != tex->w) || (h != tex->h);
    const u8 *src = (const u8 *)pixels;
    u8 *dst = texture_write_buffer(tex, partial)
              + (size_t)y * tex->pitch + (size_t)x * 4;
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
    free(tex->pixels[0]);
    free(tex->pixels[1]);
    delete tex;
}

extern "C" int SDL_LockTexture(SDL_Texture *tex, const SDL_Rect *rect,
                               void **pixels, int *pitch)
{
    u8 *buf = texture_write_buffer(tex, rect != nullptr);
    if (rect)
        *pixels = buf + (size_t)rect->y * tex->pitch + (size_t)rect->x * 4;
    else
        *pixels = buf;
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

    SDL2CirclePresentCmd cmd;
    cmd.op = SDL2CirclePresentCmd::COPY;
    cmd.dx = dx;
    cmd.dy = dy;
    cmd.w = w;
    cmd.h = h;
    cmd.color = 0;
    cmd.src = tex->pixels[tex->widx] + (size_t)sy * tex->pitch + (size_t)sx * 4;
    cmd.srcpitch = tex->pitch;
    cmd.blend = (tex->blend == SDL_BLENDMODE_BLEND) ? 1 : 0;
    cmd.alphamod = tex->alphamod;
    emit_cmd(ren, cmd);

    // This buffer now belongs to the frame being assembled; the next write
    // to the texture switches buffers while it is (or may be) in flight.
    tex->posted = tex->widx;
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

    SDL2CirclePresentCmd cmd;
    cmd.op = SDL2CirclePresentCmd::FILL;
    cmd.dx = x;
    cmd.dy = y;
    cmd.w = w;
    cmd.h = h;
    cmd.color = ((u32)ren->a << 24) | ((u32)ren->r << 16) |
                ((u32)ren->g << 8) | ren->b;
    emit_cmd(ren, cmd);
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

    // Core split: hand the recorded frame to the presentation worker (blit
    // + flip happen off-core; the post waits only for the PREVIOUS frame's
    // acknowledgement, keeping exactly one frame in flight).
    if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
    {
        SDL2Circle_PresentPost(ren->cmds, ren->ncmds, ren->back);
        ren->ncmds = 0;
        if (s_fb_halves == 2)
            ren->back ^= 1;
        return;
    }

    ren->ncmds = 0;   // commands were executed as they were emitted
    CBcmFrameBuffer *fb = ren->window->fb;
    fb->SetVirtualOffset(0, ren->back * ren->window->h);
    if (ren->vsync)
        fb->WaitForVerticalSync();   // only when the app asked for vsync:
                                     // throttled apps pace themselves, and
                                     // blocking here would double-throttle
    if (s_fb_halves == 2)
        ren->back ^= 1;
}
