/*
 * sdl-app.ld — linker script for circle-libsdl2 applications.
 *
 * Derived from Circle's circle.ld (GPLv3, (C) R. Stange); differs in two
 * respects: .tbss directly follows .tdata, because binutils 2.44+ refuses
 * to map a PT_TLS segment from non-adjacent TLS sections; and .init reserves
 * the boot-argument defaults block (src/bootargs.cpp) at a fixed offset:
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
 * Every application linking against this library gets this for free — the
 * block and its trampoline live in the library (src/bootargs.cpp), not in
 * an application's own linker script, so there is nothing for a consumer to
 * add and nothing for one to forget.
 *
 * The ASSERTs are half of the build's guarantee: a link missing the block,
 * or placing it anywhere but 0x800, fails here rather than producing an
 * image a loader would quietly corrupt.
 */

ENTRY(_rapiboot_entry)

SECTIONS
{
	.init : {
		KEEP(*(.rapiboot.entry))

		/* Reserved, then the block. Inside a section body the location
		   counter is an offset from the start of that section, which
		   is the start of the image. */
		. = 0x800;

		KEEP(*(.rapiboot.defaults))

		*(.init)
	}

	ASSERT(DEFINED(_rapiboot_entry), "circle-libsdl2: boot-argument entry trampoline missing — link bootargs.o")
	ASSERT(DEFINED(_rapiboot_defaults), "circle-libsdl2: boot-argument defaults block missing — link bootargs.o")
	ASSERT(_rapiboot_entry == 0x80000, "circle-libsdl2: the entry trampoline is not at the image start")
	ASSERT(_rapiboot_defaults == 0x80800, "circle-libsdl2: the defaults block is not at image offset 0x800")

	.text : {
		*(.text*)

		_etext = .;
	}

	.rodata : {
		*(.rodata*)
	}

	INCLUDE sdl-app-init.ld

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
}
