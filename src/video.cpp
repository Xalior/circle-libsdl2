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

// Three resolutions are in play, and every piece of geometry below belongs
// to exactly one of them.
//
//   SCANOUT   what the display hardware really scans out. The firmware's
//             geometry answers (the display-dimensions query, an
//             allocation's acknowledged width/height) do not reliably
//             describe it; a grant's pitch and size do. So it is derived
//             from THE framebuffer grant, and the one allocation happens
//             during the display-size resolve, before any window exists.
//             Boards whose firmware honors the boot request (Pi 3, Pi 4)
//             land here on the requested mode; a board whose firmware
//             ignores mode requests (Pi 5) lands on the panel's own mode.
//
//   CANVAS    the resolution the operator asked for, from cmdline.txt
//             width=/height=. It is the world the application is given and
//             the shape that decides the letterboxing. Unset means "no
//             opinion": the canvas becomes the scanout and the canvas hop
//             disappears, which is the autodetected default.
//
//   APPLICATION  whatever SDL_CreateWindow was asked for, and whatever
//             rectangles SDL_RenderCopy is handed. Those rectangles are
//             CANVAS coordinates, because the canvas is the window.
//
// The frame travels application -> canvas -> scanout, and the two hops are
// composed into a single resampling pass at present time.
static int s_scanout_w = 0, s_scanout_h = 0;
static int s_canvas_w = 0, s_canvas_h = 0;

// The canvas rectangle on the scanout, and whether the two coincide (the
// hop is then arithmetically absent, not merely cheap).
static int s_place_x = 0, s_place_y = 0, s_place_w = 0, s_place_h = 0;
static bool s_place_identity = true;

static const int DEFAULT_HZ = 60;

// The one CBcmFrameBuffer, allocated at whichever comes first of the
// display-size resolve and window creation, and never freed: Circle's
// destructor cannot return the firmware's allocation, so a second
// allocation would leak GPU memory. The window adopts this grant.
static CBcmFrameBuffer *s_fb0 = nullptr;

struct AcquireFbArgs { int w, h; };

// The allocation (a firmware mailbox transaction plus Circle device
// bookkeeping) runs on core 0, as window creation always has; callers
// marshal here via SDL2Circle_CallOn0.
static void acquire_fb_on0(void *p)
{
    if (s_fb0)
        return;
    auto *a = (AcquireFbArgs *)p;
    CBcmFrameBuffer *fb =
        new CBcmFrameBuffer(a->w, a->h, 32, 0, 0, 0, TRUE /*double buffered*/);
    if (!fb->Initialize())
    {
        delete fb;
        return;
    }
    s_fb0 = fb;
}

// Place the canvas on the scanout. Fit — aspect preserved, centered, the
// remainder left black — is the default; cmdline.txt `canvas=stretch` asks
// for the whole scanout instead. Called once, when both resolutions exist.
static void resolve_placement(void)
{
    const char *mode;
    if (s_canvas_w == s_scanout_w && s_canvas_h == s_scanout_h)
    {
        s_place_identity = true;
        mode = "identity";
    }
    else
    {
        s_place_identity = false;
        const char *want = nullptr;
        CKernelOptions *opts = CKernelOptions::Get();
        if (opts)
            want = opts->GetAppOptionString("canvas", nullptr);
        mode = (want && strcmp(want, "stretch") == 0) ? "stretch" : "fit";
    }

    if (s_place_identity || strcmp(mode, "stretch") == 0)
    {
        s_place_x = 0;
        s_place_y = 0;
        s_place_w = s_scanout_w;
        s_place_h = s_scanout_h;
    }
    else
    {
        // 16.16 fixed point: the smaller axis ratio is the one that fits,
        // and it must survive the divide before it multiplies back out.
        u32 rx = ((u32)s_scanout_w << 16) / (u32)s_canvas_w;
        u32 ry = ((u32)s_scanout_h << 16) / (u32)s_canvas_h;
        u32 r = rx < ry ? rx : ry;
        s_place_w = (int)(((u64)s_canvas_w * r) >> 16);
        s_place_h = (int)(((u64)s_canvas_h * r) >> 16);
        s_place_x = (s_scanout_w - s_place_w) / 2;
        s_place_y = (s_scanout_h - s_place_h) / 2;
    }

    CLogger::Get()->Write("sdl2video", LogNotice,
                          "canvas %dx%d on scanout %dx%d: %s -> %dx%d+%d+%d",
                          s_canvas_w, s_canvas_h, s_scanout_w, s_scanout_h,
                          mode, s_place_w, s_place_h, s_place_x, s_place_y);
}

static void resolve_display_size(void)
{
    if (s_canvas_w > 0 && s_canvas_h > 0)
        return;

    // Request the boot canvas (cmdline width=/height=); the GRANT decides
    // the scanout. An unset canvas still needs a request to allocate
    // against, and the grant overrules it either way.
    int req_w = 0, req_h = 0;
    CKernelOptions *opts = CKernelOptions::Get();
    if (opts && opts->GetWidth() > 0 && opts->GetHeight() > 0)
    {
        req_w = (int)opts->GetWidth();
        req_h = (int)opts->GetHeight();
    }

    AcquireFbArgs a{req_w > 0 ? req_w : 640, req_h > 0 ? req_h : 480};
    SDL2Circle_CallOn0(acquire_fb_on0, &a);

    const char *source;
    if (s_fb0 && s_fb0->GetPitch() == s_fb0->GetWidth() * 4)
    {
        // pitch agrees with the acknowledged width: take the
        // acknowledged mode as the scanout. Not size/pitch — size spans
        // every granted row, which is 2*h when the double-buffer
        // virtual height was granted, not the display height.
        s_scanout_w = (int)s_fb0->GetWidth();
        s_scanout_h = (int)s_fb0->GetHeight();
        source = "grant, request honored";
    }
    else if (s_fb0 && s_fb0->GetPitch() != 0)
    {
        // pitch disagrees with the acknowledged width: the acknowledged
        // mode was not granted. pitch/4 columns by size/pitch rows is
        // the surface actually granted (on the Pi 5, the native mode).
        s_scanout_w = (int)(s_fb0->GetPitch() / 4);
        s_scanout_h = (int)(s_fb0->GetSize() / s_fb0->GetPitch());
        source = "grant, native surface";
    }
    else
    {
        // No grant at all: report the request, the least-wrong answer.
        s_scanout_w = a.w;
        s_scanout_h = a.h;
        source = "no grant, cmdline request";
    }

    // No canvas asked for means no opinion: the canvas IS the scanout and
    // the canvas hop costs nothing, with nothing to configure.
    const char *csource = "cmdline width=/height=";
    if (req_w <= 0 || req_h <= 0)
    {
        req_w = s_scanout_w;
        req_h = s_scanout_h;
        csource = "unset, follows the scanout";
    }
    s_canvas_w = req_w;
    s_canvas_h = req_h;

    CLogger::Get()->Write("sdl2video", LogNotice,
                          "scanout %dx%d (%s), canvas %dx%d (%s)",
                          s_scanout_w, s_scanout_h, source,
                          s_canvas_w, s_canvas_h, csource);

    resolve_placement();
}

// Presentation geometry, published when the window exists: the worker core
// executes commands against these (it must never touch SDL structs that the
// app core mutates).
static u8 *s_fb_base = nullptr;
static unsigned s_fb_pitch = 0;
static int s_fb_w = 0, s_fb_h = 0;
// 2 = page-flip between stacked halves; 1 = the firmware's grant cannot hold
// two halves: present commands render into a SHADOW back buffer instead,
// blitted into the granted surface on flip -- true double buffering (no
// partial frame is ever scanned) without touching memory past the grant.
static unsigned s_fb_halves = 2;
static u8 *s_shadow = nullptr;         // back buffer when s_fb_halves == 1
static unsigned s_shadow_pitch = 0;

// ---- the scaler -------------------------------------------------------------
//
// Nearest-neighbour resampling with precomputed per-axis index tables. The
// tables depend only on the source and destination extents, so a steady
// stream of identical frames builds them once and reuses them; a consumer
// that changes geometry pays one rebuild.
//
// Only the presentation owner ever runs a command — the worker core under
// the core split, the calling core without it — so a single set of tables
// is enough and no lock is needed. There is never a second scaler in
// flight.
static const int SCALE_MAP_MAX = 8192;   // covers any scanout up to 8K
static u16 s_xmap[SCALE_MAP_MAX];
static u16 s_ymap[SCALE_MAP_MAX];
static int s_map_sw = 0, s_map_sh = 0, s_map_dw = 0, s_map_dh = 0;

static void build_scale_maps(int sw, int sh, int dw, int dh)
{
    if (s_map_sw == sw && s_map_sh == sh && s_map_dw == dw && s_map_dh == dh)
        return;
    for (int i = 0; i < dw; i++)
        s_xmap[i] = (u16)(((s64)i * sw) / dw);
    for (int j = 0; j < dh; j++)
        s_ymap[j] = (u16)(((s64)j * sh) / dh);
    s_map_sw = sw; s_map_sh = sh; s_map_dw = dw; s_map_dh = dh;
}

// Resample sw x sh source pixels onto dw x dh destination pixels.
static void scale_copy(const SDL2CirclePresentCmd *cmd, u8 *dst, unsigned dpitch,
                       int sw, int sh)
{
    const int dw = cmd->w, dh = cmd->h;
    if (dw > SCALE_MAP_MAX || dh > SCALE_MAP_MAX)
        return;                          // beyond any real scanout
    build_scale_maps(sw, sh, dw, dh);

    // An integer horizontal ratio replicates each source pixel a fixed
    // number of times, which needs no table lookup at all — the common
    // case for an emulator raster lifted onto a panel.
    const int xrep = (dw % sw == 0) ? dw / sw : 0;

    if (!cmd->blend && cmd->alphamod == 255)
    {
        int prev_srow = -1;
        const u8 *prev_dst = nullptr;
        u8 *drow = dst;
        for (int j = 0; j < dh; j++, drow += dpitch)
        {
            int srow = s_ymap[j];
            if (srow == prev_srow)
            {
                // Vertical magnification: this destination row is the one
                // just built. Copying it back beats resampling it again.
                memcpy(drow, prev_dst, (size_t)dw * 4);
                continue;
            }
            const u32 *s = (const u32 *)(cmd->src + (size_t)srow * cmd->srcpitch);
            u32 *d = (u32 *)drow;
            if (xrep)
            {
                for (int i = 0, x = 0; i < sw; i++)
                {
                    u32 p = s[i];
                    for (int r = 0; r < xrep; r++)
                        d[x++] = p;
                }
            }
            else
            {
                for (int i = 0; i < dw; i++)
                    d[i] = s[s_xmap[i]];
            }
            prev_srow = srow;
            prev_dst = drow;
        }
        return;
    }

    // Blended: the destination is read as well as written, so no row can be
    // reused — every destination pixel is composited in place.
    u8 *drow = dst;
    for (int j = 0; j < dh; j++, drow += dpitch)
    {
        const u32 *s = (const u32 *)(cmd->src + (size_t)s_ymap[j] * cmd->srcpitch);
        u32 *d = (u32 *)drow;
        for (int i = 0; i < dw; i++)
        {
            u32 sp = s[s_xmap[i]];
            unsigned a = ((sp >> 24) * cmd->alphamod) / 255;
            if (a == 255)
            {
                d[i] = sp;
            }
            else if (a != 0)
            {
                u32 dp = d[i];
                u32 srb = sp & 0x00FF00FF, sg = sp & 0x0000FF00;
                u32 drb = dp & 0x00FF00FF, dg = dp & 0x0000FF00;
                u32 rb = ((srb * a + drb * (255 - a)) >> 8) & 0x00FF00FF;
                u32 g = ((sg * a + dg * (255 - a)) >> 8) & 0x0000FF00;
                d[i] = 0xFF000000u | rb | g;
            }
        }
    }
}

// Execute one present command into a framebuffer half. Runs on the caller
// single-core, and on the presentation worker under the core split.
void SDL2Circle_VideoExecCmd(const SDL2CirclePresentCmd *cmd, unsigned half)
{
    if (!s_fb_base)
        return;
    u8 *dst0;
    unsigned dpitch;
    if (s_shadow)
    {
        dst0 = s_shadow;               // both halves render into the shadow
        dpitch = s_shadow_pitch;
    }
    else
    {
        dst0 = s_fb_base + (size_t)half * s_fb_h * s_fb_pitch;
        dpitch = s_fb_pitch;
    }

    if (cmd->op == SDL2CirclePresentCmd::FILL)
    {
        u8 *dst = dst0 + (size_t)cmd->dy * dpitch + (size_t)cmd->dx * 4;
        for (int row = 0; row < cmd->h; row++, dst += dpitch)
        {
            u32 *d = (u32 *)dst;
            for (int i = 0; i < cmd->w; i++)
                d[i] = cmd->color;
        }
        return;
    }

    // COPY, with straight-alpha blending when the texture asked for it.
    const u8 *src = cmd->src;
    u8 *dst = dst0 + (size_t)cmd->dy * dpitch + (size_t)cmd->dx * 4;

    // The destination extent already carries BOTH geometry hops, so one
    // pass here covers application frame -> canvas -> scanout. Equal
    // extents are the unscaled blit below, unchanged to the byte.
    const int sw = cmd->sw > 0 ? cmd->sw : cmd->w;
    const int sh = cmd->sh > 0 ? cmd->sh : cmd->h;
    if (sw != cmd->w || sh != cmd->h)
    {
        scale_copy(cmd, dst, dpitch, sw, sh);
        return;
    }

    if (!cmd->blend && cmd->alphamod == 255)
    {
        for (int y = 0; y < cmd->h; y++)
        {
            memcpy(dst, src, (size_t)cmd->w * 4);
            src += cmd->srcpitch;
            dst += dpitch;
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
        dst += dpitch;
    }
}

// Page-flip to a framebuffer half. The firmware mailbox tolerates off-core
// callers (single presentation owner at a time).
void SDL2Circle_VideoFlip(unsigned half)
{
    if (!s_window)
        return;
    if (s_shadow)
    {
        // Shadow-buffered present: the finished back buffer becomes visible
        // by one blit into the granted surface. The vsync wait (honored
        // where the firmware implements it) keeps the blit off the raster.
        s_window->fb->WaitForVerticalSync();
        const u8 *src = s_shadow;
        u8 *dst = s_fb_base;
        for (int y = 0; y < s_fb_h; y++, src += s_shadow_pitch, dst += s_fb_pitch)
            memcpy(dst, src, (size_t)s_fb_w * 4);
        return;
    }
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

// Canvas coordinates -> scanout coordinates. Both edges are mapped
// independently rather than mapping the origin and scaling the extent, so
// rectangles that abut in the canvas still abut on the scanout instead of
// leaving a seam where the two divisions round apart. A COPY keeps its
// source extent untouched: the executor resamples straight from the source
// onto this composed destination, so the canvas contributes arithmetic and
// never an intermediate copy.
//
// Returns false when nothing survives the mapping.
static bool place_on_scanout(SDL2CirclePresentCmd *cmd)
{
    if (cmd->w <= 0 || cmd->h <= 0)
        return false;
    if (s_place_identity)
        return true;

    int x0 = s_place_x + (int)(((s64)cmd->dx * s_place_w) / s_canvas_w);
    int x1 = s_place_x + (int)(((s64)(cmd->dx + cmd->w) * s_place_w) / s_canvas_w);
    int y0 = s_place_y + (int)(((s64)cmd->dy * s_place_h) / s_canvas_h);
    int y1 = s_place_y + (int)(((s64)(cmd->dy + cmd->h) * s_place_h) / s_canvas_h);
    cmd->dx = x0;
    cmd->dy = y0;
    cmd->w = x1 - x0;
    cmd->h = y1 - y0;
    return cmd->w > 0 && cmd->h > 0;
}

// The one line that makes the whole geometry chain readable on a serial
// console: what the application handed over, and what it becomes on the
// glass. Once per mapping, because a steady stream repeats it every frame.
static void log_copy_geometry(const SDL2CirclePresentCmd &app,
                              const SDL2CirclePresentCmd &out, int sw, int sh)
{
    static int last_sw = 0, last_sh = 0, last_dw = 0, last_dh = 0;
    if (sw == last_sw && sh == last_sh && out.w == last_dw && out.h == last_dh)
        return;
    last_sw = sw; last_sh = sh; last_dw = out.w; last_dh = out.h;

    const char *how;
    if (sw == out.w && sh == out.h)
        how = "1:1 blit";
    else if (out.w % sw == 0 && out.h % sh == 0)
        how = "nearest, integer ratio";
    else
        how = "nearest";
    CLogger::Get()->Write("sdl2video", LogNotice,
                          "copy src %dx%d -> canvas %dx%d+%d+%d -> scanout %dx%d+%d+%d (%s)",
                          sw, sh,
                          app.w, app.h, app.dx, app.dy,
                          out.w, out.h, out.dx, out.dy, how);
}

// Record a command (core split) or execute it into the back half now. The
// canvas hop is resolved here, once, so the recorded command and the
// directly executed one carry identical geometry.
static void emit_cmd(SDL_Renderer *ren, const SDL2CirclePresentCmd &cmd)
{
    SDL2CirclePresentCmd out = cmd;
    if (!place_on_scanout(&out))
        return;
    if (out.op == SDL2CirclePresentCmd::COPY)
        log_copy_geometry(cmd, out, cmd.sw, cmd.sh);

    if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
    {
        if (ren->ncmds < SDL2CIRCLE_PRESENT_MAX_CMDS)
            ren->cmds[ren->ncmds++] = out;
        return;
    }
    SDL2Circle_VideoExecCmd(&out, ren->back);
}

static void fill_mode(SDL_DisplayMode *mode)
{
    resolve_display_size();
    mode->format = SDL_PIXELFORMAT_ARGB8888;
    mode->w = s_window ? s_window->w : s_canvas_w;
    mode->h = s_window ? s_window->h : s_canvas_h;
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
    rect->w = s_window ? s_window->w : s_canvas_w;
    rect->h = s_window ? s_window->h : s_canvas_h;
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

    // Adopt THE framebuffer — usually already allocated by the display-
    // size resolve, which also settles the scanout, the canvas and the
    // placement between them. A consumer that creates a window without ever
    // asking for the display size arrives here first; the resolve is
    // idempotent and this is already core 0, so run it either way.
    resolve_display_size();
    CBcmFrameBuffer *fb = s_fb0;
    if (!fb)
    {
        SDL_SetError("CBcmFrameBuffer::Initialize failed (%dx%d)", a->w, a->h);
        return;
    }

    // The window is the CANVAS — the world the operator declared, which on
    // a board whose firmware granted the boot request is the scanout itself.
    // What the application asked SDL_CreateWindow for does not enter into
    // it: there is one screen and the application gets all of it. The shim's
    // present carries the canvas to the scanout, so an application never has
    // to learn what the glass is really doing.
    SDL_Window *win = new SDL_Window;
    win->fb = fb;
    win->w = s_canvas_w;
    win->h = s_canvas_h;
    win->flags = a->flags | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN;

    // Publish the presentation geometry before the window becomes visible
    // to the app core or the worker. This side is SCANOUT geometry: every
    // present command has already been mapped out of canvas coordinates by
    // the time it reaches the framebuffer.
    s_fb_base = (u8 *)(uintptr)fb->GetBuffer();
    s_fb_pitch = fb->GetPitch();
    s_fb_w = s_scanout_w;
    s_fb_h = s_scanout_h;

    // Believe the GRANT, not the request: double buffering draws and pans
    // across 2*h rows, and a firmware that grants fewer rows than that (the
    // Pi 5 grants the native mode's row count regardless of the virtual
    // height it acknowledges) would have every second frame written partly
    // past the buffer and scanned out of it. Fall back to a single half.
    unsigned nRowsGranted = s_fb_pitch != 0 ? fb->GetSize() / s_fb_pitch : 0;
    s_fb_halves = nRowsGranted >= 2u * (unsigned)s_fb_h ? 2 : 1;
    if (s_fb_halves == 1)
    {
        s_shadow_pitch = (unsigned)s_fb_w * 4;
        s_shadow = (u8 *)calloc((size_t)s_shadow_pitch, s_fb_h);
        CLogger::Get()->Write("sdl2video", LogWarning,
                              "granted %u rows < %u: shadow-buffered present",
                              nRowsGranted, 2u * (unsigned)s_fb_h);
    }
    else
    {
        // Fit leaves borders no command will ever write — every present
        // command is clipped to the canvas rectangle. Black them once, here,
        // across every granted row so both halves start clean. (The shadow
        // path gets this from calloc.)
        memset(s_fb_base, 0, fb->GetSize());
    }

    s_window = win;

    // The one line that proves the geometry chain: boot config (or panel)
    // -> display mode -> window -> this allocation. Virtual height and
    // pitch expose what the firmware really granted: the double-buffer
    // flip needs virt == 2*h, and a pitch wider than the width means the
    // buffer lives inside a native-mode surface (observed on the Pi 5,
    // whose firmware ignores mode requests).
    CLogger::Get()->Write("sdl2video", LogNotice,
                          "framebuffer %ux%u virt %ux%u depth %u pitch %u size %u",
                          fb->GetWidth(), fb->GetHeight(),
                          fb->GetVirtWidth(), fb->GetVirtHeight(),
                          fb->GetDepth(), fb->GetPitch(), fb->GetSize());

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
    // win->fb is THE framebuffer (s_fb0), kept for the process lifetime:
    // deleting it cannot return the firmware's allocation, and the next
    // window must adopt the same grant rather than allocate a leak.
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
    cmd.sw = 0;
    cmd.sh = 0;
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

// Clip one axis of a scaled blit: trim the destination span to [0, limit)
// and take the source span with it, in proportion, so the scale factor
// survives the clip instead of quietly changing.
static void clip_axis(int &d, int &dlen, int &s, int &slen, int limit)
{
    if (d < 0)
    {
        int cut = -d;
        if (cut >= dlen) { dlen = 0; return; }
        int scut = (int)(((s64)cut * slen) / dlen);
        s += scut;
        slen -= scut;
        dlen -= cut;
        d = 0;
    }
    if (d + dlen > limit)
    {
        int cut = d + dlen - limit;
        if (cut >= dlen) { dlen = 0; return; }
        slen -= (int)(((s64)cut * slen) / dlen);
        dlen -= cut;
    }
}

extern "C" int SDL_RenderCopy(SDL_Renderer *ren, SDL_Texture *tex,
                              const SDL_Rect *srcrect, const SDL_Rect *dstrect)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);

    // SDL semantics: absent rectangles mean the whole texture and the whole
    // render target, and a destination that differs from the source scales.
    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;
    int sw = srcrect ? srcrect->w : tex->w;
    int sh = srcrect ? srcrect->h : tex->h;
    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;
    int dw = dstrect ? dstrect->w : ren->window->w;
    int dh = dstrect ? dstrect->h : ren->window->h;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return 0;

    // Keep the source inside the texture (reading past a texture allocation
    // is a fault with nothing underneath to catch it), then the destination
    // inside the canvas. Each clip carries the other rectangle with it.
    clip_axis(sx, sw, dx, dw, tex->w);
    clip_axis(sy, sh, dy, dh, tex->h);
    clip_axis(dx, dw, sx, sw, ren->window->w);
    clip_axis(dy, dh, sy, sh, ren->window->h);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return 0;

    SDL2CirclePresentCmd cmd;
    cmd.op = SDL2CirclePresentCmd::COPY;
    cmd.dx = dx;
    cmd.dy = dy;
    cmd.w = dw;
    cmd.h = dh;
    cmd.color = 0;
    cmd.src = tex->pixels[tex->widx] + (size_t)sy * tex->pitch + (size_t)sx * 4;
    cmd.srcpitch = tex->pitch;
    cmd.sw = sw;
    cmd.sh = sh;
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
    return 0;   // the target is always the whole canvas
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
    cmd.sw = 0;
    cmd.sh = 0;
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

extern "C" int SDL_RenderDrawRect(SDL_Renderer *ren, const SDL_Rect *rect)
{
    // The outline of a rectangle is its four edges. Without a rectangle SDL
    // outlines the whole target, which here is the whole window.
    SDL_Rect r = rect ? *rect
                      : SDL_Rect{ 0, 0, ren->window->w, ren->window->h };
    if (r.w <= 0 || r.h <= 0)
        return 0;

    int right  = r.x + r.w - 1;
    int bottom = r.y + r.h - 1;
    if (SDL_RenderDrawLine(ren, r.x, r.y, right, r.y) < 0
        || SDL_RenderDrawLine(ren, r.x, bottom, right, bottom) < 0
        || SDL_RenderDrawLine(ren, r.x, r.y, r.x, bottom) < 0
        || SDL_RenderDrawLine(ren, right, r.y, right, bottom) < 0)
        return -1;
    return 0;
}

// Presented-frame counter, read by the pump's heartbeat so the frame rate
// is on the serial log without the app's help.
unsigned g_SDL2CirclePresents = 0;

extern "C" void SDL_RenderPresent(SDL_Renderer *ren)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    g_SDL2CirclePresents++;

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
    // One presentation path for page-flip and shadow-buffered surfaces
    // alike: VideoFlip pans, or blits the shadow, as the grant dictates.
    SDL2Circle_VideoFlip(ren->back);
    if (ren->vsync)
        ren->window->fb->WaitForVerticalSync();
                                     // only when the app asked for vsync:
                                     // throttled apps pace themselves, and
                                     // blocking here would double-throttle
    if (s_fb_halves == 2)
        ren->back ^= 1;
}
