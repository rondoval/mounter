/*
 * legacy.h — MBR/GPT on-disk partition structures for the mounter's DISKLABELS path.
 *
 * mounter.c (behind -DDISKLABELS) #includes this. The struct/GUID definitions are
 * verbatim from the a4091-software repo
 *   https://github.com/A4091/a4091-software/blob/HEAD/legacy.h
 *   (Copyright 2022-2023 Stefan Reinauer & Chris Hooper, BSD-2-Clause),
 * plus the two primitives the a4091 build provided from its own headers —
 * <stdint.h> (uint64_t, for the 64-bit GPT LBAs) and the __bswap* byte-swaps
 * (the gcc builtins).
 */
#ifndef MOUNTER_LEGACY_H
#define MOUNTER_LEGACY_H

#include <stdint.h>   /* uint64_t (GPT 64-bit LBA fields) */

#ifndef __bswap16
#define __bswap16(x) __builtin_bswap16(x)
#endif
#ifndef __bswap32
#define __bswap32(x) __builtin_bswap32(x)
#endif
#ifndef __bswap64
#define __bswap64(x) __builtin_bswap64(x)
#endif

typedef uint64_t ULLONG;

typedef struct { /* RFC4122 */
	union {
		struct {
			ULONG time_low;
			UWORD time_mid;
			UWORD time_high_and_version;
			UBYTE clock_seq_high_and_reserved;
			UBYTE clock_seq_low;
			UBYTE node[6];
		} UUID;
		UBYTE raw[16];
	} u;
} __attribute__((packed)) GUID;

struct mbr_partition {
	UBYTE  status;
	UBYTE  f_head;
	UBYTE  f_sect;
	UBYTE  f_cyl;
	UBYTE  type;
	UBYTE  l_head;
	UBYTE  l_sect;
	UBYTE  l_cyl;
	ULONG  f_lba;
	ULONG  num_sect;
} __attribute__((packed));

struct mbr {
	UBYTE  bootcode[424];
	GUID   boot_guid;
	ULONG  disk_id;
	UBYTE  magic[2];
	struct mbr_partition part[4];
	UBYTE  sig[2];
} __attribute__((packed));

struct gpt {
	UBYTE  signature[8];
	ULONG  revision;
	ULONG  size;
	ULONG  header_crc32;
	ULONG  reserved_zero;
	ULLONG my_lba;
	ULLONG alternate_lba;
	ULLONG first_usable_lba;
	ULLONG last_usable_lba;
	GUID   disk_uuid;
	ULLONG entries_lba;
	ULONG  number_of_entries;
	ULONG  size_of_entry;
	ULONG  entries_crc32;
} __attribute__((packed));

struct gpt_partition {
	GUID partition_type;
	GUID unique_partition;
	ULLONG first_lba;
	ULLONG last_lba;
	ULLONG attribute_flags;
	USHORT partition_name[36];
} __attribute__((packed));

#endif /* MOUNTER_LEGACY_H */
