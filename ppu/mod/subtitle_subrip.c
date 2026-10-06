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
subrip subtitle format parser
*/

//a little modify by cooleyes

#include<psptypes.h>
#include<string.h>

#include "subtitle_subrip.h"
#include "common/mem64.h"

#include "common/fgets_mod.h"
#include "common/libminiconv.h"

struct subtitle_frame_struct* subtitle_parse_subrip( FILE *f, char* charset, unsigned int rate, unsigned int scale )
	{
	if (!f || !charset || rate == 0 || scale == 0) return(0);
	
	struct subtitle_frame_struct *p = (struct subtitle_frame_struct*)malloc_64( sizeof(struct subtitle_frame_struct) );
	if (p==0) return(0);
	subtitle_frame_safe_constructor(p);
	
	fgets_function fgets_f;
	if ( stricmp(charset, "UTF-16LE") == 0 )
		fgets_f = fgets_utf16_le;
	else if ( stricmp(charset, "UTF-16BE") == 0 )
		fgets_f = fgets_utf16_be;
	else
		fgets_f = fgets;
		
    char line[1024];
    char c;
    int a1,a2,a3,a4,b1,b2,b3,b4;
    int i, j = 0;
    int in_tag = 0;
    
    while (1)
		{
		memset(line, 0, sizeof(line));
		if (!fgets_f(line, sizeof(line), f)) 
			{
			free_64(p);
			return(0);
			}
		/* Suppressed scansets must never write a punctuation string into an
		 * int. Widths also bound numeric conversions before range checking. */
		if (sscanf(line, "%9d:%2d:%2d%*1[,.:]%3d --> %9d:%2d:%2d%*1[,.:]%3d",
		           &a1,&a2,&a3,&a4,&b1,&b2,&b3,&b4) == 8 &&
		    subtitle_time_to_frame(a1,a2,a3,a4,rate,scale,&p->p_start_frame) &&
		    subtitle_time_to_frame(b1,b2,b3,b4,rate,scale,&p->p_end_frame) &&
		    p->p_end_frame >= p->p_start_frame)
			break;
		}
	
	p->p_num_lines = 0;
	j = 0;
	/* Continue consuming a long cue after the output fills, so its remainder
	 * cannot be mistaken for a later timestamp or write beyond p_string. */
	while (1)
		{
		memset(line, 0, sizeof(line));
	    if (!fgets_f(line, sizeof(line), f)) break;
	    if (line[0]=='\n' || line[0]=='\r') break;
	    i = 0;
		while (line[i] != '\0')
			{
			c = line[i++];
			if (c=='<') { in_tag = 1; continue; }
			if (c=='>') { in_tag = 0; continue; }
			if (c=='\r') continue;
			if (c=='\n') in_tag = 0;
			if (!in_tag && j < max_subtitle_string - 1)
				{
				if (p->p_num_lines == 0) p->p_num_lines = 1;
				p->p_string[j++]=c;
				if (c=='\n') p->p_num_lines++;
				}
			}
		}
	if (j > 0 && p->p_string[j - 1] == '\n') {
		--j;
		if (p->p_num_lines > 1) --p->p_num_lines;
	}
	p->p_string[j] = '\0';

	if ( miniConvHaveSubtitleConv(charset) ) {
		char* temp_str = miniConvSubtitleConv(p->p_string, charset);
		if( temp_str != NULL ) {
			memset(p->p_string, 0, max_subtitle_string);
			strncpy(p->p_string, temp_str, max_subtitle_string-1);
		}
	}
	else if ( stricmp(charset, "DEFAULT") == 0 && miniConvHaveDefaultSubtitleConv() ){
		char* temp_str = miniConvDefaultSubtitleConv(p->p_string);
		if( temp_str != NULL ) {
			memset(p->p_string, 0, max_subtitle_string);
			strncpy(p->p_string, temp_str, max_subtitle_string-1);
		}
	}

	return(p);
	}
