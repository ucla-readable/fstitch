/* Simple linker script for KudOS user-level programs.
   See the GNU ld 'info' manual ("info ld") to learn the syntax. */

OUTPUT_FORMAT("elf32-i386", "elf32-i386", "elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
	/* Load programs at this address: "." means the current address */
	. = 0x800020;

	.text : {
		*(.text .stub .text.* .gnu.linkonce.t.*)
	}

	PROVIDE(etext = .);	/* Define the 'etext' symbol to this value */

	.rodata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
	}

	/* Adjust the address for the data segment to the next page */
	. = ALIGN(0x1000);

	PROVIDE(sctors = .);

	.ctors : {
		KEEP(*(SORT(.ctors.*)))
		KEEP(*(.ctors))
	}

	PROVIDE(ectors = .);

	.data : {
		*(.data)
	}

	PROVIDE(edata = .);

	.bss : {
		*(.bss)
	}

	PROVIDE(end = .);

#ifdef USE_STABS
	.stab_info 0x200000 : {
		LONG(__STAB_BEGIN__);
		LONG(__STAB_END__);
		LONG(__STABSTR_BEGIN__);
		LONG(__STABSTR_END__);
	}

	.stab : {	/* Include debugging at known address */
		__STAB_BEGIN__ = DEFINED(__STAB_BEGIN__) ? __STAB_BEGIN__ : .;
		*(.stab);
		__STAB_END__ = DEFINED(__STAB_END__) ? __STAB_END__ : .;
		BYTE(0)		/* Convince linker to allocate */
	}

	.stabstr : {
		__STABSTR_BEGIN__ = DEFINED(__STABSTR_BEGIN__) ? __STABSTR_BEGIN__ : .;
		*(.stabstr);
		__STABSTR_END__ = DEFINED(__STABSTR_END__) ? __STABSTR_END__ : .;
		BYTE(0)		/* Convince linker to allocate */
	}
#endif

	/DISCARD/ : {
		*(.eh_frame .note.GNU-stack)
	}
}
