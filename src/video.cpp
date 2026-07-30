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
#include <circle/dmachannel.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/machineinfo.h>
#include <circle/synchronize.h>
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
    // Draw calls are RECORDED here, in canvas coordinates, and nothing is
    // executed until SDL_RenderPresent. What crosses to the presentation
    // core then is a finished frame, never this list.
    SDL2CirclePresentCmd cmds[SDL2CIRCLE_PRESENT_MAX_CMDS];
    unsigned ncmds;

    // Set once a frame has stopped being a simple one and is being drawn
    // into the canvas surface instead. Cleared at the start of each frame.
    bool rasterizing;

    // What the border looked like when it was last painted: the colour it
    // was painted in, and the frame rectangle it was painted around.
    // Borders are geometry, so they are repainted when this changes and
    // never otherwise.
    u32 border_color;
    int frame_x, frame_y, frame_w, frame_h;
    unsigned border_repaint;   // frames still owing a repaint
};

// The canvas-resolution surface a frame is rasterized into when it is not
// the simple shape. Allocated the first time one occurs, and never if none
// ever does — an application that clears and blits a texture, which is most
// of them, never touches this.
// Two of them, alternating, for the same reason the textures come in
// pairs: the presentation core may still be reading the one that was
// posted while this thread starts drawing the next.
static u8 *s_canvas_surface_buf[2] = { nullptr, nullptr };
static u8 *s_canvas_surface = nullptr;
static unsigned s_canvas_surface_idx = 0;
static unsigned s_canvas_surface_pitch = 0;

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
//   SCANOUT   the PHYSICAL display: what the hardware really puts on the
//             wire. cmdline.txt width=/height= asks the firmware for a mode,
//             the allocation is what sets it, and the firmware then reports
//             the mode it actually set. THAT REPORT IS THE SCANOUT — read
//             from the firmware, never calculated. Not from the pitch, not
//             from the buffer size, and not from the width and height Circle
//             echoes back out of its own constructor.
//
//             It belongs to the presentation path: the placement below and
//             the present executor are its only readers, and nothing in SDL
//             is ever answered with it.
//
//   CANVAS    the VIRTUAL display: the world the application is given, and
//             the shape that decides the letterboxing. The consumer declares
//             it before SDL_Init, and where the consumer got the numbers is
//             the consumer's business entirely — this library is told, and
//             discovers nothing. It is REQUIRED and it has no fallback: not
//             the command line, not the scanout, nothing. Undeclared, there
//             is no canvas, and the library refuses to start rather than
//             invent a display the consumer never asked for.
//
//   APPLICATION  whatever SDL_CreateWindow was asked for, and whatever
//             rectangles SDL_RenderCopy is handed. Those rectangles are
//             CANVAS coordinates, because the canvas is the window.
//
// The physical and the virtual are two numbers doing two jobs. Neither
// overrides the other and there is no order of precedence between them: one
// is asked of the firmware, the other is declared by the consumer, and the
// whole job of this file is to scale the second onto the first. The frame
// travels application -> canvas -> scanout, and the two hops are composed
// into a single resampling pass at present time.
static int s_scanout_w = 0, s_scanout_h = 0;
static int s_canvas_w = 0, s_canvas_h = 0;

// The virtual display device, as declared by the consumer through
// SDL2Circle_DeclareVirtualDevice. It states the canvas and nothing else:
// the physical mode remains what the firmware was asked for and what the
// firmware granted, neither of which this declaration touches.
//
// A resolved canvas (s_canvas_w > 0) is the point after which no declaration
// can be taken: everything downstream — the placement, the window, the
// display-mode answers — has been derived from the canvas by then, and the
// declaration promises a display whose size does not change under the
// application. This is settled before the application starts, on one core,
// so the two flags need no more protection than that.
static bool s_declared = false;
static int s_declared_w = 0, s_declared_h = 0;

// The framebuffer is allocated at 32 bits per pixel and streaming ARGB8888
// is the only texture format, so this is the whole of what the library can
// present. A declaration at any other depth is refused rather than served at
// this one.
static const unsigned VIRTUAL_DEVICE_DEPTH = 32;

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

// The physical display the firmware reports once the allocation has set it.
// Zero until it has been asked, and zero if the firmware declines to say.
static int s_phys_w = 0, s_phys_h = 0;

// Ask the firmware what the physical display is. Core 0 only — it is a
// mailbox transaction — and only meaningful after the allocation, because
// the allocation is what sets the mode and this reads back what was set.
//
// Circle cannot be asked this, which is why the question is put again here.
// CBcmFrameBuffer::Initialize sends one combined tag call and the firmware
// writes its real answer back into those same tag structures — Circle relies
// on that itself, testing the returned physical width and height for zero
// before it will accept the allocation. It then keeps only the buffer
// address, the size and the pitch. Its m_nWidth and m_nHeight are never
// updated, so GetWidth() and GetHeight() go on echoing the constructor's
// arguments for the object's whole life, and the reply that holds the real
// answer is private and unreachable from outside.
static void read_physical_display(void)
{
    CBcmPropertyTags tags;
    TPropertyTagDisplayDimensions dim;
    memset(&dim, 0, sizeof(dim));
    if (!tags.GetTag(PROPTAG_GET_DISPLAY_DIMENSIONS, &dim, sizeof(dim)))
        return;
    if (dim.nWidth == 0 || dim.nHeight == 0)
        return;
    s_phys_w = (int)dim.nWidth;
    s_phys_h = (int)dim.nHeight;
}

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

    // Read the mode back on this same trip to core 0, now that setting it
    // has happened.
    read_physical_display();
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
        // Fit, in exact integer arithmetic.
        //
        // THIS RUNS ONCE, at startup, on core 0, before a single frame
        // exists. It can afford to be slow, so it is written for obvious
        // correctness and for nothing else. Do not shave operations here, do
        // not fold the steps together to save a divide, and above all do not
        // reintroduce a scale factor held as a fraction.
        //
        // That is what this used to do — a 16.16 fixed-point ratio — and the
        // reason it was wrong is that 2.4 has no exact representation in
        // 16.16. An 800x450 canvas on a 1920x1080 scanout is exactly 2.4, so
        // it came out 1919x1079 and left a black line down the right edge
        // and along the bottom. Ratios that happen to be representable, 1.5
        // and 3.0 among them, were always right, which is what made it look
        // sound. No amount of care inside that scheme reaches the exact case.
        //
        // All values are 64-bit throughout rather than reasoning about where
        // 32 bits would still be wide enough.
        const s64 scanout_w = s_scanout_w, scanout_h = s_scanout_h;
        const s64 canvas_w  = s_canvas_w,  canvas_h  = s_canvas_h;

        // The two candidate scale factors are scanout_w/canvas_w and
        // scanout_h/canvas_h, and the smaller of the two is the one that
        // fits. Cross-multiplying compares them without forming either, so
        // nothing is rounded in order to make the choice.
        const s64 by_width  = scanout_w * canvas_h;
        const s64 by_height = scanout_h * canvas_w;

        if (by_width <= by_height)
        {
            // Width limits — including when the two are equal, which is the
            // matching-aspect case that has to land whole. The picture is
            // the full width of the scanout, exactly, with no arithmetic on
            // that axis at all.
            s_place_w = (int)scanout_w;
            s_place_h = (int)(by_width / canvas_w);
        }
        else
        {
            // Height limits: the picture is the full height of the scanout,
            // exactly.
            s_place_h = (int)scanout_h;
            s_place_w = (int)(by_height / canvas_h);
        }

        // A canvas that genuinely cannot fit whole floors on the derived
        // axis, which is the right direction: the picture stays inside the
        // scanout. What is left over is split evenly and stays black.
        s_place_x = (s_scanout_w - s_place_w) / 2;
        s_place_y = (s_scanout_h - s_place_h) / 2;
    }

    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                          "canvas %dx%d on scanout %dx%d: %s -> %dx%d+%d+%d",
                          s_canvas_w, s_canvas_h, s_scanout_w, s_scanout_h,
                          mode, s_place_w, s_place_h, s_place_x, s_place_y);
}

// Take the consumer's declaration of the virtual display device, or refuse
// it and say why. The refusal reaches the caller the way every other refusal
// in this library does, as an SDL error behind a -1 return; it is not
// logged, because a declaration is made before SDL_Init and the log route
// reaches a host kernel's logger, which may not exist that early. An
// accepted declaration is named on the resolve's own geometry line below.
extern "C" int SDL2Circle_DeclareVirtualDevice(unsigned depth, int width,
                                               int height)
{
    // The two state tests come before the values, and in this order. Once
    // the canvas is resolved nothing can be declared at all, so reporting a
    // bad value there would suggest that correcting it would help.
    if (s_canvas_w > 0 && s_canvas_h > 0)
        return SDL_SetError("SDL2Circle_DeclareVirtualDevice: the display "
                            "size is already resolved");
    if (s_declared)
        return SDL_SetError("SDL2Circle_DeclareVirtualDevice: a virtual "
                            "device of %dx%d is already declared",
                            s_declared_w, s_declared_h);
    if (depth != VIRTUAL_DEVICE_DEPTH)
        return SDL_SetError("SDL2Circle_DeclareVirtualDevice: %u bits per "
                            "pixel; only %u is implemented",
                            depth, VIRTUAL_DEVICE_DEPTH);
    if (width <= 0 || height <= 0)
        return SDL_SetError("SDL2Circle_DeclareVirtualDevice: %dx%d is not a "
                            "display size", width, height);

    s_declared_w = width;
    s_declared_h = height;
    s_declared = true;
    return 0;
}

// Non-zero once a virtual device has been declared. SDL_Init asks, because
// the declaration is what the library needs before it can start at all.
bool SDL2Circle_VirtualDeviceDeclared(void)
{
    return s_declared;
}

// Settle the canvas, the scanout and the placement between them. True once
// the canvas exists; false when nothing was declared, which is the one
// unanswerable state.
//
// SDL_Init is where that is reported loudly — a consumer is refused at the
// door, once, rather than at every call afterwards. Reaching here undeclared
// means SDL_Init already said no and was ignored, so this only sets the
// error and refuses: the canvas stays zero, and everything derived from it
// (the placement divides by it) is never computed from nothing.
static bool resolve_display_size(void)
{
    if (s_canvas_w > 0 && s_canvas_h > 0)
        return true;

    if (!s_declared)
    {
        SDL_SetError("no virtual display device has been declared "
                     "(SDL2Circle_DeclareVirtualDevice)");
        return false;
    }

    // The boot options' width=/height= is a request to the firmware for a
    // PHYSICAL DISPLAY MODE, and that is the whole of what it is. It is not
    // a canvas source and is never read as one.
    //
    // Unset, the request is ZERO BY ZERO, which is Circle's "no size
    // requested": its CBcmFrameBuffer constructor then asks the firmware for
    // the display's own dimensions and allocates that. Naming a size here
    // instead would SET that mode — the allocation is what sets it — so a
    // default of any kind drives the panel at a resolution nobody asked for
    // and then reads it back as though it were the display's own.
    int req_w = 0, req_h = 0;
    CKernelOptions *opts = CKernelOptions::Get();
    if (opts && opts->GetWidth() > 0 && opts->GetHeight() > 0)
    {
        req_w = (int)opts->GetWidth();
        req_h = (int)opts->GetHeight();
    }

    AcquireFbArgs a{req_w, req_h};
    SDL2Circle_CallOn0(acquire_fb_on0, &a);

    // The scanout is what the firmware says it set. Reported, not worked
    // out: a board that honors the request and a board that ignores it (the
    // Pi 5 scans out its panel's own mode whatever it is asked) both answer
    // this question correctly for themselves, and neither needs the library
    // to guess on its behalf.
    const char *source;
    if (s_phys_w > 0 && s_phys_h > 0)
    {
        s_scanout_w = s_phys_w;
        s_scanout_h = s_phys_h;
        source = "firmware reported";
    }
    else if (a.w > 0 && a.h > 0)
    {
        // The firmware would not say, but a mode was named on the command
        // line, so that is the only figure left. It is the one case where
        // the scanout is not a measured one, and the log says so rather than
        // presenting it as an answer.
        s_scanout_w = a.w;
        s_scanout_h = a.h;
        source = "firmware silent, physical request";
    }
    else if (s_fb0)
    {
        // Nothing named and the firmware silent to us. Circle asked the same
        // question in its constructor and allocated against whatever it got,
        // so its width and height are the geometry the grant was actually
        // made at. They are only an echo of a request when a request was
        // made, and none was.
        s_scanout_w = (int)s_fb0->GetWidth();
        s_scanout_h = (int)s_fb0->GetHeight();
        source = "firmware silent, grant geometry";
    }
    else
    {
        // No grant and no answer: there is no display to describe.
        SDL_SetError("the display size cannot be determined");
        return false;
    }

    // The declared virtual device is the canvas, and the only thing that
    // ever is. The boot options are not consulted here — they asked for a
    // physical mode and that is all they did.
    s_canvas_w = s_declared_w;
    s_canvas_h = s_declared_h;

    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                          "scanout %dx%d (%s), canvas %dx%d (declared "
                          "virtual device)",
                          s_scanout_w, s_scanout_h, source,
                          s_canvas_w, s_canvas_h);

    resolve_placement();
    return true;
}

// Presentation geometry, published when the window exists: the worker core
// executes commands against these (it must never touch SDL structs that the
// application core mutates).
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
static size_t s_shadow_bytes = 0;

// The shadow-to-framebuffer copy is the whole scanout every frame, and the
// framebuffer is uncached, so on the CPU it is the most expensive thing the
// presentation core does. The DMA engine does it instead, asynchronously,
// while the CPU gets on with the next frame.
//
// Only a DMA4 "large address" channel can reach the framebuffer's address
// range, which is why the request below asks for one specifically. A Pi 3
// has no such engine — and no shadow path either, because its firmware
// grants the two halves a page flip needs, so it never arrives here.
#if RASPPI >= 4
#define SHADOW_DMA_CHANNEL DMA_CHANNEL_EXTENDED
#else
#define SHADOW_DMA_CHANNEL DMA_CHANNEL_NORMAL
#endif

// Beats per bus transaction, where a beat is 128 bits. Circle's screen DMA
// uses 2 and its documentation says more than that congests the bus — but
// that is written for console scrolling: a small, frequent move sharing the
// bus with everything else, where latency matters and total time does not.
//
// This is the opposite job. One bulk move of a whole screen, once a frame,
// which has to finish inside the frame. At 2 beats a transaction carries 32
// bytes, and the measured result was a screen taking about 14.5 ms — around
// 450 MB/s, which is a transaction-rate limit and nothing to do with what
// the memory can do.
//
// 8 beats is 128 bytes a transaction: two cache lines, four times the
// payload, and still well under the 15 the controller allows, which leaves
// room to go further if a receipt ever asks for it.
static const unsigned SHADOW_DMA_BURST = 8;

static CDMAChannel *s_dma = nullptr;   // null: the CPU does the copy
static bool s_dma_busy = false;        // a transfer is in flight

// Two shadows exist only when the DMA does the copy: the transfer in flight
// reads one while the scaler writes the other. With the CPU copy there is
// nothing to overlap and one buffer is enough.
//
// Consequence, and it matches the page-flip path exactly: a present hands
// back a buffer holding an older frame, not the one just shown. SDL says
// the back buffer's contents are undefined after a present, so an
// application that wants to keep pixels must draw them.
static u8 *s_shadow_buf[2] = { nullptr, nullptr };
static unsigned s_shadow_idx = 0;

// The page-flip path's staging frame: where a present is composed before it
// is blitted to the half it will be panned to.
//
// Composing costs the same arithmetic wherever it lands, but the framebuffer
// is uncached, and the scaler's stream of single-pixel stores pays that price
// one store at a time — measured on a Pi 4 at 26.1 ms for a 1280x720 frame
// against 1.4 ms into ordinary memory. Composed here and blitted out in whole
// rows, the same frame costs 1.4 ms plus a 6.0 ms block move: the write to
// uncached memory is made once, in the shape that memory is good at.
//
// It is also what keeps the picture whole. A half is only ever written by
// this blit, so the raster can never catch a frame mid-composition.
static u8 *s_stage = nullptr;
static unsigned s_stage_pitch = 0;
static size_t s_stage_bytes = 0;

// Whether the present path — the shadow buffers and the DMA channel, or the
// staging frame — has been built.
//
// It is sized and shaped by THE framebuffer grant, and that grant is made
// once and never returned (see s_fb0), so nothing it depends on can change
// while the machine runs: a second window adopts the same grant, the same
// scanout geometry and the same present resources. It therefore belongs to
// the grant's lifetime and not to a window's, and window teardown leaves it
// alone.
//
// The alternative — rebuild it per window — strands what the previous one
// took, and neither resource is small: the shadows are a screen each, and
// the DMA channel comes from a pool of a few that the sound device draws
// from as well, so a consumer that restarts its video on a settings change
// exhausts it in a handful of restarts and the next allocation anywhere in
// the machine fails.
static bool s_present_ready = false;

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

// Execute one command into a surface of the caller's choosing. The
// presentation core uses it on the shadow or a framebuffer half; the main
// thread uses it on its own canvas surface when a frame has to be
// rasterized there.
static void exec_into(const SDL2CirclePresentCmd *cmd, u8 *dst0, unsigned dpitch)
{
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

// The presentation surface: the shadow where the grant is a single screen,
// otherwise the framebuffer half being drawn into.
void SDL2Circle_VideoExecCmd(const SDL2CirclePresentCmd *cmd, unsigned half)
{
    if (!s_fb_base)
        return;
    if (s_shadow)
        exec_into(cmd, s_shadow, s_shadow_pitch);
    else if (s_stage)
        exec_into(cmd, s_stage, s_stage_pitch);
    else
        exec_into(cmd, s_fb_base + (size_t)half * s_fb_h * s_fb_pitch, s_fb_pitch);
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
        // by one copy into the granted surface. The vsync wait (honored
        // where the firmware implements it) keeps that copy off the raster.
        if (s_dma)
        {
            // The channel has one control block, so the previous transfer
            // must be finished before this one is programmed. A whole frame
            // of application work and scaling has happened since it was
            // started, so in a healthy frame this costs one register read.
            // There is no cheaper way to ask: Circle's GetStatus is only
            // valid once the channel has stopped.
            if (s_dma_busy)
            {
                SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT_DMA);
                boolean ok = s_dma->Wait();
                s_dma_busy = false;
                static bool s_dma_error_logged = false;
                if (!ok && !s_dma_error_logged)
                {
                    s_dma_error_logged = true;
                    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_ERROR,
                                          "present: DMA transfer reported an error");
                }
            }

            {
                SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT_VSYNC);
                s_window->fb->WaitForVerticalSync();
            }

            // The scaler wrote the shadow through the cache. Clean that
            // range — clean, not invalidate, so the lines stay warm for the
            // next frame — or the engine reads stale memory behind it.
            CleanDataCacheRange((u64)(uintptr)s_shadow, (u64)s_shadow_bytes);
            s_dma->SetupMemCopy(s_fb_base, s_shadow, s_shadow_bytes,
                                SHADOW_DMA_BURST, FALSE);
            s_dma->Start();
            s_dma_busy = true;

            // Hand the scaler the other shadow and return without waiting:
            // the transfer reads a surface nobody is about to write.
            s_shadow_idx ^= 1;
            s_shadow = s_shadow_buf[s_shadow_idx];
            return;
        }

        {
            SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT_VSYNC);
            s_window->fb->WaitForVerticalSync();
        }
        const u8 *src = s_shadow;
        u8 *dst = s_fb_base;
        for (int y = 0; y < s_fb_h; y++, src += s_shadow_pitch, dst += s_fb_pitch)
            memcpy(dst, src, (size_t)s_fb_w * 4);
        return;
    }
    // Blit the staged frame to the half about to be panned to. Whole rows
    // into uncached memory, which is the move that memory is built for, and
    // the only writer of a half — so nothing half-composed is ever scanned.
    if (s_stage)
    {
        const u8 *s = s_stage;
        u8 *d = s_fb_base + (size_t)half * s_fb_h * s_fb_pitch;
        for (int y = 0; y < s_fb_h; y++, s += s_stage_pitch, d += s_fb_pitch)
            memcpy(d, s, (size_t)s_fb_w * 4);
    }

    boolean ok = s_window->fb->SetVirtualOffset(0, half * (unsigned)s_fb_h);
    // The pan lands at the next vertical sync, not when the call returns.
    // Hold the worker here until it has: the caller's next frame is drawn
    // straight into the other half, and returning early would let that
    // drawing start while the other half is still the one on the glass.
    {
        SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT_VSYNC);
        s_window->fb->WaitForVerticalSync();
    }
    // One-shot diagnostic: a firmware that refuses the pan (it reports the
    // granted offset back) silently breaks the page flip — the visible
    // half then only ever receives alternate frames.
    static bool s_flip_logged = false;
    if (!s_flip_logged)
    {
        s_flip_logged = true;
        SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
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
    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                          "copy src %dx%d -> canvas %dx%d+%d+%d -> scanout %dx%d+%d+%d (%s)",
                          sw, sh,
                          app.w, app.h, app.dx, app.dy,
                          out.w, out.h, out.dx, out.dy, how);
}

// The canvas surface, made the first time a frame needs one.
static bool canvas_surface_ready(void)
{
    if (s_canvas_surface)
        return true;
    if (s_canvas_w <= 0 || s_canvas_h <= 0)
        return false;
    s_canvas_surface_pitch = (unsigned)s_canvas_w * 4;
    s_canvas_surface_buf[0] = (u8 *)calloc((size_t)s_canvas_surface_pitch, s_canvas_h);
    s_canvas_surface_buf[1] = (u8 *)calloc((size_t)s_canvas_surface_pitch, s_canvas_h);
    if (!s_canvas_surface_buf[0] || !s_canvas_surface_buf[1])
    {
        free(s_canvas_surface_buf[0]);
        free(s_canvas_surface_buf[1]);
        s_canvas_surface_buf[0] = s_canvas_surface_buf[1] = nullptr;
        return false;
    }
    s_canvas_surface_idx = 0;
    s_canvas_surface = s_canvas_surface_buf[0];
    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                   "canvas surface %dx%d allocated: a frame arrived that is not the simple shape",
                   s_canvas_w, s_canvas_h);
    return true;
}

// Stop recording and start drawing. Everything recorded so far goes into
// the canvas surface, and everything after it goes straight there.
static void start_rasterizing(SDL_Renderer *ren)
{
    if (ren->rasterizing || !canvas_surface_ready())
        return;
    for (unsigned i = 0; i < ren->ncmds; i++)
        exec_into(&ren->cmds[i], s_canvas_surface, s_canvas_surface_pitch);
    ren->ncmds = 0;
    ren->rasterizing = true;
}

// Record a draw call. Nothing is executed here and nothing is placed on the
// scanout yet: both are decided at present time, when the whole frame is
// known and can be reduced to the one thing that crosses.
static void emit_cmd(SDL_Renderer *ren, const SDL2CirclePresentCmd &cmd)
{
    if (cmd.w <= 0 || cmd.h <= 0)
        return;

    if (ren->rasterizing)
    {
        exec_into(&cmd, s_canvas_surface, s_canvas_surface_pitch);
        return;
    }

    if (ren->ncmds >= SDL2CIRCLE_PRESENT_MAX_CMDS)
    {
        // More draws than the recorder holds. This frame is not a simple
        // one, so it becomes a drawn one.
        start_rasterizing(ren);
        if (ren->rasterizing)
            exec_into(&cmd, s_canvas_surface, s_canvas_surface_pitch);
        return;
    }

    ren->cmds[ren->ncmds++] = cmd;
}

// Answer one display-mode query. False when there is no display to describe,
// which only happens where SDL_Init's refusal was ignored. Zeroed first, so a
// consumer that also ignores this return reads an obviously empty mode rather
// than whatever its stack held.
static bool fill_mode(SDL_DisplayMode *mode)
{
    memset(mode, 0, sizeof(*mode));
    if (!resolve_display_size())
        return false;
    mode->format = SDL_PIXELFORMAT_ARGB8888;
    mode->w = s_window ? s_window->w : s_canvas_w;
    mode->h = s_window ? s_window->h : s_canvas_h;
    mode->refresh_rate = DEFAULT_HZ;
    return true;
}

// ---- display information ---------------------------------------------------

extern "C" int SDL_GetNumVideoDisplays(void) { return 1; }

extern "C" const char *SDL_GetDisplayName(int) { return "HDMI0"; }

extern "C" int SDL_GetDisplayBounds(int, SDL_Rect *rect)
{
    rect->x = 0;
    rect->y = 0;
    rect->w = 0;
    rect->h = 0;
    if (!resolve_display_size())
        return -1;
    rect->w = s_window ? s_window->w : s_canvas_w;
    rect->h = s_window ? s_window->h : s_canvas_h;
    return 0;
}

extern "C" int SDL_GetNumDisplayModes(int) { return 1; }

extern "C" int SDL_GetDisplayMode(int, int, SDL_DisplayMode *mode)
{
    return fill_mode(mode) ? 0 : -1;
}

extern "C" int SDL_GetCurrentDisplayMode(int, SDL_DisplayMode *mode)
{
    return fill_mode(mode) ? 0 : -1;
}

extern "C" int SDL_GetDesktopDisplayMode(int, SDL_DisplayMode *mode)
{
    return fill_mode(mode) ? 0 : -1;
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

// Bring up the shadow-buffered present: the buffers the scaler writes into,
// and the DMA channel that carries them to the framebuffer if one can be
// had. Runs on core 0 at window creation, with the presentation geometry
// already published.
static void setup_shadow_present(void)
{
    // The shadow and the granted surface agree on stride by construction —
    // the scanout width is derived from the grant's own pitch — so the
    // surface is one contiguous block and a flat copy is the whole job.
    // Should that ever stop holding, decline the engine rather than
    // transfer the wrong shape.
    const char *reason = nullptr;
    if (s_shadow_pitch != s_fb_pitch)
        reason = "shadow and framebuffer strides differ";

    if (!reason)
    {
        // Ask the resource map, then hand the answer straight back and let
        // CDMAChannel take it by number: the constructor asserts rather
        // than reports when nothing is free, and this library falls back
        // instead of dying. The HDMI sound device holds a channel from the
        // same small pool, which is why this asks rather than assumes.
        //
        // Giving the channel back and taking it again leaves a window in
        // which something else could take it. Nothing can: every DMA channel
        // this library takes or gives back — the sound device's included —
        // is taken or given back on core 0 through the call mailbox, which
        // serves one call at a time and never yields inside one. An
        // application core is live by now, and its own calls arrive through
        // that mailbox like everything else, so they queue behind this
        // rather than interleave with it.
        unsigned channel =
            CMachineInfo::Get()->AllocateDMAChannel(SHADOW_DMA_CHANNEL);
        if (channel == DMA_CHANNEL_NONE)
            reason = "no DMA channel available";
        else
        {
            CMachineInfo::Get()->FreeDMAChannel(channel);
            s_dma = new CDMAChannel(channel);
            SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                                  "present: dma copy, channel %u, %u bytes, double-shadowed",
                                  channel, (unsigned)s_shadow_bytes);
        }
    }

    s_shadow_buf[0] = (u8 *)calloc(s_shadow_bytes, 1);
    if (s_dma && s_shadow_buf[0])
        s_shadow_buf[1] = (u8 *)calloc(s_shadow_bytes, 1);

    if (s_dma && !s_shadow_buf[1])
    {
        // Without the second buffer the scaler would overwrite the surface
        // the engine is reading. Give the channel back and copy on the CPU.
        delete s_dma;
        s_dma = nullptr;
        reason = "second shadow buffer allocation failed";
    }

    s_shadow_idx = 0;
    s_shadow = s_shadow_buf[0];

    if (!s_shadow)
    {
        // No back buffer at all: present renders straight into the granted
        // surface and the raster may catch a half-drawn frame.
        delete s_dma;
        s_dma = nullptr;
        SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_WARNING,
                              "present: unbuffered, %u bytes of shadow could not be allocated",
                              (unsigned)s_shadow_bytes);
        return;
    }

    if (reason)
        SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_WARNING,
                              "present: cpu copy, %u bytes (%s)",
                              (unsigned)s_shadow_bytes, reason);
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
    //
    // A window IS the canvas, so with nothing declared there is no window to
    // make. The resolve has set the error.
    if (!resolve_display_size())
        return;
    CBcmFrameBuffer *fb = s_fb0;
    if (!fb)
    {
        SDL_SetError("CBcmFrameBuffer::Initialize failed (%dx%d)", a->w, a->h);
        return;
    }

    // The window is the CANVAS — the virtual device the consumer declared,
    // or the scanout itself where nothing was declared. What the application
    // asked SDL_CreateWindow for does not enter into it: there is one screen
    // and the application gets all of it. The shim's present carries the
    // canvas to the scanout, so an application never has to learn what the
    // glass is really doing.
    SDL_Window *win = new SDL_Window;
    win->fb = fb;
    win->w = s_canvas_w;
    win->h = s_canvas_h;
    win->flags = a->flags | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN;

    // Publish the presentation geometry before the window becomes visible
    // to the application core or the worker. This side is SCANOUT geometry: every
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
        s_shadow_bytes = (size_t)s_shadow_pitch * s_fb_h;
        if (!s_present_ready)
        {
            SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_WARNING,
                                  "granted %u rows < %u: shadow-buffered present",
                                  nRowsGranted, 2u * (unsigned)s_fb_h);
            setup_shadow_present();
        }
    }
    else
    {
        // Fit leaves borders no command will ever write — every present
        // command is clipped to the canvas rectangle. Black them once, here,
        // across every granted row so both halves start clean. (The shadow
        // path gets this from calloc.)
        memset(s_fb_base, 0, fb->GetSize());

        // The staging frame a present is composed in before it is blitted to
        // the half being panned to. Without it the composition itself would
        // be writing the framebuffer a pixel at a time.
        s_stage_pitch = (unsigned)s_fb_w * 4;
        s_stage_bytes = (size_t)s_stage_pitch * s_fb_h;
        if (!s_present_ready)
        {
            s_stage = (u8 *)calloc(s_stage_bytes, 1);
            if (!s_stage)
                SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_WARNING,
                                      "present: composing straight into the framebuffer, "
                                      "%u bytes of staging could not be allocated",
                                      (unsigned)s_stage_bytes);
        }
    }
    s_present_ready = true;

    s_window = win;

    // Pitch and size came back from the firmware. Everything else is what
    // Circle was constructed with, echoed unchanged by its getters, so the
    // two halves are labelled and never printed as one geometry.
    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                          "framebuffer: asked %ux%u virt %ux%u depth %u, "
                          "granted pitch %u, %u rows, %u bytes",
                          fb->GetWidth(), fb->GetHeight(),
                          fb->GetVirtWidth(), fb->GetVirtHeight(),
                          fb->GetDepth(),
                          fb->GetPitch(), nRowsGranted, fb->GetSize());

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

    // The presentation worker reaches the window through s_window on every
    // flip, so it has to be out of the frame before this object goes away.
    SDL2Circle_PresentQuiesce();

    if (win == s_window)
    {
        s_window = nullptr;
        s_fb_base = nullptr;
    }
    // win->fb is THE framebuffer (s_fb0), kept for the process lifetime:
    // deleting it cannot return the firmware's allocation, and the next
    // window must adopt the same grant rather than allocate a leak.
    //
    // The present path (the shadow buffers and their DMA channel, or the
    // staging frame) belongs to that same grant and outlives the window with
    // it — see s_present_ready. Releasing it here would give the next window
    // nothing to reuse, and taking a fresh DMA channel each time is what
    // empties the pool.
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
    ren->rasterizing = false;
    ren->border_color = 0;
    ren->frame_x = ren->frame_y = ren->frame_w = ren->frame_h = -1;
    ren->border_repaint = 0;
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
    // A frame the worker has not finished with may still name this texture's
    // pixels as its source — the reduced frame IS the texture, in place.
    SDL2Circle_PresentQuiesce();
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

// ---- reducing a frame ------------------------------------------------------
//
// What crosses to the presentation core is a FINISHED FRAME: pixels, and
// where on the scanout they go. Never a list of draw calls to execute
// there. The presentation core impersonates display hardware the board does
// not have — frame in, scanout out — and no part of SDL lives on it.
//
// Reducing is what turns a recorded frame into that. Almost every frame an
// application draws is the same shape: clear the target, then blit one
// opaque texture over it. That shape reduces to the texture itself, exactly
// as it already sits in memory — for a game raster, a few hundred kilobytes
// instead of a screen — and the clear reduces to nothing at all, because
// the only part of it anything can see is the border around the placed
// frame, and a border only changes when the geometry does.
//
// A frame of any other shape is drawn on this thread, into the canvas
// surface, and that surface is the finished frame. So the fast shape is a
// fact about the recorded stream, never an assumption about the caller: no
// application has to draw a particular way to be correct, only to be quick.

// Is this the simple shape? Returns the index of the one copy, or -1.
static int simple_frame_copy(SDL_Renderer *ren)
{
    int copy = -1;
    for (unsigned i = 0; i < ren->ncmds; i++)
    {
        const SDL2CirclePresentCmd &c = ren->cmds[i];
        if (c.op == SDL2CirclePresentCmd::FILL)
        {
            // Only a clear of the whole target reduces, and only before
            // the copy: a fill after one is drawing, not clearing.
            if (copy >= 0 || c.dx != 0 || c.dy != 0
                || c.w != ren->window->w || c.h != ren->window->h)
                return -1;
            continue;
        }
        // One opaque copy, and nothing else.
        if (copy >= 0 || c.blend || c.alphamod != 255)
            return -1;
        copy = (int)i;
    }
    return copy;
}

// Paint the border — the target minus the frame — when it has moved,
// resized or changed colour, and for as many frames as there are buffers
// behind the present, since each holds its own copy of it. Emits the fill
// into the reduced list; the presentation core does the painting.
static void border_if_changed(SDL_Renderer *ren, u32 color,
                              const SDL2CirclePresentCmd &frame,
                              SDL2CirclePresentCmd *out, unsigned *nout)
{
    if (color != ren->border_color
        || frame.dx != ren->frame_x || frame.dy != ren->frame_y
        || frame.w != ren->frame_w || frame.h != ren->frame_h)
    {
        ren->border_color = color;
        ren->frame_x = frame.dx; ren->frame_y = frame.dy;
        ren->frame_w = frame.w;  ren->frame_h = frame.h;
        ren->border_repaint = s_shadow_buf[1] ? 2 : 1;
    }

    if (!ren->border_repaint)
        return;
    ren->border_repaint--;

    SDL2CirclePresentCmd fill;
    memset(&fill, 0, sizeof(fill));
    fill.op = SDL2CirclePresentCmd::FILL;
    fill.dx = 0; fill.dy = 0;
    fill.w = s_fb_w; fill.h = s_fb_h;
    fill.color = color;
    out[(*nout)++] = fill;
}

// Turn the recorded frame into what crosses. Returns the number of entries
// written to `out` (at most two: a border repaint, and the frame).
static unsigned reduce_frame(SDL_Renderer *ren, SDL2CirclePresentCmd *out)
{
    unsigned nout = 0;

    int copy = ren->rasterizing ? -1 : simple_frame_copy(ren);
    u32 clear_color = 0xFF000000u;
    if (copy >= 0)
        for (unsigned i = 0; i < ren->ncmds; i++)
            if (ren->cmds[i].op == SDL2CirclePresentCmd::FILL)
                clear_color = ren->cmds[i].color;

    SDL2CirclePresentCmd frame;
    if (copy >= 0)
    {
        // The simple shape: the frame IS the application's own texture,
        // untouched, wherever it already is in memory.
        frame = ren->cmds[copy];
    }
    else
    {
        // Anything else was drawn here, on this thread, at canvas
        // resolution. That surface is the frame.
        start_rasterizing(ren);
        if (!ren->rasterizing)
            return 0;                  // no surface: nothing can be shown
        memset(&frame, 0, sizeof(frame));
        frame.op = SDL2CirclePresentCmd::COPY;
        frame.src = s_canvas_surface;
        frame.srcpitch = (int)s_canvas_surface_pitch;
        frame.sw = s_canvas_w; frame.sh = s_canvas_h;
        frame.dx = 0; frame.dy = 0;
        frame.w = s_canvas_w; frame.h = s_canvas_h;
        frame.alphamod = 255;
    }

    SDL2CirclePresentCmd placed = frame;
    if (!place_on_scanout(&placed))
        return 0;
    log_copy_geometry(frame, placed, frame.sw, frame.sh);

    border_if_changed(ren, clear_color, placed, out, &nout);
    out[nout++] = placed;
    return nout;
}

extern "C" void SDL_RenderPresent(SDL_Renderer *ren)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    g_SDL2CirclePresents++;

    // A frame with more draws than the recorder holds has already been
    // drawn into the canvas surface, whatever the grant, and there is
    // nothing left to compose. It goes the reduced way on both paths.
    //
    // The two grants are otherwise two different machines, and they are
    // answered differently.
    //
    // A grant of two screens can page-flip, and the firmware scales the
    // signal to the panel for nothing. There the frame is composed into
    // the half being drawn, command by command, and the pan makes it
    // visible — which is as cheap as this gets, and is left exactly as it
    // was.
    //
    // A grant of one screen cannot flip and cannot scale. Everything has
    // to be resampled to scanout size and copied in, so the one thing
    // worth doing is doing that ONCE: the frame is reduced here, on this
    // thread, to finished pixels and a destination, and that is all that
    // crosses.
    if (s_fb_halves == 2 && !ren->rasterizing)
    {
        // Composed command by command into the half, as before. The whole
        // recorded list travels, because composing IS what this path does.
        for (unsigned i = 0; i < ren->ncmds; i++)
        {
            SDL2CirclePresentCmd as_drawn = ren->cmds[i];
            place_on_scanout(&ren->cmds[i]);
            if (ren->cmds[i].op == SDL2CirclePresentCmd::COPY)
                log_copy_geometry(as_drawn, ren->cmds[i], as_drawn.sw, as_drawn.sh);
        }
        if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
        {
            SDL2Circle_PresentPost(ren->cmds, ren->ncmds, ren->back);
            ren->ncmds = 0;
            ren->rasterizing = false;
            ren->back ^= 1;
            return;
        }
        for (unsigned i = 0; i < ren->ncmds; i++)
            SDL2Circle_VideoExecCmd(&ren->cmds[i], ren->back);
        ren->ncmds = 0;
        ren->rasterizing = false;
        SDL2Circle_VideoFlip(ren->back);
        if (ren->vsync)
        {
            SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT_VSYNC);
            ren->window->fb->WaitForVerticalSync();
        }
        ren->back ^= 1;
        return;
    }

    SDL2CirclePresentCmd out[2];
    bool drew_canvas = ren->rasterizing;
    unsigned nout = reduce_frame(ren, out);
    ren->ncmds = 0;
    drew_canvas = drew_canvas || ren->rasterizing;
    ren->rasterizing = false;

    // A posted canvas surface belongs to the frame in flight; the next one
    // is drawn into the other.
    if (drew_canvas && s_canvas_surface_buf[1])
    {
        s_canvas_surface_idx ^= 1;
        s_canvas_surface = s_canvas_surface_buf[s_canvas_surface_idx];
    }

    if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
    {
        SDL2Circle_PresentPost(out, nout, ren->back);
        if (s_fb_halves == 2)
            ren->back ^= 1;
        return;
    }

    for (unsigned i = 0; i < nout; i++)
        SDL2Circle_VideoExecCmd(&out[i], ren->back);
    SDL2Circle_VideoFlip(ren->back);
    if (ren->vsync)
    {
        SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT_VSYNC);
        ren->window->fb->WaitForVerticalSync();
    }
    if (s_fb_halves == 2)
        ren->back ^= 1;
                                     // only when the app asked for vsync:
                                     // throttled apps pace themselves, and
                                     // blocking here would double-throttle
}
