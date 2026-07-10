/*
 * sdl-app.ld — linker script for circle-libsdl2 applications.
 *
 * Derived from Circle's circle.ld (GPLv3, (C) R. Stange); differs in one
 * respect: .tbss directly follows .tdata, because binutils 2.44+ refuses
 * to map a PT_TLS segment from non-adjacent TLS sections.
 */

ENTRY(_start)

SECTIONS
{
	.init : {
		*(.init)
	}

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
}
