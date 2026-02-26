OUTPUT_ARCH(loongarch)
ENTRY(_start)

/*
 * By default, ld may put all sections into a single PT_LOAD segment, which
 * results in RWX permissions and triggers warnings.
 *
 * Define explicit program headers so:
 * - .text is RX
 * - .rodata is R
 * - .data/.bss are RW
 */
PHDRS
{
	text PT_LOAD FLAGS(5); /* PF_R | PF_X */
	data PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS
{
	/* Keep clear of the 0..1MiB boot info/FDT area used by QEMU's direct boot. */
	. = 0x00200000;

	.text : ALIGN(16)
	{
		*(.text.start)
		*(.text*)
	} :text

	.rodata : ALIGN(16)
	{
		*(.rodata*)
	} :text

	.data : ALIGN(16)
	{
		*(.data*)
	} :data

	.bss (NOLOAD) : ALIGN(16)
	{
		*(.bss*)
		*(COMMON)
	} :data

	_end = .;
}
