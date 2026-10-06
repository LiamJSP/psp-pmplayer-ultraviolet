#ifndef __PPA_MS_PARTITION_H__
#define __PPA_MS_PARTITION_H__

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPA_MS_SECTOR_SIZE 512
#define PPA_MS_MAX_PARTITIONS 4

#define PPA_PARTITION_TYPE_EMPTY      0x00
#define PPA_PARTITION_TYPE_NTFS_EXFAT 0x07
#define PPA_PARTITION_TYPE_FAT12      0x01
#define PPA_PARTITION_TYPE_FAT16_A    0x04
#define PPA_PARTITION_TYPE_FAT16_B    0x06
#define PPA_PARTITION_TYPE_FAT32_CHS  0x0B
#define PPA_PARTITION_TYPE_FAT32_LBA  0x0C
#define PPA_PARTITION_TYPE_FAT16_LBA  0x0E

typedef struct ppa_ms_partition {
	u8 type;
	u8 bootable;
	u32 start_sector;
	u32 sector_count;
	u64 byte_offset;
	u64 byte_size;
} ppa_ms_partition;

int ppa_ms_partition_scan(ppa_ms_partition partitions[PPA_MS_MAX_PARTITIONS]);
int ppa_ms_partition_find_ntfs(ppa_ms_partition *out_partition);
int ppa_ms_partition_find_fat(ppa_ms_partition *out_partition);

#ifdef __cplusplus
}
#endif

#endif