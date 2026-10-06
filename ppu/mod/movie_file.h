/* 
 *	Copyright (C) 2008 cooleyes
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
 
#ifndef __MOVIE_FILE_H__
#define __MOVIE_FILE_H__

#define MAX_MOVIE_SUBTITLES 4

struct subtitle_ext_charset_struct {
	char* ext;
	int ext_len;
	char* charset;
};

struct movie_subtitle_struct {
	char subtitle_file[512];
	char subtitle_charset[32];
};

struct movie_file_struct {
	char movie_file[512];
	char movie_hash[16];
	int movie_subtitle_num;
	struct movie_subtitle_struct movie_subtitles[MAX_MOVIE_SUBTITLES];
};

#endif
