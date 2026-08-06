//
// defaultsblock.h — the patchable-defaults block: a SHARED ABI, copied here.
//
// PROVENANCE. This file originates in the rapi-bootloader project, as
// rapi-bootloader/defaultsblock/defaultsblock.h, where it is also consumed
// by pi-mame. It is reproduced here byte-compatibly so this kernel can be
// stamped by the same network loader.
//
// IT IS AN INTERFACE, NOT OUR CODE. The network loader writes these bytes
// into a pushed kernel image blindly, at a fixed offset, without knowing
// which project the image belongs to. Every producer and every consumer
// must agree on the layout to the byte. Change anything here — the magic,
// the field order, the widths, the offset — and the loader will keep
// stamping images that no longer parse, for one consumer only, with no
// error anywhere. If the ABI genuinely needs to change, it changes at its
// origin and every consumer is updated together.
//
// The magic spells the name of the project that first defined the block.
// That is history, not ownership; it is part of the byte layout and stays.
//
// WHAT THIS KERNEL DOES WITH IT: see defaults.cpp. The block ships empty,
// and an empty block asks for nothing, so an unstamped image boots exactly
// as it would if none of this existed.
//
// PatchDefaults and PatchResultString below are the WRITER's half of the
// interface and belong to whatever holds the image before it boots. This
// example only ever reads its own block, so it declares them and implements
// neither; nothing here references them.
//
#ifndef _defaultsblock_h
#define _defaultsblock_h

#include <circle/types.h>
#include <circle/macros.h>

// Image offset (not runtime address) of the block.
#define DEFAULTS_BLOCK_OFFSET	0x800

// The seatbelt magic. Verified at offset before any writer touches a byte.
#define DEFAULTS_MAGIC0		'P'
#define DEFAULTS_MAGIC1		'M'
#define DEFAULTS_MAGIC2		'8'
#define DEFAULTS_MAGIC3		'D'

// The buffer size the first ABI revision ships. The authoritative capacity
// for any given image is the block's own Capacity field — a writer never
// assumes this constant, it reads Capacity and enforces against it.
#define DEFAULTS_BUFFER_BYTES	512

struct TDefaultsBlock
{
	char	Magic[4];			// 'P','M','8','D'
	u16	Capacity;			// bytes available in Text[]
	u16	Length;				// bytes used in Text[], excluding NUL
	char	Text[DEFAULTS_BUFFER_BYTES];	// NUL-terminated argv string
}
PACKED;

enum TPatchResult
{
	PatchOK,		// magic verified, string fit, written
	PatchImageTooSmall,	// image cannot contain the block header
	PatchNoMagic,		// magic absent at offset — write refused (seatbelt)
	PatchTooLong		// string longer than the block's Capacity allows
};

// Verify the magic at DEFAULTS_BLOCK_OFFSET inside a staged kernel image,
// enforce the string length against the block's own Capacity field, and —
// only if both hold — write the NUL-terminated string into Text[] and update
// Length. The buffer never trusts its writer: an absent magic or an
// over-long string is refused, nothing is written, and the reason returned.
TPatchResult PatchDefaults (u8 *pImage, size_t nImageSize, const char *pString);

// Human-readable form of a TPatchResult, for logging.
const char *PatchResultString (TPatchResult Result);

#endif
