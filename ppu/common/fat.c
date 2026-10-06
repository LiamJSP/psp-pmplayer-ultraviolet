#include <strings.h>
#ifndef stricmp
#define stricmp strcasecmp
#endif
#ifndef strnicmp
#define strnicmp strncasecmp
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <pspkernel.h>

#include "fat.h"
#include "libminiconv.h"
#include "ppa_io.h"

/*
 * Read-only FAT16/FAT32 recovery layer for PSP Memory Stick browsing.
 *
 * Important design choices:
 * - never writes to msstor:;
 * - tries backup metadata and alternate FAT copies;
 * - validates cluster chains before walking them;
 * - returns partial directory contents when safe;
 * - preserves the original firmware cross-check via sceIoDread.
 */

#ifndef FAT_ATTR_LONG_NAME
#define FAT_ATTR_LONG_NAME 0x0F
#endif

#ifndef FAT_ATTR_UNUSED_MASK
#define FAT_ATTR_UNUSED_MASK 0xC0
#endif

#define FAT_SECTOR_SIZE_PSP 512U
#define FAT_MAX_PARTITIONS 4U
#define FAT_SHORT_PATH_CAPACITY 256U

#define FAT12_CLUSTER_MAX 4085U
#define FAT16_CLUSTER_MAX 65525U

#define FAT32_ENTRY_MASK 0x0FFFFFFFU
#define FAT16_ENTRY_MASK 0x0000FFFFU
#define FAT12_ENTRY_MASK 0x00000FFFU

#define FAT_CANDIDATE_SCORE_READABLE          100
#define FAT_CANDIDATE_SCORE_ACTIVE_COPY        25
#define FAT_CANDIDATE_SCORE_ROOT_CHAIN_VALID   75
#define FAT_CANDIDATE_SCORE_ROOT_CHAIN_PARTIAL 20
#define FAT_CANDIDATE_SCORE_ENTRY_HINTS        10

#define FAT_CANDIDATE_SCORE_HARD_BAD        (-1000000)
#define FAT_CANDIDATE_SCORE_BAD_CLUSTER       (-1000)
#define FAT_CANDIDATE_SCORE_CHAIN_LOOP        (-1000)
#define FAT_CANDIDATE_SCORE_OUT_OF_RANGE      (-1000)

typedef enum {
	fat12,
	fat16,
	fat32
} fat_type_enum;

typedef struct {
	u32 *table;
	u32 entry_count;
	int score;
	u32 fat_index;
} fat_table_candidate;

static int fwVersion;
static int fatfd = -1;

static t_fat_dbr dbr;
static t_fat_mbr mbr;

static u32 *fat_table = NULL;

static u64 dbr_pos = 0;
static u64 root_pos = 0;
static u64 data_pos = 0;
static u64 bytes_per_clus = 0;

static u32 loadcount = 0;
static u32 clus_max = 0;
static u32 fat_entry_count = 0;
static u32 data_cluster_count = 0;
static u32 fat_sector_count_cached = 0;
static u32 root_dir_sector_count = 0;
static u32 active_fat_copy = 0;
static u32 partition_start_sector = 0;

static fat_type_enum fat_type = fat16;

/* ------------------------------------------------------------------------- */
/* Little-endian helpers for FAT BPB fields not exposed by fat.h.             */
/* ------------------------------------------------------------------------- */

static const u8 *fat_dbr_bytes(void)
{
	return (const u8 *)&dbr;
}

static const u8 *fat_mbr_bytes(void)
{
	return (const u8 *)&mbr;
}

static u16 fat_le16(const void *p)
{
	const u8 *b = (const u8 *)p;
	return (u16)(b[0] | (b[1] << 8));
}

static u32 fat_le32(const void *p)
{
	const u8 *b = (const u8 *)p;
	return (u32)b[0] |
	       ((u32)b[1] << 8) |
	       ((u32)b[2] << 16) |
	       ((u32)b[3] << 24);
}

static u16 fat_dbr_u16(unsigned int offset)
{
	const u8 *b = fat_dbr_bytes();
	return fat_le16(b + offset);
}

static u32 fat_dbr_u32(unsigned int offset)
{
	const u8 *b = fat_dbr_bytes();
	return fat_le32(b + offset);
}

static u16 fat_mbr_signature(void)
{
	const u8 *b = fat_mbr_bytes();
	return fat_le16(b + 510);
}

/*
 * BPB offsets used directly:
 * 11: bytes/sector
 * 13: sectors/cluster
 * 14: reserved sectors
 * 16: FAT count
 * 17: FAT12/16 root entry count
 * 19: total sectors 16
 * 22: FAT12/16 sectors/FAT
 * 32: total sectors 32
 * 36: FAT32 sectors/FAT
 * 40: FAT32 ext flags
 * 44: FAT32 root cluster
 * 50: FAT32 backup boot sector
 */
static u16 fat_bpb_bytes_per_sector(void) { return fat_dbr_u16(11); }
static u8  fat_bpb_sectors_per_cluster(void) { return fat_dbr_bytes()[13]; }
static u16 fat_bpb_reserved_sectors(void) { return fat_dbr_u16(14); }
static u8  fat_bpb_num_fats(void) { return fat_dbr_bytes()[16]; }
static u16 fat_bpb_root_entries(void) { return fat_dbr_u16(17); }
static u16 fat_bpb_total_sectors_16(void) { return fat_dbr_u16(19); }
static u16 fat_bpb_fat16_sectors_per_fat(void) { return fat_dbr_u16(22); }
static u32 fat_bpb_total_sectors_32(void) { return fat_dbr_u32(32); }
static u32 fat_bpb_fat32_sectors_per_fat(void) { return fat_dbr_u32(36); }
static u16 fat_bpb_fat32_ext_flags(void) { return fat_dbr_u16(40); }
static u32 fat_bpb_fat32_root_cluster(void) { return fat_dbr_u32(44); }
static u16 fat_bpb_fat32_backup_boot_sector(void) { return fat_dbr_u16(50); }

/* ------------------------------------------------------------------------- */
/* Basic validation helpers.                                                  */
/* ------------------------------------------------------------------------- */

static int fat_is_power_of_two_u8(u8 v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

static int fat_partition_type_is_fat(u8 id)
{
	switch (id) {
	case 0x01: /* FAT12 */
	case 0x04: /* FAT16 < 32M */
	case 0x06: /* FAT16 */
	case 0x0B: /* FAT32 CHS */
	case 0x0C: /* FAT32 LBA */
	case 0x0E: /* FAT16 LBA */
		return 1;
	default:
		return 0;
	}
}

static u32 fat_current_fat_sectors(void)
{
	if (fat_type == fat32)
		return fat_bpb_fat32_sectors_per_fat();

	return fat_bpb_fat16_sectors_per_fat();
}

static u32 fat_raw_table_size_bytes(void)
{
	return fat_current_fat_sectors() * fat_bpb_bytes_per_sector();
}

static int fat_cluster_valid_number(u32 cluster)
{
	if (cluster < 2)
		return 0;

	if (fat_entry_count != 0 && cluster >= fat_entry_count)
		return 0;

	if (data_cluster_count != 0 && cluster > data_cluster_count + 1)
		return 0;

	return 1;
}

static u32 fat_entry_mask_for_type(fat_type_enum type)
{
	if (type == fat32)
		return FAT32_ENTRY_MASK;

	if (type == fat16)
		return FAT16_ENTRY_MASK;

	return FAT12_ENTRY_MASK;
}

static u32 fat_entry_value_from_table(u32 *table,
                                      u32 entry_count,
                                      fat_type_enum type,
                                      u32 cluster)
{
	u32 value;

	if (table == NULL || cluster >= entry_count)
		return 0x0FFFFFF7U;

	value = table[cluster];
	value &= fat_entry_mask_for_type(type);

	return value;
}

static u32 fat_entry_value(u32 cluster)
{
	return fat_entry_value_from_table(fat_table,
	                                  fat_entry_count,
	                                  fat_type,
	                                  cluster);
}

static int fat_entry_is_eoc_for_type(fat_type_enum type, u32 value)
{
	if (type == fat32)
		return value >= 0x0FFFFFF8U;

	if (type == fat16)
		return value >= 0xFFF8U;

	return value >= 0x0FF8U;
}

static int fat_entry_is_bad_for_type(fat_type_enum type, u32 value)
{
	if (type == fat32)
		return value == 0x0FFFFFF7U;

	if (type == fat16)
		return value == 0xFFF7U;

	return value == 0x0FF7U;
}

static int fat_entry_is_reserved_for_type(fat_type_enum type, u32 value)
{
	if (type == fat32)
		return value >= 0x0FFFFFF0U && value <= 0x0FFFFFF6U;

	if (type == fat16)
		return value >= 0xFFF0U && value <= 0xFFF6U;

	return value >= 0x0FF0U && value <= 0x0FF6U;
}

static int fat_entry_is_data_cluster_for_table(u32 entry_count, u32 value)
{
	return value >= 2 && value < entry_count;
}

static u32 fat_entry_cluster(p_fat_entry entry)
{
	u32 cluster;

	if (entry == NULL)
		return 0;

	cluster = entry->norm.clus;

	if (fat_type == fat32)
		cluster |= ((u32)entry->norm.clus_high) << 16;

	return cluster;
}

static int fat_geometry_from_current_dbr(void)
{
	u64 total_sec;
	u64 fat_sec;
	u64 root_sec;
	u64 data_sec;
	u64 data_clus;
	u16 bytes_per_sec;
	u8 sec_per_clus;
	u8 num_fats;
	u16 reserved_sec;

	bytes_per_sec = fat_bpb_bytes_per_sector();
	sec_per_clus = fat_bpb_sectors_per_cluster();
	reserved_sec = fat_bpb_reserved_sectors();
	num_fats = fat_bpb_num_fats();

	if (bytes_per_sec != FAT_SECTOR_SIZE_PSP)
		return 0;

	if (!fat_is_power_of_two_u8(sec_per_clus))
		return 0;

	if (num_fats == 0 || num_fats > 4)
		return 0;

	if (reserved_sec == 0)
		return 0;

	total_sec = fat_bpb_total_sectors_16();
	if (total_sec == 0)
		total_sec = fat_bpb_total_sectors_32();

	fat_sec = fat_bpb_fat16_sectors_per_fat();
	if (fat_sec == 0)
		fat_sec = fat_bpb_fat32_sectors_per_fat();

	if (total_sec == 0 || fat_sec == 0)
		return 0;

	root_sec = ((u64)fat_bpb_root_entries() * 32ULL + bytes_per_sec - 1ULL) /
	           bytes_per_sec;

	if (total_sec <= reserved_sec + ((u64)num_fats * fat_sec) + root_sec)
		return 0;

	data_sec = total_sec - reserved_sec - ((u64)num_fats * fat_sec) - root_sec;
	if (data_sec == 0)
		return 0;

	data_clus = data_sec / sec_per_clus;
	if (data_clus == 0 || data_clus > 0x0FFFFFEFUL)
		return 0;

	if (data_clus < FAT12_CLUSTER_MAX) {
		fat_type = fat12;
		clus_max = 0x0FF0U;
	}
	else if (data_clus < FAT16_CLUSTER_MAX) {
		fat_type = fat16;
		clus_max = 0xFFF0U;
	}
	else {
		fat_type = fat32;
		clus_max = 0x0FFFFFF0U;
	}

	if (fat_type == fat32) {
		u32 root_cluster = fat_bpb_fat32_root_cluster();

		if (fat_bpb_root_entries() != 0)
			return 0;

		if (root_cluster < 2 || root_cluster > data_clus + 1)
			return 0;
	}
	else {
		if (fat_bpb_root_entries() == 0)
			return 0;
	}

	bytes_per_clus = (u64)sec_per_clus * bytes_per_sec;
	fat_sector_count_cached = (u32)fat_sec;
	root_dir_sector_count = (u32)root_sec;
	data_cluster_count = (u32)data_clus;

	if (fat_type == fat32) {
		data_pos = dbr_pos;
		data_pos += ((u64)reserved_sec + ((u64)num_fats * fat_sec)) *
		            bytes_per_sec;

		root_pos = data_pos +
		           ((u64)(fat_bpb_fat32_root_cluster() - 2U) *
		            bytes_per_clus);
	}
	else {
		root_pos = dbr_pos;
		root_pos += ((u64)reserved_sec + ((u64)num_fats * fat_sec)) *
		            bytes_per_sec;

		data_pos = root_pos + ((u64)fat_bpb_root_entries() *
		                       sizeof(t_fat_entry));
	}

	return 1;
}

static unsigned int fat32_first_fat_to_try(void)
{
	u16 flags;
	unsigned int active;

	if (fat_type != fat32)
		return 0;

	flags = fat_bpb_fat32_ext_flags();

	/*
	 * If mirroring is disabled, low 4 bits identify the active FAT.
	 * If mirroring is enabled, FAT 0 is conventionally primary.
	 */
	if ((flags & 0x0080U) == 0)
		return 0;

	active = flags & 0x000FU;
	if (active >= fat_bpb_num_fats())
		return 0;

	return active;
}

static void fat_reset_mount_state(void)
{
	memset(&dbr, 0, sizeof(dbr));
	dbr_pos = 0;
	root_pos = 0;
	data_pos = 0;
	bytes_per_clus = 0;
	clus_max = 0;
	fat_entry_count = 0;
	data_cluster_count = 0;
	fat_sector_count_cached = 0;
	root_dir_sector_count = 0;
	active_fat_copy = 0;
	partition_start_sector = 0;
	fat_type = fat16;
}

/* ------------------------------------------------------------------------- */
/* DBR / backup DBR selection.                                                */
/* ------------------------------------------------------------------------- */

static int fat_read_dbr_at_sector(SceUID fd, u32 sector)
{
	SceOff offset = (SceOff)sector * FAT_SECTOR_SIZE_PSP;

	if (ppa_io_read_exact_at_retry(fd, offset, &dbr, sizeof(dbr)) !=
	    (int)sizeof(dbr)) {
		return 0;
	}

	dbr_pos = offset;
	partition_start_sector = sector;

	return fat_geometry_from_current_dbr();
}

static int fat_try_backup_dbr_for_partition(SceUID fd,
                                            u32 partition_sector,
                                            u16 backup_sector_hint)
{
	u16 backup_sector = backup_sector_hint;

	if (backup_sector == 0 || backup_sector > 64)
		backup_sector = 6;

	if (fat_read_dbr_at_sector(fd, partition_sector + backup_sector))
		return 1;

	if (backup_sector != 6 &&
	    fat_read_dbr_at_sector(fd, partition_sector + 6))
		return 1;

	return 0;
}

static int fat_try_mount_partition(SceUID fd, u32 partition_sector)
{
	u16 backup_sector_hint = 0;
	int primary_read = 0;

	if (ppa_io_read_exact_at_retry(fd,
	                               (SceOff)partition_sector *
	                               FAT_SECTOR_SIZE_PSP,
	                               &dbr,
	                               sizeof(dbr)) == (int)sizeof(dbr)) {
		primary_read = 1;
		backup_sector_hint = fat_bpb_fat32_backup_boot_sector();

		dbr_pos = (SceOff)partition_sector * FAT_SECTOR_SIZE_PSP;
		partition_start_sector = partition_sector;

		if (fat_geometry_from_current_dbr())
			return 1;
	}

	/*
	 * FAT32 usually has a backup boot sector. Even if the primary DBR is
	 * partly corrupt, bytes-per-sector and backup-sector may still be
	 * readable. If not, sector + 6 is the common FAT32 fallback.
	 */
	if (primary_read) {
		u16 bytes_per_sec = fat_bpb_bytes_per_sector();
		u16 root_entries = fat_bpb_root_entries();
		u16 fat16_spf = fat_bpb_fat16_sectors_per_fat();

		if (bytes_per_sec == FAT_SECTOR_SIZE_PSP &&
		    root_entries == 0 &&
		    fat16_spf == 0) {
			if (fat_try_backup_dbr_for_partition(fd,
			                                     partition_sector,
			                                     backup_sector_hint)) {
				return 1;
			}
		}
	}

	if (fat_try_backup_dbr_for_partition(fd, partition_sector, 6))
		return 1;

	return 0;
}

static int fat_mount_from_mbr_or_superfloppy(SceUID fd)
{
	u32 i;

	memset(&mbr, 0, sizeof(mbr));

	if (ppa_io_read_exact_at_retry(fd, 0, &mbr, sizeof(mbr)) !=
	    (int)sizeof(mbr)) {
		return 0;
	}

	if (fat_mbr_signature() == 0xAA55) {
		for (i = 0; i < FAT_MAX_PARTITIONS; i++) {
			if (!fat_partition_type_is_fat(mbr.dpt[i].id))
				continue;

			if (mbr.dpt[i].start_sec == 0)
				continue;

			if (fat_try_mount_partition(fd, mbr.dpt[i].start_sec))
				return 1;
		}
	}

	/*
	 * Some removable media are formatted as "superfloppy" with the DBR at
	 * sector 0 rather than inside an MBR partition.
	 */
	if (fat_try_mount_partition(fd, 0))
		return 1;

	/*
	 * Preserve the old behavior as a last resort: try partition 0's start
	 * even if the partition ID looked strange.
	 */
	if (mbr.dpt[0].start_sec != 0 &&
	    fat_try_mount_partition(fd, mbr.dpt[0].start_sec)) {
		return 1;
	}

	return 0;
}

/* ------------------------------------------------------------------------- */
/* FAT table conversion, candidate scoring, and copy selection.                */
/* ------------------------------------------------------------------------- */

static u32 *fat_convert_raw_fat12(const u8 *raw,
                                  u32 raw_size,
                                  u32 *entry_count_out)
{
	u32 entry_count;
	u32 i;
	u32 raw_pos;
	u32 *table;

	/* A malicious BPB must not wrap raw_size*2 or the allocation product. */
	if ((u64)raw_size * 2U / 3U > SIZE_MAX / sizeof(u32))
		return NULL;
	entry_count = (u32)((u64)raw_size * 2U / 3U);
	if (entry_count < 3)
		return NULL;

	table = (u32 *)calloc(entry_count, sizeof(u32));
	if (table == NULL)
		return NULL;

	for (i = 0, raw_pos = 0; i + 1 < entry_count && raw_pos + 2 < raw_size; i += 2, raw_pos += 3) {
		u16 a = raw[raw_pos];
		u16 b = raw[raw_pos + 1];
		u16 c = raw[raw_pos + 2];

		table[i] = (u32)((a | ((b & 0x0F) << 8)) & FAT12_ENTRY_MASK);
		table[i + 1] = (u32)(((b >> 4) | (c << 4)) & FAT12_ENTRY_MASK);
	}

	*entry_count_out = entry_count;
	return table;
}

static u32 *fat_convert_raw_fat16(const u8 *raw,
                                  u32 raw_size,
                                  u32 *entry_count_out)
{
	u32 entry_count;
	u32 i;
	u32 *table;

	entry_count = raw_size / 2U;
	if (entry_count < 3 || entry_count > SIZE_MAX / sizeof(u32))
		return NULL;

	table = (u32 *)malloc(sizeof(u32) * entry_count);
	if (table == NULL)
		return NULL;

	for (i = 0; i < entry_count; i++)
		table[i] = fat_le16(raw + (i * 2U)) & FAT16_ENTRY_MASK;

	*entry_count_out = entry_count;
	return table;
}

static u32 *fat_convert_raw_fat32(const u8 *raw,
                                  u32 raw_size,
                                  u32 *entry_count_out)
{
	u32 entry_count;
	u32 i;
	u32 *table;

	entry_count = raw_size / 4U;
	if (entry_count < 3)
		return NULL;

	table = (u32 *)malloc(sizeof(u32) * entry_count);
	if (table == NULL)
		return NULL;

	for (i = 0; i < entry_count; i++)
		table[i] = fat_le32(raw + (i * 4U)) & FAT32_ENTRY_MASK;

	*entry_count_out = entry_count;
	return table;
}

static u32 *fat_convert_raw_table(const u8 *raw,
                                  u32 raw_size,
                                  u32 *entry_count_out)
{
	if (entry_count_out == NULL)
		return NULL;

	*entry_count_out = 0;

	if (raw == NULL || raw_size == 0)
		return NULL;

	if (fat_type == fat12)
		return fat_convert_raw_fat12(raw, raw_size, entry_count_out);

	if (fat_type == fat16)
		return fat_convert_raw_fat16(raw, raw_size, entry_count_out);

	return fat_convert_raw_fat32(raw, raw_size, entry_count_out);
}

static int fat_score_chain_from_table(u32 *table,
                                      u32 entry_count,
                                      u32 start_cluster,
                                      int allow_partial,
                                      u32 *cluster_count_out)
{
	u32 cluster = start_cluster;
	u32 steps = 0;
	u32 count = 0;
	int score = 0;

	if (cluster_count_out)
		*cluster_count_out = 0;

	if (start_cluster < 2 || start_cluster >= entry_count)
		return FAT_CANDIDATE_SCORE_OUT_OF_RANGE;

	while (1) {
		u32 next;
		if (ppa_io_cancel_requested())
			return FAT_CANDIDATE_SCORE_HARD_BAD;

		if (cluster < 2 || cluster >= entry_count)
			return FAT_CANDIDATE_SCORE_OUT_OF_RANGE;

		count++;
		steps++;

		if (steps > data_cluster_count + 1U)
			return FAT_CANDIDATE_SCORE_CHAIN_LOOP;

		next = fat_entry_value_from_table(table,
		                                  entry_count,
		                                  fat_type,
		                                  cluster);

		if (fat_entry_is_eoc_for_type(fat_type, next)) {
			score += FAT_CANDIDATE_SCORE_ROOT_CHAIN_VALID;
			break;
		}

		if (fat_entry_is_bad_for_type(fat_type, next)) {
			if (allow_partial && count > 0) {
				score += FAT_CANDIDATE_SCORE_ROOT_CHAIN_PARTIAL;
				break;
			}
			return FAT_CANDIDATE_SCORE_BAD_CLUSTER;
		}

		if (fat_entry_is_reserved_for_type(fat_type, next)) {
			if (allow_partial && count > 0) {
				score += FAT_CANDIDATE_SCORE_ROOT_CHAIN_PARTIAL;
				break;
			}
			return FAT_CANDIDATE_SCORE_BAD_CLUSTER;
		}

		if (!fat_entry_is_data_cluster_for_table(entry_count, next)) {
			if (allow_partial && count > 0) {
				score += FAT_CANDIDATE_SCORE_ROOT_CHAIN_PARTIAL;
				break;
			}
			return FAT_CANDIDATE_SCORE_OUT_OF_RANGE;
		}

		cluster = next;
	}

	if (cluster_count_out)
		*cluster_count_out = count;

	return score;
}

static int fat_score_table_candidate(u32 *table,
                                     u32 entry_count,
                                     u32 fat_index)
{
	int score = FAT_CANDIDATE_SCORE_READABLE;

	if (table == NULL || entry_count < 3)
		return FAT_CANDIDATE_SCORE_HARD_BAD;

	/*
	 * FAT entry 0 and 1 contain reserved/media/EOC-ish values.
	 * Keep this loose because some adapters and formatters vary.
	 */
	if (fat_entry_value_from_table(table, entry_count, fat_type, 1) != 0)
		score += FAT_CANDIDATE_SCORE_ENTRY_HINTS;

	if (fat_index == active_fat_copy)
		score += FAT_CANDIDATE_SCORE_ACTIVE_COPY;

	if (fat_type == fat32) {
		int chain_score = fat_score_chain_from_table(table,
		                                             entry_count,
		                                             fat_bpb_fat32_root_cluster(),
		                                             1,
		                                             NULL);
		score += chain_score;
	}
	else {
		score += FAT_CANDIDATE_SCORE_ROOT_CHAIN_VALID;
	}

	return score;
}

static int fat_read_table_candidate(u32 fat_index,
                                    fat_table_candidate *candidate)
{
	u32 raw_size;
	u8 *raw;
	u32 *table;
	u32 entry_count;
	SceOff fat_offset;

	if (candidate == NULL)
		return 0;

	memset(candidate, 0, sizeof(*candidate));
	candidate->score = FAT_CANDIDATE_SCORE_HARD_BAD;
	candidate->fat_index = fat_index;

	raw_size = fat_raw_table_size_bytes();
	if (raw_size == 0)
		return 0;

	raw = (u8 *)malloc(raw_size);
	if (raw == NULL)
		return 0;

	fat_offset = dbr_pos;
	fat_offset += (SceOff)(fat_bpb_reserved_sectors() +
	                       (fat_index * fat_current_fat_sectors())) *
	              fat_bpb_bytes_per_sector();

	if (ppa_io_read_exact_at_retry(fatfd,
	                               fat_offset,
	                               raw,
	                               raw_size) != (int)raw_size) {
		free(raw);
		return 0;
	}

	table = fat_convert_raw_table(raw, raw_size, &entry_count);
	free(raw);

	if (table == NULL || entry_count < 3)
		return 0;

	candidate->table = table;
	candidate->entry_count = entry_count;
	candidate->score = fat_score_table_candidate(table, entry_count, fat_index);

	return 1;
}

static int fat_load_table(void)
{
	u32 i;
	u32 num_fats;
	fat_table_candidate best;

	if (loadcount > 0) {
		loadcount++;
		return 1;
	}

	fatfd = ppa_io_open_read_retry("msstor:", PSP_O_RDONLY, 0777);
	if (fatfd < 0)
		return 0;

	num_fats = fat_bpb_num_fats();
	active_fat_copy = fat32_first_fat_to_try();

	memset(&best, 0, sizeof(best));
	best.score = FAT_CANDIDATE_SCORE_HARD_BAD;

	/*
	 * Try active FAT first on FAT32 if mirroring is disabled, then every
	 * FAT copy. We still score all readable copies and keep the best.
	 */
	if (active_fat_copy < num_fats) {
		fat_table_candidate cand;

		if (fat_read_table_candidate(active_fat_copy, &cand)) {
			if (cand.score > best.score) {
				if (best.table)
					free(best.table);
				best = cand;
			}
			else {
				free(cand.table);
			}
		}
	}

	for (i = 0; i < num_fats; i++) {
		fat_table_candidate cand;

		if (i == active_fat_copy)
			continue;

		if (!fat_read_table_candidate(i, &cand))
			continue;

		if (cand.score > best.score) {
			if (best.table)
				free(best.table);
			best = cand;
		}
		else {
			free(cand.table);
		}
	}

	if (best.table == NULL || best.score <= FAT_CANDIDATE_SCORE_HARD_BAD / 2) {
		if (best.table)
			free(best.table);

		sceIoClose(fatfd);
		fatfd = -1;
		fat_table = NULL;
		fat_entry_count = 0;
		return 0;
	}

	fat_table = best.table;
	fat_entry_count = best.entry_count;
	active_fat_copy = best.fat_index;
	loadcount = 1;

	return 1;
}

static void fat_free_table(void)
{
	if (loadcount > 0) {
		loadcount--;

		if (loadcount > 0)
			return;

		if (fat_table != NULL) {
			free((void *)fat_table);
			fat_table = NULL;
		}

		fat_entry_count = 0;

		if (fatfd >= 0) {
			sceIoClose(fatfd);
			fatfd = -1;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Public init/free.                                                          */
/* ------------------------------------------------------------------------- */

int fat_init(int devkitVersion)
{
	fwVersion = devkitVersion;

	fat_reset_mount_state();

	if (fatfd >= 0) {
		sceIoClose(fatfd);
		fatfd = -1;
	}

	fatfd = ppa_io_open_read_retry("msstor:", PSP_O_RDONLY, 0777);
	if (fatfd < 0)
		return 0;

	if (!fat_mount_from_mbr_or_superfloppy(fatfd)) {
		sceIoClose(fatfd);
		fatfd = -1;
		fat_reset_mount_state();
		return 0;
	}

	sceIoClose(fatfd);
	fatfd = -1;

	return 1;
}

/* ------------------------------------------------------------------------- */
/* Directory chain and entry validation.                                      */
/* ------------------------------------------------------------------------- */

static u8 fat_calc_chksum(p_fat_entry info)
{
	u8 *n = (u8 *)&info->norm.filename[0];
	u8 chksum = 0;
	u32 i;

	for (i = 0; i < 11; i++)
		chksum = ((chksum & 1) ? 0x80 : 0) + (chksum >> 1) + n[i];

	return chksum;
}

static int fat_chain_count_clusters(u32 start_cluster,
                                    u32 *cluster_count,
                                    int *partial)
{
	u32 cluster = start_cluster;
	u32 count = 0;
	u32 steps = 0;

	if (cluster_count)
		*cluster_count = 0;

	if (partial)
		*partial = 0;

	if (!fat_cluster_valid_number(start_cluster))
		return 0;

	while (1) {
		u32 next;
		if (ppa_io_cancel_requested())
			return 0;

		if (!fat_cluster_valid_number(cluster))
			return 0;

		count++;
		steps++;

		if (steps > data_cluster_count + 1U) {
			if (partial)
				*partial = 1;
			break;
		}

		next = fat_entry_value(cluster);

		if (fat_entry_is_eoc_for_type(fat_type, next))
			break;

		if (fat_entry_is_bad_for_type(fat_type, next) ||
		    fat_entry_is_reserved_for_type(fat_type, next)) {
			if (partial)
				*partial = 1;
			break;
		}

		if (!fat_cluster_valid_number(next)) {
			if (partial)
				*partial = 1;
			break;
		}

		cluster = next;
	}

	if (cluster_count)
		*cluster_count = count;

	return count > 0;
}

static int fat_entry_is_lfn(p_fat_entry entry)
{
	if (entry == NULL)
		return 0;

	return entry->norm.attr == FAT_ATTR_LONG_NAME;
}

static int fat_entry_is_end(p_fat_entry entry)
{
	return entry != NULL && entry->norm.filename[0] == 0x00;
}

static int fat_entry_is_deleted(p_fat_entry entry)
{
	return entry != NULL && (u8)entry->norm.filename[0] == 0xE5;
}

static int fat_entry_is_dot(p_fat_entry entry)
{
	if (entry == NULL)
		return 0;

	return entry->norm.filename[0] == '.' &&
	       (entry->norm.filename[1] == 0x20 ||
	        entry->norm.filename[1] == '.');
}

static int fat_entry_attr_valid_for_normal(p_fat_entry entry)
{
	u8 attr;

	if (entry == NULL)
		return 0;

	attr = entry->norm.attr;

	if (attr == FAT_ATTR_LONG_NAME)
		return 0;

	if ((attr & FAT_ATTR_UNUSED_MASK) != 0)
		return 0;

	if ((attr & FAT_FILEATTR_VOLUME) && (attr & FAT_FILEATTR_DIRECTORY))
		return 0;

	return 1;
}

static int fat_entry_cluster_valid_for_normal(p_fat_entry entry)
{
	u32 cluster;
	u8 attr;

	if (entry == NULL)
		return 0;

	attr = entry->norm.attr;
	cluster = fat_entry_cluster(entry);

	if ((attr & FAT_FILEATTR_DIRECTORY) != 0)
		return fat_cluster_valid_number(cluster);

	if (cluster == 0 && entry->norm.filesize == 0)
		return 1;

	if (cluster == 0 && entry->norm.filesize != 0)
		return 0;

	return fat_cluster_valid_number(cluster);
}

static int fat_entry_is_usable_normal(p_fat_entry entry)
{
	if (entry == NULL)
		return 0;

	if (fat_entry_is_end(entry))
		return 0;

	if (fat_entry_is_deleted(entry))
		return 0;

	if (fat_entry_is_lfn(entry))
		return 0;

	if (fat_entry_is_dot(entry))
		return 0;

	if (!fat_entry_attr_valid_for_normal(entry))
		return 0;

	if (!fat_entry_cluster_valid_for_normal(entry))
		return 0;

	return 1;
}

static int fat_entry_should_consume_firmware_dirent(p_fat_entry entry)
{
	if (entry == NULL)
		return 0;

	if (fat_entry_is_end(entry))
		return 0;

	if (fat_entry_is_deleted(entry))
		return 0;

	if (fat_entry_is_lfn(entry))
		return 0;

	if ((entry->norm.attr & FAT_FILEATTR_VOLUME) != 0)
		return 0;

	if (!fat_entry_attr_valid_for_normal(entry))
		return 0;

	return 1;
}

static int fat_read_next_firmware_dirent(SceUID dl,
                                         SceIoDirent *sid,
                                         int skip_dot)
{
	int result;

	if (dl < 0 || sid == NULL)
		return 0;

	while (1) {
		if (ppa_io_cancel_requested())
			return 0;
		memset(sid, 0, sizeof(*sid));
		result = sceIoDread(dl, sid);

		if (result <= 0)
			return 0;
		sid->d_name[sizeof(sid->d_name) - 1U] = 0;

		if ((sid->d_stat.st_attr & FAT_FILEATTR_VOLUME) != 0)
			continue;

		if (skip_dot && sid->d_name[0] == '.' && sid->d_name[1] == 0)
			continue;

		return 1;
	}
}

static int fat_dir_list(u32 clus, u32 *count, p_fat_entry *entrys)
{
	u32 cluster_count;
	u32 epc;
	u32 ep;
	u32 c2;
	u32 steps;
	int partial;

	if (count == NULL || entrys == NULL)
		return 0;

	*count = 0;
	*entrys = NULL;

	if (clus == 0)
		return 0;

	if (clus < 2) {
		u32 root_entries = fat_bpb_root_entries();
		u32 bytes = root_entries * sizeof(t_fat_entry);

		if (fat_type == fat32)
			return 0;

		if (root_entries == 0)
			return 0;

		*entrys = (p_fat_entry)malloc(bytes);
		if (*entrys == NULL)
			return 0;

		if (ppa_io_read_exact_at_retry(fatfd,
		                               root_pos,
		                               *entrys,
		                               bytes) != (int)bytes) {
			free((void *)*entrys);
			*entrys = NULL;
			return 0;
		}

		*count = root_entries;
		return 1;
	}

	if (!fat_chain_count_clusters(clus, &cluster_count, &partial))
		return 0;

	epc = (u32)(bytes_per_clus / sizeof(t_fat_entry));
	if (epc == 0 || cluster_count > UINT_MAX / epc ||
	    (u64)cluster_count * bytes_per_clus > SIZE_MAX)
		return 0;

	*count = cluster_count * epc;
	*entrys = (p_fat_entry)malloc(*count * sizeof(t_fat_entry));
	if (*entrys == NULL) {
		*count = 0;
		return 0;
	}

	c2 = clus;
	ep = 0;
	steps = 0;

	while (fat_cluster_valid_number(c2) && ep < *count) {
		u64 epos;
		u32 next;
		if (ppa_io_cancel_requested()) {
			free((void *)*entrys);
			*entrys = NULL;
			*count = 0;
			return 0;
		}
		epos = data_pos + ((u64)(c2 - 2U) * bytes_per_clus);

		if (ppa_io_read_exact_at_retry(fatfd,
		                               (SceOff)epos,
		                               &(*entrys)[ep],
		                               (unsigned int)bytes_per_clus) !=
		    (int)bytes_per_clus) {
			/*
			 * Real-world recovery: if a later directory cluster fails,
			 * return earlier clusters rather than hiding the whole dir.
			 */
			if (ep > 0) {
				*count = ep;
				return 1;
			}

			free((void *)*entrys);
			*entrys = NULL;
			*count = 0;
			return 0;
		}

		ep += epc;
		steps++;

		if (steps > data_cluster_count + 1U) {
			*count = ep;
			return ep > 0;
		}

		next = fat_entry_value(c2);

		if (fat_entry_is_eoc_for_type(fat_type, next))
			break;

		if (fat_entry_is_bad_for_type(fat_type, next) ||
		    fat_entry_is_reserved_for_type(fat_type, next))
			break;

		if (!fat_cluster_valid_number(next))
			break;

		c2 = next;
	}

	*count = ep;
	return ep > 0;
}

/* ------------------------------------------------------------------------- */
/* Name handling.                                                             */
/* ------------------------------------------------------------------------- */

static int fat_get_longname(p_fat_entry entrys, u32 cur, char *longnamestr)
{
	u16 chksum;
	u32 j;
	u16 longname[260];
	u32 seen_last = 0;

	if (entrys == NULL || longnamestr == NULL)
		return 0;

	if (cur == 0)
		return 0;

	chksum = fat_calc_chksum(&entrys[cur]);

	memset(longname, 0, sizeof(longname));
	memset(longnamestr, 0, 256);

	j = cur;

	while (j > 0) {
		u32 order;
		u32 ppos;
		u32 k;

		j--;

		if (entrys[j].norm.attr != FAT_ATTR_LONG_NAME)
			return 0;

		if (entrys[j].longfile.checksum != chksum)
			return 0;

		if (entrys[j].norm.filename[0] == 0 ||
		    (u8)entrys[j].norm.filename[0] == 0xE5)
			return 0;

		order = entrys[j].longfile.order & 0x3F;
		if (order == 0 || order > 20)
			return 0;

		ppos = (order - 1U) * 13U;
		if (ppos + 13U > 260U)
			return 0;

		for (k = 0; k < 5; k++)
			longname[ppos++] = entrys[j].longfile.uni_name[k];

		for (k = 0; k < 6; k++)
			longname[ppos++] = entrys[j].longfile.uni_name2[k];

		for (k = 0; k < 2; k++)
			longname[ppos++] = entrys[j].longfile.uni_name3[k];

		if ((entrys[j].longfile.order & 0x40) != 0) {
			seen_last = 1;
			break;
		}
	}

	if (!seen_last)
		return 0;

	longname[255] = 0;

	{
		char *temp = miniConvUTF16LEConv(longname);
		if (temp)
			strncpy(longnamestr, temp, 255);
	}

	return longnamestr[0] != 0;
}

static void fat_get_shortname(p_fat_entry entry, char *shortnamestr)
{
	static int chartable[256] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
		0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};

	u32 i = 0;
	u8 abit = 0;

	if (shortnamestr == NULL)
		return;

	shortnamestr[0] = 0;

	if (entry == NULL)
		return;

	if ((entry->norm.flag & 0x08) > 0)
		abit = 0x20;

	while (i < 8 && entry->norm.filename[i] != 0x20) {
		*shortnamestr++ = entry->norm.filename[i] |
		                   ((chartable[(u8)entry->norm.filename[i]]) ? abit : 0);
		i++;
	}

	if (entry->norm.fileext[0] != 0x20) {
		*shortnamestr++ = '.';
		i = 0;
		while (i < 3 && entry->norm.fileext[i] != 0x20)
			*shortnamestr++ = entry->norm.fileext[i++] | abit;
	}

	*shortnamestr = 0;
}

/* A raw on-card name is one component, never a device prefix or path. Keep
 * high bytes intact (short names may be OEM encoded, long names are UTF-8).
 * Dot navigation retains its existing handling in the caller/entry filter. */
static int fat_leaf_name_is_safe(const char *name)
{
	const unsigned char *cursor = (const unsigned char *)name;
	if (cursor == NULL || *cursor == 0)
		return 0;
	for (; *cursor != 0; ++cursor) {
		if (*cursor < 0x20U || *cursor == 0x7fU || *cursor == ':' ||
		    *cursor == '/' || *cursor == '\\')
			return 0;
	}
	return 1;
}

static int fat_shortname_from_user_name(const char *name,
                                        char shortname[11],
                                        int *onlylong)
{
	u32 nlen;
	const char *dot;

	if (name == NULL || shortname == NULL || onlylong == NULL)
		return 0;

	*onlylong = 0;
	nlen = strlen(name);

	if (nlen > 12) {
		*onlylong = 1;
		return 1;
	}

	dot = strrchr(name, '.');

	if ((dot == NULL && nlen < 9) ||
	    (dot != NULL &&
	     dot - name < 9 &&
	     nlen - 1 - (u32)(dot - name) < 4)) {
		memset(shortname, 0x20, 11);
		memcpy(&shortname[0], name, (dot == NULL) ? nlen : (u32)(dot - name));
		if (dot != NULL)
			memcpy(&shortname[8], dot + 1, nlen - 1 - (u32)(dot - name));
	}
	else {
		*onlylong = 1;
	}

	return 1;
}

/* ------------------------------------------------------------------------- */
/* Locate and path traversal.                                                 */
/* ------------------------------------------------------------------------- */

static int fat_locate(const char *name, char *sname, u32 clus, p_fat_entry info)
{
	u32 count;
	p_fat_entry entrys;
	SceUID dl;
	char shortname[11];
	int onlylong = 0;
	SceIoDirent sid;
	int have_sid = 0;
	u32 i;

	if (name == NULL || sname == NULL || info == NULL)
		return 0;
	if (!fat_leaf_name_is_safe(name))
		return 0;

	if (!fat_dir_list(clus, &count, &entrys))
		return 0;

	/* Firmware names are consumed only by the pre-4.01 compatibility path
	 * below. On newer firmware the raw FAT walk already provides every name;
	 * a parallel Dread walk was doubling directory I/O for no used result. */
	dl = fwVersion < 0x04000110 ? sceIoDopen(sname) : -1;

	/*
	 * If firmware directory open fails, keep raw FAT lookup alive. This is
	 * a recovery path; the final playback open may still fail, but browsing
	 * can continue where possible.
	 */
	if (dl < 0)
		have_sid = 0;

	if (!fat_shortname_from_user_name(name, shortname, &onlylong)) {
		free((void *)entrys);
		if (dl >= 0)
			sceIoDclose(dl);
		return 0;
	}

	for (i = 0; i < count; i++) {
		char longnames[256];
		int matched;
		if (ppa_io_cancel_requested()) {
			free((void *)entrys);
			if (dl >= 0) sceIoDclose(dl);
			return 0;
		}

		if (fat_entry_is_end(&entrys[i]))
			break;

		if (!fat_entry_should_consume_firmware_dirent(&entrys[i]))
			continue;

		if (dl >= 0)
			have_sid = fat_read_next_firmware_dirent(dl, &sid, 0);
		else
			have_sid = 0;

		if (!fat_entry_is_usable_normal(&entrys[i]))
			continue;

		if (entrys[i].norm.filename[0] == 0x05)
			entrys[i].norm.filename[0] = 0xE5;

		matched = !onlylong &&
		          strnicmp(shortname, &entrys[i].norm.filename[0], 11) == 0;

		if ((u8)entrys[i].norm.filename[0] == 0xE5)
			entrys[i].norm.filename[0] = 0x05;

		if (!matched) {
			memset(longnames, 0, sizeof(longnames));
			if (!fat_get_longname(entrys, i, longnames) ||
			    stricmp(name, longnames) != 0)
				continue;
		}

		{
			char short_name[13];
			const char *component;
			size_t used, added;
			int is_directory;
			memcpy(info, &entrys[i], sizeof(t_fat_entry));
			free((void *)entrys);
			if (dl >= 0)
				sceIoDclose(dl);
			fat_get_shortname(info, short_name);
			/* FAT stores a literal leading E5 byte as 05 in a live entry. */
			if ((unsigned char)short_name[0] == 0x05U)
				short_name[0] = (char)0xe5U;
			component = fwVersion < 0x04000110 && have_sid ?
			            sid.d_name : short_name;
			if (!fat_leaf_name_is_safe(component))
				return 0;
			used = strlen(sname);
			added = strlen(component);
			is_directory = (info->norm.attr & FAT_FILEATTR_DIRECTORY) != 0;
			/* The raw FAT API has always used a 256-byte path contract. A
			 * firmware long name can be longer than the requested short name. */
			if (used >= FAT_SHORT_PATH_CAPACITY ||
			    added + (size_t)is_directory >= FAT_SHORT_PATH_CAPACITY - used)
				return 0;
			memcpy(sname + used, component, added);
			used += added;
			if (is_directory) sname[used++] = '/';
			sname[used] = 0;
			return 1;
		}
	}

	free((void *)entrys);

	if (dl >= 0)
		sceIoDclose(dl);

	return 0;
}

static u32 fat_dir_clus(const char *dir, char *shortdir)
{
	char rdir[256];
	char *partname;
	u32 clus;
	t_fat_entry entry;

	if (dir == NULL || shortdir == NULL)
		return 0;

	if (!fat_load_table() || fatfd < 0)
		return 0;

	if (strlen(dir) >= sizeof(rdir)) {
		fat_free_table();
		return 0;
	}

	strcpy(rdir, dir);

	partname = strtok(rdir, "/\\");
	if (partname == NULL) {
		fat_free_table();
		return 0;
	}

	if (strcmp(partname, "ms0:") != 0 &&
	    strcmp(partname, "fatms:") != 0 &&
	    strcmp(partname, "fatms0:") != 0) {
		fat_free_table();
		return 0;
	}

	strcpy(shortdir, partname);
	strcat(shortdir, "/");

	partname = strtok(NULL, "/\\");

	clus = (fat_type == fat32) ? fat_bpb_fat32_root_cluster() : 1;

	while (partname != NULL) {
		if (ppa_io_cancel_requested()) {
			fat_free_table();
			return 0;
		}
		if (partname[0] != 0) {
			if (fat_locate(partname, shortdir, clus, &entry)) {
				clus = fat_entry_cluster(&entry);
			}
			else {
				fat_free_table();
				return 0;
			}
		}

		partname = strtok(NULL, "/\\");
	}

	fat_free_table();
	return clus;
}

/* ------------------------------------------------------------------------- */
/* Public readdir.                                                            */
/* ------------------------------------------------------------------------- */

u32 fat_readdir(const char *dir, char *sdir, p_fat_info *info)
{
	u32 clus;
	SceUID dl = -1;
	u32 ecount = 0;
	p_fat_entry entrys = NULL;
	u32 raw_count = 0;
	u32 cur = 0;
	u32 i;
	SceIoDirent sid;
	int have_sid = 0;

	if (dir == NULL || sdir == NULL || info == NULL)
		return INVALID;

	*info = NULL;

	if (!fat_load_table() || fatfd < 0)
		return INVALID;

	clus = fat_dir_clus(dir, sdir);

	if (clus == 0) {
		fat_free_table();
		return INVALID;
	}

	/* Match fat_locate: do not enumerate this directory a second time when
	 * the firmware names cannot be used by the selected firmware path. */
	dl = fwVersion < 0x04000110 ? sceIoDopen(sdir) : -1;

	if (!fat_dir_list(clus, &ecount, &entrys)) {
		fat_free_table();
		if (dl >= 0)
			sceIoDclose(dl);
		return INVALID;
	}

	for (i = 0; i < ecount; i++) {
		if (ppa_io_cancel_requested()) {
			free(entrys);
			fat_free_table();
			if (dl >= 0) sceIoDclose(dl);
			return INVALID;
		}
		if (fat_entry_is_end(&entrys[i]))
			break;

		if (!fat_entry_is_usable_normal(&entrys[i]))
			continue;

		if ((entrys[i].norm.attr & FAT_FILEATTR_VOLUME) != 0)
			continue;

		raw_count++;
	}

	if (raw_count == 0) {
		free(entrys);
		fat_free_table();
		if (dl >= 0)
			sceIoDclose(dl);
		return INVALID;
	}

	if (raw_count > SIZE_MAX / sizeof(t_fat_info)) {
		free(entrys);
		fat_free_table();
		if (dl >= 0) sceIoDclose(dl);
		return INVALID;
	}
	*info = (p_fat_info)malloc((size_t)raw_count * sizeof(t_fat_info));
	if (*info == NULL) {
		free(entrys);
		fat_free_table();
		if (dl >= 0)
			sceIoDclose(dl);
		return INVALID;
	}

	for (i = 0; i < ecount && cur < raw_count; i++) {
		p_fat_info inf;
		if (ppa_io_cancel_requested()) {
			free(*info);
			*info = NULL;
			free(entrys);
			fat_free_table();
			if (dl >= 0) sceIoDclose(dl);
			return INVALID;
		}

		if (fat_entry_is_end(&entrys[i]))
			break;

		if (!fat_entry_should_consume_firmware_dirent(&entrys[i]))
			continue;

		if (dl >= 0)
			have_sid = fat_read_next_firmware_dirent(dl, &sid, 1);
		else
			have_sid = 0;

		if (!fat_entry_is_usable_normal(&entrys[i]))
			continue;

		if ((entrys[i].norm.attr & FAT_FILEATTR_VOLUME) != 0)
			continue;

		inf = &((*info)[cur]);
		memset(inf, 0, sizeof(*inf));

		fat_get_shortname(&entrys[i], inf->filename);

		if (inf->filename[0] == 0x05)
			inf->filename[0] = 0xE5;

		if (!fat_get_longname(entrys, i, inf->longname))
			strcpy(inf->longname, inf->filename);

		/*
		 * Preserve original firmware-name preference on older firmware,
		 * but only when sceIoDread produced a matching usable entry.
		 */
		if (fwVersion < 0x04000110 && have_sid) {
			strncpy(inf->filename, sid.d_name, sizeof(inf->filename) - 1U);
			inf->filename[sizeof(inf->filename) - 1U] = 0;
		}
		if (!fat_leaf_name_is_safe(inf->filename) ||
		    !fat_leaf_name_is_safe(inf->longname))
			continue;

		inf->filesize = entrys[i].norm.filesize;
		inf->cdate = entrys[i].norm.cr_date;
		inf->ctime = entrys[i].norm.cr_time;
		inf->mdate = entrys[i].norm.last_mod_date;
		inf->mtime = entrys[i].norm.last_mod_time;
		inf->clus = fat_entry_cluster(&entrys[i]);
		inf->attr = entrys[i].norm.attr;

		cur++;
	}

	free(entrys);
	fat_free_table();

	if (dl >= 0)
		sceIoDclose(dl);

	if (cur == 0) {
		free(*info);
		*info = NULL;
		return INVALID;
	}

	return cur;
}

void fat_free(void)
{
	if (fat_table != NULL) {
		free((void *)fat_table);
		fat_table = NULL;
	}

	if (fatfd >= 0) {
		sceIoClose(fatfd);
		fatfd = -1;
	}

	memset(&dbr, 0, sizeof(dbr));
	memset(&mbr, 0, sizeof(mbr));

	root_pos = 0;
	data_pos = 0;
	dbr_pos = 0;
	bytes_per_clus = 0;
	loadcount = 0;
	clus_max = 0;
	fat_entry_count = 0;
	data_cluster_count = 0;
	fat_sector_count_cached = 0;
	root_dir_sector_count = 0;
	active_fat_copy = 0;
	partition_start_sector = 0;
	fat_type = fat16;
}
