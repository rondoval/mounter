| bootpoint.S -- autoboot DiagArea + BootPoint for volumes mounted before DOS.
|
| RKRM (Libraries, "ROM Based and Autoboot Drivers" -> Events At BOOT Time):
| the boot point "will FindResident() the dos.library, and jump to its RT_INIT vector",
| and "if successful, should not return" -- returning is how it reports failure, after
| which strap restores the mount list and tries the next node.
|
| Position-independent (PC-relative string, SysBase from absolute $4) and entirely in
| .text, so it is ROM-clean and needs no copy into RAM.
|
| Assembly rather than a file-scope asm() in a .c: nothing here needs the C compiler --
| no offsetof() fed through "i" operands -- and a symbol defined only inside an asm string
| is invisible to LTO's symbol table, so as C this file had to be held out of LTO by hand
| (verified: it linked with "undefined reference to psd_diag_area" otherwise).  A .S is
| never LTO'd in the first place, so there is nothing to state in the CMakeLists.

	.text
	.globl	_psd_diag_area

_LVOFindResident = -96
RT_INIT          = 22
DAC_CONFIGTIME   = 0x10

| struct DiagArea -- libraries/configregs.h; both code offsets are byte offsets from the
| start of the area.  Only da_BootPoint is load-bearing here: da_Size, da_DiagPoint,
| da_Name and da_Config's bus-width bits feed expansion.library's diag copy, which never
| runs for an area already in memory.  strap tests bit 4 of da_Config on its own.
_psd_diag_area:
	dc.b	DAC_CONFIGTIME			| da_Config
	dc.b	0				| da_Flags
	dc.w	diag_end-_psd_diag_area		| da_Size
	dc.w	0				| da_DiagPoint -- no diagnostics
	dc.w	boot_entry-_psd_diag_area	| da_BootPoint
	dc.w	diag_name-_psd_diag_area	| da_Name
	dc.w	0				| da_Reserved01
	dc.w	0				| da_Reserved02
boot_entry:
	lea	(dos_name,pc),a1
	movea.l	4,a6				| SysBase (strap passes it in a6 too)
	jsr	_LVOFindResident(a6)
	tst.l	d0
	beq.s	boot_failed
	movea.l	d0,a0
	movea.l	RT_INIT(a0),a0
	jmp	(a0)				| into dos.library -- does not return
boot_failed:
	rts
dos_name:
	.asciz	"dos.library"
diag_name:
	.asciz	"USB mass storage"
	.even
diag_end:
