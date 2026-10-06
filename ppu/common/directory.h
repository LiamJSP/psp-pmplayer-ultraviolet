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
#ifndef __DIRECTORY_H__
#define __DIRECTORY_H__

#include <psptypes.h>

typedef enum {
//*/
	FS_DIRECTORY = 0,
	FS_MP4_FILE,
	FS_MKV_FILE,
	FS_SUB_FILE,
	FS_SRT_FILE,
	FS_ASS_FILE,
	FS_SSA_FILE,
	FS_PNG_FILE,
	FS_UNKNOWN_FILE
//*/
} file_type_enum;

typedef struct {
	const char * ext;
	file_type_enum filetype;
} file_type_ext_struct;

typedef struct {
	char shortname[256];
	char longname[256];
	char* compname; 
	u64 filesize;
	u16 cdate;
	u16 ctime;
	u16 mdate;
	u16 mtime;
	file_type_enum filetype;
} directory_item_struct;

#ifdef __cplusplus
extern "C" {
#endif

file_type_enum directory_get_filetype(const char* filename, file_type_ext_struct* file_type_ext_table);

int open_directory(const char* dir, char* sdir, int show_hidden, int show_unknown, file_type_ext_struct* file_type_ext_table, directory_item_struct** list); 

/* Browser/cache listing variant: populate only names, hierarchy/type and
 * compname. File size/date fields remain zero. */
int open_directory_names_only(const char* dir, char* sdir, int show_hidden, int show_unknown, file_type_ext_struct* file_type_ext_table, directory_item_struct** list);

int is_next_movie(const char* prev, const char* next);

#ifdef __cplusplus
}
#endif

#endif

