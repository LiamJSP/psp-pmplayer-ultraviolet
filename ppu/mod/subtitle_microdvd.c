#include <strings.h>
#ifndef stricmp
#define stricmp strcasecmp
#endif
#ifndef strnicmp
#define strnicmp strncasecmp
#endif
/*
PMP Mod
Copyright (C) 2006 Raphael

E-mail:   raphael@fx-world.org

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
microdvd subtitle format parser
*/

#include<psptypes.h>
#include<string.h>
#include "subtitle_microdvd.h"
#include "common/mem64.h"

#include "common/libminiconv.h"

struct subtitle_frame_struct* subtitle_parse_microdvd( FILE *f, char* charset, unsigned int rate, unsigned int scale )
	{
	if (!f || !charset) return(0);
	
	struct subtitle_frame_struct *p = (struct subtitle_frame_struct*)malloc_64( sizeof(struct subtitle_frame_struct) );
	if (p==0) return(0);
	subtitle_frame_safe_constructor(p);
	
	/* Read one bounded header, then consume the entire payload line even
	 * when its displayed text is truncated. Malformed lines must advance. */
	int result;
	int start, end;
	int c;
	for (;;) {
		result = fscanf(f, " {%9d}{%9d}", &start, &end);
		if (result == 2 && start >= 0 && end >= start) {
			p->p_start_frame = (unsigned int)start;
			p->p_end_frame = (unsigned int)end;
			break;
		}
		if (result == EOF) { free_64(p); return 0; }
		do { c = fgetc(f); } while (c != EOF && c != '\n');
		if (c == EOF) { free_64(p); return 0; }
	}
	
	int i = 0;
	p->p_num_lines = 1;
	while (1)
		{
		c = fgetc(f);
		if (c=='\n' || c==EOF) break;
		if (c=='\r' || i >= max_subtitle_string - 1) continue;
		if (c=='|')
			{
			p->p_string[i++]='\n';
			p->p_num_lines++;
			}
		else
			{
			p->p_string[i++]=c;
			}
		}
	p->p_string[i] = '\0';
	
	if ( miniConvHaveSubtitleConv(charset) ) {
		char* temp_str = miniConvSubtitleConv(p->p_string, charset);
		if( temp_str != NULL ) {
			strncpy(p->p_string, temp_str, max_subtitle_string-1);
			p->p_string[max_subtitle_string-1] = '\0';
		}
	}
	else if ( stricmp(charset, "DEFAULT") == 0 && miniConvHaveDefaultSubtitleConv() ){
		char* temp_str = miniConvDefaultSubtitleConv(p->p_string);
		if( temp_str != NULL ) {
			strncpy(p->p_string, temp_str, max_subtitle_string-1);
			p->p_string[max_subtitle_string-1] = '\0';
		}
	}
	
	return(p);
	}
	
