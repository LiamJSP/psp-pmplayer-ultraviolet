/* gu_font.h
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
font rendering system
*/

#ifndef gu_font_h__
#define gu_font_h__

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_STROKER_H
#include FT_SYNTHESIS_H

//#include "mmgr.h"
//#define DEBUG
#ifdef __cplusplus
extern "C" {
#endif

#define MAX_NAME_SIZE 256
#define MAX_CACHE_LINES 8
#define MAX_CACHE_STRING 512
#define MAX_GLYPH_CACHES 96

#define FLAG_NONE			0
#define FLAG_ALIGN_LEFT		0
#define FLAG_ALIGN_CENTER	1
#define FLAG_ALIGN_RIGHT	2
#define FLAG_ALIGN_MASK		3
#define FLAG_SHADOW		4
#define FLAG_NOCACHE		8			// Bypass glyph caching (for text that changes every frame)
#define FLAG_UTF8			16			// interpret input string as UTF8
#define FLAG_UTF16BE		32
#define FLAG_UTF16LE		64

#define FLAG_CLIP_WRAP	0x10
#define FLAG_CLIP_CLAMP	0x20

#define GLYPH_CACHE_VRAM 	0x1000
#define GLYPH_CACHE_SYSMEM	0x2000

// Flags defined in font header
#define GU_FONT_HAS_NORMAL	0x0
#define GU_FONT_HAS_ITALIC	0x1
#define GU_FONT_HAS_BOLD	0x2
#define GU_FONT_HAS_NO_BORDER 0x10
#define GU_FONT_HAS_UNICODE_CHARMAP 0x20

#define GU_FONT_NORMAL GU_FONT_HAS_NORMAL
#define GU_FONT_ITALIC GU_FONT_HAS_ITALIC
#define GU_FONT_BOLD GU_FONT_HAS_BOLD
#define GU_FONT_BOLDITALIC (GU_FONT_HAS_ITALIC|GU_FONT_HAS_BOLD)

#define GU_FONT_REPLACEMENT_CHAR	((unsigned char)'?')

#define FOURCC(A,B,C,D) (unsigned long)((unsigned char)A << 24 | (unsigned char)B << 16 | (unsigned char)C << 8 | (unsigned char)D)

#define BGR444(col) (((col & 0xF0) >> 4) | ((col & 0xF000) >> 8) | ((col & 0xF00000) >> 12))
#define BGR444_blend1(col,i) ((((col & 0xF0)*i/15) >> 4) | (((col & 0xF000)*i/15) >> 8) | (((col & 0xF00000)*i/15) >> 12))
#define BGR444_blend2(col,col2,i) ((((int)((col & 0xF0)*i+(col2 & 0xF0)*(15-i))/15) >> 4) | (((int)((col & 0xF000)*i+(col2 & 0xF000)*(15-i))/15) >> 8) | (((int)((col & 0xF00000)*i+(col2 & 0xF00000)*(15-i))/15) >> 12))

extern int	gufont_haveflags ;
extern int	gufont_hasitalic ;
extern int	gufont_hasbold ;

char* gu_font_init();							// call once on startup before any gu_font_load calls, sets white color clut with black border
void gu_font_close();
char* gu_font_load(char* name );				// loads font 'name' and makes it current
char* gu_font_add_fallback_file(const char* name);
void gu_font_clear_fallbacks(void);
void gu_font_notify_font_set_changed(void);
int gu_font_any_face_has_unicode_charmap(void);
char* gu_font_load_memory(const char* label, const unsigned char* data, unsigned int size);
void gu_font_on_suspend();
void gu_font_on_resume();

void gu_font_pixelsize_set( int size );
void gu_font_scale_set(float asc_scale, float multcode_scale);
void gu_font_border_enable( int enable );
void gu_font_border_set(float border);
void gu_font_border_color_set( unsigned int color );
void gu_font_color_set( unsigned int color );
unsigned int gu_font_color_get(void);
unsigned int gu_font_border_color_get(void);
int gu_font_border_enable_get(void);
void gu_font_embolden_enable( int enable );

void gu_font_align_set(unsigned int align);
unsigned int gu_font_align_get();
void gu_font_distance_set(unsigned int distance);
unsigned int gu_font_distance_get();

// ASCII
int gu_font_line_width_get( char* s );
// flag < 0 : return width of whole string
// flag > 0 : return width of 'flag' first chars
int gu_font_width_get( char* s, int flag );

// UTF-8
int gu_font_utf8_line_width_get( char* s );
int gu_font_utf8_width_get( char* s, int flag );

// UTF-16BE
int gu_font_utf16be_line_width_get( short* s );
int gu_font_utf16be_width_get( short* s, int flag );
int gu_font_utf16be_height_get( short* s );

// UTF-16LE
int gu_font_utf16le_line_width_get( short* s );
int gu_font_utf16le_width_get( short* s, int flag );
int gu_font_utf16le_height_get( short* s );

int gu_font_height_get( char* s );
int gu_font_height();							// same as gu_char_height_get( 'I' )+3

void gu_font_output_set(int x, int y, int w, int h);

void gu_font_printf( int x, int y, int flags, int output_inversion, char* fmt, ... );	// This only works with non UTF-16 encodings
void gu_font_print( int x, int y, int flags, char* s , int output_inversion);

#ifdef __cplusplus
}
#endif

#endif
