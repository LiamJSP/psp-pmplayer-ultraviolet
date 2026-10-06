// directory.c
#include <strings.h>
#ifndef stricmp
#define stricmp strcasecmp
#endif
#ifndef strnicmp
#define strnicmp strncasecmp
#endif
/*
 *	Copyright (C) 2006 cooleyes
 *	eyes.cooleyes@gmail.com
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <pspkernel.h>
#include "fat.h"
#include "directory.h"
#include "ppa_vfs.h"
#include "ppa_io.h"
#include "libminiconv.h"
#ifdef PPA_ENABLE_NTFS
#include "ppa_ntfs.h"
#endif

/* Grow transactionally: realloc failure must never lose the previous list.
 * These readers return an empty result on OOM, so release it deliberately. */
static int directory_grow(directory_item_struct **list, unsigned int count)
{
    directory_item_struct *grown;
    if (count > (unsigned int)INT_MAX / sizeof(**list) - 256U) {
        free(*list); *list = 0; return 0;
    }
    grown = (directory_item_struct *)realloc(*list,
                 (size_t)(count + 256U) * sizeof(**list));
    if (!grown) { free(*list); *list = 0; return 0; }
    *list = grown;
    return 1;
}

static const char * get_file_ext(const char * filename){
	int len = strlen(filename);
	const char * p = filename + len;
	while(p > filename && *p != '.' && *p != '/') p --;
	if(*p == '.')
		return p + 1;
	else
		return NULL;
}

static int compare_directory_item(const void *left, const void *right) {
	const directory_item_struct *item1 = (const directory_item_struct *)left;
	const directory_item_struct *item2 = (const directory_item_struct *)right;
	const char *s1;
	const char *s2;
	int item1_is_parent;
	int item2_is_parent;
	if ( (item1->filetype == FS_DIRECTORY) && (item2->filetype != FS_DIRECTORY) )
		return -1;
	if ( (item1->filetype != FS_DIRECTORY) && (item2->filetype == FS_DIRECTORY) )
		return 1;
	item1_is_parent = stricmp(item1->longname, "..") == 0;
	item2_is_parent = stricmp(item2->longname, "..") == 0;
	if (item1_is_parent && item2_is_parent)
		return 0;
	if (item1_is_parent)
		return -1;
	if (item2_is_parent)
		return 1;
	s1 = item1->longname;
	s2 = item2->longname;
	while (*s1 && *s2) {
		char c1 = *s1++;
		char c2 = *s2++;

		if ((c1 >= 'a') && (c1 <= 'z'))
			c1 -= 'a' - 'A';
		if ((c2 >= 'a') && (c2 <= 'z'))
			c2 -= 'a' - 'A';

		if (c1 > c2)
			return 1;
		if (c1 < c2)
			return -1;
	}
	if (*s1)
		return 1;
	if (*s2)
		return -1;
	return 0;
}

static void directory_copy_string(char *dst, size_t capacity, const char *src)
{
	if (dst == 0 || capacity == 0) return;
	if (src == 0) { dst[0] = 0; return; }
	strncpy(dst, src, capacity - 1U);
	dst[capacity - 1U] = 0;
}

file_type_enum directory_get_filetype(const char* filename, file_type_ext_struct* file_type_ext_table) {
	if (filename == NULL || file_type_ext_table == NULL)
		return FS_UNKNOWN_FILE;
	const char * ext = get_file_ext(filename);
	if(ext == NULL)
		return FS_UNKNOWN_FILE;
	file_type_ext_struct *exts = file_type_ext_table;
	while(exts->ext != NULL){
		if(stricmp(ext, exts->ext) == 0)
			return exts->filetype;
		exts++;
	}
	return FS_UNKNOWN_FILE;
}

static void directory_set_root_device_item(directory_item_struct *item, const char *name)
{
	if (item == NULL || name == NULL)
		return;

	memset(item, 0, sizeof(directory_item_struct));

	item->filetype = FS_DIRECTORY;
	directory_copy_string(item->shortname, sizeof(item->shortname), name);
	directory_copy_string(item->longname, sizeof(item->longname), name);
	item->compname = item->longname;
}

static int open_ms_directory_internal(const char* dir, char* sdir,
                                      int show_hidden, int show_unknown,
                                      file_type_ext_struct* file_type_ext_table,
                                      directory_item_struct** list,
                                      int names_only) {
	int item_count;
	p_fat_info info;
	if(*list != NULL){
		free((void *)(*list));
		*list = NULL;
	}
	u32 count = fat_readdir(dir, sdir, &info);
	if(count == INVALID)
		return 0;
	u32 i, cur_count = 0;
	for(i = 0; i < count; i ++) {
		if (ppa_io_cancel_requested()) {
			free((void *)info);
			if (*list != NULL) { free(*list); *list = NULL; }
			return 0;
		}
		if(!show_hidden && (info[i].attr & FAT_FILEATTR_HIDDEN) > 0)
			continue;
		if(cur_count % 256 == 0){
			if (!directory_grow(list, cur_count)){
				free((void *)info);
				return 0;
			}
		}
		memset(&(*list)[cur_count], 0, sizeof(directory_item_struct));
		if(info[i].attr & FAT_FILEATTR_DIRECTORY){
			(*list)[cur_count].filetype = FS_DIRECTORY;
			directory_copy_string((*list)[cur_count].shortname, sizeof((*list)[cur_count].shortname), info[i].filename);
			directory_copy_string((*list)[cur_count].longname, sizeof((*list)[cur_count].longname), info[i].longname);
			(*list)[cur_count].compname = (*list)[cur_count].longname;
		}
		else{
			/* Names-only browser/cache enumeration must not make acceptance or
			 * latency depend on file size. A zero-byte movie remains visible and
			 * is rejected later by the normal playback/open path if selected. */
			if(!names_only && info[i].filesize == 0)
				continue;
			file_type_enum ft = directory_get_filetype(info[i].longname, file_type_ext_table);
			if(!show_unknown && ft == FS_UNKNOWN_FILE)
				continue;
			(*list)[cur_count].filetype = ft;
			directory_copy_string((*list)[cur_count].shortname, sizeof((*list)[cur_count].shortname), info[i].filename);
			directory_copy_string((*list)[cur_count].longname, sizeof((*list)[cur_count].longname), info[i].longname);
			(*list)[cur_count].compname = (*list)[cur_count].longname;
			if (!names_only) {
				(*list)[cur_count].filesize = info[i].filesize;
				(*list)[cur_count].cdate = info[i].cdate;
				(*list)[cur_count].ctime = info[i].ctime;
				(*list)[cur_count].mdate = info[i].mdate;
				(*list)[cur_count].mtime = info[i].mtime;
			}
		}
		cur_count ++;
	}
	free((void *)info);
	if ( strcmp(sdir, "ms0:/") == 0 ) {
		if(cur_count % 256 == 0)
		{
			if (!directory_grow(list, cur_count))
			{
				return 0;
			}
		}
		for( i = cur_count ; i > 0 ; i--) {
			(*list)[i].filetype = (*list)[i-1].filetype;
			directory_copy_string((*list)[i].shortname, sizeof((*list)[i].shortname), (*list)[i-1].shortname);
			directory_copy_string((*list)[i].longname, sizeof((*list)[i].longname), (*list)[i-1].longname);
			(*list)[i].compname = (*list)[i].longname;
			(*list)[i].filesize = (*list)[i-1].filesize;
			(*list)[i].cdate = (*list)[i-1].cdate;
			(*list)[i].ctime = (*list)[i-1].ctime;
			(*list)[i].mdate = (*list)[i-1].mdate;
			(*list)[i].mtime = (*list)[i-1].mtime;
		}
		memset(&(*list)[0], 0, sizeof(directory_item_struct));
		(*list)[0].filetype = FS_DIRECTORY;
		directory_copy_string((*list)[0].shortname, sizeof((*list)[0].shortname), "..");
		directory_copy_string((*list)[0].longname, sizeof((*list)[0].longname), "..");
		(*list)[0].compname = (*list)[0].longname;
		cur_count ++ ;
	}
	item_count = cur_count;
	return item_count;
}

static int open_firmware_directory_internal(const char* dir, char* sdir,
                                           int show_hidden, int show_unknown,
                                           file_type_ext_struct* file_type_ext_table,
                                           directory_item_struct** list,
                                           int names_only) {
	int item_count;
	if(*list != NULL)
	{
		free((void *)(*list));
		*list = NULL;
	}
	strcpy(sdir, dir);
	int fd = sceIoDopen(dir);
	if ( fd < 0 )
		return 0;
	SceIoDirent temp_dir;
	memset(&temp_dir, 0, sizeof(SceIoDirent));
	u32 cur_count = 0;
	while (!ppa_io_cancel_requested() &&
	       sceIoDread(fd, &temp_dir) > 0) {
		/* Firmware names are fixed-size records, not trusted C strings. */
		temp_dir.d_name[sizeof(temp_dir.d_name) - 1U] = 0;
		if(cur_count % 256 == 0)
		{
			if (!directory_grow(list, cur_count))
			{
				sceIoDclose(fd);
				return 0;
			}
		}
		memset(&(*list)[cur_count], 0, sizeof(directory_item_struct));
		if ( temp_dir.d_stat.st_attr & FIO_SO_IFDIR ) {
			if ( strcmp( temp_dir.d_name , "." ) == 0 )
				continue;
			(*list)[cur_count].filetype = FS_DIRECTORY;
			directory_copy_string((*list)[cur_count].shortname, sizeof((*list)[cur_count].shortname), temp_dir.d_name);
			directory_copy_string((*list)[cur_count].longname, sizeof((*list)[cur_count].longname), temp_dir.d_name);

			if ( miniConvHaveFileSystemConv() ){
				char* temp_str = miniConvFileSystemConv(temp_dir.d_name);
				if( temp_str != NULL ) {
					directory_copy_string((*list)[cur_count].longname, sizeof((*list)[cur_count].longname), temp_str);
				}
			}

			(*list)[cur_count].compname = (*list)[cur_count].shortname;
		}
		else {
			file_type_enum ft = directory_get_filetype(temp_dir.d_name, file_type_ext_table);
			if(!show_unknown && ft == FS_UNKNOWN_FILE)
				continue;
			(*list)[cur_count].filetype = ft;
			directory_copy_string((*list)[cur_count].shortname, sizeof((*list)[cur_count].shortname), temp_dir.d_name);
			directory_copy_string((*list)[cur_count].longname, sizeof((*list)[cur_count].longname), temp_dir.d_name);

			if ( miniConvHaveFileSystemConv() ){
				char* temp_str = miniConvFileSystemConv(temp_dir.d_name);
				if( temp_str != NULL ) {
					directory_copy_string((*list)[cur_count].longname, sizeof((*list)[cur_count].longname), temp_str);
				}
			}

			(*list)[cur_count].compname = (*list)[cur_count].shortname;
			if (!names_only)
				(*list)[cur_count].filesize = temp_dir.d_stat.st_size > 0 ?
					(u64)temp_dir.d_stat.st_size : 0;
		}

		cur_count ++;
	}
	item_count = cur_count;
	sceIoDclose(fd);
	if (ppa_io_cancel_requested()) {
		if (*list != NULL) { free(*list); *list = NULL; }
		return 0;
	}
	return item_count;
}

static int open_directory_internal(const char* dir,
                                   char* sdir,
                                   int show_hidden,
                                   int show_unknown,
                                   file_type_ext_struct* file_type_ext_table,
                                   directory_item_struct** list,
                                   int names_only)
{
	int item_count = 0;
	if (list == NULL || sdir == NULL)
		return 0;
	if (dir == NULL)
		dir = "";
	/* All directory callers provide a 512-byte short-path destination. Reject
	 * long input before either firmware strcpy or FAT path expansion sees it. */
	if (strlen(dir) >= 512U) {
		free(*list);
		*list = NULL;
		return 0;
	}

	if (ppa_io_cancel_requested()) {
		if (list != 0 && *list != 0) { free(*list); *list = 0; }
		return 0;
	}

	if (dir != 0 && dir[0] != 0 && !ppa_vfs_path_is_supported(dir)) {
		if (list != 0 && *list != 0) {
			free(*list);
			*list = 0;
		}
		return 0;
	}

#ifdef PPA_ENABLE_NTFS
	if (ppa_ntfs_path_is_ntfs(dir)) {
		/*
		 * ms1:/ is a PPA userspace pseudo-device. PSP firmware APIs
		 * must never see it, so intercept before sceIoDopen/fat paths.
		 */

		return ppa_ntfs_readdir(dir,
		                        show_hidden,
		                        show_unknown,
		                        file_type_ext_table,
		                        names_only,
		                        list);
	}
#endif

	if ( strnicmp(dir,"ms0:", 4) == 0 )
		item_count = open_ms_directory_internal(dir, sdir, show_hidden, show_unknown, file_type_ext_table, list, names_only);
	else if ( strnicmp(dir,"ef0:", 4) == 0 )
		item_count = open_firmware_directory_internal(dir, sdir, show_hidden, show_unknown, file_type_ext_table, list, names_only);
	else {
		int root_item_count = 2;
		int root_index = 0;

		if(*list != NULL) {
			free((void *)(*list));
			*list = NULL;
		}

#ifdef PPA_ENABLE_NTFS
		if (ppa_ntfs_available())
			root_item_count++;
#endif

		item_count = root_item_count;

		*list = (directory_item_struct*)malloc(sizeof(directory_item_struct) * root_item_count);
		if(*list == NULL) {
			item_count = 0;
			return item_count;
		}

		memset(*list, 0, sizeof(directory_item_struct) * root_item_count);

		directory_set_root_device_item(&((*list)[root_index]), "ms0:");
		root_index++;

		directory_set_root_device_item(&((*list)[root_index]), "ef0:");
		root_index++;

#ifdef PPA_ENABLE_NTFS
		if (ppa_ntfs_available()) {
			directory_set_root_device_item(&((*list)[root_index]), "ms1:");
			root_index++;
		}
#endif
	}

	/* The previous bubble sort copied ~600-byte directory records O(n^2)
	 * times. Large directories could monopolize the UI thread for
	 * seconds. qsort bounds this phase to O(n log n); internal compname
	 * pointers are repaired after byte-wise swaps. */
	if (item_count > 1 && list != NULL && *list != NULL) {
		int use_short_name =
			dir != NULL &&
			strnicmp(dir, "ef0:", 4) == 0;
		int i;
		qsort(*list, (size_t)item_count, sizeof(directory_item_struct),
		      compare_directory_item);
		for (i = 0; i < item_count; ++i)
			(*list)[i].compname = use_short_name ?
			                      (*list)[i].shortname : (*list)[i].longname;
	}

	if (ppa_io_cancel_requested()) {
		if (list != NULL && *list != NULL) { free(*list); *list = NULL; }
		return 0;
	}

	return item_count;
}

int open_directory(const char* dir, char* sdir, int show_hidden,
                   int show_unknown,
                   file_type_ext_struct* file_type_ext_table,
                   directory_item_struct** list)
{
	return open_directory_internal(dir, sdir, show_hidden, show_unknown,
	                               file_type_ext_table, list, 0);
}

int open_directory_names_only(const char* dir, char* sdir, int show_hidden,
                              int show_unknown,
                              file_type_ext_struct* file_type_ext_table,
                              directory_item_struct** list)
{
	return open_directory_internal(dir, sdir, show_hidden, show_unknown,
	                               file_type_ext_table, list, 1);
}

int is_next_movie(const char* prev, const char* next) {
	if (prev == NULL || next == NULL)
		return 0;
	int prev_len = strlen(prev);
	int next_len = strlen(next);
	if (prev_len == 0 || next_len == 0 || prev_len >= 256 || next_len >= 256)
		return 0;

	if ( (prev_len != next_len) && (prev_len != next_len-1) )
		return 0;

	const char* s1 = prev;
	const char* s2 = next;
	while (*s1 && *s2) {
		char c1 = *s1;
		char c2 = *s2;

		if ((c1 >= 'a') && (c1 <= 'z'))
			c1 -= 'a' - 'A';
		if ((c2 >= 'a') && (c2 <= 'z'))
			c2 -= 'a' - 'A';
		if ( c1 != c2 )
			break;
		else {
			s1++;
			s2++;
		}
	}
	if ( *s1 == 0)
		return 0;
	const char* s3 = prev + prev_len - 1;
	const char* s4 = next + next_len - 1;
	while( (s3>s1) && (s4>s2) ) {
		char c3 = *s3;
		char c4 = *s4;

		if ((c3 >= 'a') && (c3 <= 'z'))
			c3 -= 'a' - 'A';
		if ((c4 >= 'a') && (c4 <= 'z'))
			c4 -= 'a' - 'A';
		if ( c3 != c4 )
			break;
		else {
			s3--;
			s4--;
		}
	}

	char value1[256], value2[256];
	memset(value1, 0, 256);
	memset(value2, 0, 256);
	strncpy(value1, s1, (s3-s1)+1);
	strncpy(value2, s2, (s4-s2)+1);

	if ( (strlen(value1)==1) && (strlen(value2)==1) ) {
		if ((value1[0] >= 'a') && (value1[0] <= 'z'))
			value1[0] -= 'a' - 'A';
		if ((value2[0] >= 'a') && (value2[0] <= 'z'))
			value2[0] -= 'a' - 'A';
		if ( value2[0] - value1[0] == 1)
			return 1;
		else
			return 0;
	}
	else {
		unsigned int values[2] = {0U, 0U};
		const char *parts[2] = {value1, value2};
		int i;
		for (i = 0; i < 2; ++i) {
			const unsigned char *p = (const unsigned char *)parts[i];
			if (*p == 0) return 0;
			for (; *p; ++p) {
				unsigned int digit;
				if (*p < '0' || *p > '9') return 0;
				digit = *p - '0';
				if (values[i] > ((unsigned int)INT_MAX - digit) / 10U)
					return 0;
				values[i] = values[i] * 10U + digit;
			}
		}
		return values[0] < (unsigned int)INT_MAX && values[1] == values[0] + 1U;
	}
}
