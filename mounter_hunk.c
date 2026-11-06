// SPDX-License-Identifier: BSD-2-Clause
// mounter_hunk.c — filesystems loaded off the disk: the RDB's LoadSegBlock chain
// read as a stream, and the hunk loader that relocates the load file it holds.
//
// Part of the generic autoboot/automount mounter; see LICENSE for the
// copyright and licence covering this file.
#include "mounter_internal.h"

// Longs ahead of lsb_LoadData in a LoadSegBlock: ID, SummedLongs, ChkSum,
// HostID, Next.
#define LSEG_HEADER_LONGS 5

// Hardening caps against corrupt/hostile on-disk metadata. The caps keep the
// (longs + 2) * 4 byte allocations below the 32-bit wrap point.
#define MAX_HUNKS          4096         // hunks in one loaded filesystem
#define MAX_HUNK_LONGS     (16UL * 1024 * 1024 / sizeof(ULONG))  // 16 MB per hunk
// LSEG blocks read in one load: far more than any filesystem needs, and the end
// of a chain that links back on itself.
#define MAX_LSEG_BLOCKS    65536

// The RDB's LoadSegBlock chain, read as a stream of longs and words.
struct LSegStream
{
	struct MountData *md;           /* for mnt_read_rdb_block() */
	struct LoadSegBlock *block;     /* the LSEG block being read, in a scratch sector */
	ULONG nextBlock;                /* the block number after it; RDB_END: the chain ends */
	ULONG count;                    /* longs of data in block */
	ULONG pos;                      /* the next one to read; pos == count: block used up */
	ULONG blocksLeft;               /* blocks this load may still read */
	UWORD pendingWord;              /* the second half of a long that lseg_read_word() split */
	BOOL hasPendingWord;
};

/**
 * lseg_read_longs() - Read @longs longs from the LSEG chain into @data.
 * @ls:    the stream; its position advances, and the next LSEG block is read
 *         into ls->block (mnt_read_rdb_block() with IDNAME_LOADSEG) when the
 *         current one is used up
 * @longs: how many longs to read; 0 reads nothing
 * @data:  receives @longs longs, and must hold that many; NULL to skip them
 *
 * Long-aligned only: a pending word from lseg_read_word() is dropped first. Each
 * LSEG block holds lsb_SummedLongs - 5 longs of data: the checksummed size,
 * which mnt_read_rdb_block() has bounded by the block size. struct
 * LoadSegBlock's lsb_LoadData[123] is the 512-byte case only.
 *
 * Return: TRUE when all @longs were read; FALSE when the chain ends early
 * (lsb_Next RDB_END), a block fails to read, is not a valid LSEG block (ID,
 * checksum) or carries no data, or the load has read MAX_LSEG_BLOCKS blocks,
 * with @data partly written
 */
static BOOL lseg_read_longs(struct LSegStream *ls, ULONG longs, ULONG *data)
{
	ls->hasPendingWord = FALSE;
	for (ULONG got = 0; got < longs; ) {
		if (ls->pos < ls->count) {
			ULONG value = ls->block->lsb_LoadData[ls->pos++];
			if (data)
				data[got] = value;
			got++;
			continue;
		}
		if (ls->nextBlock == RDB_END || ls->blocksLeft == 0) {
			dbg("lseg_read_longs: the chain ends early\n");
			return FALSE;
		}
		ls->blocksLeft--;
		if (!mnt_read_rdb_block((UBYTE *)ls->block, ls->nextBlock, IDNAME_LOADSEG, ls->md))
			return FALSE;
		// Every block carries data.
		if (ls->block->lsb_SummedLongs <= LSEG_HEADER_LONGS)
			return FALSE;
		ls->count = ls->block->lsb_SummedLongs - LSEG_HEADER_LONGS;
		ls->pos = 0;
		ls->nextBlock = ls->block->lsb_Next;
	}
	return TRUE;
}

/**
 * lseg_read_long() - Read one long from the LSEG chain.
 * @ls:   the stream
 * @data: receives the long; written only on success
 *
 * With a word pending from lseg_read_word() the long straddles two stream longs:
 * it is the pending word and the high word of the next long, whose low word is
 * left pending in turn.
 *
 * Return: TRUE on success; FALSE as for lseg_read_longs()
 */
static BOOL lseg_read_long(struct LSegStream *ls, ULONG *data)
{
	if (!ls->hasPendingWord)
		return lseg_read_longs(ls, 1, data);

	ULONG high = ls->pendingWord;
	ULONG next;
	if (!lseg_read_longs(ls, 1, &next))     // drops the pending word
		return FALSE;
	*data = (high << 16) | (next >> 16);
	ls->pendingWord = (UWORD)next;
	ls->hasPendingWord = TRUE;
	return TRUE;
}

/**
 * lseg_read_word() - Read one word from the LSEG chain.
 * @ls:   the stream
 * @data: receives the word, zero-extended; left as it was on failure
 *
 * The pending word if there is one; else the high word of the next long, whose
 * low word is left pending for the next read.
 *
 * Return: TRUE on success, including a pending word served; FALSE as for
 * lseg_read_longs()
 */
static BOOL lseg_read_word(struct LSegStream *ls, ULONG *data)
{
	if (ls->hasPendingWord) {
		*data = ls->pendingWord;
		ls->hasPendingWord = FALSE;
		dbg("lseg_read_word 2/2 %08lx\n", *data);
		return TRUE;
	}
	ULONG whole;
	if (!lseg_read_longs(ls, 1, &whole))
		return FALSE;
	*data = whole >> 16;
	ls->pendingWord = (UWORD)whole;
	ls->hasPendingWord = TRUE;
	dbg("lseg_read_word 1/2 %08lx\n", *data);
	return TRUE;
}

/**
 * lseg_read_value() - Read one long or one word from the LSEG chain.
 * @ls:   the stream
 * @word: TRUE to read a word (lseg_read_word()), FALSE a long (lseg_read_long())
 * @data: receives the value, a word zero-extended
 *
 * Return: TRUE on success; FALSE as for lseg_read_longs()
 */
static BOOL lseg_read_value(struct LSegStream *ls, BOOL word, ULONG *data)
{
	return word ? lseg_read_word(ls, data) : lseg_read_long(ls, data);
}

// One hunk of the load file: its size in longs, and its memory, just past the
// two longs LoadSeg() puts in front of a segment.
struct Hunk
{
	ULONG longs;
	ULONG *data;
};

/**
 * read_header() - Read a load file's HUNK_HEADER.
 * @ls:        the stream, at the start of the load file
 * @firstHunk: out: the first hunk's number
 * @total:     out: the number of hunks, 1..MAX_HUNKS
 *
 * The header lists the resident libraries to open first (a filesystem has none),
 * the size of the hunk table, the numbers of the first and last hunk to load,
 * and then one size long per hunk to load. The hunk table is the loader's map
 * from hunk number to hunk memory: relocations name their target by number.
 *
 * Its size counts every hunk number in the file, overlays included, so it
 * exceeds last + 1 only in an overlay file, which a filesystem stored in an RDB
 * never is. The table built here covers first..last, the hunks loaded, and a
 * relocation into any other hunk is refused; the size serves only to check
 * that the hunks to load fit in it.
 *
 * Return: TRUE for such a header; FALSE if the data is not a HUNK_HEADER, names
 * a library, has an empty hunk table or one that does not hold the last hunk,
 * has hunk numbers that are negative, out of order or span more than MAX_HUNKS,
 * or a read fails
 */
static BOOL read_header(struct LSegStream *ls, ULONG *firstHunk, ULONG *total)
{
	ULONG value, tableSize, first, last;

	if (!lseg_read_long(ls, &value) || value != HUNK_HEADER)
		return FALSE;
	if (!lseg_read_long(ls, &value) || value != 0)  // no resident library names
		return FALSE;
	if (!lseg_read_long(ls, &tableSize) || tableSize == 0)
		return FALSE;
	if (!lseg_read_long(ls, &first) || !lseg_read_long(ls, &last))
		return FALSE;
	// Hunk numbers are non-negative on disk; the top bit set is corrupt data.
	if (((first | last) & 0x80000000UL) || first > last)
		return FALSE;
	// Hunk numbers index the hunk table.
	if (last >= tableSize)
		return FALSE;
	// A huge span would overflow the hunk table's allocation.
	if (last - first + 1 > MAX_HUNKS) {
		dbg("Too many hunks (%lu)\n", last - first + 1);
		return FALSE;
	}
	dbg("first hunk %lu, last hunk %lu\n", first, last);
	*firstHunk = first;
	*total = last - first + 1;
	return TRUE;
}

/**
 * alloc_hunks() - Allocate every hunk the header's size table lists.
 * @ls:    the stream, at the size table
 * @hunks: zeroed, @total entries; filled with each hunk's size and memory
 * @total: the number of hunks
 *
 * Each size long may carry HUNKF_CHIP (MEMF_CHIP), or HUNKF_CHIP and HUNKF_FAST
 * together, when the long that follows holds the memory flags. Every hunk is
 * MEMF_CLEAR, with two longs in front as LoadSeg() leaves a segment: the
 * allocation's size in bytes, and the link, a BPTR to the next hunk's link long
 * (0 after the last).
 *
 * Return: TRUE if every hunk was allocated; FALSE if a read or an allocation
 * fails or a hunk is larger than MAX_HUNK_LONGS, the hunks allocated so far
 * linked from @hunks[0] as a segment list, for mnt_unload_lseg()
 */
static BOOL alloc_hunks(struct LSegStream *ls, struct Hunk *hunks, ULONG total)
{
	struct ExecBase *SysBase = ls->md->SysBase;
	ULONG *prevLink = NULL;

	for (ULONG i = 0; i < total; i++) {
		ULONG longs, memFlags = MEMF_PUBLIC;
		if (!lseg_read_long(ls, &longs))
			return FALSE;
		if ((longs & (HUNKF_CHIP | HUNKF_FAST)) == (HUNKF_CHIP | HUNKF_FAST)) {
			if (!lseg_read_long(ls, &memFlags))
				return FALSE;
		} else if (longs & HUNKF_CHIP) {
			memFlags |= MEMF_CHIP;
		}
		longs &= ~(HUNKF_CHIP | HUNKF_FAST);
		// Cap the size so (longs + 2) * 4 cannot wrap the 32-bit allocation (a
		// masked size can still be up to 0x3FFFFFFF longs).
		if (longs > MAX_HUNK_LONGS) {
			dbg("Hunk too large (%lu longs)\n", longs);
			return FALSE;
		}
		ULONG *mem = AllocMem((longs + 2) * sizeof(ULONG), memFlags | MEMF_CLEAR);
		if (!mem)
			return FALSE;
		dbg("hunk %lu: ptr 0x%08lx, %lu longs, memory flags %08lx\n", i, (ULONG)mem, longs, memFlags);
		// The two longs LoadSeg() puts in front of a segment: the allocation's
		// size in bytes, and the link to the next segment, 0 for the last.
		mem[0] = (longs + 2) * sizeof(ULONG);
		mem[1] = 0;
		// Chain the segment list as it grows: the previous hunk's link becomes
		// a BPTR to this hunk's link long (a segment list points at the links,
		// not at the hunks' data), and this link is the one the next hunk fills
		// in. So a load cut short still leaves a whole list to free.
		if (prevLink)
			*prevLink = (ULONG)MKBADDR(&mem[1]);
		prevLink = &mem[1];
		hunks[i].longs = longs;
		hunks[i].data = mem + 2;
	}
	return TRUE;
}

/**
 * add_reloc() - Add a hunk's address to the long at one relocation offset.
 * @at:   the long to relocate, inside a loaded hunk; may be at an odd address
 * @addr: the address of the hunk it points into
 *
 * At an odd address the long is read and written byte by byte: the 68000 and
 * 68010 cannot access a long there.
 */
static void add_reloc(UBYTE *at, ULONG addr)
{
	if ((ULONG)at & 1) {
		ULONG value = ((ULONG)at[0] << 24) | ((ULONG)at[1] << 16) | ((ULONG)at[2] << 8) | at[3];
		value += addr;
		at[0] = (UBYTE)(value >> 24);
		at[1] = (UBYTE)(value >> 16);
		at[2] = (UBYTE)(value >>  8);
		at[3] = (UBYTE)value;
	} else {
		*(ULONG *)at += addr;
	}
}

/**
 * relocate() - Apply one relocation hunk to the hunk loaded last.
 * @ls:        the stream, just past the relocation hunk's type
 * @hunk:      the hunk loaded last, which the offsets point into
 * @hunks:     every hunk, for the targets' addresses
 * @firstHunk: the first hunk's number, which target numbers count from
 * @total:     the number of hunks
 * @shortForm: TRUE for HUNK_RELOC32SHORT and HUNK_DREL32 (words, padded to a
 *             long), FALSE for HUNK_RELOC32 (longs)
 *
 * Groups of a count, a target hunk number and that many offsets, until a count
 * of 0. HUNK_DREL32 is laid out as HUNK_RELOC32SHORT, as V37 LoadSeg() takes it
 * in a load file (dos/doshunks.h). An odd number of words ends in a pad word,
 * which is dropped.
 *
 * Return: TRUE if the hunk was applied; FALSE if a read fails, a target hunk is
 * not in the file, or an offset lies outside @hunk
 */
static BOOL relocate(struct LSegStream *ls, const struct Hunk *hunk,
                     const struct Hunk *hunks, ULONG firstHunk, ULONG total, BOOL shortForm)
{
	for (;;) {
		ULONG count, target;
		if (!lseg_read_value(ls, shortForm, &count))
			return FALSE;
		if (!count)
			break;
		if (!lseg_read_value(ls, shortForm, &target))
			return FALSE;
		target -= firstHunk;
		if (target >= total)
			return FALSE;
		dbg("HUNK_RELOC32: relocs %lu hunk %lu\n", count, target + firstHunk);
		ULONG targetAddr = (ULONG)hunks[target].data;
		while (count--) {
			ULONG offset;
			if (!lseg_read_value(ls, shortForm, &offset))
				return FALSE;
			// Guard longs == 0: (0 - 1) * 4 would wrap to a huge bound and let
			// any offset through into a zero-size hunk.
			if (hunk->longs == 0 || offset > (hunk->longs - 1) * sizeof(ULONG))
				return FALSE;
			add_reloc((UBYTE *)hunk->data + offset, targetAddr);
		}
	}
	ls->hasPendingWord = FALSE;     // the pad word of an odd count of words
	return TRUE;
}

/**
 * skip_counted() - Skip a hunk that is a count of longs and that many longs.
 * @ls: the stream, just past the hunk's type
 *
 * HUNK_NAME, HUNK_DEBUG and an advisory hunk the loader does not know have this
 * layout.
 *
 * Return: TRUE if the hunk was skipped; FALSE if a read fails or the count is
 * larger than MAX_HUNK_LONGS
 */
static BOOL skip_counted(struct LSegStream *ls)
{
	ULONG longs;

	return lseg_read_long(ls, &longs) && longs <= MAX_HUNK_LONGS &&
	       lseg_read_longs(ls, longs, NULL);
}

/**
 * skip_symbols() - Skip a HUNK_SYMBOL.
 * @ls: the stream, just past the hunk's type
 *
 * Symbols up to a count of 0: each a name (a count of longs, and that many longs)
 * and its value. The count's top byte is the symbol's type, as in HUNK_EXT
 * (EXT_SYMB, 0, here), so only its low 24 bits count.
 *
 * Return: TRUE if the hunk was skipped; FALSE if a read fails
 */
static BOOL skip_symbols(struct LSegStream *ls)
{
	for (;;) {
		ULONG longs;
		if (!lseg_read_long(ls, &longs))
			return FALSE;
		longs &= 0xFFFFFF;
		if (longs == 0)
			return TRUE;
		if (!lseg_read_longs(ls, longs + 1, NULL))      // the name, and the value
			return FALSE;
	}
}

/**
 * load_hunks() - Load every hunk's contents and apply its relocations.
 * @ls:        the stream, just past the header
 * @hunks:     every hunk, allocated
 * @firstHunk: the first hunk's number
 * @total:     the number of hunks
 *
 * HUNK_CODE, HUNK_DATA and HUNK_BSS fill the hunks in order; relocation hunks
 * apply to the hunk loaded last. HUNK_NAME, HUNK_SYMBOL and HUNK_DEBUG are
 * skipped, as LoadSeg() skips them, and so is a hunk of a type not known here
 * that is marked advisory, which V39 dos.library treats like HUNK_DEBUG
 * (dos/doshunks.h). A type's memory bits are ignored: the hunk was allocated by
 * the header's. The load ends at the HUNK_END of the last hunk, or where the
 * chain runs out after it.
 *
 * Return: TRUE if every hunk was loaded; FALSE if the chain ends before that, a
 * hunk is larger than its allocation or comes one too many, a relocation fails,
 * or a hunk type is none of those
 */
static BOOL load_hunks(struct LSegStream *ls, struct Hunk *hunks, ULONG firstHunk,
                       ULONG total)
{
	const struct Hunk *last = NULL;     // the hunk loaded last, which relocations apply to
	ULONG loaded = 0;

	for (;;) {
		ULONG raw;
		if (!lseg_read_long(ls, &raw))
			return loaded == total;
		dbg("HUNK %08lx\n", raw);
		// A type's top three bits are flags: memory (as in the header) and advisory.
		ULONG type = raw & ~(HUNKF_ADVISORY | HUNKF_CHIP | HUNKF_FAST);
		switch (type) {
		case HUNK_CODE:
		case HUNK_DATA:
		case HUNK_BSS:
		{
			ULONG longs;
			if (loaded == total || !lseg_read_long(ls, &longs))
				return FALSE;
			last = &hunks[loaded++];
			if (longs > last->longs)
				return FALSE;
			if (type != HUNK_BSS && !lseg_read_longs(ls, longs, last->data))
				return FALSE;
			break;
		}
		case HUNK_RELOC32:
		case HUNK_RELOC32SHORT:
		case HUNK_DREL32:
			if (!last || !relocate(ls, last, hunks, firstHunk, total, type != HUNK_RELOC32))
				return FALSE;
			break;
		case HUNK_END:
			if (loaded == total)
				return TRUE;
			break;
		case HUNK_NAME:
		case HUNK_DEBUG:
			if (!skip_counted(ls))
				return FALSE;
			break;
		case HUNK_SYMBOL:
			if (!skip_symbols(ls))
				return FALSE;
			break;
		default:
			if (!(raw & HUNKF_ADVISORY) || !skip_counted(ls)) {
				dbg("Unexpected HUNK %08lx\n", raw);
				return FALSE;
			}
			break;
		}
	}
}

/**
 * mnt_load_lseg() - Load and relocate a filesystem off its LoadSegBlock chain.
 * @md:    mount state; the blocks are read with mnt_read_rdb_block()
 * @block: the first LSEG block (fhb_SegListBlocks)
 * @buf:   a scratch sector for the chain's blocks
 *
 * Takes a HUNK_HEADER load file without resident library names or overlays
 * (read_header()), allocates every hunk up front (alloc_hunks()), then loads and
 * relocates them (load_hunks()). The chain is untrusted disk data: the hunk count
 * and each hunk's size are capped, and every loaded size and relocation offset is
 * checked against the allocation. Ends with CacheClearU(), since the relocated
 * filesystem is about to be executed.
 *
 * Return: the segment list, a BPTR to the first hunk's link long, for
 * fse_SegList; 0, with every hunk freed, when the chain holds no such load file,
 * a read fails before the last hunk is loaded, an allocation fails, or a hunk or
 * relocation is out of bounds
 */
BPTR mnt_load_lseg(struct MountData *md, ULONG block, UBYTE *buf)
{
	struct ExecBase *SysBase = md->SysBase;
	struct LSegStream ls = {
		.md = md, .block = (struct LoadSegBlock *)buf, .nextBlock = block,
		.blocksLeft = MAX_LSEG_BLOCKS,
	};
	ULONG firstHunk, total;

	if (!read_header(&ls, &firstHunk, &total))
		return 0;
	struct Hunk *hunks = AllocMem(total * sizeof(struct Hunk), MEMF_CLEAR);
	if (!hunks)
		return 0;

	BOOL ok = alloc_hunks(&ls, hunks, total) && load_hunks(&ls, hunks, firstHunk, total);
	BPTR seglist = hunks[0].data ? MKBADDR(hunks[0].data - 1) : 0;
	FreeMem(hunks, total * sizeof(struct Hunk));
	if (!ok) {
		dbg("reloc failed\n");
		mnt_unload_lseg(md, seglist);
		return 0;
	}
	CacheClearU();   // the relocated filesystem is about to be executed
	dbg("reloc ok, segment list 0x%08lx\n", (ULONG)BADDR(seglist));
	return seglist;
}

/**
 * mnt_unload_lseg() - Free a segment list mnt_load_lseg() built.
 * @md:      mount state
 * @seglist: the segment list; 0 accepted
 *
 * Each segment is freed by the size long in front of it, as UnLoadSeg() does,
 * but without DOS, so this works before DOS exists. A list cut short by a failed
 * load is whole as far as it goes: each hunk is linked as it is allocated.
 */
void mnt_unload_lseg(struct MountData *md, BPTR seglist)
{
	struct ExecBase *SysBase = md->SysBase;

	for (ULONG *seg = (ULONG *)BADDR(seglist); seg; ) {
		ULONG *next = (ULONG *)BADDR((BPTR)*seg);
		FreeMem(seg - 1, seg[-1]);
		seg = next;
	}
}
