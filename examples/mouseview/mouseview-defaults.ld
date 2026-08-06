/*
 * mouseview-defaults.ld — this example's linker script.
 *
 * Derived from circle-libsdl2's sdl-app.ld (itself from Circle's circle.ld,
 * GPLv3, (C) R. Stange), and remains GPLv3. It differs from that script in
 * the .init layout only, to make room for the patchable-defaults block:
 *
 *   image offset 0x000   four bytes: a branch over everything below, to
 *                        Circle's startup. The image still enters here.
 *   0x004 .. 0x7ff       reserved. Room for boot furniture that may be
 *                        wanted later without moving the block.
 *   image offset 0x800   the defaults block: magic, capacity, length and
 *                        512 bytes of text. This offset is the interface
 *                        every pre-boot writer knows; it cannot move.
 *   after the block      Circle's startup, pushed along by the above.
 *
 * The .tbss-follows-.tdata rule from sdl-app.ld is kept: binutils 2.44 and
 * later refuse to map a thread-local segment from sections that are not
 * adjacent, and the C++ threading support carries thread-local data.
 *
 * The ASSERTs are half of the build's guarantee: a link missing the block,
 * or placing it anywhere but 0x800, fails here rather than producing an
 * image the loader would quietly corrupt. The other half checks the bytes
 * of the produced image, in the Makefile.
 */

ENTRY(_mouseview_entry)

SECTIONS
{
	.init : {
		KEEP(*(.mouseview.entry))

		/* Reserved, then the block. Inside a section body the location
		   counter is an offset from the start of that section, which
		   is the start of the image. */
		. = 0x800;

		KEEP(*(.mouseview.defaults))

		*(.init)
	}

	ASSERT(DEFINED(_mouseview_entry), "mouseview: entry trampoline missing — link defaults.o")
	ASSERT(DEFINED(_mouseview_defaults), "mouseview: defaults block missing — link defaults.o")
	ASSERT(_mouseview_entry == 0x80000, "mouseview: the trampoline is not at the image start")
	ASSERT(_mouseview_defaults == 0x80800, "mouseview: the defaults block is not at image offset 0x800")

	.text : {
		*(.text*)

		_etext = .;
	}

	.rodata : {
		*(.rodata*)
	}

	.init_array : {
		__init_start = .;

		KEEP(*(.init_array*))

		__init_end = .;
	}

	.ARM.exidx : {
		__exidx_start = .;

		*(.ARM.exidx*)

		__exidx_end = .;
	}

	.eh_frame_hdr : {
		__eh_frame_hdr_start = .;

		*(.eh_frame_hdr) *(.eh_frame_hdr.*)

		__eh_frame_hdr_end = .;
	}

	.eh_frame : {
		__eh_frame_start = .;

		*(.eh_frame) *(.eh_frame.*)

		__eh_frame_end = .;
	}

	.data : {
		*(.data*)
	}

	.tdata : ALIGN(8) {
		__tdata_start = .;

		*(.tdata .tdata.*)

		__tdata_end = .;
	}

	/* pi-mame: .tbss must directly follow .tdata — binutils 2.44+ refuses
	   to map a PT_TLS segment from non-adjacent TLS sections. */
	.tbss (NOLOAD) : ALIGN(8) {
		__tbss_start = .;

		*(.tbss .tbss.*)

		__tbss_end = .;
	}

	.bss : {
		__bss_start = .;

		*(.bss*)
		*(COMMON)

		__bss_end = .;
	}

	_end = .;

	/* Nothing may sit past .bss. Circle's startup zeroes exactly
	   __bss_start..__bss_end, so a section the script did not place — an
	   orphan the linker put wherever it liked — landing after .bss would
	   never be cleared, and whatever it holds would be read as whatever
	   the last boot left there. That is silent, and it is data, so it
	   would surface as a wrong number rather than a crash. This is the
	   one assertion that catches a section going astray, as opposed to
	   the trampoline and the block being where they were put. */
	ASSERT(_end == __bss_end, "mouseview: something is placed after .bss and would never be zeroed")
}
