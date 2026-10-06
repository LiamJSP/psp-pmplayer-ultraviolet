#include "ppa_ms_partition.h"
#include "ppa_io.h"

#include <string.h>
#include <pspiofilemgr.h>

static u16 ppa_le16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

static u32 ppa_le32(const u8 *p)
{
	return ((u32)p[0]) |
	       ((u32)p[1] << 8) |
	       ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static int ppa_partition_type_is_fat(u8 type)
{
	switch (type) {
	case PPA_PARTITION_TYPE_FAT12:
	case PPA_PARTITION_TYPE_FAT16_A:
	case PPA_PARTITION_TYPE_FAT16_B:
	case PPA_PARTITION_TYPE_FAT32_CHS:
	case PPA_PARTITION_TYPE_FAT32_LBA:
	case PPA_PARTITION_TYPE_FAT16_LBA:
		return 1;
	default:
		return 0;
	}
}

int ppa_ms_partition_scan(ppa_ms_partition partitions[PPA_MS_MAX_PARTITIONS])
{
	SceUID fd;
	u8 mbr[PPA_MS_SECTOR_SIZE];
	int i;

	if (partitions == 0)
		return 0;

	memset(partitions, 0, sizeof(ppa_ms_partition) * PPA_MS_MAX_PARTITIONS);

	fd = ppa_io_open_read_retry("msstor:", PSP_O_RDONLY, 0777);
	if (fd < 0)
		return 0;

	if (ppa_io_read_exact_at32_retry(fd, 0, mbr, sizeof(mbr)) != sizeof(mbr)) {
		sceIoClose(fd);
		return 0;
	}

	sceIoClose(fd);

	if (ppa_le16(&mbr[510]) != 0xAA55)
		return 0;

	for (i = 0; i < PPA_MS_MAX_PARTITIONS; i++) {
		const u8 *entry = &mbr[446 + i * 16];

		partitions[i].bootable = entry[0];
		partitions[i].type = entry[4];
		partitions[i].start_sector = ppa_le32(&entry[8]);
		partitions[i].sector_count = ppa_le32(&entry[12]);
		partitions[i].byte_offset = (u64)partitions[i].start_sector * PPA_MS_SECTOR_SIZE;
		partitions[i].byte_size = (u64)partitions[i].sector_count * PPA_MS_SECTOR_SIZE;
	}

	return 1;
}

int ppa_ms_partition_find_ntfs(ppa_ms_partition *out_partition)
{
	ppa_ms_partition partitions[PPA_MS_MAX_PARTITIONS];
	int i;

	if (out_partition == 0)
		return 0;

	memset(out_partition, 0, sizeof(*out_partition));

	if (!ppa_ms_partition_scan(partitions))
		return 0;

	/*
	 * Prefer partition 1 for the requested layout:
	 * partition 0 = PSP FAT, partition 1 = NTFS.
	 */
	if (partitions[1].type == PPA_PARTITION_TYPE_NTFS_EXFAT &&
	    partitions[1].start_sector != 0 &&
	    partitions[1].sector_count != 0) {
		*out_partition = partitions[1];
		return 1;
	}

	for (i = 0; i < PPA_MS_MAX_PARTITIONS; i++) {
		if (partitions[i].type == PPA_PARTITION_TYPE_NTFS_EXFAT &&
		    partitions[i].start_sector != 0 &&
		    partitions[i].sector_count != 0) {
			*out_partition = partitions[i];
			return 1;
		}
	}

	return 0;
}

int ppa_ms_partition_find_fat(ppa_ms_partition *out_partition)
{
	ppa_ms_partition partitions[PPA_MS_MAX_PARTITIONS];
	int i;

	if (out_partition == 0)
		return 0;

	memset(out_partition, 0, sizeof(*out_partition));

	if (!ppa_ms_partition_scan(partitions))
		return 0;

	for (i = 0; i < PPA_MS_MAX_PARTITIONS; i++) {
		if (ppa_partition_type_is_fat(partitions[i].type) &&
		    partitions[i].start_sector != 0 &&
		    partitions[i].sector_count != 0) {
			*out_partition = partitions[i];
			return 1;
		}
	}

	return 0;
}