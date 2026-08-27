//
// video.cpp - window / software renderer / streaming texture over
// Circle's CBcmFrameBuffer (double-buffered, vsync page flip).
//
// Scope matches MAME's drawsdl.cpp software path: one fullscreen window,
// an SDL_Renderer, streaming ARGB8888 textures.
//
#include <limits.h>
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

    // The mode SDL_SetWindowDisplayMode named for this window. Kept because
    // SDL_WINDOW_FULLSCREEN means "drive the display at that mode", so going
    // fullscreen has to know it - and a program may set the mode first and go
    // fullscreen after, or the other way about.
    int mode_w, mode_h;
    SDL_bool grabbed;
    char title[128];

    // A hit test tells a window manager which part of a window acts as a
    // title bar or a resize edge. There is no window manager here - one
    // window, the whole display - so the condition the callback exists to
    // answer never arises. It is kept rather than refused so that an
    // application which relies on it for a borderless drag bar still links
    // and still gets the success SDL promises; the callback itself is
    // simply never the thing that decides anything on this board.
    SDL_HitTest hit_test;
    void *hit_test_data;
};

// The coordinate state that belongs to whatever is being drawn into. SDL2
// gives every render target its own: a viewport or a logical size set while a
// texture is the target applies to that texture, and the window's own comes
// back unchanged when the target is released.
//
// It is saved and restored as one block when the target changes, so the copy
// held in the renderer is always the state of the current target and every
// drawing call reads it without asking which target it is drawing into.
struct RenderView
{
    int   logical_w, logical_h;
    bool  integer_scale;
    float scale_x, scale_y;
    SDL_Rect viewport;
    bool     viewport_set;
    SDL_Rect clip;
    bool     clip_enabled;
};

struct SDL_Renderer
{
    SDL_Window *window;
    // The framebuffer half the presentation core is to draw into next. It is
    // carried with the frame and handed to the executor, and it is the only
    // thing here that refers to the panel at all - a slot number, never its
    // geometry or its address. Nothing on this side of the library knows what
    // the panel is or where it lives.
    unsigned back;
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
    // size is then the coordinate system - the identity case, which costs
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

    // The texture drawing lands in, and null when that is the frame.
    //
    // A target's draw calls are never recorded and never cross to the
    // presentation core. They are executed into the texture as they are
    // made, because a target's pixels are not a frame going anywhere - they
    // are a texture the application may copy from on its very next call.
    SDL_Texture *target;

    // The coordinate state the window's own drawing goes back to. The block
    // above is the live state; this copy holds the window's for as long as a
    // target is set.
    RenderView window_view;

    // Draw calls held back, in canvas coordinates, while the frame is still
    // short enough to cross to the presentation core as a list. A frame that
    // outgrows the crossing count is composed into the virtual framebuffer
    // instead, and at the default count of zero every frame is.
    SDL2CirclePresentCmd cmds[SDL2CIRCLE_RECORD_MAX_CMDS];
    unsigned ncmds;

    // Set once this frame has started being drawn into the virtual
    // framebuffer. Cleared at the start of each frame.
    bool rasterizing;

    // Pixels a command needs that exist nowhere else - a mirrored copy for
    // SDL_RenderCopyEx, which the texture itself does not hold. A recorded
    // command is not executed until present, so those pixels have to outlive
    // the call that made them: they are bump-allocated here and the arena is
    // emptied when the next frame starts.
    //
    // Two of them, alternating, for the same reason the textures come in
    // pairs. Under the core split a posted frame is still being read by the
    // presentation worker after RenderPresent returns, so emptying the arena
    // the application just drew into would pull those pixels out from under
    // it. The frame in flight keeps the one it was posted with while the
    // application fills the other.
    u8    *scratch[2];
    size_t scratch_bytes[2];
    size_t scratch_used;
    u8     scratch_idx;

    // Mouse coordinates are reported in this renderer's logical space, so a
    // relative movement is divided by the same factor the position is. That
    // division has a remainder, and throwing it away would lose slow movement
    // entirely: at a scale of 2, a stream of one-pixel reports would every
    // one of them truncate to zero and the pointer would never move at all.
    // So the fraction is carried to the next event. SDL2 keeps exactly this
    // pair on the renderer, for exactly this reason.
    float rel_frac_x, rel_frac_y;

    // SDL_HINT_MOUSE_RELATIVE_SCALING, read once when the renderer is made,
    // as SDL2 reads it. It decides whether the relative movement above is
    // scaled with the position or left in window pixels.
    bool relative_scaling;
};

// The virtual framebuffer: the one framebuffer SDL has, at canvas
// resolution, and the surface every frame is composed into. It exists for as
// long as a window does - it is allocated when the window is made, and a
// window cannot be made without it - so nothing downstream has to ask whether
// SDL's framebuffer is really there.
//
// Two of them, alternating, for the same reason the textures come in
// pairs: the presentation core may still be reading the one that was
// posted while this thread starts drawing the next.
static u8 *s_canvas_surface_buf[2] = { nullptr, nullptr };
static u8 *s_canvas_surface = nullptr;
static unsigned s_canvas_surface_idx = 0;
static unsigned s_canvas_surface_pitch = 0;

// A texture is stored as ARGB8888 whatever format it was asked for, because
// the presentation path reads a texture's pixels directly - a frame that is
// one opaque copy crosses to the presentation core as that texture, in
// place, with nothing painted. Storing a texture in the application's format
// would mean converting during presentation, on the core that must not be
// delayed, every frame.
//
// The format is honoured at the edge instead. `format` is what the
// application asked for and what SDL_QueryTexture answers; pixels handed in
// through SDL_UpdateTexture are converted on the way in, and
// SDL_LockTexture hands back a staging buffer in the application's format
// which SDL_UnlockTexture converts. An application therefore writes the
// pixels it believes it is writing, and the cost falls on its own core.
//
// When the application's format is ARGB8888 - still the common case - there
// is no staging buffer and no conversion, and the path is unchanged.
struct SDL_Texture
{
    int w, h;
    Uint32 format;     // the format the application asked for
    int access;        // SDL_TEXTUREACCESS_*, as asked for
    u8 *pixels[3];     // [1] and [2] exist only under the core split: the app
                       // renders into one buffer while the presentation
                       // worker still reads the frame in flight
    u8 widx;           // buffer the app writes next

    // Which frame each store is spoken for by.
    //
    // A recorded copy hands the presentation core a raw pointer into one of
    // these stores, so the store must not be written again until that core
    // has finished reading the frame holding the pointer. busy_seq[i] is the
    // frame sequence of the copy that last referenced store i; 0 means never.
    //
    // Tracking the buffer that was last posted is not the same question: it
    // says where the writer went, not what the reader still holds.
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
    // colour modulation is not applied when drawing - see
    // SDL_SetTextureColorMod - and the scale mode has no effect because the
    // blitters are nearest-neighbour throughout.
    Uint8 colormod_r, colormod_g, colormod_b;
    SDL_ScaleMode scale_mode;
    void *userdata;

    // Only for a texture created with SDL_TEXTUREACCESS_TARGET: the
    // coordinate state to use while this texture is the render target. It
    // starts at the whole texture, unscaled and unclipped, and keeps
    // whatever the application sets between the times it is the target.
    RenderView view;
};

// The size of what is being drawn into: the render target where one is set,
// the canvas otherwise -- not necessarily the window's own reported size; see
// the geometry comment below the canvas variables, where these are defined.
// Every rectangle an application hands over is placed against it, so a call
// cannot be found honouring one and a call beside it honouring the other.
static inline int render_target_w(const SDL_Renderer *ren);
static inline int render_target_h(const SDL_Renderer *ren);

// The one fullscreen window (ID 1). Display-mode queries answer with the
// canvas, not this - see the geometry comment below - because the two are
// not always the same rectangle.
static SDL_Window *s_window = nullptr;

// Every piece of geometry below belongs to exactly one of the resolutions
// defined here.
//
//   scanout   the physical display: what the hardware really puts on the
//             wire. cmdline.txt width=/height= asks the firmware for a mode,
//             the allocation is what sets it, and the firmware then reports
//             the mode it actually set - that report is the scanout, read
//             from the firmware and never calculated. Not from the pitch,
//             not from the buffer size, and not from the width and height
//             Circle echoes back out of its own constructor.
//
//             It belongs to the presentation path: the placement below and
//             the present executor are its only readers, and nothing in SDL
//             is ever answered with it.
//
//   canvas    the virtual display: the world the application is given, the
//             shape that decides the letterboxing, and the shape every
//             render target draws into that is not a texture of its own (see
//             render_target_w/h below). Settled once, at the first of four
//             moments, in this order:
//
//               1. --rapi-vfb=WxH (src/bootargs.cpp). A boot switch, so
//                  it wins over anything the application does.
//               2. SDL2Circle_DeclareVirtualDevice, called by the consumer
//                  before SDL_Init.
//               3. The first SDL_CreateWindow's own width and height. A
//                  window is the canvas here, so with neither override this
//                  is where the two become the same rectangle.
//               4. The physical panel size, read from the firmware. Reached
//                  only when none of the above named a size, so the library
//                  never refuses to start for want of one.
//
//             Once any of the four has settled it, the canvas is fixed for
//             the rest of the run - the placement, the window, the
//             display-mode answers all derive from it from that point on.
//
//   application  what SDL_CreateWindow was asked for, reported back exactly
//             as asked (SDL_GetWindowSize). It is the canvas by construction
//             under (2) and (3) above, but under (1) the switch may give the
//             canvas a different shape - a consumer that named a size still
//             gets a window of that size, honoured as an answer to the
//             question it asked, but draws into a canvas of the switch's
//             shape. render_target_w/h is what settles which shape a given
//             piece of drawing lands in, and it is always the canvas.
//
// The physical and the virtual are two numbers doing two jobs. Neither
// overrides the other and there is no order of precedence between them: one
// is asked of the firmware, the other is settled as above, and the whole job
// of this file is to scale the second onto the first. The frame travels
// application -> canvas -> scanout, and the two hops are composed into a
// single resampling pass at present time.
static int s_scanout_w = 0, s_scanout_h = 0;
static int s_canvas_w = 0, s_canvas_h = 0;

// The virtual display this machine gave the application, recorded once and
// never written again.
//
// SDL keeps two display sizes apart on every platform. The DESKTOP mode is
// the display an application was given, and a fullscreen program changing
// mode does not move it - a monitor does not shrink because a game asked for
// 640x400 on it. The CURRENT mode is the mode in effect, and that does move.
// Applications are written against the difference: read the desktop to find
// out how much room there is, set a mode inside it, read the desktop again
// later and expect the same answer.
//
// Here the display an application is given is the vFB, so the vFB is its
// desktop. It is settled once, by settle_canvas, from the --rapi-vfb switch,
// SDL2Circle_DeclareVirtualDevice, the first window's own size or the panel,
// and that decision is the machine's statement of what this program gets.
//
// s_canvas_w/h used to serve both roles, which was correct while a canvas
// could not move. Now that it follows the application, an application that
// set a small mode was afterwards told its desktop was that small, and could
// never ask for anything larger for the rest of the run - a one-way ratchet
// with no way back, in a library whose job is that any SDL program works.
// ScummVM found it by leaving Myst: Myst set 544x332, and the launcher's own
// 640x400 was then clipped to a screen that had shrunk underneath it.
//
// This is not the scanout and never becomes it except on the rung where
// nothing else named a size at all. An application still never learns the
// panel's resolution from the desktop, so the canvas stays small, C1 pays
// only for what it drew, and C2 does the upscaling.
static int s_vfb_w = 0, s_vfb_h = 0;

// The one path every size change takes: window creation against a canvas that
// already exists, SDL_SetWindowSize, SDL_SetWindowDisplayMode, and going
// exclusive-fullscreen at a mode. Every one of those is the same request -
// the application wants to draw at a different size - so every one of them
// arrives here, and the handover with the presentation core is paid once,
// where it can be seen, rather than at each call site.
static void canvas_set_size(SDL_Window *win, int w, int h);

// render_target_w/h (declared above, by the struct they read): every
// rectangle an application hands over is placed against the canvas, whatever
// the window's own reported size is under the switch - see the geometry
// comment above. A render target already answers with its own texture, which
// this never touches.
static inline int render_target_w(const SDL_Renderer *ren)
{
    return ren->target ? ren->target->w : s_canvas_w;
}

static inline int render_target_h(const SDL_Renderer *ren)
{
    return ren->target ? ren->target->h : s_canvas_h;
}

// The --rapi-vfb=WxH switch (src/bootargs.cpp), read from the boot
// argument block before SDL_Init runs and before any window can exist. Top
// of the canvas precedence: once set, nothing below can out-rank it.
static bool s_switch_set = false;
static int s_switch_w = 0, s_switch_h = 0;

void SDL2Circle_SetVfbSwitch(int width, int height)
{
    s_switch_w = width;
    s_switch_h = height;
    s_switch_set = true;
}

// The virtual display device, as declared by the consumer through
// SDL2Circle_DeclareVirtualDevice. It states the canvas and nothing else:
// the physical mode remains what the firmware was asked for and what the
// firmware granted, neither of which this declaration touches.
//
// A resolved canvas (s_canvas_w > 0) is the point after which no declaration
// can be taken: everything downstream - the placement, the window, the
// display-mode answers - has been derived from the canvas by then, and the
// declaration promises a display whose size does not change under the
// application. This is settled before or at the first window, on one core,
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

// The geometry the grant was made at, taken from the framebuffer object at
// the moment it is created and held here.
//
// Held rather than asked for again, because asking means calling into a
// Circle device object and the callers that want this answer - the display
// mode and display bounds queries - run on the application core. It never
// changes: the grant is made once and kept for the life of the program.
static int s_grant_w = 0, s_grant_h = 0;

// Ask the firmware what the physical display is. Core 0 only - it is a
// mailbox transaction - and only meaningful after the allocation, because
// the allocation is what sets the mode and this reads back what was set.
//
// Circle cannot be asked this, which is why the question is put again here.
// CBcmFrameBuffer::Initialize sends one combined tag call and the firmware
// writes its real answer back into those same tag structures - Circle relies
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
    {
        SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_WARNING,
                       "the firmware refused PROPTAG_GET_DISPLAY_DIMENSIONS");
        return;
    }
    if (dim.nWidth == 0 || dim.nHeight == 0)
    {
        SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_WARNING,
                       "the firmware reported a %ux%u display",
                       dim.nWidth, dim.nHeight);
        return;
    }
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

    // TWO SCREENS ARE ASKED FOR, ONE IS ENOUGH.
    //
    // Double buffering is what makes a present a page flip, so it is worth
    // asking for. A firmware that will not allocate two screens refuses the
    // whole allocation rather than granting one, and that refusal used to end
    // here with no display at all - on a Pi 5, which is exactly the board the
    // present path already expects to grant a single screen to.
    //
    // So a refusal is retried with one screen. Everything downstream reads the
    // grant rather than the request (see the row count in the present path),
    // so a single-buffered grant needs nothing else told to it: the frame is
    // copied into the granted surface instead of panned to.
    CBcmFrameBuffer *fb =
        new CBcmFrameBuffer(a->w, a->h, 32, 0, 0, 0, TRUE /*double buffered*/);
    if (!fb->Initialize())
    {
        delete fb;
        SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                       "the firmware refused two %dx%d 32bpp screens; "
                       "asking for one", a->w, a->h);

        fb = new CBcmFrameBuffer(a->w, a->h, 32, 0, 0, 0, FALSE /*single*/);
        if (!fb->Initialize())
        {
            SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_ERROR,
                           "the firmware refused a %dx%d 32bpp framebuffer "
                           "(0x0 asks for the panel's own mode)", a->w, a->h);
            delete fb;
            return;
        }
    }
    s_fb0 = fb;
    s_grant_w = (int)fb->GetWidth();
    s_grant_h = (int)fb->GetHeight();
    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                   "framebuffer granted: asked %dx%d, got %ux%u",
                   a->w, a->h, fb->GetWidth(), fb->GetHeight());

    // Read the mode back on this same trip to core 0, now that setting it
    // has happened.
    read_physical_display();
}

// Place the canvas on the scanout. Fit - aspect preserved, centered, the
// remainder left black - is the default; cmdline.txt `canvas=stretch` asks
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
        // Fit, in exact integer arithmetic. This runs once, at startup, on
        // core 0, before a single frame exists, so it can afford to be slow:
        // it is written for obvious correctness and nothing else. Do not
        // fold the steps together to save a divide, and do not hold the
        // scale factor as a fixed-point fraction - a ratio such as 2.4 has
        // no exact fixed-point representation, and rounding it misplaces the
        // picture by a pixel on the affected edge.
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
            // Width limits - including when the two are equal, which is the
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
//
// It is entirely optional, and stands or falls independently of the
// --rapi-vfb switch: both may be set, and settle_canvas below is the one
// place that decides which of them, or the window, actually becomes the
// canvas.
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

// Acquire the framebuffer and settle the scanout it was granted on, without
// touching the canvas.
//
// Kept apart from the display-size resolve below because the screen log
// destination (src/console.cpp) needs exactly this and nothing more: it comes
// up during the host kernel's initialisation, long before an application has
// declared a canvas or asked for a window. One routine so the two can never
// ask the firmware for different things - the request below is what sets the
// display mode, and a second, different request would be a second mode.
//
// Returns where the scanout figure came from, or null when there is no
// display to describe. Idempotent: the grant is made once and kept.
static const char *acquire_scanout(void)
{
    // The boot options' width=/height= is a request to the firmware for a
    // physical display mode, and that is the whole of what it is. It is not
    // a canvas source and is never read as one.
    //
    // Unset, the request is zero by zero, which is Circle's "no size
    // requested": its CBcmFrameBuffer constructor then asks the firmware for
    // the display's own dimensions and allocates that. Naming a size here
    // instead would set that mode - the allocation is what sets it - so a
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
    if (s_phys_w > 0 && s_phys_h > 0)
    {
        s_scanout_w = s_phys_w;
        s_scanout_h = s_phys_h;
        return "firmware reported";
    }

    if (a.w > 0 && a.h > 0)
    {
        // The firmware would not say, but a mode was named on the command
        // line, so that is the only figure left. It is the one case where
        // the scanout is not a measured one, and the log says so rather than
        // presenting it as an answer.
        s_scanout_w = a.w;
        s_scanout_h = a.h;
        return "firmware silent, physical request";
    }

    if (s_grant_w > 0 && s_grant_h > 0)
    {
        // Nothing named and the firmware silent to us. Circle asked the same
        // question in its constructor and allocated against whatever it got,
        // so its width and height are the geometry the grant was actually
        // made at. They are only an echo of a request when a request was
        // made, and none was. Read off the object on core 0 when the grant
        // was made, and held since.
        s_scanout_w = s_grant_w;
        s_scanout_h = s_grant_h;
        return "firmware silent, grant geometry";
    }

    // No grant and no answer: there is no display to describe.
    return nullptr;
}

// The grant, for anything in this library that draws on the screen itself.
// The screen log destination is the one such consumer; see sdl2circle.h.
bool SDL2Circle_ScanoutAcquire(SDL2CircleScanout *out)
{
    if (!out)
        return false;
    if (!acquire_scanout() || !s_fb0)
        return false;

    // Pitch, size and base address are the firmware's reply to the
    // allocation; Circle keeps all three unchanged. Width and height are the
    // scanout the routine above settled, which is the firmware's report of
    // the display and not the request made of it.
    out->base   = (u8 *)(uintptr)s_fb0->GetBuffer();
    out->pitch  = s_fb0->GetPitch();
    out->bytes  = s_fb0->GetSize();
    out->width  = s_scanout_w;
    out->height = s_scanout_h;
    return true;
}

// Decide the canvas size, in precedence order: the --rapi-vfb switch,
// then SDL2Circle_DeclareVirtualDevice, then the caller's own size, then the
// physical panel size read from the firmware - which only a window has, so a
// display query arriving before any of the first three exist is passed 0,0
// and falls through to the firmware. Nothing is written to s_canvas_w/h
// here: the caller commits them only once the scanout is known too, so a
// failed resolve leaves nothing partially set.
static bool settle_canvas(int fallback_w, int fallback_h,
                          int *out_w, int *out_h, const char **out_how)
{
    if (s_switch_set)
    {
        *out_w = s_switch_w;
        *out_h = s_switch_h;
        *out_how = "--rapi-vfb switch";
        return true;
    }
    if (s_declared)
    {
        *out_w = s_declared_w;
        *out_h = s_declared_h;
        *out_how = "declared virtual device";
        return true;
    }
    if (fallback_w > 0 && fallback_h > 0)
    {
        *out_w = fallback_w;
        *out_h = fallback_h;
        *out_how = "first window created";
        return true;
    }

    // The first three rungs all found nothing: no switch, no declaration, no
    // window with a usable size. Last resort, ask the firmware what the
    // panel actually is and use that as the canvas, so a Pascal consumer -
    // which cannot reach Circle's property tags itself - still starts.
    // acquire_scanout() is the same routine, and the same cached answer,
    // that settles the scanout below; it performs the mailbox read once and
    // every further call, this one included, reuses what it already has.
    acquire_scanout();
    if (s_phys_w > 0 && s_phys_h > 0)
    {
        *out_w = s_phys_w;
        *out_h = s_phys_h;
        *out_how = "physical panel size";
        return true;
    }

    if (fallback_w != 0 || fallback_h != 0)
        SDL_SetError("the window has no usable size (%dx%d)",
                     fallback_w, fallback_h);
    else
        SDL_SetError("the display size is not yet known");
    return false;
}

// Settle the canvas, the scanout and the placement between them. True once
// the canvas exists.
//
// fallback_w/h are the window's own size, offered only by SDL_CreateWindow
// (create_window_on0, below) - every other caller passes 0,0, so a display
// query that arrives before the canvas has a source of its own is refused
// rather than guessing at one.
static bool resolve_display_size(int fallback_w = 0, int fallback_h = 0)
{
    if (s_canvas_w > 0 && s_canvas_h > 0)
        return true;

    int w = 0, h = 0;
    const char *how = nullptr;
    if (!settle_canvas(fallback_w, fallback_h, &w, &h, &how))
        return false;

    const char *source = acquire_scanout();
    if (!source)
    {
        SDL_SetError("the display size cannot be determined");
        return false;
    }

    s_canvas_w = w;
    s_canvas_h = h;

    // The settled size is also the desktop, and this is the only place it is
    // written. canvas_set_size moves the canvas afterwards and never this.
    s_vfb_w = w;
    s_vfb_h = h;

    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                          "scanout %dx%d (%s), canvas %dx%d (%s)",
                          s_scanout_w, s_scanout_h, source,
                          s_canvas_w, s_canvas_h, how);

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
// two halves: present commands render into a shadow back buffer instead,
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
// has no such engine - and no shadow path either, because its firmware
// grants the two halves a page flip needs, so it never arrives here.
#if RASPPI >= 4
#define SHADOW_DMA_CHANNEL DMA_CHANNEL_EXTENDED
#else
#define SHADOW_DMA_CHANNEL DMA_CHANNEL_NORMAL
#endif

// Beats per bus transaction, where a beat is 128 bits. Circle's screen DMA
// uses 2, sized for small, frequent console-scroll moves sharing the bus
// with everything else, where latency matters and total time does not; this
// is the opposite job, one bulk move of a whole screen, once a frame, which
// has to finish inside the frame.
//
// At 2 beats (32 bytes a transaction) a screen took about 14.5 ms, around
// 450 MB/s - a transaction-rate limit, not a memory-bandwidth one. 8 beats
// is 128 bytes a transaction (two cache lines) and still well under the 15
// the controller allows, leaving room to go further if ever needed.
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
// one store at a time - measured on a Pi 4 at 26.1 ms for a 1280x720 frame
// against 1.4 ms into ordinary memory. Composed here and blitted out in whole
// rows, the same frame costs 1.4 ms plus a 6.0 ms block move: the write to
// uncached memory is made once, in the shape that memory is good at.
//
// It is also what keeps the picture whole. A half is only ever written by
// this blit, so the raster can never catch a frame mid-composition.
static u8 *s_stage = nullptr;
static unsigned s_stage_pitch = 0;
static size_t s_stage_bytes = 0;

// Whether the present path - the shadow buffers and the DMA channel, or the
// staging frame - has been built.
//
// It is sized and shaped by the framebuffer grant, and that grant is made
// once and never returned (see s_fb0), so nothing it depends on can change
// while the machine runs: a second window adopts the same grant, the same
// scanout geometry and the same present resources. It therefore belongs to
// the grant's lifetime and not to a window's, and window teardown leaves it
// alone.
//
// The alternative - rebuild it per window - strands what the previous one
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
// Only the presentation owner ever runs a command - the worker core under
// the core split, the calling core without it - so a single set of tables
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
//
// dst_alpha says whether the destination's own alpha is worth composing.
// It is false for every call that lands on the frame - a panel has nothing
// behind its pixels, so the byte is never read back and is not spent on.
// It is true only for a copy into a render target, where that byte is the
// texture's own alpha and something may read it next: SDL_RenderReadPixels,
// or a further blended copy that uses this texture as its destination.
static void scale_copy(const SDL2CirclePresentCmd *cmd, u8 *dst, unsigned dpitch,
                       int sw, int sh, bool dst_alpha)
{
    const int dw = cmd->w, dh = cmd->h;
    if (dw > SCALE_MAP_MAX || dh > SCALE_MAP_MAX)
        return;                          // beyond any real scanout
    build_scale_maps(sw, sh, dw, dh);

    // An integer horizontal ratio replicates each source pixel a fixed
    // number of times, which needs no table lookup at all - the common
    // case for an emulator raster lifted onto a panel.
    const int xrep = (dw % sw == 0) ? dw / sw : 0;

    if (!cmd->blend && cmd->alphamod == 255)
    {
        // Every destination row is resampled from the source; no row is
        // ever copied from another one.
        //
        // Under vertical magnification several destination rows share a
        // source row, so a copy-back of the first destination row - cheaper
        // by index arithmetic alone, since the source row is small and the
        // destination row is the magnified one - looks like the right
        // shortcut. It is not: the destination is a whole-screen shadow, and
        // reading those rows back evicts the small source row from cache
        // that every other destination row mapping to it still needs. On a
        // Pi 5, 398x224 scaled into a 796x448 canvas on a 1920x1080 panel
        // measured the presentation core awake 76.4% of the time with that
        // copy and 33-41% without it. Do not reintroduce it.
        u8 *drow = dst;
        for (int j = 0; j < dh; j++, drow += dpitch)
        {
            const u32 *s = (const u32 *)(cmd->src
                                         + (size_t)s_ymap[j] * cmd->srcpitch);
            u32 *d = (u32 *)drow;
            if (xrep)
            {
                // Integer horizontal ratio: the source is read once and the
                // destination only written. Nothing is read back, so the
                // cache problem above does not apply here.
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
    // reused - every destination pixel is composited in place.
    //
    // Which alpha byte is written is decided once here, outside both loops,
    // not per pixel: the frame path (dst_alpha false) keeps the exact loop
    // this always ran, spending nothing extra; the target path composes the
    // destination alpha SDL_BLENDMODE_BLEND defines,
    // dstA = srcA + dstA * (1 - srcA), with the same 8-bit approximation
    // already used above for the colour channels.
    if (dst_alpha)
    {
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
                    unsigned da = a + (((dp >> 24) * (255 - a)) >> 8);
                    d[i] = (da << 24) | rb | g;
                }
            }
        }
        return;
    }

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
// rasterized there; emit_cmd uses it on a render target's own pixels.
//
// dst_alpha marks that last case. It defaults false, so every frame-path
// call site is unchanged, and is passed true only for a render target,
// where a blended copy has to compose the destination alpha it is writing
// over rather than discard it - see scale_copy just above for the reason
// and the arithmetic.
static void exec_into(const SDL2CirclePresentCmd *cmd, u8 *dst0, unsigned dpitch,
                      bool dst_alpha = false)
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

    // The destination extent already carries both geometry hops, so one
    // pass here covers application frame -> canvas -> scanout. Equal
    // extents are the unscaled blit below, unchanged to the byte.
    const int sw = cmd->sw > 0 ? cmd->sw : cmd->w;
    const int sh = cmd->sh > 0 ? cmd->sh : cmd->h;
    if (sw != cmd->w || sh != cmd->h)
    {
        scale_copy(cmd, dst, dpitch, sw, sh, dst_alpha);
        return;
    }

    // Unblended, at full alphamod: the source pixel replaces the
    // destination outright, its own alpha byte included, so this already
    // carries a straight-alpha source's transparency through untouched -
    // the frame path and the target path want exactly the same bytes here
    // and neither reads dst_alpha.
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

    // Blended, 1:1. Same split as scale_copy's blended loop and for the same
    // reason: chosen once here, not per pixel, so the frame path's cost does
    // not move at all.
    if (dst_alpha)
    {
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
                    unsigned da = a + (((dp >> 24) * (255 - a)) >> 8);
                    d[x] = (da << 24) | rb | g;
                }
            }
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
// to the glass. The executor below is the only caller, and that is the whole
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

    // A fill of the whole canvas is a clear of the whole display, border
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

// Execute one command onto the glass. The command arrives in canvas
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

    // The pointer is not part of the picture's geometry chain and is kept out
    // of the line that describes it: it has a chain of its own, and the memo
    // in log_copy_geometry holds one, so the two would alternate and put a
    // line on the console every frame.
    if (placed.op == SDL2CirclePresentCmd::COPY && !placed.pointer)
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
                                          "present: dma transfer error");
                }
            }

            {
                SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT_VSYNC);
                s_window->fb->WaitForVerticalSync();
            }

            // The scaler wrote the shadow through the cache. Clean that
            // range - clean, not invalidate, so the lines stay warm for the
            // next frame - or the engine reads stale memory behind it.
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
    // the only writer of a half - so nothing half-composed is ever scanned.
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
    // granted offset back) silently breaks the page flip - the visible
    // half then only ever receives alternate frames.
    static bool s_flip_logged = false;
    if (!s_flip_logged)
    {
        s_flip_logged = true;
        SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                              "first flip to half %u: SetVirtualOffset %s",
                              half, ok ? "ok" : "refused");
    }
}

// Allocate the virtual framebuffer. Called once, from window creation, and
// a window that cannot have one is not created: everything below this point
// composes into it, so a missing one is not a condition to carry around.
static bool canvas_surface_alloc(void)
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
                   "virtual framebuffer %dx%d, double buffered, %u bytes",
                   s_canvas_w, s_canvas_h,
                   (unsigned)(2 * (size_t)s_canvas_surface_pitch * s_canvas_h));
    return true;
}

// ---- the pointer -----------------------------------------------------------
//
// The mouse pointer is composed onto a frame at present, as one more command
// laid over the picture in canvas coordinates. It goes through the same
// executor as everything else, so it is placed onto the scanout by the same
// arithmetic and lands correctly at every canvas size and every placement,
// letterbox included.
//
// It is not drawn into the canvas surface. That surface is the application's
// frame, and the pointer is not part of what the application drew: painting
// it in would leave it behind in a buffer the application draws into again
// two frames later.
//
// Its pixels are copied here first, into a buffer of this library's own,
// because the cursor object belongs to the application. SDL_FreeCursor may
// release it and SDL_SetCursor may point somewhere else the moment present
// returns, while a presentation core is still reading the frame - and
// CORE-SPLIT.md's rule is that no memory the other side may still write is
// ever handed across. Two buffers, alternating, exactly as the canvas surface
// has two: the copy the presentation core is reading is never the one the
// next frame's pointer is written into.
static u8     *s_cursor_stage[2] = { nullptr, nullptr };
static size_t  s_cursor_stage_bytes[2] = { 0, 0 };
static unsigned s_cursor_stage_idx = 0;

// The frame each staging buffer was last handed over with, so the one about
// to be written can be waited on. This is the same acknowledgement a texture
// store waits for and it means the same thing: the presentation core has
// finished reading that frame's pixels. Two frames have normally passed by
// the time a buffer comes round again, so the wait costs a load and a
// compare; it is here because "normally" is not a guarantee.
static u64 s_cursor_stage_seq[2] = { 0, 0 };

// Build the command that draws the pointer, or answer that there is none.
// Runs on the application's own core, from present, which is the only core
// that may look at a cursor object at all.
static bool cursor_command(SDL2CirclePresentCmd *cmd)
{
    if (s_canvas_w <= 0 || s_canvas_h <= 0)
        return false;

    SDL2CircleCursorImage img;
    if (!SDL2Circle_CursorImage(&img))
        return false;

    // Clip against the canvas, moving the source origin with the destination.
    // A pointer at the right or bottom edge is half off the screen, and a hot
    // spot inside the image puts it off the top or the left just as easily.
    int sx = 0, sy = 0;
    int dx = img.x, dy = img.y;
    int w = img.w, h = img.h;

    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > s_canvas_w) w = s_canvas_w - dx;
    if (dy + h > s_canvas_h) h = s_canvas_h - dy;
    if (w <= 0 || h <= 0)
        return false;

    const size_t pitch = (size_t)w * 4;
    const size_t need  = pitch * (size_t)h;
    const unsigned i   = s_cursor_stage_idx;

    SDL2Circle_PresentWaitAck(s_cursor_stage_seq[i]);

    if (s_cursor_stage_bytes[i] < need)
    {
        u8 *grown = (u8 *)realloc(s_cursor_stage[i], need);
        if (!grown)
            return false;
        s_cursor_stage[i] = grown;
        s_cursor_stage_bytes[i] = need;
    }

    const u8 *src = img.pixels + (size_t)sy * img.pitch + (size_t)sx * 4;
    u8 *dst = s_cursor_stage[i];
    for (int row = 0; row < h; row++, src += img.pitch, dst += pitch)
        memcpy(dst, src, pitch);

    memset(cmd, 0, sizeof(*cmd));
    cmd->op = SDL2CirclePresentCmd::COPY;
    cmd->dx = dx;
    cmd->dy = dy;
    cmd->w = w;
    cmd->h = h;
    cmd->src = s_cursor_stage[i];
    cmd->srcpitch = (int)pitch;
    cmd->sw = w;
    cmd->sh = h;
    cmd->blend = 1;            // the cursor's own alpha is its shape
    cmd->alphamod = 255;
    cmd->pointer = 1;

    // The frame this buffer is about to be part of, and the other buffer for
    // the frame after it.
    //
    // Only a frame that is going to cross has a sequence to wait for. A
    // present that executes in band - no split, or the caller is core 0 -
    // reads this buffer before it returns and posts nothing, so naming a
    // sequence there would leave the next pass waiting on a frame that is
    // never sent.
    const bool crossing =
        SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0;
    s_cursor_stage_seq[i] = crossing ? SDL2Circle_PresentPostedSeq() + 1 : 0;
    s_cursor_stage_idx ^= 1;
    return true;
}

// The store of a texture the application may write, declared here because
// drawing into a render target needs it and is defined long before it. Its
// own definition carries the rule it enforces.
static u8 *texture_write_buffer(SDL_Texture *tex, bool preserve);

// Stop recording and start drawing. Everything recorded so far goes into
// the virtual framebuffer, and everything after it goes straight there.
static void start_rasterizing(SDL_Renderer *ren)
{
    if (ren->rasterizing)
        return;
    for (unsigned i = 0; i < ren->ncmds; i++)
        exec_into(&ren->cmds[i], s_canvas_surface, s_canvas_surface_pitch);
    ren->ncmds = 0;
    ren->rasterizing = true;
}

// Record a draw call, or draw it.
//
// A frame is only ever held back for one reason: it may still be short
// enough to cross as a list, and then the presentation core composes it and
// this core paints nothing. The moment it is too long for that, everything
// held is replayed into the virtual framebuffer and this command and every
// later one go straight in.
//
// At the default crossing count of zero nothing is ever held: the first draw
// call of the frame starts the painting.
//
// There is no third possibility. A frame is never handed over as the
// application's own texture, however simple its shape - that pointer belongs
// to the application, which is free to destroy the texture or draw the next
// frame into it the moment present returns, while the presentation core is
// still reading it.
static void emit_cmd(SDL_Renderer *ren, const SDL2CirclePresentCmd &cmd)
{
    if (cmd.w <= 0 || cmd.h <= 0)
        return;

    // A render target is a fourth destination, and the only one that is not
    // the frame. The command is executed into the texture's pixels here and
    // now, at the texture's own pitch, by the same executor that composes the
    // virtual framebuffer - a target is that machinery aimed somewhere else.
    //
    // Nothing about it may be held back. The recording path exists so that a
    // short frame can cross to the presentation core as a list, and a target
    // is not going to the presentation core at all: the application may copy
    // from it on its very next call, so the pixels have to be there.
    //
    // The store is asked for on every command rather than remembered, because
    // the answer can change under a frame in flight - see
    // texture_write_buffer. When it does change, the content is carried
    // across with it, so a target keeps what was drawn into it before.
    if (ren->target)
    {
        exec_into(&cmd, texture_write_buffer(ren->target, true),
                  (unsigned)ren->target->pitch, /* dst_alpha */ true);
        return;
    }

    if (!ren->rasterizing
        && ren->ncmds < (unsigned)SDL2CIRCLE_PRESENT_MAX_CMDS)
    {
        ren->cmds[ren->ncmds++] = cmd;
        return;
    }

    start_rasterizing(ren);
    exec_into(&cmd, s_canvas_surface, s_canvas_surface_pitch);
}

// Answer one display-mode query. False when there is no display to describe:
// no --rapi-vfb switch, no SDL2Circle_DeclareVirtualDevice and no window
// yet, or a board with no scanout to read. Zeroed first, so a consumer that
// also ignores this return reads an obviously empty mode rather than whatever
// its stack held.
// desktop says which of SDL's two display sizes is being asked for. The
// desktop mode is the vFB, settled once and never moved; the current mode is
// the canvas, which is the mode actually in effect. See s_vfb_w/h above for
// why they are not the same variable any more.
static bool fill_mode(SDL_DisplayMode *mode, bool desktop)
{
    memset(mode, 0, sizeof(*mode));
    if (!resolve_display_size())
        return false;
    mode->format = SDL_PIXELFORMAT_ARGB8888;
    mode->w = desktop ? s_vfb_w : s_canvas_w;
    mode->h = desktop ? s_vfb_h : s_canvas_h;
    mode->refresh_rate = DEFAULT_HZ;
    return true;
}

// The sizes this display can be asked for.
//
// The canvas is memory, not hardware: any size can be allocated and the
// presentation core scales it onto the panel. So the only thing that cannot
// be served is a size larger than the panel, because this library scales up
// and never down - the picture would not fit on the glass.
//
// That is what an application is told. The list is the standard sizes that
// fit inside the panel, largest first as SDL orders them, and a request for
// anything else that fits is answered with itself rather than refused. An
// application that enumerates finds what it is looking for; one that asks
// for a particular size is not turned away from a size this library would
// have honoured had it simply set it.
//
// Reporting one mode - the canvas - is what this did before the canvas
// followed the application, and it made every consumer name a size in its
// own kernel just to put that size in the list.
struct ModeSize { int w, h; };

static const ModeSize s_modes[] =
{
    { 1920, 1080 }, { 1600, 1200 }, { 1440, 1080 }, { 1400, 1050 },
    { 1366,  768 }, { 1280, 1024 }, { 1280,  960 }, { 1280,  800 },
    { 1280,  720 }, { 1152,  864 }, { 1024,  768 }, {  960,  720 },
    {  856,  480 }, {  800,  600 }, {  720,  576 }, {  720,  480 },
    {  720,  400 }, {  640,  480 }, {  640,  400 }, {  640,  350 },
    {  512,  384 }, {  400,  300 }, {  360,  240 }, {  320,  240 },
    {  320,  200 }, {  256,  240 }, {  256,  192 }, {  160,  120 },
};

// Is this size the panel's own?
static inline bool is_scanout(int w, int h)
{
    return w == s_scanout_w && h == s_scanout_h;
}

// Does the table already carry the panel's own mode?
static bool scanout_in_table(void)
{
    for (const ModeSize &m : s_modes)
        if (is_scanout(m.w, m.h))
            return true;
    return false;
}

// How many modes this panel has. The scanout has to be known first; before it
// is, there is nothing to measure against and the answer is none.
//
// The panel's own mode is always index 0, whether or not the table names it.
// The table is a list of sizes that were standard on desktop hardware, and a
// panel that is not one of them - 800x480 is the common case, and there are
// plenty of others on small displays - would otherwise never be offered its
// own resolution at all: on 800x480 the widest entry that fits is 720x480,
// because 856x480 is too wide and 800x600 too tall. An application choosing
// the top of the list would then pick a mode that has to be scaled, when the
// one size needing no scaling at all was the panel's and was never shown.
//
// Nothing above the panel is ever listed. This library scales a canvas up
// onto the glass, so a larger canvas is being scaled the other way, and
// nearest neighbour discards source pixels rather than resampling them.
static int modes_available(void)
{
    if (!acquire_scanout() || s_scanout_w <= 0 || s_scanout_h <= 0)
        return 0;

    int n = 1;                        // index 0 is always the panel
    for (const ModeSize &m : s_modes)
        if (m.w <= s_scanout_w && m.h <= s_scanout_h && !is_scanout(m.w, m.h))
            n++;
    return n;
}

// ---- display information ---------------------------------------------------

// ---- metering the answers ---------------------------------------------------
//
// The setters each put a line on the log, so what an application asks the
// display to become is visible. What it is TOLD, and therefore why it asked
// for that, was not - and a program that sizes itself from an answer is doing
// the arithmetic somewhere this library cannot see.
//
// A getter is called many times a frame, so a line per call would fill the
// console and push out everything competing with it (LOGGING.md). Each site
// remembers what it last answered and writes only when the answer changes,
// which is the rule log_copy_geometry already follows for the present path.
// The statics are per call site and are read and written on one core.
static bool answer_moved(int &was_a, int &was_b, int a, int b)
{
    if (was_a == a && was_b == b)
        return false;
    was_a = a;
    was_b = b;
    return true;
}

#define METER(tag, a, b, fmt, ...)                                            \
    do {                                                                      \
        static int was_a__ = INT_MIN, was_b__ = INT_MIN;                      \
        if (answer_moved(was_a__, was_b__, (a), (b)))                         \
            SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_DEBUG, fmt,            \
                           ##__VA_ARGS__);                                    \
    } while (0)

extern "C" int SDL_GetNumVideoDisplays(void) { return 1; }

extern "C" const char *SDL_GetDisplayName(int) { return "HDMI0"; }

extern "C" int SDL_GetDisplayBounds(int, SDL_Rect *rect)
{
    rect->x = 0;
    rect->y = 0;
    rect->w = 0;
    rect->h = 0;
    if (!resolve_display_size())
    {
        METER("bounds", -1, -1, "GetDisplayBounds -> refused");
        return -1;
    }
    // The display's own rectangle in desktop coordinates, which is SDL's
    // other spelling of the desktop mode - so it is the vFB, not the canvas.
    // A program reads this to find out how much room it has before choosing a
    // size, and answering with the canvas told it the room was whatever it
    // had last asked for.
    rect->w = s_vfb_w;
    rect->h = s_vfb_h;
    METER("bounds", rect->w, rect->h,
          "GetDisplayBounds -> %dx%d (canvas %dx%d)",
          rect->w, rect->h, s_canvas_w, s_canvas_h);
    return 0;
}

// The usable area is the display minus whatever the system keeps for itself:
// on a desktop, a menu bar, a dock or a taskbar. There is no window manager
// here and nothing reserves any part of the panel, so the usable bounds are
// the display's bounds - the same answer SDL gives on a desktop with nothing
// docked, rather than a refusal every caller has to work around.
//
// SDL documents `rect' as optional for this call, unlike SDL_GetDisplayBounds,
// so a caller that only wants the return value is answered rather than
// dereferenced.
extern "C" int SDL_GetDisplayUsableBounds(int displayIndex, SDL_Rect *rect)
{
    SDL_Rect ignored;
    if (!rect)
        rect = &ignored;

    if (SDL_GetDisplayBounds(displayIndex, rect) != 0)
    {
        METER("usable", -1, -1, "GetDisplayUsableBounds -> refused");
        return -1;
    }

    METER("usable", rect->w, rect->h,
          "GetDisplayUsableBounds -> %dx%d (the whole panel)",
          rect->w, rect->h);
    return 0;
}

// Where the mouse pointer is allowed to be (src/mouse.cpp): the canvas,
// which is the same rectangle SDL_GetDisplayBounds answers with and the same
// one render_target_w/h draws into - on one screen with no window manager,
// the application's coordinate space and the display are the same rectangle,
// whatever the window's own reported size is under the switch. A plain read
// of state core 0 wrote, so the mouse pump may ask from there.
void SDL2Circle_PointerBounds(int *w, int *h)
{
    if (w) *w = s_canvas_w;
    if (h) *h = s_canvas_h;
}

extern "C" int SDL_GetNumDisplayModes(int)
{
    const int n = modes_available();
    METER("nmodes", n, 0, "GetNumDisplayModes -> %d (scanout %dx%d)",
          n, s_scanout_w, s_scanout_h);
    return n;
}

extern "C" int SDL_GetDisplayMode(int, int index, SDL_DisplayMode *mode)
{
    if (!mode)
        return SDL_SetError("SDL_GetDisplayMode: nowhere to put the mode");
    if (index < 0 || index >= modes_available())
        return SDL_SetError("SDL_GetDisplayMode: no mode %d", index);

    // Index 0 is the panel itself, listed whether or not the table names it.
    if (index == 0)
    {
        memset(mode, 0, sizeof(*mode));
        mode->format = SDL_PIXELFORMAT_ARGB8888;
        mode->w = s_scanout_w;
        mode->h = s_scanout_h;
        mode->refresh_rate = DEFAULT_HZ;
        METER("mode", mode->w, mode->h,
              "GetDisplayMode[0] -> %dx%d (the panel)", mode->w, mode->h);
        return 0;
    }

    int n = 1;
    for (const ModeSize &m : s_modes)
    {
        if (m.w > s_scanout_w || m.h > s_scanout_h)
            continue;
        if (is_scanout(m.w, m.h))
            continue;             // already given as index 0
        if (n++ != index)
            continue;
        memset(mode, 0, sizeof(*mode));
        mode->format = SDL_PIXELFORMAT_ARGB8888;
        mode->w = m.w;
        mode->h = m.h;
        mode->refresh_rate = DEFAULT_HZ;
        METER("mode", mode->w, mode->h,
              "GetDisplayMode[%d] -> %dx%d", index, mode->w, mode->h);
        return 0;
    }
    return SDL_SetError("SDL_GetDisplayMode: no mode %d", index);
}

extern "C" int SDL_GetCurrentDisplayMode(int, SDL_DisplayMode *mode)
{
    const bool ok = fill_mode(mode, /* desktop */ false);
    METER("current", ok ? mode->w : -1, ok ? mode->h : -1,
          "GetCurrentDisplayMode -> %dx%d", ok ? mode->w : -1,
          ok ? mode->h : -1);
    return ok ? 0 : -1;
}

extern "C" int SDL_GetDesktopDisplayMode(int, SDL_DisplayMode *mode)
{
    const bool ok = fill_mode(mode, /* desktop */ true);
    METER("desktop", ok ? mode->w : -1, ok ? mode->h : -1,
          "GetDesktopDisplayMode -> %dx%d", ok ? mode->w : -1,
          ok ? mode->h : -1);
    return ok ? 0 : -1;
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
    // The shadow and the granted surface agree on stride by construction -
    // the scanout width is derived from the grant's own pitch - so the
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
        // this library takes or gives back - the sound device's included -
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
                              "present: %u byte shadow not allocated, unbuffered",
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

    // The library's own boot switches, in case this is reached without an
    // SDL_Init that already read them (SDL2Circle_ReadBootArgs is
    // idempotent) - --rapi-vfb has to be known before the canvas size
    // below is decided.
    SDL2Circle_ReadBootArgs();

    // Settle the canvas: the switch, then a declaration, then this window's
    // own size - the first window is what makes a canvas out of nothing at
    // all. Idempotent and already core 0, so later windows just confirm what
    // is already settled.
    if (!resolve_display_size(a->w, a->h))
        return;
    CBcmFrameBuffer *fb = s_fb0;
    if (!fb)
    {
        SDL_SetError("CBcmFrameBuffer::Initialize failed (%dx%d)", a->w, a->h);
        return;
    }

    // SDL's own framebuffer, at canvas resolution. Every frame is composed
    // into it and it is the only surface anything on this side of the library
    // reads or writes, so a window without one would be a window with nowhere
    // to draw. There is no fallback to borrow: say so and make no window.
    if (!canvas_surface_alloc())
    {
        SDL_SetError("the %dx%d virtual framebuffer could not be allocated "
                     "(%u bytes, double buffered)",
                     s_canvas_w, s_canvas_h,
                     (unsigned)(2 * (size_t)s_canvas_w * 4 * s_canvas_h));
        return;
    }

    // The window's own size: what was asked for under the switch - the
    // canvas is fixed at the switch's resolution regardless, so the window
    // is honoured as an answer to the question it asked rather than folded
    // into the canvas - and the canvas itself in every other case, where the
    // window is the canvas by construction (see the geometry comment above
    // s_canvas_w/h). Drawing always targets the canvas; render_target_w/h is
    // where that is settled, and it never reads this field.
    SDL_Window *win = new SDL_Window;
    win->fb = fb;
    // A request of 0x0 is not a size, it is "you choose", so the switch does
    // not honour it. Handed back as-is it makes a window no renderer accepts,
    // and the error names neither the switch nor the request.
    win->w = (s_switch_set && a->w > 0) ? a->w : s_canvas_w;
    win->h = (s_switch_set && a->h > 0) ? a->h : s_canvas_h;
    // The window's state, and the flags a game branches on.
    //
    // This window always has input focus. There is one window and no window
    // manager to take focus away from it, so a game asking whether it is
    // focused is asking a question with only one possible answer here. A
    // flag that is never set is indistinguishable from a flag that is false,
    // and a game told it has no focus pauses, stops drawing or ignores
    // input - a black screen with a clean log, which is the worst shape a
    // failure can take.
    //
    // The flags describe the machine, not the request. What an application
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
    //                       which is the path that works - rather than going
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
    win->mode_w = win->mode_h = 0;
    win->grabbed = SDL_FALSE;
    win->title[0] = '\0';
    win->hit_test = nullptr;
    win->hit_test_data = nullptr;

    // Publish the presentation geometry before the window becomes visible to
    // the application core or the worker. This side is scanout geometry, and
    // it is the executor's alone: a present command is still in canvas
    // coordinates right up to the moment the executor maps it, and the
    // placement it maps into was settled by resolve_display_size above, out
    // of the same two numbers this reads.
    s_fb_base = (u8 *)(uintptr)fb->GetBuffer();
    s_fb_pitch = fb->GetPitch();
    s_fb_w = s_scanout_w;
    s_fb_h = s_scanout_h;

    // Believe the grant, not the request: double buffering draws and pans
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
        // Fit leaves borders no command will ever write - every present
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
                                      "present: %u byte staging not allocated, "
                                      "composing into the framebuffer",
                                      (unsigned)s_stage_bytes);
        }
    }
    s_present_ready = true;

    s_window = win;

    // The screen stops being drawn on here. Creating a window is the
    // application taking the framebuffer - SDL_Init only brought video up,
    // which is not the same thing - and the console and the application must
    // never hold it at once. Already core 0, so no marshalling is needed;
    // src/console.cpp's own lock covers a line already part way onto the
    // screen.
    SDL2Circle_ConsoleReleaseScreen();

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
    // Whether a canvas already existed decides what this window means. The
    // first window settles the canvas from its own size; a later one is a
    // program asking for a different size, which is the same request
    // SDL_SetWindowSize makes and takes the same path.
    const bool had_canvas = s_canvas_w > 0 && s_canvas_h > 0;

    CreateWindowArgs a{w, h, flags, nullptr};
    SDL2Circle_CallOn0(create_window_on0, &a);
    if (!a.result)
        return nullptr;

    // The switch is the one case where a window does not get what it asked
    // for and that is deliberate: --rapi-vfb fixes the canvas and the window
    // is answered honestly about its own size (see the switch's note in
    // docs/DISPLAY.md).
    if (had_canvas && !s_switch_set && w > 0 && h > 0)
        canvas_set_size(a.result, w, h);

    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_DEBUG,
                   "window created: asked %dx%d, given %dx%d, canvas %dx%d",
                   w, h, a.result->w, a.result->h, s_canvas_w, s_canvas_h);
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
// whichever renderer was made - tracked here rather than on the window,
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

static void grant_screen_on0(void *)
{
    SDL2Circle_ConsoleGrantScreen();
}

extern "C" void SDL_DestroyWindow(SDL_Window *win)
{
    if (!win)
        return;

    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_DEBUG,
                   "window destroyed: %dx%d, canvas %dx%d",
                   win->w, win->h, s_canvas_w, s_canvas_h);

    // The presentation worker reaches the window through s_window on every
    // flip, so it has to be out of the frame before this object goes away.
    SDL2Circle_PresentQuiesce();

    if (win == s_window)
    {
        s_window = nullptr;
        s_fb_base = nullptr;

        // Give the screen back: this was the window holding the
        // framebuffer, and it no longer does. Marshalled to core 0 for the
        // same reason the take is (create_window_on0) - the drawing belongs
        // to the core that owns the devices.
        SDL2Circle_CallOn0(grant_screen_on0, nullptr);
    }
    // win->fb is the framebuffer (s_fb0), kept for the process lifetime:
    // deleting it cannot return the firmware's allocation, and the next
    // window must adopt the same grant rather than allocate a leak.
    //
    // The present path (the shadow buffers and their DMA channel, or the
    // staging frame) belongs to that same grant and outlives the window with
    // it - see s_present_ready. Releasing it here would give the next window
    // nothing to reuse, and taking a fresh DMA channel each time is what
    // empties the pool.
    delete win;
}

extern "C" void SDL_GetWindowSize(SDL_Window *win, int *w, int *h)
{
    METER("winsize", win ? win->w : 0, win ? win->h : 0,
          "GetWindowSize -> %dx%d (canvas %dx%d)",
          win ? win->w : 0, win ? win->h : 0, s_canvas_w, s_canvas_h);
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
    // SDL_GetMouseFocus can never disagree - a game that tests the flag and
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
// and reads it back gets what it set - some use it as their own record of
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
// A board has one screen and no window manager. The window fills the
// display, and it never moves or changes size once created. Every call below
// is accepted and none of them changes the geometry.
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
    // Reported and then ignored: this display is only ever fullscreen, so the
    // flag changes nothing. Worth seeing, because on a desktop it would.
    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_DEBUG,
                   "SetWindowFullscreen flags 0x%x, canvas %dx%d",
                   (unsigned)flags, s_canvas_w, s_canvas_h);
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

    // SDL_WINDOW_FULLSCREEN means "drive the display at the mode set for this
    // window", so it is a size change and takes the one path every size
    // change takes. SDL_WINDOW_FULLSCREEN_DESKTOP means "cover the desktop at
    // the size you have", which changes nothing here: this display is only
    // ever fullscreen and the canvas already covers it.
    //
    // Which of the two a program uses decides whether its mode reaches the
    // canvas, and the order it calls them in does not - the mode is applied
    // here as well as at SDL_SetWindowDisplayMode.
    if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != SDL_WINDOW_FULLSCREEN_DESKTOP
        && (flags & SDL_WINDOW_FULLSCREEN) != 0
        && win->mode_w > 0 && win->mode_h > 0)
        canvas_set_size(win, win->mode_w, win->mode_h);
    return 0;
}

// The canvas follows the window. An application that changes video mode says
// so here, and the present path is re-pointed at the new size: the canvas
// buffers, the placement the executor maps through, and the surface the
// application draws into.
//
// The scanout is untouched. Only the source size and the rectangle it is
// placed in change, so the presentation core carries on scaling whatever
// canvas it is handed onto the same panel.
//
// The geometry this writes - the canvas size and the placement - is read by
// the presentation core, in map_onto_scanout, as it executes a frame. So the
// change of geometry is a handover, and it waits below for the presentation
// core to finish with the frame it already has. SDL2Circle_PresentPost is no
// help here: it returns as soon as the command list has been copied out, by
// design, and the copy that survives it is mapped afterwards. Without the
// wait, a frame authored under the old canvas is mapped under the new
// placement - which puts the picture on the glass at a size neither geometry
// ever asked for - and the canvas buffers it reads from are freed under it.
// Paint the whole display black, both framebuffer halves. Defined with the
// rest of the present path, below, and declared here because a resize is
// what needs it.
static void clear_display(void);

// The surface the application draws into. Declared here because a resize has
// to release it: it belongs to the old canvas size, and presenting from it
// afterwards reads old-sized rows with the new pitch.
static SDL_Surface *s_window_surface;

static void canvas_set_size(SDL_Window *win, int w, int h)
{
    if (!win || w <= 0 || h <= 0)
        return;

    // The panel is the ceiling, and it is enforced here rather than only
    // being absent from the list. This library scales a canvas UP onto the
    // glass: a canvas larger than the panel is scaled the other way, and
    // nearest neighbour then throws source pixels away rather than resampling
    // them. It also costs the application core a frame it cannot see - the
    // canvas-sized copy every frame is C1's bill, and pixels beyond the
    // panel's are paid for and then discarded.
    //
    // Clamped rather than refused, because SDL_SetWindowSize returns nothing
    // and an application cannot be told. It reads the size back through
    // SDL_GetWindowSize, SDL_GetRendererOutputSize or the current display
    // mode, all of which report what it actually got - which is what SDL does
    // on a desktop when a mode is not available.
    if (acquire_scanout() && s_scanout_w > 0 && s_scanout_h > 0
        && (w > s_scanout_w || h > s_scanout_h))
    {
        const int cw = w > s_scanout_w ? s_scanout_w : w;
        const int ch = h > s_scanout_h ? s_scanout_h : h;
        SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                       "canvas %dx%d exceeds panel %dx%d: clamped to %dx%d",
                       w, h, s_scanout_w, s_scanout_h, cw, ch);
        w = cw;
        h = ch;
    }

    if (w == s_canvas_w && h == s_canvas_h)
        return;

    // The handover. Everything below - the buffers freed, the canvas size,
    // the placement - belongs to the presentation core until the frame it
    // holds has been through the executor. A mode change is rare and this
    // costs one frame's wait when it happens.
    SDL2Circle_PresentQuiesce();

    // Allocated before anything is released, so a failure leaves the window
    // as it was rather than half resized.
    const unsigned pitch = (unsigned)w * 4;
    u8 *b0 = (u8 *)calloc((size_t)pitch, (size_t)h);
    u8 *b1 = (u8 *)calloc((size_t)pitch, (size_t)h);
    if (!b0 || !b1)
    {
        free(b0);
        free(b1);
        SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_WARNING,
                       "no memory for a %dx%d canvas: staying at %dx%d",
                       w, h, s_canvas_w, s_canvas_h);
        return;
    }

    free(s_canvas_surface_buf[0]);
    free(s_canvas_surface_buf[1]);
    s_canvas_surface_buf[0] = b0;
    s_canvas_surface_buf[1] = b1;
    s_canvas_surface_pitch  = pitch;
    s_canvas_surface_idx    = 0;
    s_canvas_surface        = s_canvas_surface_buf[0];

    s_canvas_w = w;
    s_canvas_h = h;
    win->w = w;
    win->h = h;

    // The surface is refitted rather than replaced, and the object it is
    // held in outlives the change.
    //
    // A program is entitled to keep what SDL_GetWindowSurface handed it -
    // several do, in a global they set once - and freeing that object here
    // leaves them holding freed memory. What they then read through it is
    // whatever the allocator has since put there, which is how a resize
    // turns into a fault somewhere else entirely, in code that has nothing
    // to do with the display.
    //
    // So the pixels are replaced and the dimensions rewritten in place. A
    // pointer taken before the change still describes the surface after it.
    if (s_window_surface)
    {
        const int surf_pitch = w * 4;
        void *px = calloc((size_t)surf_pitch, (size_t)h);
        if (px)
        {
            if (!(s_window_surface->flags & SDL_PREALLOC))
                free(s_window_surface->pixels);
            s_window_surface->pixels = px;
            s_window_surface->w = w;
            s_window_surface->h = h;
            s_window_surface->pitch = surf_pitch;
            s_window_surface->clip_rect = SDL_Rect{ 0, 0, w, h };
        }
        else
        {
            // Nothing to hand back is better than a surface whose size and
            // storage disagree; the next SDL_GetWindowSurface builds a new
            // one, and a program that kept the old pointer was going to be
            // wrong either way.
            SDL_FreeSurface(s_window_surface);
            s_window_surface = nullptr;
        }
    }

    resolve_placement();

    // The pointer is in canvas coordinates, so it means something different
    // now: the panel has not moved and the canvas still covers all of it, so
    // the point of glass the pointer is on is a different canvas coordinate
    // by exactly the ratio the canvas changed by. This converts it in that
    // ratio, on core 0, which is the only core that may write it - see the
    // record in src/mouse.cpp. It is done here, before the size-changed event
    // below, so an application handling that event reads a position that is
    // already in the canvas the event is telling it about.
    SDL2Circle_PointerCanvasChanged();

    // The canvas lands on a different rectangle, so whatever the last
    // placement painted outside the new one is still on the glass. Nothing
    // the application draws from here on can reach it - the letterbox is
    // outside the canvas, so no canvas rectangle names it - which is why the
    // library clears it rather than leaving it to the application.
    clear_display();

    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_WINDOWEVENT;
    ev.window.timestamp = SDL_GetTicks();
    ev.window.windowID = 1;
    ev.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
    ev.window.data1 = w;
    ev.window.data2 = h;
    SDL_PushEvent(&ev);
}

extern "C" void SDL_SetWindowSize(SDL_Window *win, int w, int h)
{
    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_DEBUG,
                   "SetWindowSize %dx%d, canvas %dx%d", w, h,
                   s_canvas_w, s_canvas_h);
    canvas_set_size(win, w, h);
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

// Window opacity needs something behind the window to show through. There is
// one framebuffer and it is scanned out to the panel, so the window is always
// fully opaque and asking for anything else fails, which is what SDL does on
// any platform without a compositor.
//
// The entry point exists because callers set opacity without reading the
// result - a program that dims its window on losing focus, say - and an
// absent symbol would stop such a program linking at all.
extern "C" int SDL_SetWindowOpacity(SDL_Window *win, float opacity)
{
    if (!win)
        return SDL_SetError("SDL_SetWindowOpacity: no window");
    if (opacity >= 1.0f)
        return 0;
    return SDL_SetError("SDL_SetWindowOpacity: the display has no compositor, "
                        "so a window is always fully opaque");
}

extern "C" int SDL_GetWindowOpacity(SDL_Window *win, float *out_opacity)
{
    if (!win)
        return SDL_SetError("SDL_GetWindowOpacity: no window");
    if (out_opacity)
        *out_opacity = 1.0f;
    return 0;
}

// There is nothing to grab away from and nowhere for a pointer to leave to,
// so the grab is recorded and the input path is unaffected.
// Recorded and reported back. A program reads this to decide whether the
// pointer is its to move, so it is metered beside the rectangle above.
extern "C" void SDL_SetWindowGrab(SDL_Window *win, SDL_bool grabbed)
{
    METER("grab", grabbed ? 1 : 0, 0, "SetWindowGrab %s",
          grabbed ? "on" : "off");
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

// Accepted and not acted on: the pointer is already confined to the one
// screen. What a caller asks for is metered, because a program that sets this
// believes the pointer cannot leave the rectangle, and when the rectangle is
// not the whole canvas the two sides disagree about where the pointer is.
extern "C" int SDL_SetWindowMouseRect(SDL_Window *, const SDL_Rect *rect)
{
    if (rect)
        METER("mouserect", rect->w, rect->h,
              "SetWindowMouseRect %dx%d+%d+%d, canvas %dx%d (not applied)",
              rect->w, rect->h, rect->x, rect->y, s_canvas_w, s_canvas_h);
    else
        METER("mouserect", -1, -1, "SetWindowMouseRect cleared");
    return 0;
}

extern "C" const SDL_Rect *SDL_GetWindowMouseRect(SDL_Window *)
{
    return nullptr;
}

// A hit test exists so a window manager can be told which part of a window
// drags it or resizes it, in place of a title bar. There is no window
// manager here - one window, filling the one display, nothing else on the
// glass to hand a region to - so the condition it exists to answer can never
// come up, and the callback can never fire. Upstream reserves -1 for
// "platform does not support this"; that would be honest for a board with
// no window manager at all, except that the application calling this one
// treats -1 as fatal. So the call is accepted instead: the callback and its
// data are recorded, in case a future caller reads them back, and success is
// returned, matching what SDL_SetWindowHitTest promises when a callback is
// genuinely in effect. A NULL callback means what it means upstream -
// disable hit-testing - which costs nothing extra to honour, since hit
// testing was never going to run either way.
extern "C" int SDL_SetWindowHitTest(SDL_Window *win, SDL_HitTest callback, void *callback_data)
{
    if (!win)
        return SDL_SetError("SDL_SetWindowHitTest: no window");

    win->hit_test = callback;
    win->hit_test_data = callback_data;
    return 0;
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
// nothing, and every query answers with the mode actually in force - which
// is what an application checks before deciding how much it can draw.
// ---------------------------------------------------------------------------

extern "C" int SDL_GetWindowDisplayMode(SDL_Window *win, SDL_DisplayMode *mode)
{
    if (!win || !mode)
        return SDL_SetError("SDL_GetWindowDisplayMode: no window or mode");
    return SDL_GetCurrentDisplayMode(0, mode);
}

// A fullscreen application states the size it wants here rather than through
// SDL_SetWindowSize, so this resizes the canvas too. Both routes lead to the
// same place: SDL_GetWindowSize then reports the new size and
// SDL_GetWindowSurface hands back a surface of it, which is what an
// application checks before it will draw.
extern "C" int SDL_SetWindowDisplayMode(SDL_Window *win, const SDL_DisplayMode *mode)
{
    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_DEBUG,
                   "SetWindowDisplayMode %dx%d, canvas %dx%d",
                   mode ? mode->w : -1, mode ? mode->h : -1,
                   s_canvas_w, s_canvas_h);
    if (!win)
        return SDL_SetError("SDL_SetWindowDisplayMode: no window");
    if (mode && mode->w > 0 && mode->h > 0)
    {
        // Recorded as well as applied, because SDL_WINDOW_FULLSCREEN means
        // "drive the display at this mode" and may arrive afterwards.
        win->mode_w = mode->w;
        win->mode_h = mode->h;
        canvas_set_size(win, mode->w, mode->h);
    }
    return 0;
}

// SDL's contract is to return the closest mode it has, and there is exactly
// one to be closest.
// A size that fits the panel is answered with itself: this library would
// allocate exactly that canvas if the application went on to set it, so
// offering something else would refuse a mode that works. A size larger than
// the panel cannot be scaled down onto the glass, and comes back as the panel.
extern "C" SDL_DisplayMode *SDL_GetClosestDisplayMode(int displayIndex,
                                                      const SDL_DisplayMode *wanted,
                                                      SDL_DisplayMode *closest)
{
    if (!closest)
        return nullptr;
    if (!wanted || wanted->w <= 0 || wanted->h <= 0)
        return SDL_GetCurrentDisplayMode(displayIndex, closest) < 0 ? nullptr : closest;
    if (!acquire_scanout() || s_scanout_w <= 0 || s_scanout_h <= 0)
        return nullptr;

    memset(closest, 0, sizeof(*closest));
    closest->format = SDL_PIXELFORMAT_ARGB8888;
    closest->w = wanted->w <= s_scanout_w ? wanted->w : s_scanout_w;
    closest->h = wanted->h <= s_scanout_h ? wanted->h : s_scanout_h;
    closest->refresh_rate = wanted->refresh_rate > 0 ? wanted->refresh_rate
                                                     : DEFAULT_HZ;
    METER("closest", closest->w, closest->h,
          "GetClosestDisplayMode %dx%d -> %dx%d",
          wanted->w, wanted->h, closest->w, closest->h);
    return closest;
}

// No panel reports its physical size here, and SDL's contract is to fail
// rather than invent one - an application that scales its text by the answer
// would lay itself out to a number that means nothing.
extern "C" int SDL_GetDisplayDPI(int, float *ddpi, float *hdpi, float *vdpi)
{
    if (ddpi) *ddpi = 0.0f;
    if (hdpi) *hdpi = 0.0f;
    if (vdpi) *vdpi = 0.0f;
    return SDL_SetError("SDL_GetDisplayDPI: the display does not report its "
                        "physical size");
}

// There is no compositor between the canvas and the panel, and no scaling
// factor of the kind a desktop applies on a high-density display, so a
// drawable pixel is a canvas pixel - not necessarily a window pixel: under
// the --rapi-vfb switch a window may report a size of its own that the
// canvas does not share (see the geometry comment above s_canvas_w/h).
extern "C" void SDL_GL_GetDrawableSize(SDL_Window *win, int *w, int *h)
{
    if (w) *w = win ? s_canvas_w : 0;
    if (h) *h = win ? s_canvas_h : 0;
}

extern "C" void SDL_GetWindowSizeInPixels(SDL_Window *win, int *w, int *h)
{
    if (w) *w = win ? s_canvas_w : 0;
    if (h) *h = win ? s_canvas_h : 0;
}

extern "C" void SDL_Vulkan_GetDrawableSize(SDL_Window *win, int *w, int *h)
{
    if (w) *w = win ? s_canvas_w : 0;
    if (h) *h = win ? s_canvas_h : 0;
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
// are decided by which window system the configuration names - and this
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

// A fill of the whole canvas is the one command the executor widens to the
// whole display, letterbox included (see map_onto_scanout).
//
// Twice, because the present path always draws into a back buffer and there
// are two of them: the two framebuffer halves when the grant allowed
// panning, and the two shadows otherwise. A back buffer this misses keeps
// the old borders and is shown next, which reads on the glass as the borders
// flashing rather than as a clear that did nothing. Two covers both schemes,
// and covers a single-buffered one harmlessly, so this does not have to know
// which is in use.
//
// Each fill is posted as a frame of its own, because the frame record
// carries a single command, and the two are separated by a quiesce, because
// the back buffer only changes when the present that used it completes.
static void clear_display(void)
{
    if (s_canvas_w <= 0 || s_canvas_h <= 0)
        return;

    SDL2CirclePresentCmd clear;
    memset(&clear, 0, sizeof(clear));
    clear.op = SDL2CirclePresentCmd::FILL;
    clear.dx = 0; clear.dy = 0;
    clear.w = s_canvas_w; clear.h = s_canvas_h;
    clear.color = 0xFF000000;
    clear.alphamod = 255;

    const bool split = SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0;
    for (unsigned i = 0; i < 2; i++)
    {
        if (split)
        {
            SDL2Circle_PresentPost(&clear, 1, s_window_surface_back);
            SDL2Circle_PresentQuiesce();
        }
        else
        {
            SDL2Circle_VideoExecCmd(&clear, s_window_surface_back);
        }
        if (s_fb_halves == 2)
            s_window_surface_back ^= 1;
    }
}

extern "C" int SDL_UpdateWindowSurfaceRects(SDL_Window *win,
                                            const SDL_Rect *rects, int numrects)
{
    if (!win || !s_window_surface)
        return SDL_SetError("SDL_UpdateWindowSurfaceRects: no window surface");

    // The window surface is a frame like any other, and it takes the same
    // route to the glass: mapped from canvas coordinates onto the fitted
    // rectangle on the scanout, then handed to the presentation core.
    //
    // The whole canvas is carried across each time, not just the rectangles
    // the caller named. The surface handed over has to be one the
    // application cannot write while the presentation core is still reading
    // it, so the frame is copied into the virtual framebuffer, which is
    // double buffered exactly for that: this frame goes into the buffer the
    // worker is not holding. Copying only the named rectangles into that
    // buffer would leave everything else in it showing the previous frame,
    // so what is copied is the whole surface; the named rectangles are read
    // for nothing but their bounds, and this path otherwise ignores them.
    //
    // The window is the canvas, so these agree by construction - but the
    // copy below writes canvas-sized storage from window-sized rows, and a
    // disagreement would write past the end of it. Tested rather than
    // assumed.
    (void)rects; (void)numrects;
    if (win->w != s_canvas_w || win->h != s_canvas_h)
        return SDL_SetError("SDL_UpdateWindowSurfaceRects: the window is "
                            "%dx%d and the canvas is %dx%d",
                            win->w, win->h, s_canvas_w, s_canvas_h);

    SDL2CirclePresentCmd frame[2];
    memset(&frame[0], 0, sizeof(frame[0]));
    frame[0].op = SDL2CirclePresentCmd::COPY;
    frame[0].alphamod = 255;


    SDL2CirclePresentCmd in = frame[0];
    in.dx = 0; in.dy = 0;
    in.w = win->w; in.h = win->h;
    in.sw = win->w; in.sh = win->h;
    in.src = (u8 *)s_window_surface->pixels;
    in.srcpitch = s_window_surface->pitch;
    exec_into(&in, s_canvas_surface, s_canvas_surface_pitch);

    frame[0].src = s_canvas_surface;
    frame[0].srcpitch = (int)s_canvas_surface_pitch;
    frame[0].dx = 0; frame[0].dy = 0;
    frame[0].w = win->w; frame[0].h = win->h;
    frame[0].sw = win->w; frame[0].sh = win->h;

    // The pointer, over the frame, exactly as the renderer's present puts it
    // there: this path is a frame reaching the glass like any other.
    unsigned nout = 1;
    if (cursor_command(&frame[1]))
        nout = 2;

    if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
    {
        SDL2Circle_PresentPost(frame, nout, s_window_surface_back);
        s_canvas_surface_idx ^= 1;
        s_canvas_surface = s_canvas_surface_buf[s_canvas_surface_idx];
        if (s_fb_halves == 2)
            s_window_surface_back ^= 1;
        return 0;
    }

    for (unsigned i = 0; i < nout; i++)
        SDL2Circle_VideoExecCmd(&frame[i], s_window_surface_back);

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
    // Half 0 is visible after init, so draw into half 1 first -- unless the
    // grant forced single-buffering, where half 0 is all there is.
    ren->back = s_fb_halves == 2 ? 1 : 0;
    ren->vsync = (flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    // Diagnostic: which flags the caller actually asked for and what this
    // resolved to, so a consumer's own fallback chain (retrying with fewer
    // flags after a failed create elsewhere) is visible rather than assumed.
    SDL2Circle_Log("sdl2video", SDL2CIRCLE_LOG_NOTICE,
                  "renderer created: %dx%d, flags 0x%x, vsync %s",
                  s_canvas_w, s_canvas_h, flags, ren->vsync ? "on" : "off");
    ren->r = ren->g = ren->b = 0;
    ren->a = 255;
    ren->ncmds = 0;
    ren->rasterizing = false;
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
    ren->target = nullptr;
    memset(&ren->window_view, 0, sizeof ren->window_view);
    ren->scratch[0] = ren->scratch[1] = nullptr;
    ren->scratch_bytes[0] = ren->scratch_bytes[1] = 0;
    ren->scratch_used = 0;
    ren->scratch_idx = 0;
    ren->rel_frac_x = 0.0f;
    ren->rel_frac_y = 0.0f;
    ren->relative_scaling =
        SDL_GetHintBoolean(SDL_HINT_MOUSE_RELATIVE_SCALING, SDL_TRUE) == SDL_TRUE;
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
// Between what an application draws and what the window receives sit the
// render scale, the logical size (which scales and centres), and the
// viewport (which offsets), applied in that order by SDL2. One function
// does all three, and every entry point that takes a destination
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

// The mapping, from the coordinate state and the extent it is being applied
// to, rather than from a renderer. The renderer's live block is one caller;
// the block belonging to the window while a render target is set is the other
// - see pointer_map, further down. One implementation, because a pointer
// translated by anything other than the arithmetic that placed the picture
// would report the arrow somewhere it is not drawn.
LogicalMap logical_map_from(int logical_w, int logical_h, bool integer_scale,
                            float scale_x, float scale_y,
                            const SDL_Rect *viewport, bool viewport_set,
                            int ow, int oh)
{
    LogicalMap m = { scale_x, scale_y, 0, 0 };

    if (logical_w > 0 && logical_h > 0)
    {
        float sx = (float)ow / (float)logical_w;
        float sy = (float)oh / (float)logical_h;

        if (integer_scale)
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

        m.sx = sx * scale_x;
        m.sy = sy * scale_y;
        m.ox = (int)((ow - logical_w * m.sx) / 2.0f);
        m.oy = (int)((oh - logical_h * m.sy) / 2.0f);
    }

    // The viewport is given in the application's coordinates, like every
    // other rectangle it hands over, so its origin is scaled by the same
    // factors before it moves the origin.
    if (viewport_set)
    {
        m.ox += (int)(viewport->x * m.sx);
        m.oy += (int)(viewport->y * m.sy);
    }
    return m;
}

LogicalMap logical_map(const SDL_Renderer *ren)
{
    return logical_map_from(ren->logical_w, ren->logical_h, ren->integer_scale,
                            ren->scale_x, ren->scale_y,
                            &ren->viewport, ren->viewport_set,
                            render_target_w(ren), render_target_h(ren));
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
        r = { 0, 0, render_target_w(ren), render_target_h(ren) };
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

    // Never outside what is being drawn into, whatever was asked for. The
    // executor writes where it is told and has nothing underneath it to
    // catch a rectangle that leaves the surface.
    const SDL_Rect win = { 0, 0, render_target_w(ren), render_target_h(ren) };
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

// ---------------------------------------------------------------------------
// The pointer's own mapping
//
// The mouse position is kept, clamped and drawn in canvas coordinates, and an
// application that has set a logical size is not working in those: it draws
// 640x480 into a 1280x960 window and tests every click against the 640x480.
// So the position is reported through the inverse of the mapping that placed
// the picture - the same function, run backwards - and not through a divide
// of its own, because a pointer translated by anything else would be reported
// somewhere other than where the arrow was drawn.
//
// It is the WINDOW's coordinate state, never a render target's. An event is
// delivered whenever the application polls, which may be while it has a
// texture set as its target, and the pointer is on the window whatever the
// renderer is aimed at meanwhile. SDL2 keeps the window's block for exactly
// this and reads that copy in its own event watch. Here the live block IS the
// window's until a target is set, and window_view holds it while one is.
//
// The application's core owns all of it: the renderer is that core's object,
// that core aims the target, and both callers - the event queue's delivery
// point and SDL_GetMouseState - run there as well. Nothing crosses a core, so
// nothing here is locked. Core 0 goes on holding the position in canvas
// coordinates, which is the only space core 0 can know: it has no renderer
// and may not read one.
// ---------------------------------------------------------------------------

// False when there is nothing to translate - no renderer, or no logical size.
// That is nearly every application, and it is what makes this cost one
// comparison and change nothing for them.
bool pointer_map(LogicalMap *out)
{
    const SDL_Renderer *ren = s_renderer;
    if (!ren)
        return false;

    const bool        targeted = ren->target != nullptr;
    const RenderView &v        = ren->window_view;

    const int lw = targeted ? v.logical_w : ren->logical_w;
    const int lh = targeted ? v.logical_h : ren->logical_h;
    if (lw <= 0 || lh <= 0)
        return false;

    *out = logical_map_from(lw, lh,
                            targeted ? v.integer_scale : ren->integer_scale,
                            targeted ? v.scale_x       : ren->scale_x,
                            targeted ? v.scale_y       : ren->scale_y,
                            targeted ? &v.viewport     : &ren->viewport,
                            targeted ? v.viewport_set  : ren->viewport_set,
                            s_canvas_w, s_canvas_h);
    return true;
}
} // namespace

// Canvas coordinates - what core 0 holds, and where the arrow is composed -
// into the space the application draws in. Answers false, having changed
// nothing, when the application has no logical size and the two spaces are
// therefore the same one.
//
// Nothing is clamped to the logical rectangle. A logical size whose aspect
// ratio differs from the window's letterboxes inside it, and a pointer parked
// in the letterbox is genuinely outside the picture: the coordinate comes
// back negative above or to the left of it, and past the logical width or
// height below or to the right. That is what upstream SDL2 reports, and it is
// what lets an application tell "outside my picture" from "on its edge".
bool SDL2Circle_PointerToLogical(int *x, int *y)
{
    LogicalMap m;
    if (!pointer_map(&m))
        return false;

    if (x && m.sx != 0.0f) *x = (int)((float)(*x - m.ox) / m.sx);
    if (y && m.sy != 0.0f) *y = (int)((float)(*y - m.oy) / m.sy);
    return true;
}

// The other direction, for a coordinate the application names: a warp says
// where the pointer is to go, in the space the application reads it back in.
bool SDL2Circle_PointerFromLogical(int *x, int *y)
{
    LogicalMap m;
    if (!pointer_map(&m))
        return false;

    if (x) *x = m.ox + (int)((float)*x * m.sx);
    if (y) *y = m.oy + (int)((float)*y * m.sy);
    return true;
}

// A movement, rather than a place: no origin to take off, only the scale.
//
// The division has a remainder and throwing it away would lose slow movement
// outright - at a scale of two, a stream of one-pixel reports would every one
// of them truncate to zero and the pointer would never move at all - so the
// fraction is carried on the renderer to the next event, as SDL2 carries it.
// SDL_HINT_MOUSE_RELATIVE_SCALING is what an application sets to say it wants
// the device's own movement instead, and it is read when the renderer is made.
void SDL2Circle_PointerRelToLogical(int *dx, int *dy)
{
    SDL_Renderer *ren = s_renderer;
    LogicalMap m;
    if (!ren || !ren->relative_scaling || !pointer_map(&m))
        return;

    // A mouse report's displacement divided by a scale factor is a small
    // number, so truncating through int is exact here.
    if (dx && *dx != 0 && m.sx != 0.0f)
    {
        const float rel  = ren->rel_frac_x + (float)*dx / m.sx;
        const float whole = (float)(int)rel;
        ren->rel_frac_x = rel - whole;
        *dx = (int)whole;
    }
    if (dy && *dy != 0 && m.sy != 0.0f)
    {
        const float rel  = ren->rel_frac_y + (float)*dy / m.sy;
        const float whole = (float)(int)rel;
        ren->rel_frac_y = rel - whole;
        *dy = (int)whole;
    }
}

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

// The size of what is being drawn into: the render target's own size while
// one is set, the canvas otherwise - render_target_w/h, not the window's own
// reported size, which under the switch is not necessarily the same thing.
extern "C" int SDL_GetRendererOutputSize(SDL_Renderer *ren, int *w, int *h)
{
    if (ren && ren->target)
        return SDL_QueryTexture(ren->target, nullptr, nullptr, w, h);
    const int ow = ren ? render_target_w(ren) : 0;
    const int oh = ren ? render_target_h(ren) : 0;
    METER("rendout", ow, oh, "GetRendererOutputSize -> %dx%d", ow, oh);
    if (w) *w = ow;
    if (h) *h = oh;
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
    cmd.w = render_target_w(ren);
    cmd.h = render_target_h(ren);
    cmd.sw = 0;
    cmd.sh = 0;
    cmd.color = ((u32)ren->a << 24) | ((u32)ren->r << 16) |
                ((u32)ren->g << 8) | ren->b;
    cmd.pointer = 0;
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
    const int app_bpp = SDL2Circle_BytesPerPixel(format);
    if (app_bpp == 0)
    {
        SDL_SetError("texture pixel format 0x%08x is not supported",
                     (unsigned)format);
        return nullptr;
    }
    if (format == SDL_PIXELFORMAT_INDEX8)
    {
        // SDL2 gives a texture no palette - SDL_UpdateTexture takes pixels
        // and nothing else - so an indexed texture has no way to say what
        // its indices mean. A paletted game wants an indexed surface, which
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

    // The coordinate state this texture is drawn into with, once it is the
    // render target: the whole texture, unscaled, unclipped.
    memset(&tex->view, 0, sizeof tex->view);
    tex->view.scale_x = 1.0f;
    tex->view.scale_y = 1.0f;
    tex->view.viewport = { 0, 0, w, h };

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
// Whether a store is spoken for: named by a frame that has been posted and
// not yet finished with.
//
// Testing only whether the worker has started reading a store is not
// enough. A store carries one sequence - the last frame to name it - so
// once a newer frame overwrites that mark, an older frame still queued to
// read the same store is forgotten, and allowing the writer into a
// posted-but-unstarted store lets a second frame claim it while the worker
// still has to reach the first.
//
// A store named by the frame still being built is correctly free: it
// carries a sequence above the posted one, and nothing has been sent that
// could read it. Without that an application drawing to one texture twice
// before presenting would wait for a frame nobody has posted.
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
// and the worker reads it for as long as its scale runs, so this must never
// return the store that scale is reading. Tracking the store the last
// recorded copy named is not the same test: it tracks where the writer went
// last, not what the reader still holds, and the two agree only by
// coincidence - a mismatch produces a frame torn between two pictures, every
// pitch correct and every pixel real, just composed from the wrong stores.
static u8 *texture_write_buffer(SDL_Texture *tex, bool preserve)
{
    if (!SDL2Circle_SplitActive())
        return tex->pixels[tex->widx];

    if (!texture_store_busy(tex, tex->widx))
        return tex->pixels[tex->widx];      // still ours; no copy needed

    // Three stores; the third is what removes the wait. With two, a poster
    // running ahead of the worker has nowhere to put a frame: one store is
    // being read and the other holds the frame already posted behind it, so
    // the writer stops - the game core waiting on the presentation core,
    // which this must not do. With three there is always one that is
    // neither.
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
    // Destroying the texture the renderer is aimed at puts the aim back on
    // the frame, as SDL2 does. Leaving it would point every later draw call
    // at freed memory.
    if (s_renderer && s_renderer->target == tex)
        SDL_SetRenderTarget(s_renderer, nullptr);
    // A frame the worker has not finished with may still name this texture's
    // pixels as its source - the reduced frame is the texture, in place.
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

    // The application must see its own format under the lock, so it gets a
    // staging buffer the size of the whole texture - kept between locks,
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
    int dw = dstrect ? dstrect->w : render_target_w(ren);
    int dh = dstrect ? dstrect->h : render_target_h(ren);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return 0;

    // A texture cannot be copied onto itself: source and destination would
    // be the same store, and the blitter reads rows as it writes them. SDL
    // does not define the result either; refusing says so, where drawing it
    // would put a picture on screen that nothing explains.
    if (ren->target != nullptr && tex == ren->target)
        return SDL_SetError("SDL_RenderCopy: the texture is the render "
                            "target it is being copied into");

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
    cmd.pointer = 0;
    emit_cmd(ren, cmd);

    // A command the renderer drew has already read this store, here, on this
    // core, before returning - the pixels are in the virtual framebuffer and
    // the store is the application's again immediately.
    //
    // A command the renderer held back is a different matter: it will cross
    // as a list, carrying a raw pointer into this store, so the store is
    // spoken for by the frame being assembled - which will be posted as the
    // next sequence - and nothing may write here until the worker
    // acknowledges that frame.
    //
    // A copy into a render target was drawn as well, wherever it came from:
    // nothing a target does crosses to the presentation core, so no frame
    // can be holding this store either.
    if (!ren->rasterizing && !ren->target)
        tex->busy_seq[tex->widx] = SDL2Circle_PresentPostedSeq() + 1;
    return 0;
}

// A copy that may be mirrored. The texture holds one set of pixels and has
// no mirrored copy of its own, so the mirrored region is written into the
// frame's scratch arena and copied from there - which is why the arena
// exists, and why it lasts exactly as long as a recorded command does.
//
// Rotation is refused. Turning a picture by an arbitrary angle means
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

    if (ren->target != nullptr && tex == ren->target)
        return SDL_SetError("SDL_RenderCopyEx: the texture is the render "
                            "target it is being copied into");

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
    int dw = dstrect ? dstrect->w : render_target_w(ren);
    int dh = dstrect ? dstrect->h : render_target_h(ren);
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
    cmd.pointer = 0;
    emit_cmd(ren, cmd);
    return 0;
}

// Reads back what has been drawn, out of whatever is being drawn into: the
// render target where one is set, and otherwise the virtual framebuffer -
// the only framebuffer SDL has, and the one every frame is composed into.
// The panel does not come into it: it is a different size, it holds the
// picture fitted and centred inside a black margin, and none of that is
// anything SDL was ever told about.
//
// The rectangle is in canvas coordinates, which is what the caller drew in,
// and the answer is the frame as it stands right now: everything drawn since
// this frame began, over whatever the buffer held before that. An application
// reading back before its present therefore gets the picture it just drew.
extern "C" int SDL_RenderReadPixels(SDL_Renderer *ren, const SDL_Rect *rect,
                                    Uint32 format, void *pixels, int pitch)
{
    if (!ren || !pixels)
        return SDL_SetError("SDL_RenderReadPixels: no renderer or destination");

    // The read comes from whatever is being drawn into. A render target is
    // already complete - every call into it was executed as it was made -
    // so there is nothing to finish first, and the store the last of them
    // went into is the one holding the content.
    const u8 *surface;
    unsigned surface_pitch;
    if (ren->target)
    {
        surface = ren->target->pixels[ren->target->widx];
        surface_pitch = (unsigned)ren->target->pitch;
    }
    else
    {
        if (!s_canvas_surface)
            return SDL_SetError("SDL_RenderReadPixels: no virtual framebuffer");

        // Anything the renderer is still holding back to cross as a list has
        // not been drawn anywhere yet. Draw it, so the read sees the whole
        // frame.
        start_rasterizing(ren);
        surface = s_canvas_surface;
        surface_pitch = s_canvas_surface_pitch;
    }

    SDL_Rect r = rect ? *rect
                      : SDL_Rect{ 0, 0, render_target_w(ren),
                                  render_target_h(ren) };
    const SDL_Rect canvas = { 0, 0, render_target_w(ren),
                              render_target_h(ren) };
    if (!SDL_IntersectRect(&r, &canvas, &r))
        return 0;

    if (format == 0)
        format = SDL_PIXELFORMAT_ARGB8888;

    for (int y = 0; y < r.h; y++)
    {
        const u8 *src = surface
                      + (size_t)(r.y + y) * surface_pitch
                      + (size_t)r.x * 4;
        u8 *dst = (u8 *)pixels + (size_t)y * pitch;
        if (SDL_ConvertPixels(r.w, 1, SDL_PIXELFORMAT_ARGB8888, src,
                              (int)surface_pitch,
                              format, dst, pitch) < 0)
            return -1;
    }
    return 0;
}

extern "C" int SDL_GetRendererInfo(SDL_Renderer *, SDL_RendererInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->name = "circle";
    info->flags = SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC
                | SDL_RENDERER_TARGETTEXTURE;
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
        ren->viewport = { 0, 0, render_target_w(ren), render_target_h(ren) };
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
        *rect = { 0, 0, render_target_w(ren), render_target_h(ren) };
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

// ---------------------------------------------------------------------------
// The render target
//
// Two destinations, and the renderer is aimed at one of them at a time: the
// frame, or a texture created with SDL_TEXTUREACCESS_TARGET. Everything about
// the second follows from one fact - a target's pixels are executed into as
// each call is made, by the same executor that composes the frame - so the
// only work here is aiming, and the coordinate state that goes with the aim.
// ---------------------------------------------------------------------------

namespace
{
void save_view(const SDL_Renderer *ren, RenderView *v)
{
    v->logical_w = ren->logical_w;
    v->logical_h = ren->logical_h;
    v->integer_scale = ren->integer_scale;
    v->scale_x = ren->scale_x;
    v->scale_y = ren->scale_y;
    v->viewport = ren->viewport;
    v->viewport_set = ren->viewport_set;
    v->clip = ren->clip;
    v->clip_enabled = ren->clip_enabled;
}

void load_view(SDL_Renderer *ren, const RenderView *v)
{
    ren->logical_w = v->logical_w;
    ren->logical_h = v->logical_h;
    ren->integer_scale = v->integer_scale;
    ren->scale_x = v->scale_x;
    ren->scale_y = v->scale_y;
    ren->viewport = v->viewport;
    ren->viewport_set = v->viewport_set;
    ren->clip = v->clip;
    ren->clip_enabled = v->clip_enabled;
}
} // namespace

// Aim the renderer. A null texture means the frame, which is where a
// renderer starts and what an application returns to before presenting.
//
// The texture's own coordinate state comes with it and the outgoing one is
// put away, as SDL2 does: a viewport set while a texture is the target
// belongs to that texture, and the window's own is untouched by it.
extern "C" int SDL_SetRenderTarget(SDL_Renderer *ren, SDL_Texture *tex)
{
    if (!ren)
        return SDL_SetError("SDL_SetRenderTarget: no renderer");
    if (tex && tex->access != SDL_TEXTUREACCESS_TARGET)
        return SDL_SetError("SDL_SetRenderTarget: this texture was not "
                            "created with SDL_TEXTUREACCESS_TARGET");
    if (tex == ren->target)
        return 0;

    save_view(ren, ren->target ? &ren->target->view : &ren->window_view);
    ren->target = tex;
    load_view(ren, tex ? &tex->view : &ren->window_view);
    return 0;
}

extern "C" SDL_Texture *SDL_GetRenderTarget(SDL_Renderer *ren)
{
    return ren ? ren->target : nullptr;
}

extern "C" SDL_bool SDL_RenderTargetSupported(SDL_Renderer *)
{
    return SDL_TRUE;
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

// A texture holds ARGB8888, so a surface in any other format - or one
// carrying a colour key, which has to become real transparency before the
// key is lost - is converted once here rather than at every update.
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
    //
    // A colour key overrides that: a key is not a blend mode, and a surface
    // may carry one while its own blend mode says none, which is
    // what SDL_SetColorKey leaves behind and what an image loaded from a
    // paletted file therefore has. The key became per-pixel alpha in the
    // conversion above, and a texture drawn with blending off would put those
    // transparent pixels on screen as opaque black - a sprite in a black box,
    // erasing whatever it was meant to sit in front of. So a keyed surface
    // makes a blending texture, as SDL2 does.
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    if (SDL_HasColorKey(surf) == SDL_TRUE)
        blend = SDL_BLENDMODE_BLEND;
    else
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
    int w = rect ? rect->w : render_target_w(ren);
    int h = rect ? rect->h : render_target_h(ren);

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
    cmd.pointer = 0;
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
    // outlines the whole render target.
    SDL_Rect r = rect ? *rect
                      : SDL_Rect{ 0, 0, render_target_w(ren),
                                  render_target_h(ren) };
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

// ---- what crosses to the presentation core ---------------------------------
//
// What crosses is the virtual framebuffer - a bitmap at canvas resolution
// that this library owns and composes itself, double buffered so the buffer
// handed over is never the one the application's next frame is drawn into -
// or, when the frame is short enough for the far side to compose it, a list
// of commands in canvas coordinates. That crossing count is a build-time
// choice (SDL2CIRCLE_PRESENT_MAX_CMDS, described in sdl2circle.h), and at
// the default of zero only an empty frame ever takes it.
//
// Never the application's own memory: the presentation core must never be
// handed a pointer into a texture or surface the application owns, because
// the application is free to destroy or redraw it the moment present
// returns while that core is still reading it. Every frame is composed into
// the virtual framebuffer first, and the virtual framebuffer - or the
// command list describing it - is what crosses. The cost is one full-canvas
// copy per frame on this core, and a second resample at present time: the
// canvas is resampled onto the scanout rather than a texture going straight
// there.

extern "C" void SDL_RenderPresent(SDL_Renderer *ren)
{
    SDL2CirclePerfScope perf(SDL2CIRCLE_PERF_RENDER);
    g_SDL2CirclePresents++;

    // The pointer, worked out before the frame's shape is decided because it
    // is one of the commands that has to fit. It goes on last, over
    // everything the application drew.
    SDL2CirclePresentCmd cursor;
    const bool has_cursor = cursor_command(&cursor);
    const unsigned extra = has_cursor ? 1u : 0u;

    // A frame still short enough to cross as a list has not been composed
    // anywhere, and crosses as it was recorded. The grant does not enter into
    // it: both endings reach the same executor, which writes into the shadow
    // or the staging frame according to what was granted, and the flip is the
    // grant's business either way.
    const bool as_list =
        !ren->rasterizing
        && ren->ncmds + extra <= (unsigned)SDL2CIRCLE_PRESENT_MAX_CMDS;

    SDL2CirclePresentCmd frame[2];
    const SDL2CirclePresentCmd *out;
    unsigned nout;

    if (as_list)
    {
        out = ren->cmds;
        nout = ren->ncmds;
    }
    else
    {
        // Everything else was composed here, into the virtual framebuffer,
        // as it was drawn. That surface is the frame, at canvas resolution
        // and at the canvas origin: placing it on the panel is the executor's
        // job and not this one's.
        start_rasterizing(ren);
        memset(&frame[0], 0, sizeof(frame[0]));
        frame[0].op = SDL2CirclePresentCmd::COPY;
        frame[0].src = s_canvas_surface;
        frame[0].srcpitch = (int)s_canvas_surface_pitch;
        frame[0].sw = s_canvas_w; frame[0].sh = s_canvas_h;
        frame[0].dx = 0; frame[0].dy = 0;
        frame[0].w = s_canvas_w; frame[0].h = s_canvas_h;
        frame[0].alphamod = 255;
        out = frame;
        nout = 1;

        // The buffer just handed over belongs to the frame in flight; the
        // next frame is drawn into the other one.
        s_canvas_surface_idx ^= 1;
        s_canvas_surface = s_canvas_surface_buf[s_canvas_surface_idx];
    }

    // The pointer rides with the frame either way. There is room for it in
    // both endings by construction: a list was only chosen if it fits with
    // this command counted, and a picture is one command with a slot beside
    // it.
    if (has_cursor)
    {
        if (as_list)
            ren->cmds[nout++] = cursor;
        else
            frame[nout++] = cursor;
    }

    ren->ncmds = 0;
    ren->rasterizing = false;
    frame_scratch_next(ren);

    if (SDL2Circle_SplitActive() && SDL2Circle_ThisCore() != 0)
    {
        SDL2Circle_PresentPost(out, nout, ren->back);
        if (s_fb_halves == 2)
            ren->back ^= 1;

        // Only when the app asked for vsync: an app that did not is free to
        // keep refilling the box, and whatever the worker cannot keep up
        // with is coalesced away, same as always. An app that did ask waits
        // here for the worker to have carried this frame through VideoFlip -
        // which itself is vsync-locked - so this call paces the poster to
        // the display's real rate instead of letting it post as fast as it
        // can compute. Without this the worker is handed a fresh frame the
        // instant it finishes the last one, is never idle, and its transfer
        // to the glass is squeezed against the raster instead of started
        // into a clear frame.
        if (ren->vsync)
            SDL2Circle_PresentQuiesce();
        return;
    }

    for (unsigned i = 0; i < nout; i++)
        SDL2Circle_VideoExecCmd(&out[i], ren->back);
    SDL2Circle_VideoFlip(ren->back);

    // Only when the app asked for vsync: throttled apps pace themselves, and
    // blocking here would double-throttle.
    if (ren->vsync)
    {
        SDL2CirclePerfScope wait(SDL2CIRCLE_PERF_WAIT_VSYNC);
        ren->window->fb->WaitForVerticalSync();
    }

    // Only a grant of two halves has a second half to draw into. On a
    // single-half grant the half is not a target at all - the executor and
    // the flip both ignore it and work through the shadow - and naming half 1
    // there would address memory past the grant.
    if (s_fb_halves == 2)
        ren->back ^= 1;
}
