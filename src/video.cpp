//
// video.cpp — window / software renderer / streaming texture over
// Circle's CBcmFrameBuffer (double-buffered, vsync page flip).
//
// Scope matches MAME's drawsdl.cpp software path: one fullscreen window,
// an SDL_Renderer, streaming ARGB8888 textures.
//
#include <SDL2/SDL.h>
#include "sdl2circle.h"
#include "shim_internal.h"
#include "pixels.h"
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

    // State a desktop window manager would own. There is none here: the
    // window is the canvas and the canvas is the whole screen, so none of
    // this changes what is drawn or where it lands. It is kept so that the
    // getter for each setter answers with what was set, which is what
    // applications read it for.
    int min_w, min_h;
    int max_w, max_h;
    SDL_bool grabbed;
    char title[128];
};

struct SDL_Renderer
{
    SDL_Window *window;
    u8 *base;          // start of the (2x height) virtual framebuffer
    unsigned pitch;
    unsigned back;     // half we're drawing into: 0 = top, 1 = bottom
    bool vsync;        // present blocks for vertical sync
    Uint8 r, g, b, a;  // draw color
    SDL_BlendMode draw_blend;

    // The coordinate system the application draws in, and how it lands on
    // the window. SDL2 lets an application say "I draw 320x200" and have
    // every rectangle it hands over scaled and centred for it; a game
    // written for a fixed retro resolution relies on that rather than doing
    // the arithmetic itself.
    //
    // logical_w/h is 0 when no logical size is set, and the window's own
    // size is then the coordinate system — the identity case, which costs
    // one comparison.
    int   logical_w, logical_h;
    bool  integer_scale;
    float scale_x, scale_y;      // SDL_RenderSetScale, applied on top

    // The sub-rectangle of the window drawing is confined to, and the clip
    // inside it. Both are in the application's coordinates.
    SDL_Rect viewport;
    bool     viewport_set;
    SDL_Rect clip;
    bool     clip_enabled;

    // Draw calls become present commands. Single-core they execute
    // immediately (the degenerate case of the same design); under the core
    // split they are recorded here and RenderPresent posts the frame to the
    // presentation worker, which blits and flips off-core.
    // Draw calls are RECORDED here, in canvas coordinates, and nothing is
    // executed until SDL_RenderPresent. What crosses to the presentation
    // core then is a finished frame, never this list.
    SDL2CirclePresentCmd cmds[SDL2CIRCLE_RECORD_MAX_CMDS];
    unsigned ncmds;

    // Set once a frame has stopped being a simple one and is being drawn
    // into the canvas surface instead. Cleared at the start of each frame.
    bool rasterizing;

    // Whether the frame being recorded has had its one opaque copy yet. It
    // is what lets the simple shape be ruled out as each command arrives
    // rather than only when the whole frame is known: a second copy, or any
    // fill after the copy, cannot be part of a clear-plus-one-blit frame no
    // matter what follows. Cleared at the start of each frame.
    bool have_copy;

    // What the border looked like when it was last painted: the colour it
    // was painted in, and the frame rectangle it was painted around.
    // Borders are geometry, so they are repainted when this changes and
    // never otherwise.
    u32 border_color;
    int frame_x, frame_y, frame_w, frame_h;
    unsigned border_repaint;   // frames still owing a repaint

    // Pixels a command needs that exist nowhere else — a mirrored copy for
    // SDL_RenderCopyEx, which the texture itself does not hold. A recorded
    // command is not executed until present, so those pixels have to outlive
    // the call that made them: they are bump-allocated here and the arena is
    // emptied when the next frame starts.
    //
    // TWO of them, alternating, for the same reason the textures come in
    // pairs. Under the core split a posted frame is still being read by the
    // presentation worker after RenderPresent returns, so emptying the arena
    // the application just drew into would pull those pixels out from under
    // it. The frame in flight keeps the one it was posted with while the
    // application fills the other.
    u8    *scratch[2];
    size_t scratch_bytes[2];
    size_t scratch_used;
    u8     scratch_idx;
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

// A texture is STORED as ARGB8888 whatever format it was asked for, because
// the presentation path reads a texture's pixels directly — a frame that is
// one opaque copy crosses to the presentation core as that texture, in
// place, with nothing painted. Storing a texture in the application's format
// would mean converting during presentation, on the core that must not be
// delayed, every frame.
//
// So the format is honoured at the EDGE instead. `format` is what the
// application asked for and what SDL_QueryTexture answers; pixels handed in
// through SDL_UpdateTexture are converted on the way in, and
// SDL_LockTexture hands back a staging buffer in the application's format
// which SDL_UnlockTexture converts. An application therefore writes the
// pixels it believes it is writing, and the cost falls on its own core.
//
// When the application's format IS ARGB8888 — still the common case — there
// is no staging buffer and no conversion, and the path is exactly what it
// was.
struct SDL_Texture
{
    int w, h;
    Uint32 format;     // the format the APPLICATION asked for
    int access;        // SDL_TEXTUREACCESS_*, as asked for
    u8 *pixels[3];     // [1] and [2] exist only under the core split: the app
                       // renders into one buffer while the presentation
                       // worker still reads the frame in flight
    u8 widx;           // buffer the app writes next

    // WHICH FRAME EACH STORE IS SPOKEN FOR BY.
    //
    // A recorded copy hands the presentation core a RAW POINTER into one of
    // these stores, so the store must not be written again until that core
    // has finished reading the frame holding the pointer. busy_seq[i] is the
    // frame sequence of the copy that last referenced store i; 0 means never.
    //
    // Tracking the buffer that was last POSTED is not the same question and
    // is what went wrong before: it says where the writer went, not what the
    // reader still holds.
    u64 busy_seq[3];
    int pitch;         // stored pitch, always w * 4

    // Only for a texture whose format is not the stored one.
    int  app_bpp;      // bytes per pixel in the application's format
    u8  *staging;      // lock buffer in the application's format
    int  staging_pitch;
    SDL_Rect locked_rect;
    bool locked;

    SDL_BlendMode blend;
    Uint8 alphamod;

    // Held so that each setter's getter answers with what was set. The
    // colour modulation is not applied when drawing — see
    // SDL_SetTextureColorMod — and the scale mode has no effect because the
    // blitters are nearest-neighbour throughout.
    Uint8 colormod_r, colormod_g, colormod_b;
    SDL_ScaleMode scale_mode;
    void *userdata;
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
// room to go further if a report ever asks for it.
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
        // EVERY destination row is resampled from the source. No destination
        // row is ever copied from another one, and it is worth knowing why,
        // because the opposite looks obvious and this code used to do it.
        //
        // Under vertical magnification several destination rows share a
        // source row, so the second and later ones can be had by copying the
        // first destination row back. That reads the WIDE side: the source
        // row is small and the destination row is the magnified one — 398
        // pixels against 1918, about 1.6 KB against 7.6 KB — so the copy
        // reads five times as much as resampling from the source, to save
        // some index arithmetic.
        //
        // The cache is the real cost. The destination is a whole-screen
        // shadow, megabytes of it, and reading those lines back fills the
        // cache with data nothing will ever read again — evicting the one
        // small source row that every destination row mapping to it still
        // needs. Measured on a Pi 5 at a locked 59.9 fps, 398x224 into a
        // 796x448 canvas on a 1920x1080 panel, the presentation core was
        // awake 76.4% of the time with the copy-back and 33-41% without it.
        // Half the core, handed back by deleting the optimisation.
        //
        // It also hid in the numbers. The more magnification, the more rows
        // are "reused" and the worse it gets, so a low source resolution
        // paid more of the penalty than a high one and the cheaper mode
        // measured as the dearer.
        u8 *drow = dst;
        for (int j = 0; j < dh; j++, drow += dpitch)
        {
            const u32 *s = (const u32 *)(cmd->src
                                         + (size_t)s_ymap[j] * cmd->srcpitch);
            u32 *d = (u32 *)drow;
            if (xrep)
            {
                // Integer horizontal ratio: the source is read once and the
                // destination only written. Nothing is read back, so this is
                // not the same shape of mistake and it stays.
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

// Canvas coordinates -> scanout coordinates, applied to a command on its way
// to the glass. THE EXECUTOR BELOW IS THE ONLY CALLER, and that is the whole
// point of where this sits: every command that reaches the framebuffer goes
// through one function, so the mapping cannot be forgotten on a path that
// nobody thought about. SDL never sees it, never asks for it, and has no
// business knowing the panel is a different size from the canvas.
//
// The rectangle it maps into was worked out once, by resolve_placement, at
// the only moment it can change: when the scanout and the canvas both became
// known. Nothing here recomputes it.
//
// Both edges are mapped independently rather than mapping the origin and
// scaling the extent, so rectangles that abut in the canvas still abut on the
// scanout instead of leaving a seam where the two divisions round apart. A
// COPY keeps its source extent untouched: the executor resamples straight
// from the source onto this composed destination, so the canvas contributes
// arithmetic and never an intermediate copy.
//
// Returns false when nothing survives the mapping.
static bool map_onto_scanout(SDL2CirclePresentCmd *cmd)
{
    if (cmd->w <= 0 || cmd->h <= 0)
        return false;

    // A fill of the WHOLE canvas is a clear of the whole display, border
    // included. The letterbox is outside the canvas, so no canvas rectangle
    // can ever name it, and mapping this one the ordinary way would leave the
    // frame before last showing in the margins for as long as the picture
    // stays where it is.
    if (cmd->op == SDL2CirclePresentCmd::FILL
        && cmd->dx == 0 && cmd->dy == 0
        && cmd->w == s_canvas_w && cmd->h == s_canvas_h)
    {
        cmd->dx = 0;
        cmd->dy = 0;
        cmd->w = s_fb_w;
        cmd->h = s_fb_h;
        return cmd->w > 0 && cmd->h > 0;
    }

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

// Execute one command onto the glass. The command arrives in CANVAS
// coordinates, whoever posted it and whichever core is running this: the
// presentation worker under the core split, the application's own core
// without it. Both go through here, so both get the same picture.
//
// The presentation surface: the shadow where the grant is a single screen,
// otherwise the framebuffer half being drawn into.
void SDL2Circle_VideoExecCmd(const SDL2CirclePresentCmd *cmd, unsigned half)
{
    if (!s_fb_base)
        return;

    SDL2CirclePresentCmd placed = *cmd;
    if (!map_onto_scanout(&placed))
        return;

    if (placed.op == SDL2CirclePresentCmd::COPY)
        log_copy_geometry(*cmd, placed,
                          cmd->sw > 0 ? cmd->sw : cmd->w,
                          cmd->sh > 0 ? cmd->sh : cmd->h);

    if (s_shadow)
        exec_into(&placed, s_shadow, s_shadow_pitch);
    else if (s_stage)
        exec_into(&placed, s_stage, s_stage_pitch);
    else
        exec_into(&placed, s_fb_base + (size_t)half * s_fb_h * s_fb_pitch,
                  s_fb_pitch);
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
                   "canvas surface %dx%d allocated: a frame arrived that has to "
                   "be composed here before it crosses",
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
    ren->have_copy = false;
    ren->rasterizing = true;
}

// Could this command still be part of a clear-plus-one-opaque-copy frame?
// The same rule simple_frame_copy applies to a finished list, asked one
// command at a time: a fill that is not a full-target clear, a fill after
// the copy, a second copy, or a copy that is blended or alpha-modded, and
// the frame cannot be the simple shape whatever follows.
//
// simple_frame_copy remains the authority on a finished list. This only ever
// brings the same verdict FORWARD, so the two cannot disagree about what is
// simple — at worst this is the more cautious of the two and the frame is
// painted that much sooner.
static bool keeps_simple_shape(SDL_Renderer *ren, const SDL2CirclePresentCmd &cmd)
{
    if (cmd.op == SDL2CirclePresentCmd::FILL)
        return !ren->have_copy
            && cmd.dx == 0 && cmd.dy == 0
            && cmd.w == ren->window->w && cmd.h == ren->window->h;

    return !ren->have_copy && !cmd.blend && cmd.alphamod == 255;
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

    // Painting starts at the first moment the frame can be NEITHER of the
    // two things worth holding it back for.
    //
    // It is worth holding back while the frame could still be the simple
    // shape, because that shape is the application's own texture and never
    // needs painting at all. It is also worth holding back while the list
    // is still short enough to CROSS as a list, because then the far side
    // composes it and this core paints nothing either. A command that ends
    // both possibilities ends the waiting with it: replay what is held,
    // paint this one straight in, and every later one goes straight in too.
    //
    // Waiting past that point would add latency to exactly the frames the
    // recogniser cannot help — the work is the same either way, but done
    // here it is spread across the application's own draw calls instead of
    // landing in one lump at present.
    const bool could_be_simple = keeps_simple_shape(ren, cmd);
    const bool could_still_cross =
        ren->ncmds < (unsigned)SDL2CIRCLE_PRESENT_MAX_CMDS;

    // The recorder's own capacity is the third way to run out, and it is
    // never the crossing count: recognising the simple shape has to keep
    // working however few commands this build lets cross.
    if ((!could_be_simple && !could_still_cross)
        || ren->ncmds >= SDL2CIRCLE_RECORD_MAX_CMDS)
    {
        start_rasterizing(ren);
        if (ren->rasterizing)
        {
            exec_into(&cmd, s_canvas_surface, s_canvas_surface_pitch);
            return;
        }

        // The canvas surface could not be allocated, so there is nowhere to
        // draw this command and it is lost — and with it everything else
        // this frame, since every later command lands here too.
        //
        // SAY SO. A frame that vanishes with nothing on the log is the
        // shape of fault that costs whole nights: the picture stops
        // changing, or never appears, and every other reading looks
        // healthy. Once per run, because the condition that caused it does
        // not clear and a line per command would bury the one that matters.
        static bool told = false;
        if (!told)
        {
            told = true;
            SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_ERROR,
                           "no canvas surface (%dx%d, %u bytes): frames that "
                           "are not the simple shape are being DISCARDED",
                           s_canvas_w, s_canvas_h,
                           (unsigned)((size_t)s_canvas_w * 4 * s_canvas_h));
        }
        return;
    }

    if (cmd.op == SDL2CirclePresentCmd::COPY)
        ren->have_copy = true;
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

// Where the mouse pointer is allowed to be (src/mouse.cpp). The application's
// window while one exists, and the declared canvas before that — the same
// answer SDL_GetDisplayBounds gives, because on one screen with one fullscreen
// window the display and the window are the same rectangle. Both numbers are
// plain reads of state core 0 wrote, so the mouse pump may ask from there.
void SDL2Circle_PointerBounds(int *w, int *h)
{
    if (w) *w = s_window ? s_window->w : s_canvas_w;
    if (h) *h = s_window ? s_window->h : s_canvas_h;
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

// SDL_PixelFormatEnumToMasks and the rest of the format machinery live in
// pixels.cpp, which is where every format this library understands is
// described once.

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
    // The window's state, and the flags a game branches on.
    //
    // THIS WINDOW ALWAYS HAS INPUT FOCUS. There is one window and no window
    // manager to take focus away from it, so a game asking whether it is
    // focused is asking a question with only one possible answer here. A
    // flag that is never set is indistinguishable from a flag that is false,
    // and a game told it has no focus pauses, stops drawing or ignores
    // input — a black screen with a clean log, which is the worst shape a
    // failure can take.
    //
    // THE FLAGS DESCRIBE THE MACHINE, NOT THE REQUEST. What an application
    // asked SDL_CreateWindow for is carried through only where this window
    // can honour it; every bit below is cleared because it would otherwise
    // report the request back to the asker as though it had been granted.
    //
    //   HIDDEN, MINIMIZED   this window cannot be either, and claiming
    //                       SDL_WINDOW_SHOWN alongside SDL_WINDOW_HIDDEN
    //                       leaves a game to act on whichever it tested.
    //   OPENGL, VULKAN,     there is no accelerated renderer of any kind
    //   METAL               here. A game that treats the window flag as the
    //                       test then takes its software path immediately,
    //                       which is the path that works — rather than going
    //                       down an accelerated one and finding out at
    //                       SDL_GL_CreateContext, or never checking and
    //                       drawing nothing at all. Upstream SDL reaches the
    //                       same outcome by refusing to create the window;
    //                       the window is worth having here, so the bit goes
    //                       instead.
    win->flags = a->flags & ~(Uint32)(SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED
                                    | SDL_WINDOW_OPENGL | SDL_WINDOW_VULKAN
                                    | SDL_WINDOW_METAL);
    win->flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS;
    win->min_w = win->min_h = 0;
    win->max_w = win->max_h = 0;
    win->grabbed = SDL_FALSE;
    win->title[0] = '\0';

    // Publish the presentation geometry before the window becomes visible to
    // the application core or the worker. This side is SCANOUT geometry, and
    // it is the executor's alone: a present command is still in canvas
    // coordinates right up to the moment the executor maps it, and the
    // placement it maps into was settled by resolve_display_size above, out
    // of the same two numbers this reads.
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

// The renderer belonging to a window. There is one of each, so the answer is
// whichever renderer was made — tracked here rather than on the window,
// which does not otherwise know it has one.
static SDL_Renderer *s_renderer = nullptr;

extern "C" SDL_Renderer *SDL_GetRenderer(SDL_Window *win)
{
    if (!win || win != s_window || !s_renderer)
    {
        SDL_SetError("SDL_GetRenderer: this window has no renderer");
        return nullptr;
    }
    return s_renderer;
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
    if (win == nullptr)
        return 0;

    // Mouse focus is not stored, because it can change after the window is
    // made: a USB mouse may be plugged in or pulled out at any time. It is
    // asked of the mouse subsystem here so that this answer and
    // SDL_GetMouseFocus can never disagree — a game that tests the flag and
    // a game that calls the function are asking the same question, and one
    // of them getting a different answer is a bug nobody would think to look
    // for.
    Uint32 flags = win->flags;
    if (SDL_GetMouseFocus() == win)
        flags |= SDL_WINDOW_MOUSE_FOCUS;
    else
        flags &= ~(Uint32)SDL_WINDOW_MOUSE_FOCUS;
    return flags;
}

// There is no title bar to put a title in, but an application that sets one
// and reads it back gets what it set — some use it as their own record of
// what is on screen.
extern "C" void SDL_SetWindowTitle(SDL_Window *win, const char *title)
{
    if (!win)
        return;
    if (!title)
        title = "";
    size_t n = strlen(title);
    if (n >= sizeof(win->title))
        n = sizeof(win->title) - 1;
    memcpy(win->title, title, n);
    win->title[n] = '\0';
}

extern "C" const char *SDL_GetWindowTitle(SDL_Window *win)
{
    return win ? win->title : "";
}

extern "C" void SDL_ShowWindow(SDL_Window *) {}

// ---------------------------------------------------------------------------
// Window geometry and window-manager state
//
// A board has one screen and no window manager. The window is the canvas the
// consumer declared, it fills the display, and it never moves or changes
// size. Every call below is accepted and none of them changes the geometry.
//
// They are accepted rather than refused because of how applications use
// them. A game toggling fullscreen calls SDL_SetWindowFullscreen and then
// SDL_SetWindowSize, and treats a failure as a fatal video error; refusing
// would stop a game that would otherwise run perfectly on a display that was
// already showing it what it wanted. What each call must not do is claim the
// geometry changed, so none of them sends a size or move event.
// ---------------------------------------------------------------------------

extern "C" int SDL_SetWindowFullscreen(SDL_Window *win, Uint32 flags)
{
    if (!win)
        return SDL_SetError("SDL_SetWindowFullscreen: no window");

    // The display is always fullscreen; what is recorded is the application's
    // own request, so that SDL_GetWindowFlags answers a fullscreen toggle
    // with the state the application believes it just set. A game that tracks
    // its mode by reading the flags back therefore stays consistent with
    // itself, and the picture is unchanged either way.
    const Uint32 kFullscreenBits = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP;
    win->flags &= ~kFullscreenBits;
    win->flags |= (flags & kFullscreenBits);
    return 0;
}

extern "C" void SDL_SetWindowSize(SDL_Window *, int, int)
{
    // The canvas size is fixed before SDL_Init and the present path is built
    // around it. Nothing to do, and no SDL_WINDOWEVENT_SIZE_CHANGED, because
    // the size did not change.
}

extern "C" void SDL_SetWindowMinimumSize(SDL_Window *win, int w, int h)
{
    if (win) { win->min_w = w; win->min_h = h; }
}

extern "C" void SDL_GetWindowMinimumSize(SDL_Window *win, int *w, int *h)
{
    if (w) *w = win ? win->min_w : 0;
    if (h) *h = win ? win->min_h : 0;
}

extern "C" void SDL_SetWindowMaximumSize(SDL_Window *win, int w, int h)
{
    if (win) { win->max_w = w; win->max_h = h; }
}

extern "C" void SDL_GetWindowMaximumSize(SDL_Window *win, int *w, int *h)
{
    if (w) *w = win ? win->max_w : 0;
    if (h) *h = win ? win->max_h : 0;
}

// The window is at the origin of the only display there is.
extern "C" void SDL_SetWindowPosition(SDL_Window *, int, int) {}

extern "C" void SDL_GetWindowPosition(SDL_Window *win, int *x, int *y)
{
    (void)win;
    if (x) *x = 0;
    if (y) *y = 0;
}

// No decoration exists, so every border is zero pixels thick.
extern "C" int SDL_GetWindowBordersSize(SDL_Window *win, int *top, int *left,
                                        int *bottom, int *right)
{
    if (!win)
        return SDL_SetError("SDL_GetWindowBordersSize: no window");
    if (top)    *top    = 0;
    if (left)   *left   = 0;
    if (bottom) *bottom = 0;
    if (right)  *right  = 0;
    return 0;
}

extern "C" void SDL_SetWindowBordered(SDL_Window *, SDL_bool) {}
extern "C" void SDL_SetWindowResizable(SDL_Window *, SDL_bool) {}
extern "C" void SDL_SetWindowAlwaysOnTop(SDL_Window *, SDL_bool) {}

// There is nothing to grab away from and nowhere for a pointer to leave to,
// so the grab is recorded and the input path is unaffected.
extern "C" void SDL_SetWindowGrab(SDL_Window *win, SDL_bool grabbed)
{
    if (win)
    {
        win->grabbed = grabbed;
        if (grabbed) win->flags |= SDL_WINDOW_INPUT_GRABBED;
        else         win->flags &= ~SDL_WINDOW_INPUT_GRABBED;
    }
}

extern "C" SDL_bool SDL_GetWindowGrab(SDL_Window *win)
{
    return win ? win->grabbed : SDL_FALSE;
}

extern "C" void SDL_SetWindowKeyboardGrab(SDL_Window *win, SDL_bool g)
{
    SDL_SetWindowGrab(win, g);
}

extern "C" SDL_bool SDL_GetWindowKeyboardGrab(SDL_Window *win)
{
    return SDL_GetWindowGrab(win);
}

extern "C" void SDL_SetWindowMouseGrab(SDL_Window *win, SDL_bool g)
{
    SDL_SetWindowGrab(win, g);
}

extern "C" SDL_bool SDL_GetWindowMouseGrab(SDL_Window *win)
{
    return SDL_GetWindowGrab(win);
}

extern "C" SDL_Window *SDL_GetGrabbedWindow(void)
{
    return (s_window && s_window->grabbed) ? s_window : nullptr;
}

extern "C" int SDL_SetWindowMouseRect(SDL_Window *, const SDL_Rect *)
{
    return 0;   // the pointer is already confined to the one screen
}

extern "C" const SDL_Rect *SDL_GetWindowMouseRect(SDL_Window *)
{
    return nullptr;
}

// The window cannot leave the screen it is, so these are the states it is
// already in.
extern "C" void SDL_MaximizeWindow(SDL_Window *) {}
extern "C" void SDL_MinimizeWindow(SDL_Window *) {}
extern "C" void SDL_RestoreWindow(SDL_Window *) {}
extern "C" void SDL_RaiseWindow(SDL_Window *) {}
extern "C" void SDL_HideWindow(SDL_Window *) {}

// No window manager means no icon to hand it. Taking the surface and doing
// nothing with it is the whole of the correct behaviour; the call returns
// void, so an application cannot be told otherwise in any case.
extern "C" void SDL_SetWindowIcon(SDL_Window *, SDL_Surface *) {}

extern "C" Uint32 SDL_GetWindowPixelFormat(SDL_Window *)
{
    return SDL_PIXELFORMAT_ARGB8888;   // what the canvas is
}

// A bare-metal board has no screen blanking to suppress, so the screen saver
// is permanently disabled and saying so costs nothing.
extern "C" void SDL_DisableScreenSaver(void) {}
extern "C" void SDL_EnableScreenSaver(void) {}
extern "C" SDL_bool SDL_IsScreenSaverEnabled(void) { return SDL_FALSE; }

// ---------------------------------------------------------------------------
// The window's display mode
//
// There is one mode, it is the one the panel is in, and it cannot be
// changed from here. A request for a different one is accepted and changes
// nothing, and every query answers with the mode actually in force — which
// is what an application checks before deciding how much it can draw.
// ---------------------------------------------------------------------------

extern "C" int SDL_GetWindowDisplayMode(SDL_Window *win, SDL_DisplayMode *mode)
{
    if (!win || !mode)
        return SDL_SetError("SDL_GetWindowDisplayMode: no window or mode");
    return SDL_GetCurrentDisplayMode(0, mode);
}

extern "C" int SDL_SetWindowDisplayMode(SDL_Window *win, const SDL_DisplayMode *)
{
    if (!win)
        return SDL_SetError("SDL_SetWindowDisplayMode: no window");
    return 0;
}

// SDL's contract is to return the closest mode it has, and there is exactly
// one to be closest.
extern "C" SDL_DisplayMode *SDL_GetClosestDisplayMode(int displayIndex,
                                                      const SDL_DisplayMode *,
                                                      SDL_DisplayMode *closest)
{
    if (!closest || SDL_GetCurrentDisplayMode(displayIndex, closest) < 0)
        return nullptr;
    return closest;
}

// No panel reports its physical size here, and SDL's contract is to fail
// rather than invent one — an application that scales its text by the answer
// would lay itself out to a number that means nothing.
extern "C" int SDL_GetDisplayDPI(int, float *ddpi, float *hdpi, float *vdpi)
{
    if (ddpi) *ddpi = 0.0f;
    if (hdpi) *hdpi = 0.0f;
    if (vdpi) *vdpi = 0.0f;
    return SDL_SetError("SDL_GetDisplayDPI: the display does not report its "
                        "physical size");
}

// There is no compositor between the window and the panel, so a drawable
// pixel is a window pixel.
extern "C" void SDL_GL_GetDrawableSize(SDL_Window *win, int *w, int *h)
{
    SDL_GetWindowSize(win, w, h);
}

// Same reason: a window pixel IS a canvas pixel, with no scaling factor of
// the kind a desktop applies on a high-density display.
extern "C" void SDL_GetWindowSizeInPixels(SDL_Window *win, int *w, int *h)
{
    SDL_GetWindowSize(win, w, h);
}

extern "C" void SDL_Vulkan_GetDrawableSize(SDL_Window *win, int *w, int *h)
{
    SDL_GetWindowSize(win, w, h);
}

// Gamma is applied by hardware this shim does not drive. Refused rather than
// accepted silently: an application correcting its own output would believe
// a correction had been made that had not.
extern "C" int SDL_SetWindowGammaRamp(SDL_Window *, const Uint16 *,
                                      const Uint16 *, const Uint16 *)
{
    return SDL_SetError("SDL_SetWindowGammaRamp: this display has no gamma "
                        "ramp");
}

extern "C" int SDL_GetWindowGammaRamp(SDL_Window *, Uint16 *, Uint16 *, Uint16 *)
{
    return SDL_SetError("SDL_GetWindowGammaRamp: this display has no gamma "
                        "ramp");
}

// There is no window system to describe, and SDL_syswm.h's structure is
// entirely made of window-system handles. Declared with the structure left
// opaque: naming it would mean including SDL_syswm.h here, whose contents
// are decided by which window system the configuration names — and this
// configuration names none.
struct SDL_SysWMinfo;
extern "C" SDL_bool SDL_GetWindowWMInfo(SDL_Window *, SDL_SysWMinfo *)
{
    SDL_SetError("SDL_GetWindowWMInfo: there is no window system");
    return SDL_FALSE;
}

// ---------------------------------------------------------------------------
// Drawing to the window without a renderer
//
// SDL's older path: ask the window for a surface, draw into it, and say when
// to put it on screen. It is kept as one surface for the window's lifetime,
// because that is the pointer applications hold on to across frames.
// ---------------------------------------------------------------------------

static SDL_Surface *s_window_surface = nullptr;

extern "C" SDL_Surface *SDL_GetWindowSurface(SDL_Window *win)
{
    if (!win)
    {
        SDL_SetError("SDL_GetWindowSurface: no window");
        return nullptr;
    }
    if (s_window_surface
        && s_window_surface->w == win->w && s_window_surface->h == win->h)
        return s_window_surface;

    SDL_FreeSurface(s_window_surface);
    s_window_surface = SDL_CreateRGBSurfaceWithFormat(0, win->w, win->h, 32,
                                                      SDL_PIXELFORMAT_ARGB8888);
    return s_window_surface;
}

static void flip_on0(void *p)
{
    SDL2Circle_VideoFlip((unsigned)(uintptr)p);
}

// Which framebuffer half this path draws into next. It alternates only on a
// grant of two halves, for the reason the renderer alternates only then: on a
// single-half grant the executor and the flip both work through the shadow
// and ignore the half entirely, and naming half 1 there would address memory
// past the grant.
static unsigned s_window_surface_back = 0;

extern "C" int SDL_UpdateWindowSurfaceRects(SDL_Window *win,
                                            const SDL_Rect *rects, int numrects)
{
    if (!win || !s_window_surface)
        return SDL_SetError("SDL_UpdateWindowSurfaceRects: no window surface");

    // THE WINDOW SURFACE IS A FRAME LIKE ANY OTHER, and it takes the same
    // route to the glass: mapped from canvas coordinates onto the fitted
    // rectangle on the scanout, then handed to the presentation core.
    //
    // Both halves of that used to be missing here, and each on its own is
    // enough to put the picture in the wrong place. The command was built in
    // CANVAS coordinates and executed unmapped, so a canvas smaller than the
    // scanout landed at its own size in the corner with the fit — already
    // computed, already logged — ignored; and it was executed on whichever
    // core called, so the scale that the presentation core exists to do was
    // done by the application's core instead. A game that draws through a
    // renderer never saw either, which is why this survived: the renderer
    // path does both, a few hundred lines up.
    //
    // WHY THE WHOLE CANVAS IS CARRIED ACROSS EACH TIME, and not just the
    // rectangles the caller named. The surface handed over has to be one the
    // application cannot write while the presentation core is still reading
    // it, so the frame is copied into the canvas surface, which is double
    // buffered exactly for that: this frame goes into the buffer the worker
    // is not holding. Copying only the named rectangles into that buffer
    // would leave everything else in it showing the frame BEFORE last, so
    // what is copied is the whole surface. It buys back far more than it
    // costs — the scale it moves off this core is the larger picture, at
    // scanout size, every frame.
    //
    // The rectangles are still honoured where they can be: with no canvas
    // surface to be had this falls back to presenting the named rectangles
    // straight from the application's own surface, on this core, which is
    // safe precisely because nothing crosses.
    const SDL_Rect whole = { 0, 0, win->w, win->h };

    SDL2CirclePresentCmd frame;
    memset(&frame, 0, sizeof(frame));
    frame.op = SDL2CirclePresentCmd::COPY;
    frame.alphamod = 255;

    // The window IS the canvas here, so these agree by construction — but
    // the copy below writes canvas-sized storage from window-sized rows, and
    // a disagreement would write past the end of it. Tested rather than
    // assumed, and a disagreement simply takes the other route.
    const bool bCrossable = win->w == s_canvas_w && win->h == s_canvas_h
                            && canvas_surface_ready();
    if (bCrossable)
    {
        SDL2CirclePresentCmd in = frame;
        in.dx = 0; in.dy = 0;
        in.w = win->w; in.h = win->h;
        in.sw = win->w; in.sh = win->h;
        in.src = (u8 *)s_window_surface->pixels;
        in.srcpitch = s_window_surface->pitch;
        exec_into(&in, s_canvas_surface, s_canvas_surface_pitch);

        frame.src = s_canvas_surface;
        frame.srcpitch = (int)s_canvas_surface_pitch;
        frame.dx = 0; frame.dy = 0;
        frame.w = win->w; frame.h = win->h;
        frame.sw = win->w; frame.sh = win->h;

        if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
        {
            SDL2Circle_PresentPost(&frame, 1, s_window_surface_back);
            if (s_canvas_surface_buf[1])
            {
                s_canvas_surface_idx ^= 1;
                s_canvas_surface = s_canvas_surface_buf[s_canvas_surface_idx];
            }
            if (s_fb_halves == 2)
                s_window_surface_back ^= 1;
            return 0;
        }

        SDL2Circle_VideoExecCmd(&frame, s_window_surface_back);
    }
    else
    {
        for (int i = 0; i < (rects ? numrects : 1); i++)
        {
            SDL_Rect r = rects ? rects[i] : whole;
            if (!SDL_IntersectRect(&r, &whole, &r))
                continue;

            SDL2CirclePresentCmd cmd = frame;
            cmd.dx = r.x;
            cmd.dy = r.y;
            cmd.w = r.w;
            cmd.h = r.h;
            cmd.src = (u8 *)s_window_surface->pixels
                    + (size_t)r.y * s_window_surface->pitch + (size_t)r.x * 4;
            cmd.srcpitch = s_window_surface->pitch;
            cmd.sw = r.w;
            cmd.sh = r.h;

            SDL2Circle_VideoExecCmd(&cmd, s_window_surface_back);
        }
    }

    // The flip asks the firmware, through the VideoCore mailbox, and this
    // function runs on whichever core the application is on. The mailbox is
    // guarded by one spin lock shared by every core and the wait inside it
    // has no timeout, so the fewer cores that ever reach it the fewer there
    // are to collide. Marshalled to core 0, like every other firmware call.
    SDL2Circle_CallOn0(flip_on0, (void *)(uintptr)s_window_surface_back);
    if (s_fb_halves == 2)
        s_window_surface_back ^= 1;
    return 0;
}

extern "C" int SDL_UpdateWindowSurface(SDL_Window *win)
{
    return SDL_UpdateWindowSurfaceRects(win, nullptr, 1);
}

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
    ren->have_copy = false;
    ren->rasterizing = false;
    ren->border_color = 0;
    ren->frame_x = ren->frame_y = ren->frame_w = ren->frame_h = -1;
    ren->border_repaint = 0;
    ren->draw_blend = SDL_BLENDMODE_NONE;
    ren->logical_w = 0;
    ren->logical_h = 0;
    ren->integer_scale = false;
    ren->scale_x = 1.0f;
    ren->scale_y = 1.0f;
    ren->viewport = { 0, 0, win->w, win->h };
    ren->viewport_set = false;
    ren->clip = { 0, 0, 0, 0 };
    ren->clip_enabled = false;
    ren->scratch[0] = ren->scratch[1] = nullptr;
    ren->scratch_bytes[0] = ren->scratch_bytes[1] = 0;
    ren->scratch_used = 0;
    ren->scratch_idx = 0;
    s_renderer = ren;
    return ren;
}

// Room inside this frame's scratch arena, or null when it cannot be had. The
// arena keeps whatever it grew to, so a game that flips the same sprites
// every frame allocates once and never again.
static void *frame_scratch(SDL_Renderer *ren, size_t bytes)
{
    const u8 i = ren->scratch_idx;
    if (ren->scratch_used + bytes > ren->scratch_bytes[i])
    {
        const size_t want = ren->scratch_used + bytes;
        u8 *grown = (u8 *)realloc(ren->scratch[i], want);
        if (!grown)
            return nullptr;
        ren->scratch[i] = grown;
        ren->scratch_bytes[i] = want;
    }
    void *p = ren->scratch[i] + ren->scratch_used;
    ren->scratch_used += bytes;
    return p;
}

// Start the next frame's arena. The one just posted stays intact for as long
// as the frame referencing it is in flight.
static void frame_scratch_next(SDL_Renderer *ren)
{
    ren->scratch_idx ^= 1;
    ren->scratch_used = 0;
}

// ---------------------------------------------------------------------------
// The application's coordinates, and where they land on the window
//
// Three things sit between what an application draws and what the window
// receives, in the order SDL2 applies them: the render scale, the logical
// size (which scales and centres), and the viewport (which offsets). One
// function does all three, and every entry point that takes a destination
// rectangle goes through it, so an application cannot find one call honouring
// its logical size and another ignoring it.
// ---------------------------------------------------------------------------

namespace
{
struct LogicalMap
{
    float sx, sy;     // application units -> window pixels
    int   ox, oy;     // where the application's origin sits in the window
};

LogicalMap logical_map(const SDL_Renderer *ren)
{
    LogicalMap m = { ren->scale_x, ren->scale_y, 0, 0 };

    if (ren->logical_w > 0 && ren->logical_h > 0)
    {
        const int ow = ren->window->w;
        const int oh = ren->window->h;
        float sx = (float)ow / (float)ren->logical_w;
        float sy = (float)oh / (float)ren->logical_h;

        if (ren->integer_scale)
        {
            // Whole pixels only: what a pixel-art game asks for so that
            // every source pixel is the same size on screen. Below 1:1
            // there is no whole factor to use, so the fractional one
            // stands rather than the picture vanishing.
            float s = (sx < sy) ? sx : sy;
            if (s >= 1.0f)
                s = (float)(int)s;
            sx = sy = s;
        }
        else
        {
            // Letterbox: one factor for both axes so the aspect is kept.
            const float s = (sx < sy) ? sx : sy;
            sx = sy = s;
        }

        m.sx = sx * ren->scale_x;
        m.sy = sy * ren->scale_y;
        m.ox = (int)((ow - ren->logical_w * m.sx) / 2.0f);
        m.oy = (int)((oh - ren->logical_h * m.sy) / 2.0f);
    }

    // The viewport is given in the application's coordinates, like every
    // other rectangle it hands over, so its origin is scaled by the same
    // factors before it moves the origin.
    if (ren->viewport_set)
    {
        m.ox += (int)(ren->viewport.x * m.sx);
        m.oy += (int)(ren->viewport.y * m.sy);
    }
    return m;
}

// The window-pixel rectangle drawing is confined to. It is the viewport
// where one is set and the whole window otherwise, narrowed by the clip
// rectangle when clipping is enabled. Both come in the application's
// coordinates and are mapped with the factors above, so a viewport under a
// logical size letterboxes with everything else rather than against it.
SDL_Rect confine_rect(const SDL_Renderer *ren)
{
    const LogicalMap m = logical_map(ren);
    SDL_Rect r;

    if (ren->viewport_set)
    {
        // m.ox/m.oy already carry the viewport's own origin.
        r.x = m.ox;
        r.y = m.oy;
        r.w = (int)(ren->viewport.w * m.sx);
        r.h = (int)(ren->viewport.h * m.sy);
    }
    else
    {
        r = { 0, 0, ren->window->w, ren->window->h };
    }

    if (ren->clip_enabled)
    {
        SDL_Rect c;
        c.x = m.ox + (int)(ren->clip.x * m.sx);
        c.y = m.oy + (int)(ren->clip.y * m.sy);
        c.w = (int)(ren->clip.w * m.sx);
        c.h = (int)(ren->clip.h * m.sy);
        if (!SDL_IntersectRect(&r, &c, &r))
            r = { 0, 0, 0, 0 };
    }

    // Never outside the window, whatever was asked for.
    const SDL_Rect win = { 0, 0, ren->window->w, ren->window->h };
    if (!SDL_IntersectRect(&r, &win, &r))
        r = { 0, 0, 0, 0 };
    return r;
}

// Map one destination rectangle from the application's coordinates into the
// window's. Returns false when the result has no area, which is a no-op and
// not an error.
bool map_dst(const SDL_Renderer *ren, int &x, int &y, int &w, int &h)
{
    const LogicalMap m = logical_map(ren);
    if (m.sx == 1.0f && m.sy == 1.0f && m.ox == 0 && m.oy == 0)
        return (w > 0 && h > 0);

    const int x1 = m.ox + (int)(x * m.sx);
    const int y1 = m.oy + (int)(y * m.sy);
    const int x2 = m.ox + (int)((x + w) * m.sx);
    const int y2 = m.oy + (int)((y + h) * m.sy);
    x = x1;
    y = y1;
    w = x2 - x1;
    h = y2 - y1;
    return (w > 0 && h > 0);
}
} // namespace

extern "C" void SDL_DestroyRenderer(SDL_Renderer *ren)
{
    if (!ren)
        return;

    // A posted frame may still be referencing an arena, so nothing here can
    // go back until the presentation worker is out of the frame.
    SDL2Circle_PresentQuiesce();
    if (s_renderer == ren)
        s_renderer = nullptr;
    free(ren->scratch[0]);
    free(ren->scratch[1]);
    delete ren;
}

// The pair in one call, which is how a great many SDL2 programs start. The
// window is the canvas whatever size is asked for, as SDL_CreateWindow
// explains.
extern "C" int SDL_CreateWindowAndRenderer(int width, int height,
                                           Uint32 window_flags,
                                           SDL_Window **window,
                                           SDL_Renderer **renderer)
{
    if (!window || !renderer)
        return SDL_SetError("SDL_CreateWindowAndRenderer: nowhere to put the "
                            "window or the renderer");

    *window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED,
                               SDL_WINDOWPOS_UNDEFINED, width, height,
                               window_flags);
    if (!*window)
    {
        *renderer = nullptr;
        return -1;
    }

    *renderer = SDL_CreateRenderer(*window, -1, 0);
    if (!*renderer)
    {
        SDL_DestroyWindow(*window);
        *window = nullptr;
        return -1;
    }
    return 0;
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
    if (w <= 0 || h <= 0)
    {
        SDL_SetError("texture dimensions must be positive");
        return nullptr;
    }
    if (access == SDL_TEXTUREACCESS_TARGET)
    {
        // Render-to-texture would need the whole draw path to be able to
        // aim somewhere other than the frame, which it cannot. Saying so is
        // the point: an application that checks
        // SDL_RenderTargetSupported gets FALSE and takes its other path.
        SDL_SetError("render targets are not supported");
        return nullptr;
    }
    const int app_bpp = SDL2Circle_BytesPerPixel(format);
    if (app_bpp == 0)
    {
        SDL_SetError("texture pixel format 0x%08x is not supported",
                     (unsigned)format);
        return nullptr;
    }
    if (format == SDL_PIXELFORMAT_INDEX8)
    {
        // SDL2 gives a texture no palette — SDL_UpdateTexture takes pixels
        // and nothing else — so an indexed texture has no way to say what
        // its indices mean. A paletted game wants an indexed SURFACE, which
        // this library does provide, converted on the way to a texture.
        SDL_SetError("indexed textures have no palette in SDL2; use an "
                     "indexed surface and convert it");
        return nullptr;
    }

    SDL_Texture *tex = new SDL_Texture;
    tex->w = w;
    tex->h = h;
    tex->format = format;
    tex->access = access;
    tex->pitch = w * 4;
    tex->pixels[0] = (u8 *)malloc((size_t)tex->pitch * h);
    tex->pixels[1] = nullptr;   // allocated on first split-mode reuse
    tex->pixels[2] = nullptr;
    tex->widx = 0;
    tex->busy_seq[0] = tex->busy_seq[1] = tex->busy_seq[2] = 0;
    tex->app_bpp = app_bpp;
    tex->staging = nullptr;
    tex->staging_pitch = 0;
    tex->locked_rect = { 0, 0, 0, 0 };
    tex->locked = false;
    tex->blend = SDL_BLENDMODE_NONE;
    tex->alphamod = 255;
    tex->colormod_r = tex->colormod_g = tex->colormod_b = 255;
    tex->scale_mode = SDL_ScaleModeNearest;
    tex->userdata = nullptr;

    if (tex->pixels[0] == nullptr)
    {
        delete tex;
        SDL_SetError("out of memory allocating texture");
        return nullptr;
    }
    memset(tex->pixels[0], 0, (size_t)tex->pitch * h);
    return tex;
}

// Whether this texture's pixels are stored in the format the application
// believes it is writing. When they are, every path below is a straight
// copy; when they are not, a conversion sits in the way.
static inline bool texture_is_native(const SDL_Texture *tex)
{
    return tex->format == SDL_PIXELFORMAT_ARGB8888;
}

// Core split: a texture referenced by the frame in flight must not be
// written; hand the app the other buffer. One frame is in flight at most
// (SDL2Circle_PresentPost waits for the previous ACK), so two buffers are
// provably enough. MAME's software path redraws the full texture each
// frame; the partial-update path still copies the stable content across
// first.
// Whether a store is spoken for: named by a frame that has been POSTED and
// not yet finished with.
//
// IT IS NOT ENOUGH TO ASK WHETHER THE WORKER HAS STARTED READING IT, and the
// difference is worth writing down because the weaker test looks obviously
// better and tears. A store carries ONE sequence — the last frame to name it
// — so once a newer frame overwrites that mark, an older frame still queued
// to read the same store is forgotten. Allowing the writer into a
// posted-but-unstarted store is what lets a second frame claim it, and the
// worker then reaches the first frame and reads a store two writers have
// been through. Simulated with a worker slower than the poster, that tears
// on essentially every frame.
//
// A store named by the frame still being BUILT is correctly free: it carries
// a sequence above the posted one, and nothing has been sent that could read
// it. Without that an application drawing to one texture twice before
// presenting would wait for a frame nobody has posted.
static bool texture_store_busy(const SDL_Texture *tex, u8 i)
{
    const u64 b = tex->busy_seq[i];
    if (b == 0)
        return false;
    return b > SDL2Circle_PresentAckedSeq() && b <= SDL2Circle_PresentPostedSeq();
}

// Hand back a store the application may write.
//
// Under the core split a posted frame holds a raw pointer into one of these,
// and the worker reads it for as long as its scale runs — so the one thing
// this must never do is return the store that scale is reading.
//
// WHAT WENT WRONG BEFORE, since the fix is easier to keep if the fault it
// replaced is written down: the old rule flipped stores when the last
// RECORDED copy named the one about to be written. That is a proxy for the
// question and not the question. It tracks where the WRITER went last; what
// matters is what the READER still holds, and the two agree only by
// coincidence. The result when they disagreed was a frame torn between two
// pictures — every pitch correct, every pixel real, all of it in the wrong
// place, which is why it read as a stride fault and was not one.
//
// (An earlier account of this blamed frames that were recorded and never
// posted. There are none: every SDL_RenderPresent posts. The proxy was
// simply the wrong question.)
static u8 *texture_write_buffer(SDL_Texture *tex, bool preserve)
{
    if (!SDL2Circle_SplitActive())
        return tex->pixels[tex->widx];

    if (!texture_store_busy(tex, tex->widx))
        return tex->pixels[tex->widx];      // still ours; no copy needed

    // THREE STORES, AND THE THIRD IS WHAT REMOVES THE WAIT.
    //
    // With two, a poster running ahead of the worker has nowhere to put a
    // frame: one store is being read and the other holds the frame already
    // posted behind it, so the writer stops. That is the game core waiting
    // for the presentation core, which is the thing this must not do — and
    // simulation puts it at roughly every other frame once the worker is
    // slower than the poster. With three there is always one that is neither.
    //
    // Taken in rotation rather than by a flip, so a store released by the
    // worker comes back into use rather than one being favoured.
    const unsigned n = (unsigned)(sizeof tex->pixels / sizeof tex->pixels[0]);
    for (unsigned k = 1; k < n; k++)
    {
        const u8 c = (u8)((tex->widx + k) % n);
        if (!tex->pixels[c])
            tex->pixels[c] = (u8 *)malloc((size_t)tex->pitch * tex->h);
        if (!tex->pixels[c] || texture_store_busy(tex, c))
            continue;

        if (preserve)
            memcpy(tex->pixels[c], tex->pixels[tex->widx],
                   (size_t)tex->pitch * tex->h);
        tex->widx = c;
        return tex->pixels[tex->widx];
    }

    // Every store is spoken for, or none could be allocated. Waiting is the
    // only answer left: writing into one the worker is reading is what all
    // of this exists to prevent. Simulation never reaches this with three
    // stores at any worker speed, so it is the safety net rather than the
    // path.
    SDL2Circle_PresentWaitAck(tex->busy_seq[tex->widx]);
    return tex->pixels[tex->widx];
}

extern "C" int SDL_QueryTexture(SDL_Texture *tex, Uint32 *format, int *access,
                                int *w, int *h)
{
    if (tex == nullptr)
        return SDL_InvalidParamError("texture");
    if (format) *format = tex->format;
    if (access) *access = tex->access;
    if (w) *w = tex->w;
    if (h) *h = tex->h;
    return 0;
}

extern "C" int SDL_UpdateTexture(SDL_Texture *tex, const SDL_Rect *rect,
                                 const void *pixels, int pitch)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    if (tex == nullptr)
        return SDL_InvalidParamError("texture");
    if (pixels == nullptr)
        return SDL_InvalidParamError("pixels");

    int x = rect ? rect->x : 0;
    int y = rect ? rect->y : 0;
    int w = rect ? rect->w : tex->w;
    int h = rect ? rect->h : tex->h;
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > tex->w || y + h > tex->h)
        return SDL_SetError("update rectangle lies outside the texture");
    if (w == 0 || h == 0)
        return 0;

    bool partial = (x != 0) || (y != 0) || (w != tex->w) || (h != tex->h);
    u8 *dst = texture_write_buffer(tex, partial)
              + (size_t)y * tex->pitch + (size_t)x * 4;

    if (texture_is_native(tex))
    {
        const u8 *src = (const u8 *)pixels;
        for (int row = 0; row < h; row++)
        {
            memcpy(dst, src, (size_t)w * 4);
            src += pitch;
            dst += tex->pitch;
        }
        return 0;
    }

    return SDL_ConvertPixels(w, h, tex->format, pixels, pitch,
                             SDL_PIXELFORMAT_ARGB8888, dst, tex->pitch);
}

extern "C" int SDL_UpdateYUVTexture(SDL_Texture *, const SDL_Rect *,
                                    const Uint8 *, int, const Uint8 *, int,
                                    const Uint8 *, int)
{
    // No YUV texture can be created here (SDL_CreateTexture refuses the
    // formats), so there is never a texture for this call to update.
    return SDL_SetError("YUV textures are not supported");
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

// Recorded but not applied when drawing. It is kept so that the getter
// answers with what was set, which is what an application that saves and
// restores the modulation around a draw depends on.
extern "C" int SDL_SetTextureColorMod(SDL_Texture *tex, Uint8 r, Uint8 g,
                                      Uint8 b)
{
    if (!tex)
        return SDL_SetError("SDL_SetTextureColorMod: no texture");
    tex->colormod_r = r;
    tex->colormod_g = g;
    tex->colormod_b = b;
    return 0;
}

extern "C" int SDL_GetTextureColorMod(SDL_Texture *tex, Uint8 *r, Uint8 *g,
                                      Uint8 *b)
{
    if (!tex)
        return SDL_SetError("SDL_GetTextureColorMod: no texture");
    if (r) *r = tex->colormod_r;
    if (g) *g = tex->colormod_g;
    if (b) *b = tex->colormod_b;
    return 0;
}

extern "C" int SDL_GetTextureAlphaMod(SDL_Texture *tex, Uint8 *alpha)
{
    if (!tex)
        return SDL_SetError("SDL_GetTextureAlphaMod: no texture");
    if (alpha)
        *alpha = tex->alphamod;
    return 0;
}

// The blitters are nearest-neighbour throughout, so the scale mode is
// recorded and changes nothing. Recorded rather than refused because an
// application asking for linear scaling on a machine that cannot do it
// should still get a picture.
extern "C" int SDL_SetTextureScaleMode(SDL_Texture *tex, SDL_ScaleMode mode)
{
    if (!tex)
        return SDL_SetError("SDL_SetTextureScaleMode: no texture");
    tex->scale_mode = mode;
    return 0;
}

extern "C" int SDL_GetTextureScaleMode(SDL_Texture *tex, SDL_ScaleMode *mode)
{
    if (!tex)
        return SDL_SetError("SDL_GetTextureScaleMode: no texture");
    if (mode)
        *mode = tex->scale_mode;
    return 0;
}

// A pointer the application hangs on the texture for its own purposes. The
// library never looks at it.
extern "C" int SDL_SetTextureUserData(SDL_Texture *tex, void *userdata)
{
    if (!tex)
        return SDL_SetError("SDL_SetTextureUserData: no texture");
    tex->userdata = userdata;
    return 0;
}

extern "C" void *SDL_GetTextureUserData(SDL_Texture *tex)
{
    return tex ? tex->userdata : nullptr;
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
    free(tex->pixels[2]);
    free(tex->staging);
    delete tex;
}

extern "C" int SDL_LockTexture(SDL_Texture *tex, const SDL_Rect *rect,
                               void **pixels, int *pitch)
{
    if (tex == nullptr)
        return SDL_InvalidParamError("texture");
    if (pixels == nullptr || pitch == nullptr)
        return SDL_InvalidParamError("pixels");
    if (tex->access != SDL_TEXTUREACCESS_STREAMING)
        return SDL_SetError("only a streaming texture can be locked");
    if (tex->locked)
        return SDL_SetError("texture is already locked");

    const SDL_Rect area = rect ? *rect : SDL_Rect{ 0, 0, tex->w, tex->h };
    if (area.x < 0 || area.y < 0 || area.w < 0 || area.h < 0 ||
        area.x + area.w > tex->w || area.y + area.h > tex->h)
        return SDL_SetError("lock rectangle lies outside the texture");

    u8 *buf = texture_write_buffer(tex, rect != nullptr);

    if (texture_is_native(tex))
    {
        *pixels = buf + (size_t)area.y * tex->pitch + (size_t)area.x * 4;
        *pitch  = tex->pitch;
        tex->locked_rect = area;
        tex->locked = true;
        return 0;
    }

    // The application must see its OWN format under the lock, so it gets a
    // staging buffer the size of the whole texture — kept between locks,
    // because a streaming texture is locked every frame and reallocating it
    // each time is the cost this path is trying to avoid.
    const int need_pitch = tex->w * tex->app_bpp;
    if (tex->staging == nullptr)
    {
        tex->staging = (u8 *)malloc((size_t)need_pitch * tex->h);
        if (tex->staging == nullptr)
            return SDL_SetError("out of memory allocating texture lock buffer");
        tex->staging_pitch = need_pitch;
        memset(tex->staging, 0, (size_t)need_pitch * tex->h);
    }

    *pixels = tex->staging + (size_t)area.y * tex->staging_pitch
            + (size_t)area.x * tex->app_bpp;
    *pitch  = tex->staging_pitch;
    tex->locked_rect = area;
    tex->locked = true;
    return 0;
}

extern "C" int SDL_LockTextureToSurface(SDL_Texture *tex, const SDL_Rect *rect,
                                        SDL_Surface **surface)
{
    if (surface == nullptr)
        return SDL_InvalidParamError("surface");
    void *pixels = nullptr;
    int   pitch  = 0;
    if (SDL_LockTexture(tex, rect, &pixels, &pitch) < 0)
        return -1;
    const SDL_Rect area = rect ? *rect : SDL_Rect{ 0, 0, tex->w, tex->h };
    *surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, area.w, area.h, 0,
                                                  pitch, tex->format);
    return (*surface != nullptr) ? 0 : -1;
}

extern "C" void SDL_UnlockTexture(SDL_Texture *tex)
{
    if (tex == nullptr || !tex->locked)
        return;
    tex->locked = false;

    if (texture_is_native(tex))
        return;   // the application wrote straight into the stored pixels

    const SDL_Rect &area = tex->locked_rect;
    if (area.w <= 0 || area.h <= 0)
        return;

    u8 *dst = tex->pixels[tex->widx]
            + (size_t)area.y * tex->pitch + (size_t)area.x * 4;
    const u8 *src = tex->staging + (size_t)area.y * tex->staging_pitch
                  + (size_t)area.x * tex->app_bpp;
    SDL_ConvertPixels(area.w, area.h, tex->format, src, tex->staging_pitch,
                      SDL_PIXELFORMAT_ARGB8888, dst, tex->pitch);
}

// Clip one axis of a scaled blit: trim the destination span to [lo, hi) and
// take the source span with it, in proportion, so the scale factor survives
// the clip instead of quietly changing.
static void clip_axis_range(int &d, int &dlen, int &s, int &slen, int lo, int hi)
{
    if (d < lo)
    {
        int cut = lo - d;
        if (cut >= dlen) { dlen = 0; return; }
        int scut = (int)(((s64)cut * slen) / dlen);
        s += scut;
        slen -= scut;
        dlen -= cut;
        d = lo;
    }
    if (d + dlen > hi)
    {
        int cut = d + dlen - hi;
        if (cut >= dlen) { dlen = 0; return; }
        slen -= (int)(((s64)cut * slen) / dlen);
        dlen -= cut;
    }
}

static void clip_axis(int &d, int &dlen, int &s, int &slen, int limit)
{
    clip_axis_range(d, dlen, s, slen, 0, limit);
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

    // An absent destination means the whole render target, which under a
    // logical size is the logical rectangle rather than the window.
    if (dstrect == nullptr && ren->logical_w > 0 && ren->logical_h > 0)
    {
        dw = ren->logical_w;
        dh = ren->logical_h;
    }
    if (!map_dst(ren, dx, dy, dw, dh))
        return 0;

    // Keep the source inside the texture (reading past a texture allocation
    // is a fault with nothing underneath to catch it), then the destination
    // inside the canvas. Each clip carries the other rectangle with it.
    clip_axis(sx, sw, dx, dw, tex->w);
    clip_axis(sy, sh, dy, dh, tex->h);

    // The destination is confined to the viewport and clip rectangle, which
    // are the whole window when neither is set.
    const SDL_Rect confine = confine_rect(ren);
    clip_axis_range(dx, dw, sx, sw, confine.x, confine.x + confine.w);
    clip_axis_range(dy, dh, sy, sh, confine.y, confine.y + confine.h);
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

    // This store is now spoken for by the frame being assembled, which will
    // be posted as the next sequence. Until the worker acknowledges that
    // frame, nothing may write here.
    tex->busy_seq[tex->widx] = SDL2Circle_PresentPostedSeq() + 1;
    return 0;
}

// A copy that may be mirrored. The texture holds one set of pixels and has
// no mirrored copy of its own, so the mirrored region is written into the
// frame's scratch arena and copied from there — which is why the arena
// exists, and why it lasts exactly as long as a recorded command does.
//
// ROTATION IS REFUSED. Turning a picture by an arbitrary angle means
// resampling every destination pixel from a source position between four
// others, which is a different piece of work from a blit and is not here.
// Refusing says so; drawing it unrotated would be a picture that is wrong in
// a way nothing on screen would explain.
extern "C" int SDL_RenderCopyEx(SDL_Renderer *ren, SDL_Texture *tex,
                                const SDL_Rect *srcrect, const SDL_Rect *dstrect,
                                const double angle, const SDL_Point *center,
                                const SDL_RendererFlip flip)
{
    if (!ren || !tex)
        return SDL_SetError("SDL_RenderCopyEx: no renderer or texture");

    if (angle != 0.0)
        return SDL_SetError("SDL_RenderCopyEx: rotation by %g degrees is not "
                            "implemented", angle);
    (void)center;   // only meaningful with a rotation

    if (flip == SDL_FLIP_NONE)
        return SDL_RenderCopy(ren, tex, srcrect, dstrect);

    const int sx = srcrect ? srcrect->x : 0;
    const int sy = srcrect ? srcrect->y : 0;
    const int sw = srcrect ? srcrect->w : tex->w;
    const int sh = srcrect ? srcrect->h : tex->h;
    if (sw <= 0 || sh <= 0)
        return 0;
    if (sx < 0 || sy < 0 || sx + sw > tex->w || sy + sh > tex->h)
        return SDL_SetError("SDL_RenderCopyEx: source rectangle is outside "
                            "the texture");

    // The mirrored region, in the stored format, in this frame's arena.
    const size_t pitch = (size_t)sw * 4;
    u8 *mirror = (u8 *)frame_scratch(ren, pitch * (size_t)sh);
    if (!mirror)
        return SDL_SetError("SDL_RenderCopyEx: no room to mirror a %dx%d "
                            "region", sw, sh);

    const u8 *base = tex->pixels[tex->widx];
    const bool flip_h = (flip & SDL_FLIP_HORIZONTAL) != 0;
    const bool flip_v = (flip & SDL_FLIP_VERTICAL) != 0;

    for (int y = 0; y < sh; y++)
    {
        const int src_y = flip_v ? (sy + sh - 1 - y) : (sy + y);
        const Uint32 *s = (const Uint32 *)(base + (size_t)src_y * tex->pitch)
                        + sx;
        Uint32 *d = (Uint32 *)(mirror + (size_t)y * pitch);
        if (flip_h)
            for (int x = 0; x < sw; x++)
                d[x] = s[sw - 1 - x];
        else
            memcpy(d, s, pitch);
    }

    // From here it is an ordinary copy of a whole image that happens to live
    // in the arena, so the destination mapping and clipping are the same.
    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;
    int dw = dstrect ? dstrect->w : ren->window->w;
    int dh = dstrect ? dstrect->h : ren->window->h;
    if (!dstrect && ren->logical_w > 0 && ren->logical_h > 0)
    {
        dw = ren->logical_w;
        dh = ren->logical_h;
    }
    if (dw <= 0 || dh <= 0)
        return 0;
    if (!map_dst(ren, dx, dy, dw, dh))
        return 0;

    int msx = 0, msy = 0, msw = sw, msh = sh;
    const SDL_Rect confine = confine_rect(ren);
    clip_axis_range(dx, dw, msx, msw, confine.x, confine.x + confine.w);
    clip_axis_range(dy, dh, msy, msh, confine.y, confine.y + confine.h);
    if (msw <= 0 || msh <= 0 || dw <= 0 || dh <= 0)
        return 0;

    SDL2CirclePresentCmd cmd;
    cmd.op = SDL2CirclePresentCmd::COPY;
    cmd.dx = dx;
    cmd.dy = dy;
    cmd.w = dw;
    cmd.h = dh;
    cmd.color = 0;
    cmd.src = mirror + (size_t)msy * pitch + (size_t)msx * 4;
    cmd.srcpitch = (int)pitch;
    cmd.sw = msw;
    cmd.sh = msh;
    cmd.blend = (tex->blend == SDL_BLENDMODE_BLEND) ? 1 : 0;
    cmd.alphamod = tex->alphamod;
    emit_cmd(ren, cmd);
    return 0;
}

// Reads back what has been drawn. The frame being assembled has not been
// executed yet — commands are recorded and run at present — so this answers
// with the last frame actually placed on the canvas.
extern "C" int SDL_RenderReadPixels(SDL_Renderer *ren, const SDL_Rect *rect,
                                    Uint32 format, void *pixels, int pitch)
{
    if (!ren || !pixels)
        return SDL_SetError("SDL_RenderReadPixels: no renderer or destination");

    SDL_Rect r = rect ? *rect
                      : SDL_Rect{ 0, 0, ren->window->w, ren->window->h };
    const SDL_Rect win = { 0, 0, ren->window->w, ren->window->h };
    if (!SDL_IntersectRect(&r, &win, &r))
        return 0;

    if (format == 0)
        format = SDL_PIXELFORMAT_ARGB8888;

    // The visible half is the one NOT being drawn into.
    const unsigned front = (s_fb_halves == 2) ? (ren->back ^ 1) : 0;
    const u8 *base = ren->base + (size_t)front * ren->pitch * (unsigned)ren->window->h;

    for (int y = 0; y < r.h; y++)
    {
        const u8 *src = base + (size_t)(r.y + y) * ren->pitch + (size_t)r.x * 4;
        u8 *dst = (u8 *)pixels + (size_t)y * pitch;
        if (SDL_ConvertPixels(r.w, 1, SDL_PIXELFORMAT_ARGB8888, src, ren->pitch,
                              format, dst, pitch) < 0)
            return -1;
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

// ---------------------------------------------------------------------------
// The renderer's coordinate state
//
// These are the setters for the mapping applied by logical_map above. None of
// them draws anything; each changes how every later destination rectangle is
// placed, and the getters answer with exactly what was set.
// ---------------------------------------------------------------------------

// SDL2: a logical size of 0x0 turns the logical coordinate system off and
// gives the window's own size back as the coordinate system.
extern "C" int SDL_RenderSetLogicalSize(SDL_Renderer *ren, int w, int h)
{
    if (!ren)
        return SDL_SetError("SDL_RenderSetLogicalSize: no renderer");
    if (w < 0 || h < 0)
        return SDL_SetError("SDL_RenderSetLogicalSize: negative size");

    if (w == 0 || h == 0)
    {
        ren->logical_w = 0;
        ren->logical_h = 0;
        return 0;
    }
    ren->logical_w = w;
    ren->logical_h = h;
    return 0;
}

extern "C" void SDL_RenderGetLogicalSize(SDL_Renderer *ren, int *w, int *h)
{
    if (w) *w = ren ? ren->logical_w : 0;
    if (h) *h = ren ? ren->logical_h : 0;
}

extern "C" int SDL_RenderSetIntegerScale(SDL_Renderer *ren, SDL_bool enable)
{
    if (!ren)
        return SDL_SetError("SDL_RenderSetIntegerScale: no renderer");
    ren->integer_scale = (enable == SDL_TRUE);
    return 0;
}

extern "C" SDL_bool SDL_RenderGetIntegerScale(SDL_Renderer *ren)
{
    return (ren && ren->integer_scale) ? SDL_TRUE : SDL_FALSE;
}

extern "C" int SDL_RenderSetScale(SDL_Renderer *ren, float sx, float sy)
{
    if (!ren)
        return SDL_SetError("SDL_RenderSetScale: no renderer");
    if (sx <= 0.0f || sy <= 0.0f)
        return SDL_SetError("SDL_RenderSetScale: scale must be positive");
    ren->scale_x = sx;
    ren->scale_y = sy;
    return 0;
}

extern "C" void SDL_RenderGetScale(SDL_Renderer *ren, float *sx, float *sy)
{
    if (sx) *sx = ren ? ren->scale_x : 1.0f;
    if (sy) *sy = ren ? ren->scale_y : 1.0f;
}

// A null rectangle means the whole render target, which under a logical size
// is the logical rectangle rather than the window.
extern "C" int SDL_RenderSetViewport(SDL_Renderer *ren, const SDL_Rect *rect)
{
    if (!ren)
        return SDL_SetError("SDL_RenderSetViewport: no renderer");

    if (!rect)
    {
        ren->viewport_set = false;
        ren->viewport = { 0, 0, ren->window->w, ren->window->h };
        return 0;
    }
    if (rect->w < 0 || rect->h < 0)
        return SDL_SetError("SDL_RenderSetViewport: negative size");

    ren->viewport = *rect;
    ren->viewport_set = true;
    return 0;
}

extern "C" void SDL_RenderGetViewport(SDL_Renderer *ren, SDL_Rect *rect)
{
    if (!rect)
        return;
    if (!ren)
    {
        *rect = { 0, 0, 0, 0 };
        return;
    }
    if (ren->viewport_set)
    {
        *rect = ren->viewport;
    }
    else if (ren->logical_w > 0 && ren->logical_h > 0)
    {
        *rect = { 0, 0, ren->logical_w, ren->logical_h };
    }
    else
    {
        *rect = { 0, 0, ren->window->w, ren->window->h };
    }
}

// A null rectangle disables clipping. An empty one does not: SDL keeps
// clipping enabled with an empty rectangle, which discards every later draw,
// and an application can tell the two apart with SDL_RenderIsClipEnabled.
extern "C" int SDL_RenderSetClipRect(SDL_Renderer *ren, const SDL_Rect *rect)
{
    if (!ren)
        return SDL_SetError("SDL_RenderSetClipRect: no renderer");

    if (!rect)
    {
        ren->clip_enabled = false;
        ren->clip = { 0, 0, 0, 0 };
        return 0;
    }
    ren->clip = *rect;
    ren->clip_enabled = true;
    return 0;
}

extern "C" void SDL_RenderGetClipRect(SDL_Renderer *ren, SDL_Rect *rect)
{
    if (!rect)
        return;
    *rect = (ren && ren->clip_enabled) ? ren->clip : SDL_Rect{ 0, 0, 0, 0 };
}

extern "C" SDL_bool SDL_RenderIsClipEnabled(SDL_Renderer *ren)
{
    return (ren && ren->clip_enabled) ? SDL_TRUE : SDL_FALSE;
}

// There is one render target — the canvas. A request for the default target
// is what the renderer is already doing; a request for a texture target is
// refused rather than silently ignored, because an application that believes
// it is drawing into a texture and is in fact drawing onto the screen has no
// way to notice.
extern "C" int SDL_SetRenderTarget(SDL_Renderer *ren, SDL_Texture *tex)
{
    if (!ren)
        return SDL_SetError("SDL_SetRenderTarget: no renderer");
    if (!tex)
        return 0;
    return SDL_SetError("SDL_SetRenderTarget: render-to-texture is not supported");
}

extern "C" SDL_Texture *SDL_GetRenderTarget(SDL_Renderer *)
{
    return nullptr;   // always the default target
}

extern "C" SDL_bool SDL_RenderTargetSupported(SDL_Renderer *)
{
    return SDL_FALSE;
}

extern "C" int SDL_GetRenderDrawColor(SDL_Renderer *ren, Uint8 *r, Uint8 *g,
                                      Uint8 *b, Uint8 *a)
{
    if (!ren)
        return SDL_SetError("SDL_GetRenderDrawColor: no renderer");
    if (r) *r = ren->r;
    if (g) *g = ren->g;
    if (b) *b = ren->b;
    if (a) *a = ren->a;
    return 0;
}

extern "C" int SDL_GetRenderDrawBlendMode(SDL_Renderer *ren,
                                          SDL_BlendMode *blendMode)
{
    if (!ren)
        return SDL_SetError("SDL_GetRenderDrawBlendMode: no renderer");
    if (blendMode)
        *blendMode = ren->draw_blend;
    return 0;
}

extern "C" SDL_Window *SDL_RenderGetWindow(SDL_Renderer *ren)
{
    if (!ren)
    {
        SDL_SetError("SDL_RenderGetWindow: no renderer");
        return nullptr;
    }
    return ren->window;
}

// Drawing is recorded and executed at present, so there is never anything
// queued in the sense this asks about.
extern "C" int SDL_RenderFlush(SDL_Renderer *ren)
{
    return ren ? 0 : SDL_SetError("SDL_RenderFlush: no renderer");
}

// ---------------------------------------------------------------------------
// Between the application's coordinates and the window's
//
// The pair an application needs once it has set a logical size: a pointer
// arrives in window coordinates and has to be tested against things drawn in
// logical ones. Both go through the same mapping every drawing call uses, so
// a hit test cannot disagree with what is on screen.
// ---------------------------------------------------------------------------

extern "C" void SDL_RenderLogicalToWindow(SDL_Renderer *ren, float logicalX,
                                          float logicalY, int *windowX,
                                          int *windowY)
{
    if (!ren)
    {
        if (windowX) *windowX = 0;
        if (windowY) *windowY = 0;
        return;
    }
    const LogicalMap m = logical_map(ren);
    if (windowX) *windowX = (int)(m.ox + logicalX * m.sx);
    if (windowY) *windowY = (int)(m.oy + logicalY * m.sy);
}

extern "C" void SDL_RenderWindowToLogical(SDL_Renderer *ren, int windowX,
                                          int windowY, float *logicalX,
                                          float *logicalY)
{
    if (!ren)
    {
        if (logicalX) *logicalX = 0.0f;
        if (logicalY) *logicalY = 0.0f;
        return;
    }
    const LogicalMap m = logical_map(ren);
    if (logicalX) *logicalX = (m.sx != 0.0f) ? ((float)windowX - m.ox) / m.sx : 0.0f;
    if (logicalY) *logicalY = (m.sy != 0.0f) ? ((float)windowY - m.oy) / m.sy : 0.0f;
}

// ---------------------------------------------------------------------------
// The float-rectangle spellings
//
// SDL2 offers every drawing call a second time taking floats, for
// applications that keep their geometry in them. The canvas is whole pixels,
// so each rounds to the integer call it already has rather than being a
// second implementation that could drift from it.
// ---------------------------------------------------------------------------

namespace
{
SDL_Rect ToRect(const SDL_FRect *r)
{
    SDL_Rect out = { (int)r->x, (int)r->y, (int)r->w, (int)r->h };
    return out;
}
} // namespace

extern "C" int SDL_RenderCopyF(SDL_Renderer *ren, SDL_Texture *tex,
                               const SDL_Rect *srcrect, const SDL_FRect *dstrect)
{
    if (!dstrect)
        return SDL_RenderCopy(ren, tex, srcrect, nullptr);
    const SDL_Rect d = ToRect(dstrect);
    return SDL_RenderCopy(ren, tex, srcrect, &d);
}

extern "C" int SDL_RenderCopyExF(SDL_Renderer *ren, SDL_Texture *tex,
                                 const SDL_Rect *srcrect, const SDL_FRect *dstrect,
                                 const double angle, const SDL_FPoint *center,
                                 const SDL_RendererFlip flip)
{
    SDL_Point c;
    SDL_Point *cp = nullptr;
    if (center)
    {
        c.x = (int)center->x;
        c.y = (int)center->y;
        cp = &c;
    }
    if (!dstrect)
        return SDL_RenderCopyEx(ren, tex, srcrect, nullptr, angle, cp, flip);
    const SDL_Rect d = ToRect(dstrect);
    return SDL_RenderCopyEx(ren, tex, srcrect, &d, angle, cp, flip);
}

extern "C" int SDL_RenderDrawPointF(SDL_Renderer *ren, float x, float y)
{
    return SDL_RenderDrawPoint(ren, (int)x, (int)y);
}

extern "C" int SDL_RenderDrawPointsF(SDL_Renderer *ren, const SDL_FPoint *points,
                                     int count)
{
    if (!ren)
        return SDL_SetError("SDL_RenderDrawPointsF: no renderer");
    if (!points && count > 0)
        return SDL_SetError("SDL_RenderDrawPointsF: no points");
    for (int i = 0; i < count; ++i)
        if (SDL_RenderDrawPoint(ren, (int)points[i].x, (int)points[i].y) < 0)
            return -1;
    return 0;
}

extern "C" int SDL_RenderDrawLineF(SDL_Renderer *ren, float x1, float y1,
                                   float x2, float y2)
{
    return SDL_RenderDrawLine(ren, (int)x1, (int)y1, (int)x2, (int)y2);
}

extern "C" int SDL_RenderDrawLinesF(SDL_Renderer *ren, const SDL_FPoint *points,
                                    int count)
{
    if (!ren)
        return SDL_SetError("SDL_RenderDrawLinesF: no renderer");
    if (!points && count > 0)
        return SDL_SetError("SDL_RenderDrawLinesF: no points");
    for (int i = 1; i < count; ++i)
        if (SDL_RenderDrawLine(ren, (int)points[i - 1].x, (int)points[i - 1].y,
                               (int)points[i].x, (int)points[i].y) < 0)
            return -1;
    return 0;
}

extern "C" int SDL_RenderDrawRectF(SDL_Renderer *ren, const SDL_FRect *rect)
{
    if (!rect)
        return SDL_RenderDrawRect(ren, nullptr);
    const SDL_Rect r = ToRect(rect);
    return SDL_RenderDrawRect(ren, &r);
}

extern "C" int SDL_RenderDrawRectsF(SDL_Renderer *ren, const SDL_FRect *rects,
                                    int count)
{
    if (!ren)
        return SDL_SetError("SDL_RenderDrawRectsF: no renderer");
    if (!rects && count > 0)
        return SDL_SetError("SDL_RenderDrawRectsF: no rectangles");
    for (int i = 0; i < count; ++i)
        if (SDL_RenderDrawRectF(ren, &rects[i]) < 0)
            return -1;
    return 0;
}

extern "C" int SDL_RenderFillRectF(SDL_Renderer *ren, const SDL_FRect *rect)
{
    if (!rect)
        return SDL_RenderFillRect(ren, nullptr);
    const SDL_Rect r = ToRect(rect);
    return SDL_RenderFillRect(ren, &r);
}

extern "C" int SDL_RenderFillRectsF(SDL_Renderer *ren, const SDL_FRect *rects,
                                    int count)
{
    if (!ren)
        return SDL_SetError("SDL_RenderFillRectsF: no renderer");
    if (!rects && count > 0)
        return SDL_SetError("SDL_RenderFillRectsF: no rectangles");
    for (int i = 0; i < count; ++i)
        if (SDL_RenderFillRectF(ren, &rects[i]) < 0)
            return -1;
    return 0;
}

// Vsync is a property of how a finished frame reaches the panel, which is
// decided when the renderer is made. Changing it afterwards is accepted and
// takes effect on the next present.
extern "C" int SDL_RenderSetVSync(SDL_Renderer *ren, int vsync)
{
    if (!ren)
        return SDL_SetError("SDL_RenderSetVSync: no renderer");
    ren->vsync = (vsync != 0);
    return 0;
}

// A texture holds ARGB8888, so a surface in any other format — or one
// carrying a colour key, which has to become real transparency before the
// key is lost — is converted once here rather than at every update.
extern "C" SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *ren,
                                                     SDL_Surface *surf)
{
    if (!ren)
    {
        SDL_SetError("SDL_CreateTextureFromSurface: no renderer");
        return nullptr;
    }
    if (!surf || !surf->format)
    {
        SDL_SetError("SDL_CreateTextureFromSurface: no surface");
        return nullptr;
    }

    SDL_Surface *src = surf;
    SDL_Surface *converted = nullptr;
    if (surf->format->format != SDL_PIXELFORMAT_ARGB8888
        || SDL_HasColorKey(surf) == SDL_TRUE)
    {
        converted = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ARGB8888, 0);
        if (!converted)
            return nullptr;   // SDL_ConvertSurfaceFormat has set the error
        src = converted;
    }

    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC,
                                         src->w, src->h);
    if (!tex)
    {
        if (converted)
            SDL_FreeSurface(converted);
        return nullptr;
    }

    if (SDL_UpdateTexture(tex, nullptr, src->pixels, src->pitch) < 0)
    {
        SDL_DestroyTexture(tex);
        if (converted)
            SDL_FreeSurface(converted);
        return nullptr;
    }

    // SDL carries the surface's blending across to the texture, so a surface
    // that blended goes on blending once it is one.
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    SDL_GetSurfaceBlendMode(surf, &blend);
    SDL_SetTextureBlendMode(tex, blend);

    Uint8 alpha = 255;
    SDL_GetSurfaceAlphaMod(surf, &alpha);
    SDL_SetTextureAlphaMod(tex, alpha);

    Uint8 r = 255, g = 255, b = 255;
    SDL_GetSurfaceColorMod(surf, &r, &g, &b);
    SDL_SetTextureColorMod(tex, r, g, b);

    if (converted)
        SDL_FreeSurface(converted);
    return tex;
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

    // An absent rectangle means the whole render target, which under a
    // logical size is the logical rectangle rather than the window.
    if (!rect && ren->logical_w > 0 && ren->logical_h > 0)
    {
        w = ren->logical_w;
        h = ren->logical_h;
    }
    if (!map_dst(ren, x, y, w, h))
        return 0;

    // Confined to the viewport and clip rectangle, which are the whole
    // window when neither is set.
    const SDL_Rect confine = confine_rect(ren);
    const SDL_Rect want = { x, y, w, h };
    SDL_Rect out;
    if (!SDL_IntersectRect(&want, &confine, &out))
        return 0;
    x = out.x; y = out.y; w = out.w; h = out.h;
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

// The plural forms SDL offers so an application can hand over a batch
// without a call per item. Each is its singular applied in order, and stops
// at the first failure so the error describes the item that caused it.
extern "C" int SDL_RenderFillRects(SDL_Renderer *ren, const SDL_Rect *rects,
                                   int count)
{
    if (!ren)
        return SDL_SetError("SDL_RenderFillRects: no renderer");
    if (!rects && count > 0)
        return SDL_SetError("SDL_RenderFillRects: no rectangles");
    for (int i = 0; i < count; ++i)
        if (SDL_RenderFillRect(ren, &rects[i]) < 0)
            return -1;
    return 0;
}

extern "C" int SDL_RenderDrawRects(SDL_Renderer *ren, const SDL_Rect *rects,
                                   int count)
{
    if (!ren)
        return SDL_SetError("SDL_RenderDrawRects: no renderer");
    if (!rects && count > 0)
        return SDL_SetError("SDL_RenderDrawRects: no rectangles");
    for (int i = 0; i < count; ++i)
        if (SDL_RenderDrawRect(ren, &rects[i]) < 0)
            return -1;
    return 0;
}

extern "C" int SDL_RenderDrawPoint(SDL_Renderer *ren, int x, int y)
{
    const SDL_Rect r = { x, y, 1, 1 };
    return SDL_RenderFillRect(ren, &r);
}

extern "C" int SDL_RenderDrawPoints(SDL_Renderer *ren, const SDL_Point *points,
                                    int count)
{
    if (!ren)
        return SDL_SetError("SDL_RenderDrawPoints: no renderer");
    if (!points && count > 0)
        return SDL_SetError("SDL_RenderDrawPoints: no points");
    for (int i = 0; i < count; ++i)
        if (SDL_RenderDrawPoint(ren, points[i].x, points[i].y) < 0)
            return -1;
    return 0;
}

// SDL draws a connected polyline: each point joins the one before it, so N
// points make N-1 segments.
extern "C" int SDL_RenderDrawLines(SDL_Renderer *ren, const SDL_Point *points,
                                   int count)
{
    if (!ren)
        return SDL_SetError("SDL_RenderDrawLines: no renderer");
    if (!points && count > 0)
        return SDL_SetError("SDL_RenderDrawLines: no points");
    for (int i = 1; i < count; ++i)
        if (SDL_RenderDrawLine(ren, points[i - 1].x, points[i - 1].y,
                               points[i].x, points[i].y) < 0)
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

    // The WHOLE CANVAS, which is as much of the display as anything on this
    // side of the library can name. The executor is what knows that clearing
    // the whole canvas means clearing the whole panel, border and all.
    SDL2CirclePresentCmd fill;
    memset(&fill, 0, sizeof(fill));
    fill.op = SDL2CirclePresentCmd::FILL;
    fill.dx = 0; fill.dy = 0;
    fill.w = s_canvas_w; fill.h = s_canvas_h;
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

    if (frame.w <= 0 || frame.h <= 0)
        return 0;

    border_if_changed(ren, clear_color, frame, out, &nout);
    out[nout++] = frame;
    return nout;
}

extern "C" void SDL_RenderPresent(SDL_Renderer *ren)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    g_SDL2CirclePresents++;

    // A short enough frame crosses as a LIST and is composed on the far
    // side; anything else crosses as a PICTURE, below. How short is short
    // enough was fixed when the library was built
    // (SDL2CIRCLE_PRESENT_MAX_CMDS, described in sdl2circle.h), and at the
    // default of zero nothing but an empty frame takes this path.
    //
    // The grant does not enter into it. Both endings reach the same
    // executor, which writes into the shadow or the staging frame according
    // to what was granted, and the flip is the grant's business either way.
    //
    // A frame the recorder gave up on has already been drawn into the canvas
    // surface and has no list left to send, whatever the count.
    if (!ren->rasterizing && ren->ncmds <= (unsigned)SDL2CIRCLE_PRESENT_MAX_CMDS)
    {
        // Composed command by command on the far side. The whole recorded
        // list travels, because composing IS what this path does — in canvas
        // coordinates exactly as it was recorded, because placing it on the
        // panel is the far side's job and not this one's.
        if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
        {
            SDL2Circle_PresentPost(ren->cmds, ren->ncmds, ren->back);
            ren->ncmds = 0;
            ren->have_copy = false;
            ren->rasterizing = false;
            frame_scratch_next(ren);
            if (s_fb_halves == 2)
                ren->back ^= 1;
            return;
        }
        for (unsigned i = 0; i < ren->ncmds; i++)
            SDL2Circle_VideoExecCmd(&ren->cmds[i], ren->back);
        ren->ncmds = 0;
        ren->have_copy = false;
        ren->rasterizing = false;
        frame_scratch_next(ren);
        SDL2Circle_VideoFlip(ren->back);
        if (ren->vsync)
        {
            SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT_VSYNC);
            ren->window->fb->WaitForVerticalSync();
        }
        // Only a grant of two halves has a second half to draw into. On a
        // single-half grant the half is not a target at all — the executor
        // and the flip both ignore it and work through the shadow — and
        // naming half 1 there would address memory past the grant.
        if (s_fb_halves == 2)
            ren->back ^= 1;
        return;
    }

    SDL2CirclePresentCmd out[2];
    bool drew_canvas = ren->rasterizing;
    unsigned nout = reduce_frame(ren, out);
    ren->ncmds = 0;
    ren->have_copy = false;
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
