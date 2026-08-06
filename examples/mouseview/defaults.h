//
// defaults.h — this example's side of the patchable-defaults block.
//
// The block is a fixed region inside the kernel image that anything holding
// the image before it boots — a build step, a network loader, a boot picker
// — can write a line of text into. Without one, a loader has nowhere to put
// a switch and the image can only ever run the way it was built.
//
// The block's layout is a shared interface, in defaultsblock.h. This header
// only says how this example consumes it.
//
// Arguments beginning `--rapi-` belong to the kernel. This example has no
// program to pass anything else on to, so every other token is reported and
// ignored rather than silently dropped: on a bench, being told what was
// stamped is the whole value of the block.
//
#ifndef _mouseview_defaults_h
#define _mouseview_defaults_h

// Read the block, log what it carries, and act on the kernel's own switches.
// Called once, from core 0, after the logger is up — the block is only worth
// reading when there is somewhere to report it.
void DefaultsApply(void);

// Set by --rapi-perf=<seconds> in the block.
extern "C" unsigned rapi_perf_seconds;

#endif
