/*
 * bootpoint.c -- autoboot DiagArea + BootPoint for volumes mounted before DOS.
 *
 * RKRM (Libraries, "ROM Based and Autoboot Drivers" -> Events At BOOT Time):
 * the boot point "will FindResident() the dos.library, and jump to its RT_INIT vector",
 * and "if successful, should not return" -- returning is how it reports failure, after
 * which strap restores the mount list and tries the next node.
 *
 * Position-independent (PC-relative string, SysBase from absolute $4) and
 * entirely in .text, so it is ROM-clean and needs no copy into RAM.
 */

asm(
    "	.text\n"
    "	.globl	_psd_diag_area\n"
    "_LVOFindResident = -96\n"
    "RT_INIT          = 22\n"
    "DAC_CONFIGTIME   = 0x10\n"
    "| struct DiagArea -- libraries/configregs.h; both code offsets are byte offsets\n"
    "| from the start of the area.  Only da_BootPoint is load-bearing here: da_Size,\n"
    "| da_DiagPoint, da_Name and da_Config's bus-width bits feed expansion.library's\n"
    "| diag copy, which never runs for an area already in memory.  strap tests bit 4\n"
    "| of da_Config on its own.\n"
    "_psd_diag_area:\n"
    "	dc.b	DAC_CONFIGTIME			| da_Config\n"
    "	dc.b	0				| da_Flags\n"
    "	dc.w	diag_end-_psd_diag_area		| da_Size\n"
    "	dc.w	0				| da_DiagPoint -- no diagnostics\n"
    "	dc.w	boot_entry-_psd_diag_area	| da_BootPoint\n"
    "	dc.w	diag_name-_psd_diag_area	| da_Name\n"
    "	dc.w	0				| da_Reserved01\n"
    "	dc.w	0				| da_Reserved02\n"
    "boot_entry:\n"
    "	lea	(dos_name,pc),a1\n"
    "	movea.l	4,a6				| SysBase (strap passes it in a6 too)\n"
    "	jsr	_LVOFindResident(a6)\n"
    "	tst.l	d0\n"
    "	beq.s	boot_failed\n"
    "	movea.l	d0,a0\n"
    "	movea.l	RT_INIT(a0),a0\n"
    "	jmp	(a0)				| into dos.library -- does not return\n"
    "boot_failed:\n"
    "	rts\n"
    "dos_name:\n"
    "	.asciz	\"dos.library\"\n"
    "diag_name:\n"
    "	.asciz	\"USB mass storage\"\n"
    "	.even\n"
    "diag_end:\n");
