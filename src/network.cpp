//
// network.cpp - circle-newlib's network prerequisite. circle_glue.h asks a
// system for two things: src/stdio.cpp does the first, this does the second.
// Without it getaddrinfo asserts on its first line and stops the board.
//
// What comes up is a stack, not a network: no address, no DHCP, no wait for a
// link, nothing on the wire. That is enough for a name lookup to fail cleanly
// rather than halt - but it makes the socket calls reachable, not safe, from
// an application core. Nothing marshals them, and each one ends in a
// scheduler yield that belongs to core 0; docs/CORE-SPLIT.md has why that
// matters, and what files do instead.
//
#include <SDL2/SDL_circle.h>
#include "sdl2circle.h"

#include <circle/net/netsubsystem.h>
#include <circle_glue.h>

#include <new>

namespace
{

// This library's own record of what it made. CNetSubSystem::Get() stops the
// machine rather than reporting an absence, so Circle cannot be asked - the
// position CCPUThrottle is in (src/hardware.cpp), not the one USB is in.
bool s_bUp = false;

// Placement new, for the reason the CPU throttle is: the constructor builds a
// MAC driver and receive buffers, and a static would run before the heap.
alignas(CNetSubSystem) u8 s_NetStore[sizeof(CNetSubSystem)];

}   // namespace

// The object exists; it is never started.
//
// What halted the board was a null pointer, not an absent network:
// circle-newlib's getaddrinfo asserts on it and its socket() dereferences it.
// CGlueNetworkInit does nothing but store the address, so constructing the
// subsystem and handing it over is the whole of the fix. Everything those two
// calls reach - the configuration, the transport layer - are members, alive
// from the constructor.
//
// Initialize() is deliberately not called. It brings the MAC up and starts a
// CNetTask, and a running stack is one that can put a packet on somebody's
// network or block waiting for one. Nothing here wants either. Left unstarted,
// a name lookup finds no DNS server and fails before it makes a socket.
void SDL2Circle_NetworkInit(void)
{
    if (s_bUp)
        return;
    s_bUp = true;

    CNetSubSystem *pNet = new (s_NetStore) CNetSubSystem(
        0, 0, 0, 0, 0 /* no hostname: nothing here announces one */);

    CGlueNetworkInit(*pNet);

    SDL2Circle_Log("sdl2net", SDL2CIRCLE_LOG_NOTICE,
                   "stack present, not started: a name lookup fails, it does not halt");
}
