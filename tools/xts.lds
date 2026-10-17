/* XTS console link script: libxenon's devkitxenon app.lds (/usr/local/xenon/app.lds) plus the XTS registration
 * sections. --gc-sections is used, so the registration sections are KEPT and their bounds defined explicitly
 * (src/core/registry.c, src/model/model.c read __start_/__stop_). .xts_testwords holds deliberate test words (data,
 * excluded from tools/check_illegal.py). */
ENTRY(_start)
SECTIONS
{
	. = 0x80000000;
	.text : {  *(.except*); *(.pagetable); pagetable_end = .; *(.init*); *(.fini*); *(.text*); }
	.elfldr : { elfldr_start = .;*(.elfldr); elfldr_end = .;}

	.data : { *(.data*) }
	.sdata : { *(.sdata*) }
	.rodata : { *(.rodata*)}

	.xts_reg ALIGN(8) : {
		__start_xts_fam = .;   KEEP(*(xts_fam))   __stop_xts_fam = .;
		__start_xts_axis = .;  KEEP(*(xts_axis))  __stop_xts_axis = .;
		__start_xts_model = .; KEEP(*(xts_model)) __stop_xts_model = .;
		__start_xts_vocab = .; KEEP(*(xts_vocab)) __stop_xts_vocab = .;
	}
	.xts_testwords ALIGN(4) : { xts_testwords_start = .; KEEP(*(.xts_testwords)) xts_testwords_end = .; }

	.ctors : { __CTOR_LIST__ = .; *(.ctors); __CTOR_END__ = .; }
	.dtors : {__DTOR_LIST__ = .; *(.dtors); __DTOR_END__ = .; }

	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame ALIGN(0x4): { PROVIDE (__eh_frame_start = .); KEEP (*(.eh_frame)) LONG (0); }
	.gcc_except_table :  { *(.gcc_except_table .gcc_except_table.*) }

	bss_start = .;
	.bss : { *(.bss.beginning.lower); *(.bss.beginning.upper); *(.bss*) }
	.sbss : { *(.sbss .sbss.*) }
	bss_end = .;

	heap_begin = .;
	. = 0x9E000000;
	__libc_stack_end = . ;
	heap_end = .;
	. = 0xa0000000;
}
