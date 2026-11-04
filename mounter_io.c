
// mounter_io.c — reading the unit being scanned: blocks, raw or as checked RDB
// blocks, and the scratch sectors to read them into.
//
// Part of the generic autoboot/automount mounter; see mounter.c for the
// copyright and licence covering this file.
#include "mounter_internal.h"

#define MAX_RETRIES 3

/**
 * mnt_read_block() - Read one block of the unit being scanned, with retries.
 * @buf:   destination, at least md->blocksize bytes
 * @block: block number, in md->blocksize units; past 2^32 bytes the read is
 *         TD_READ64
 * @md:    mount state: md->request (the open unit), md->blocksize, md->flags
 *
 * Tries DoIO() up to MAX_RETRIES times, 15 with MSF_SLOW_SPINUP, with no delay
 * between attempts.
 *
 * Return: TRUE if a read succeeded; FALSE if every attempt failed
 */
BOOL mnt_read_block(UBYTE *buf, uint64_t block, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct IOExtTD *request = md->request;
	UWORD retries = (md->flags & MSF_SLOW_SPINUP) ? 15 : MAX_RETRIES;

	// Byte offsets past 4GB need TD_READ64 (io_Actual = high 32 bits)
	uint64_t offset = block * md->blocksize;
	request->iotd_Req.io_Command = (offset >> 32) ? TD_READ64 : CMD_READ;
	request->iotd_Req.io_Actual = (ULONG)(offset >> 32);
	request->iotd_Req.io_Offset = (ULONG)offset;
	request->iotd_Req.io_Data = buf;
	request->iotd_Req.io_Length = md->blocksize;
	for (UWORD i = 0; i < retries; i++) {
		if (DoIO((struct IORequest *)request) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * block_checksum() - Verify the checksum of an RDB block (RDSK, PART, FSHD, LSEG).
 * @l:         the block as read, @blocksize bytes; longword 1 holds its
 *             summed-longs count (rdb_SummedLongs, pb_SummedLongs, ...)
 * @blocksize: the block size in bytes, which bounds the count
 *
 * The block is valid when its first summed-longs longwords add up to 0 (mod
 * 2^32). The count has to cover at least the ID, the count and the checksum
 * longs: a smaller one would check nothing.
 *
 * Return: TRUE if they do; FALSE on a nonzero sum, or a count below 3 or larger
 * than the block
 */
static BOOL block_checksum(const ULONG *l, ULONG blocksize)
{
	ULONG count = l[1];
	// A block's summed-longs count can never exceed the block itself; reject
	// garbage counts so the checksum loop can't read past the sector buffer.
	if (count < 3 || count > blocksize / sizeof(ULONG))
		return FALSE;

	ULONG sum = 0;
	for (ULONG i = 0; i < count; i++)
		sum += l[i];
	if (sum) {
		dbg("Checksum error %08lx\n", sum);
		return FALSE;
	}
	return TRUE;
}

/**
 * mnt_read_rdb_block() - Read one RDB block and check its ID and checksum.
 * @buf:   destination, a scratch sector (mnt_sector_take()): long-aligned, at
 *         least md->blocksize bytes
 * @block: block number, in md->blocksize units
 * @id:    the first longword the block must carry: IDNAME_RIGIDDISK,
 *         IDNAME_PARTITION, IDNAME_FILESYSHEADER or IDNAME_LOADSEG
 * @md:    mount state, as for mnt_read_block()
 *
 * The block is read as longs in place: the sector is long-aligned and the 68k
 * reads them big-endian, as they are on the disk.
 *
 * Return: TRUE if the block was read and carries @id and a valid checksum;
 * FALSE otherwise
 */
BOOL mnt_read_rdb_block(UBYTE *buf, ULONG block, ULONG id, struct MountData *md)
{
	const ULONG *l = (const ULONG *)buf;

	if (!mnt_read_block(buf, block, md))
		return FALSE;
	return l[0] == id && block_checksum(l, md->blocksize);
}

/**
 * mnt_sector_take() - Borrow a scratch sector for the duration of one scan step.
 * @md: mount state holding the pool
 *
 * The scanners nest — block 0, then a GPT header, then the entry array; an RDSK
 * or PART block, then an FSHD block, then its LSEG chain — and each wants a
 * MAX_BLOCKSIZE buffer. Use is strictly LIFO (every buffer is released with
 * mnt_sector_drop() before its taker returns), so a stack of SECTOR_SLOTS slots
 * is enough.
 *
 * Slots are allocated on first use and live until mnt_sector_pool_free(), when
 * MountDrive() (or mnt_find_containers()) returns, so a plain FAT stick never
 * allocates the GPT path's buffers and a 10-partition RDB allocates its FSHD
 * sectors once instead of twenty times.
 *
 * Return: a MAX_BLOCKSIZE-byte MEMF_PUBLIC buffer, not cleared; NULL if all
 * SECTOR_SLOTS are taken or AllocMem() fails, and nothing is taken
 */
UBYTE *mnt_sector_take(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;

	if (md->sectorsUsed >= SECTOR_SLOTS) {
		mnt_log("Sector pool exhausted\n");
		return NULL;
	}
	UBYTE **slot = &md->sector[md->sectorsUsed];
	if (!*slot) {
		*slot = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);
		if (!*slot)
			return NULL;
	}
	md->sectorsUsed++;
	return *slot;
}

/**
 * mnt_sector_drop() - Release the most recently taken sector.
 * @md:  mount state holding the pool
 * @buf: the buffer the latest mnt_sector_take() returned
 *
 * The buffer stays allocated in its slot for the next taker. Anything but the
 * current top is a programming error: report it and leave the stack alone
 * rather than corrupt it.
 */
void mnt_sector_drop(struct MountData *md, const UBYTE *buf)
{
	if (!md->sectorsUsed || md->sector[md->sectorsUsed - 1] != buf) {
		mnt_log("Sector pool released out of order\n");
		return;
	}
	md->sectorsUsed--;
}

/**
 * mnt_sector_pool_free() - Free every slot of the scratch sector pool.
 * @md: mount state holding the pool; the MountData itself stays allocated
 *
 * FreeMem()s each allocated slot and resets the stack to empty, so it must not
 * run while a sector is still taken. MountDrive() and mnt_find_containers()
 * call it before freeing their MountData.
 */
void mnt_sector_pool_free(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;

	for (UWORD i = 0; i < SECTOR_SLOTS; i++) {
		if (md->sector[i]) {
			FreeMem(md->sector[i], MAX_BLOCKSIZE);
			md->sector[i] = NULL;
		}
	}
	md->sectorsUsed = 0;
}
