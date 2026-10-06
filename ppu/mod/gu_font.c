/*
gu_font.c
PMPlayer Advance
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

/*
 * This renderer intentionally keeps a compact process-global glyph cache.
 * PSP memory pressure makes per-font duplicate caches expensive; callers must
 * serialize font use through the player/UI thread rather than treating this as
 * a re-entrant desktop font service.
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspgu.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include "common/mem64.h"

#include "../libmkvinfo.psp/mkvinfo_type.h"

#include "gu_font.h"
#include "cpu_clock.h"
#include "common/texture_subdivision.h"

#include "common/ppa_cache.h"
#include "common/ppa_io.h"
#include "../common/ppa_scratchpad.h"

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>
#endif

#if defined(PPA_USE_FRIBIDI) && PPA_USE_FRIBIDI
#include <fribidi/fribidi.h>
#endif

static FT_Library library = 0;

static FT_Open_Args open_args;
static FT_StreamRec font_stream;

static char font_filename[1024];
static FT_Face	face = 0;
static FT_Stroker stroker = 0;
/* Font faces/shaping stay identical in saver mode. Only the expensive
 * separately rasterized outline becomes a one-pixel fill-glyph shadow. */
static int gu_font_saver_shadow = 0;

#define GU_FONT_MAX_FALLBACK_FACES 12
#define GU_FONT_FALLBACK_PATH_MAX 256

typedef struct GUFontFallbackFace_ {
	FT_Face face;
	FT_Open_Args open_args;
	FT_StreamRec stream;
	char filename[GU_FONT_FALLBACK_PATH_MAX];
	int loaded;

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	hb_font_t *hb_font;
#endif
} GUFontFallbackFace;

static GUFontFallbackFace gu_font_fallbacks[GU_FONT_MAX_FALLBACK_FACES];
static int gu_font_fallback_count = 0;

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
static hb_font_t *gu_font_primary_hb_font = 0;
#endif

static unsigned char *font_memory_data = 0;
static unsigned int font_memory_size = 0;
static int font_loaded_from_memory = 0;

static unsigned long gufont_font_stream_io(FT_Stream stream,
                                           unsigned long offset,
                                           unsigned char* buffer,
                                           unsigned long count)
{
	int result;

	if (stream == 0)
		return 0;

	if (count == 0)
		return 0;

	if (buffer == 0)
		return 0;

	if (offset > 0x7fffffffUL)
		return 0;

	result = ppa_io_read_exact_at32_retry(stream->descriptor.value,
	                                     (int)offset,
	                                     buffer,
	                                     (unsigned int)count);

	if (result < 0)
		return 0;

	return (unsigned long)result;
}

static void gufont_font_stream_close(FT_Stream stream)
{
	if (stream == 0)
		return;

	if (stream->descriptor.value >= 0) {
		sceIoClose(stream->descriptor.value);
		stream->descriptor.value = -1;
	}
}

int	gufont_haveflags = GU_FONT_HAS_UNICODE_CHARMAP;
int	gufont_hasitalic = 0;
int	gufont_hasbold = 0;

static unsigned int			gufont_color = 0xffffff;
static unsigned int			gufont_border_color = 0x000000;
static unsigned int			gufont_border_enable = 0;
static float				gufont_border_size = 1.0;
static unsigned int			gufont_pixel_size = 16;
static unsigned int			gufont_char_width = 16;
static unsigned int			gufont_char_height = 16;
static float				gufont_asc_scale = 2.0;
static float				gufont_multcode_scale = 1.0;
static unsigned int			gufont_embolden_enable = 0;
static unsigned int			gufont_align = 1;
static unsigned int 			gufont_distance = 16;

static int gu_font_initialized = 0;

#define DIVWIDTH	3

#define CHARSCALEWIDTH( fixed_width, scale ) ((int)(1.0*fixed_width/scale))
#define CHARWIDTH(c) (((c > 255)?CHARSCALEWIDTH(gufont_char_width,gufont_multcode_scale) : CHARSCALEWIDTH(gufont_char_width,gufont_asc_scale)))
#define CHARWIDTH2(c,style) (((c > 255)?CHARSCALEWIDTH(gufont_char_width,gufont_multcode_scale):CHARSCALEWIDTH(gufont_char_width,gufont_asc_scale)))

#define SBIT_HASH_SIZE (997)

typedef struct  Cache_Bitmap_{
    int width;
    int height;
    int left;
    int	top;
    char format;
    short max_grays;
    int	pitch;
    unsigned char*  buffer;
} Cache_Bitmap;

typedef struct SBit_HashItem_ {
	unsigned long ucs_code;
	int face_index;
	int glyph_index;
	int size;
	int embolden;
	int xadvance;
	int yadvance;
	Cache_Bitmap bitmap;
	Cache_Bitmap border_bitmap;
	unsigned int atlas_generation;
	unsigned short atlas_fill_x;
	unsigned short atlas_fill_y;
	unsigned short atlas_border_x;
	unsigned short atlas_border_y;
} SBit_HashItem;

static SBit_HashItem sbit_hash_root[SBIT_HASH_SIZE];

#define IsSet(val,flag) ((((val) & (flag)) == (flag)))

typedef u32 Color;
#define A(color) ((u8)(color >> 24 & 0x000000FF))
#define B(color) ((u8)(color >> 16 & 0x000000FF))
#define G(color) ((u8)(color >> 8 & 0x000000FF))
#define R(color) ((u8)(color & 0x000000FF))

#define DRAW_BUFFER_SIZE 262144

#define SUB_SCREEN_WIDTH 480
#define SUB_SCREEN_HEIGHT 128
#define SUB_SCREEN_TEXTURE_WIDTH 512

static int gufont_output_width = 480;
static int gufont_output_height = 272;
static int gufont_output_sub_width = SUB_SCREEN_WIDTH;
static int gufont_output_sub_height = SUB_SCREEN_HEIGHT;
static int gufont_output_x = 0;
static int gufont_output_y = 0;

/* The legacy renderer allocated two 512x128 ABGR8888 line surfaces (512 KiB)
 * and rewrote/scanned them whenever subtitle text changed.  Phase 5 keeps that
 * path as a lazy correctness fallback, while the normal path stores only 8-bit
 * glyph coverage in a persistent GE texture atlas. */
static unsigned char *sub_8888 = 0;
static unsigned char *border_sub_8888 = 0;

static char cache_string[2048];

#ifndef PPA_ENABLE_GE_FONT_ATLAS
#define PPA_ENABLE_GE_FONT_ATLAS 1
#endif
#ifndef PPA_FONT_ATLAS_LINE_CACHE_COUNT
#define PPA_FONT_ATLAS_LINE_CACHE_COUNT 4
#endif
#ifndef PPA_FONT_ATLAS_MAX_GLYPHS_PER_LINE
#define PPA_FONT_ATLAS_MAX_GLYPHS_PER_LINE 384
#endif
#ifndef PPA_FONT_ATLAS_BATCH_GLYPHS
#define PPA_FONT_ATLAS_BATCH_GLYPHS 96
#endif
#ifndef PPA_FONT_ATLAS_USE_SCRATCHPAD
#define PPA_FONT_ATLAS_USE_SCRATCHPAD 1
#endif

#if PPA_FONT_ATLAS_LINE_CACHE_COUNT < 1
#error PPA_FONT_ATLAS_LINE_CACHE_COUNT must be at least 1
#endif
#if PPA_FONT_ATLAS_MAX_GLYPHS_PER_LINE < 1
#error PPA_FONT_ATLAS_MAX_GLYPHS_PER_LINE must be at least 1
#endif
#if PPA_FONT_ATLAS_BATCH_GLYPHS < 1
#error PPA_FONT_ATLAS_BATCH_GLYPHS must be at least 1
#endif

#define GU_FONT_ATLAS_WIDTH 512
#define GU_FONT_ATLAS_PLANE_HEIGHT 128
#define GU_FONT_ATLAS_HEIGHT (GU_FONT_ATLAS_PLANE_HEIGHT * 2)
#define GU_FONT_ATLAS_BYTES (GU_FONT_ATLAS_WIDTH * GU_FONT_ATLAS_HEIGHT)
#define GU_FONT_ATLAS_PADDING 1
#define GU_FONT_ATLAS_INVALID_COORD 0xffffU

typedef struct GUFontAtlasPacker_ {
    unsigned short x;
    unsigned short y;
    unsigned short row_height;
    unsigned short y_base;
} GUFontAtlasPacker;

typedef struct GUFontAtlasPlacement_ {
    SBit_HashItem *item;
    unsigned long cache_codepoint;
    int face_index;
    unsigned int glyph_index;
    signed short fill_x;
    signed short fill_y;
    signed short border_x;
    signed short border_y;
} GUFontAtlasPlacement;

typedef struct GUFontAtlasLine_ {
    int valid;
    int overflow;
    int fallback_only;
    unsigned int fallback_atlas_generation;
    int x;
    int flags;
    unsigned int font_generation;
    unsigned int lru_tick;
    int glyph_count;
    char text[1024];
    GUFontAtlasPlacement glyphs[PPA_FONT_ATLAS_MAX_GLYPHS_PER_LINE];
} GUFontAtlasLine;

typedef struct GUFontAtlasStats_ {
    unsigned int draw_calls;
    unsigned int line_hits;
    unsigned int line_misses;
    unsigned int atlas_resets;
    unsigned int glyph_uploads;
    unsigned int upload_bytes;
    unsigned int scratchpad_stages;
    unsigned int ge_batches;
    unsigned int legacy_fallbacks;
} GUFontAtlasStats;

#define GU_FONT_ATLAS_STORAGE_BYTES \
    (PPA_ENABLE_GE_FONT_ATLAS ? GU_FONT_ATLAS_BYTES : 1)
#define GU_FONT_ATLAS_STORAGE_LINES \
    (PPA_ENABLE_GE_FONT_ATLAS ? PPA_FONT_ATLAS_LINE_CACHE_COUNT : 1)
#define GU_FONT_ATLAS_STORAGE_CLUT_ENTRIES \
    (PPA_ENABLE_GE_FONT_ATLAS ? 256 : 1)

static unsigned char __attribute__((aligned(64)))
    gu_font_atlas[GU_FONT_ATLAS_STORAGE_BYTES];
static unsigned int __attribute__((aligned(64)))
    gu_font_fill_clut[GU_FONT_ATLAS_STORAGE_CLUT_ENTRIES];
static unsigned int __attribute__((aligned(64)))
    gu_font_border_clut[GU_FONT_ATLAS_STORAGE_CLUT_ENTRIES];
static GUFontAtlasLine gu_font_line_cache[GU_FONT_ATLAS_STORAGE_LINES];
static GUFontAtlasLine *gu_font_capture_line = 0;
static GUFontAtlasPacker gu_font_fill_packer;
static GUFontAtlasPacker gu_font_border_packer;
static GUFontAtlasStats gu_font_atlas_stats;
static unsigned int gu_font_atlas_generation = 1U;
static unsigned int gu_font_layout_generation = 1U;
static unsigned int gu_font_line_lru_tick = 1U;
static unsigned int gu_font_fill_clut_color = 0xffffffffU;
static unsigned int gu_font_border_clut_color = 0xffffffffU;
static int gu_font_atlas_force_texflush = 1;

static void gu_font_atlas_reset(int clear_pixels);
static void gu_font_atlas_invalidate_lines(void);
static void gu_font_atlas_emit_summary(void);

struct vertex_struct
	{
	short texture_x;
	short texture_y;

	float vertex_x;
	float vertex_y;
	float vertex_z;
	};

void putPixelScreenBuffer(void* buffer, Color color, int x, int y)
{
	*((Color*)buffer+y*SUB_SCREEN_TEXTURE_WIDTH+x) = color;
}

void draw_cachedbitmap(void *buffer, Cache_Bitmap* sbt, FT_Int x, FT_Int y, int width, int height, Color color)
{
	FT_Int i, j;
	Color pixel, grey;
	int a, r, g, b;
	if( !sbt->buffer )
		return;
	if(sbt->format ==FT_PIXEL_MODE_MONO){
		for(j = 0; j< sbt->height; j++)
			for(i = 0; i< sbt->width ; i++) {
				if( i+x < 0 || i+x >= width || j+y < 0 || j+y >= height)
					continue;
				if ( sbt->buffer[j*sbt->pitch+i/8] & (0x80>>(i%8)) )
					pixel = color;
				else
					pixel = 0;
				if (pixel)
					putPixelScreenBuffer(buffer, pixel, i+x, j+y);
			}
	}
	else if(sbt->format ==FT_PIXEL_MODE_GRAY){
		for(j = 0; j< sbt->height; j++)
			for(i = 0; i< sbt->width ; i++) {
				if( i+x < 0 || i+x >= width || j+y < 0 || j+y >= height)
					continue;
				grey = sbt->buffer[j*sbt->pitch+i];
				
				if ( grey ) {
					{
						a = (grey * A(color)) / 255;
						r = R(color);//(grey * R(color)) / 255;
						g = G(color);//(grey * G(color)) / 255;
						b = B(color);//(grey * B(color)) / 255;
					}
						
	  				pixel = (a << 24) | (b << 16) | (g << 8) | r;
	  			}
				else
					pixel = 0;
				if (pixel)
					putPixelScreenBuffer(buffer, pixel, i+x, j+y);
			}
	}
}

void draw_bitmap(void *buffer, FT_Bitmap *bitmap, FT_Int x, FT_Int y, int width, int height, Color color)
{
	FT_Int i, j;
	Color pixel, grey;
	int a, r, g, b;
	if( !bitmap->buffer )
		return;
	if(bitmap->pixel_mode ==FT_PIXEL_MODE_MONO){
		for(j = 0; j< bitmap->rows; j++)
			for(i = 0; i< bitmap->width ; i++) {
				if( i+x < 0 || i+x >= width || j+y < 0 || j+y >= height)
					continue;
				if ( bitmap->buffer[j*bitmap->pitch+i/8] & (0x80>>(i%8)) )
					pixel = color;
				else
					pixel = 0;
				if (pixel)
					putPixelScreenBuffer(buffer, pixel, i+x, j+y);
			}
	}
	else if(bitmap->pixel_mode ==FT_PIXEL_MODE_GRAY){
		for(j = 0; j< bitmap->rows; j++)
			for(i = 0; i< bitmap->width ; i++) {
				if( i+x < 0 || i+x >= width || j+y < 0 || j+y >= height)
					continue;
				grey = bitmap->buffer[j*bitmap->pitch+i];
				
				if ( grey ) {
					{
						a = (grey * A(color)) / 255;
						r = R(color);//(grey * R(color)) / 255;
						g = G(color);//(grey * G(color)) / 255;
						b = B(color);//(grey * B(color)) / 255;
					}
					pixel = (a << 24) | (b << 16) | (g << 8) | r;
	  			}
				else
					pixel = 0;
				if (pixel)
					putPixelScreenBuffer(buffer, pixel, i+x, j+y);
			}
	}
}

// NOTE: the following function only handles triple byte utf8 encodings maximum

static unsigned long get_next_utf8(char **utf8)
{
	unsigned char *p;
	unsigned long cp;

	if (utf8 == 0 || *utf8 == 0)
		return '?';

	p = (unsigned char *)(*utf8);

	while ((*p & 0xC0) == 0x80)
		p++;

	if (*p == 0) {
		*utf8 = (char *)p;
		return 0;
	}

	if ((*p & 0x80) == 0) {
		cp = (unsigned long)*p;
		*utf8 = (char *)(p + 1);
		return cp;
	}

	if ((*p & 0xE0) == 0xC0) {
		if ((p[1] & 0xC0) != 0x80) {
			*utf8 = (char *)(p + 1);
			return '?';
		}

		cp = ((unsigned long)(p[0] & 0x1F) << 6) |
		     ((unsigned long)(p[1] & 0x3F));

		if (cp < 0x80)
			cp = '?';

		*utf8 = (char *)(p + 2);
		return cp;
	}

	if ((*p & 0xF0) == 0xE0) {
		if ((p[1] & 0xC0) != 0x80 ||
		    (p[2] & 0xC0) != 0x80) {
			*utf8 = (char *)(p + 1);
			return '?';
		}

		cp = ((unsigned long)(p[0] & 0x0F) << 12) |
		     ((unsigned long)(p[1] & 0x3F) << 6) |
		     ((unsigned long)(p[2] & 0x3F));

		if (cp < 0x800 ||
		    (cp >= 0xD800 && cp <= 0xDFFF))
			cp = '?';

		*utf8 = (char *)(p + 3);
		return cp;
	}

	if ((*p & 0xF8) == 0xF0) {
		if ((p[1] & 0xC0) != 0x80 ||
		    (p[2] & 0xC0) != 0x80 ||
		    (p[3] & 0xC0) != 0x80) {
			*utf8 = (char *)(p + 1);
			return '?';
		}

		cp = ((unsigned long)(p[0] & 0x07) << 18) |
		     ((unsigned long)(p[1] & 0x3F) << 12) |
		     ((unsigned long)(p[2] & 0x3F) << 6) |
		     ((unsigned long)(p[3] & 0x3F));

		if (cp < 0x10000 || cp > 0x10FFFF)
			cp = '?';

		*utf8 = (char *)(p + 4);
		return cp;
	}

	*utf8 = (char *)(p + 1);
	return '?';
}

static unsigned short get_next_utf16le( short** utf16 )
	{
	if ((*utf16)==0) return 0;

	return (unsigned short)(*(*utf16));
	}

static unsigned short get_next_utf16be( short** utf16 )
	{
	if ((*utf16)==0) return 0;
	
	unsigned short u1 =*(*utf16);
	
	return (unsigned short)(((u1&0xFF)<<8)|(u1>>8));
	}

#define SWAPBYTES(c) ((((c)&0xFF)<<8)|((c)>>8))

static int gu_font_parse_bbcode( char** c,  int* style )
	{
	if (c==0 || *c==0) return(0);
	register char c0 = (*c)[0], c1 = (*c)[1], c2 = (*c)[2], c3 = (*c)[3];
	if (c0=='[')
		{
		// parse bbcode formatting
		if (c1=='/' && c3==']')
			{
			if (c2=='i' || c2=='I')
				{
				*style &= ~GU_FONT_ITALIC;
				(*c)+=3;
				}
			else
			if (c2=='b' || c2=='B')
				{
				*style &= ~GU_FONT_BOLD;
				(*c)+=3;
				}
			else
				return(0);
			}
		else if (c2==']')
			{
			if (c1=='i' || c1=='I')
				{
				*style |= GU_FONT_ITALIC;
				(*c)+=2;
				}
			else
			if (c1=='b' || c1=='B')
				{
				*style |= GU_FONT_BOLD;
				(*c)+=2;
				}
			else
				return(0);
			}
		else
			return(0);
		}
	else
		return(0);
	
	return(1);
	}

static int gu_font_utf16le_parse_bbcode( short** c,  int* style )
	{
	if (c==0 || *c==0) return(0);
	register short c0 = (*c)[0], c1 = (*c)[1], c2 = (*c)[2], c3 = (*c)[3];
	if (c0=='[')
		{
		// parse bbcode formatting
		if (c1=='/' && c3==']')
			{
			if (c2=='i' || c2=='I')
				{
				*style &= ~GU_FONT_ITALIC;
				(*c)+=3;
				}
			else
			if (c2=='b' || c2=='B')
				{
				*style &= ~GU_FONT_BOLD;
				(*c)+=3;
				}
			else
				return(0);
			}
		else if (c2==']')
			{
			if (c1=='i' || c1=='I')
				{
				*style |= GU_FONT_ITALIC;
				(*c)+=2;
				}
			else
			if (c1=='b' || c1=='B')
				{
				*style |= GU_FONT_BOLD;
				(*c)+=2;
				}
			else
				return(0);
			}
		else
			return(0);
		}
	else
		return(0);
	
	return(1);
	}

static int gu_font_utf16be_parse_bbcode( short** c,  int* style )
	{
	if (c==0 || *c==0) return(0);
	
	if (SWAPBYTES((*c)[0])=='[')
		{
		// parse bbcode formatting
		if (SWAPBYTES((*c)[1])=='/' && SWAPBYTES((*c)[3])==']')
			{
			if (SWAPBYTES((*c)[2])=='i' || SWAPBYTES((*c)[2])=='I')
				{
				*style &= ~GU_FONT_ITALIC;
				(*c)+=3;
				}
			else
			if (SWAPBYTES((*c)[2])=='b' || SWAPBYTES((*c)[2])=='B')
				{
				*style &= ~GU_FONT_BOLD;
				(*c)+=3;
				}
			else
				return(0);
			}
		else if (SWAPBYTES((*c)[2])==']')
			{
			if (SWAPBYTES((*c)[1])=='i' || SWAPBYTES((*c)[1])=='I')
				{
				*style |= GU_FONT_ITALIC;
				(*c)+=2;
				}
			else
			if (SWAPBYTES((*c)[1])=='b' || SWAPBYTES((*c)[1])=='B')
				{
				*style |= GU_FONT_BOLD;
				(*c)+=2;
				}
			else
				return(0);
			}
		else
			return(0);
		}
	else
		return(0);
	
	return(1);
	}

void sbit_cache_init(void)
{
	int i;
	for (i = 0; i < SBIT_HASH_SIZE; ++i) {
		memset(&sbit_hash_root[i], 0, sizeof(SBit_HashItem));
	}
}

void sbit_cache_done(void)
{
	int i;
	for (i = 0; i < SBIT_HASH_SIZE; ++i) {
		if(sbit_hash_root[i].bitmap.buffer) {
			free(sbit_hash_root[i].bitmap.buffer);
		}
		if(sbit_hash_root[i].border_bitmap.buffer) {
			free(sbit_hash_root[i].border_bitmap.buffer);
		}
	}
}
SBit_HashItem* sbit_cache_find(int face_index,
                               unsigned long ucs_code,
                               int glyph_index,
                               int size,
                               int embolden)
{
    unsigned long hash_value;
    unsigned long probe;

    hash_value =
        ((ucs_code * 33UL) ^
         ((unsigned long)glyph_index * 131UL) ^
         ((unsigned long)face_index * 997UL)) % SBIT_HASH_SIZE;

    /* Linear probing avoids the old direct-hash overwrite behavior.  A single
     * subtitle line can legitimately contain colliding glyphs; atlas plans
     * retain pointers until the GE draw is submitted. */
    for (probe = 0; probe < SBIT_HASH_SIZE; ++probe) {
        SBit_HashItem *item =
            &sbit_hash_root[(hash_value + probe) % SBIT_HASH_SIZE];

        if (item->glyph_index == 0)
            return 0;

        if (item->ucs_code == ucs_code &&
            item->face_index == face_index &&
            item->glyph_index == glyph_index &&
            item->size == size &&
            item->embolden == embolden)
            return item;
    }

    return 0;
}

void sbit_cache_add(int face_index,
                    unsigned long ucs_code,
                    int glyph_index,
                    int size,
                    int embolden,
                    FT_Bitmap *bitmap,
                    int left,
                    int top,
                    FT_Bitmap *border_bitmap,
                    int border_left,
                    int border_top,
                    int xadvance,
                    int yadvance)
{
    unsigned long hash_value;
    unsigned long probe;
    SBit_HashItem* item = 0;
    int pitch;

    hash_value =
        ((ucs_code * 33UL) ^
         ((unsigned long)glyph_index * 131UL) ^
         ((unsigned long)face_index * 997UL)) % SBIT_HASH_SIZE;

    for (probe = 0; probe < SBIT_HASH_SIZE; ++probe) {
        SBit_HashItem *candidate =
            &sbit_hash_root[(hash_value + probe) % SBIT_HASH_SIZE];

        if (candidate->glyph_index == 0 ||
            (candidate->ucs_code == ucs_code &&
             candidate->face_index == face_index &&
             candidate->glyph_index == glyph_index &&
             candidate->size == size &&
             candidate->embolden == embolden)) {
            item = candidate;
            break;
        }
    }

    /* A full 997-entry cache is far beyond what the atlas can keep resident.
     * Reuse the home slot rather than allocating unbounded glyph state. */
    if (item == 0)
        item = &sbit_hash_root[hash_value];

    if (item->bitmap.buffer) {
        free(item->bitmap.buffer);
        item->bitmap.buffer = 0;
    }

    if (item->border_bitmap.buffer) {
        free(item->border_bitmap.buffer);
        item->border_bitmap.buffer = 0;
    }

    memset(&item->bitmap, 0, sizeof(item->bitmap));
    memset(&item->border_bitmap, 0, sizeof(item->border_bitmap));

    item->ucs_code = ucs_code;
    item->face_index = face_index;
    item->glyph_index = glyph_index;
    item->size = size;
    item->embolden = embolden;
    item->xadvance = xadvance;
    item->yadvance = yadvance;
    item->atlas_generation = 0U;
    item->atlas_fill_x = GU_FONT_ATLAS_INVALID_COORD;
    item->atlas_fill_y = GU_FONT_ATLAS_INVALID_COORD;
    item->atlas_border_x = GU_FONT_ATLAS_INVALID_COORD;
    item->atlas_border_y = GU_FONT_ATLAS_INVALID_COORD;

    item->bitmap.width = bitmap->width;
    item->bitmap.height = bitmap->rows;
    item->bitmap.left = left;
    item->bitmap.top = top;
    item->bitmap.format = bitmap->pixel_mode;
    item->bitmap.max_grays = bitmap->num_grays > 0 ? bitmap->num_grays - 1 : 255;

    item->border_bitmap.width = border_bitmap->width;
    item->border_bitmap.height = border_bitmap->rows;
    item->border_bitmap.left = border_left;
    item->border_bitmap.top = border_top;
    item->border_bitmap.format = border_bitmap->pixel_mode;
    item->border_bitmap.max_grays =
        border_bitmap->num_grays > 0 ? border_bitmap->num_grays - 1 : 255;

    /* FreeType may expose bottom-up bitmaps through a negative pitch.  The old
     * direct memcpy assumed positive pitch and could copy from the wrong side
     * of that allocation.  Normalize every cached bitmap to top-to-bottom,
     * positive-pitch storage once; all later atlas and fallback reads then use
     * the same compact row convention. */
    pitch = bitmap->pitch;
    if (pitch < 0)
        pitch = -pitch;
    item->bitmap.pitch = pitch;

    if (pitch * bitmap->rows > 0) {
        int row;
        item->bitmap.buffer = malloc_64(pitch * bitmap->rows);
        if (item->bitmap.buffer != 0) {
            memset(item->bitmap.buffer, 0, pitch * bitmap->rows);
            for (row = 0; row < bitmap->rows; ++row) {
                const unsigned char *src =
                    bitmap->buffer + row * bitmap->pitch;
                memcpy(item->bitmap.buffer + row * pitch,
                       src,
                       (unsigned int)pitch);
            }
        }
    }

    pitch = border_bitmap->pitch;
    if (pitch < 0)
        pitch = -pitch;
    item->border_bitmap.pitch = pitch;

    if (pitch * border_bitmap->rows > 0) {
        int row;
        item->border_bitmap.buffer = malloc_64(pitch * border_bitmap->rows);
        if (item->border_bitmap.buffer != 0) {
            memset(item->border_bitmap.buffer, 0, pitch * border_bitmap->rows);
            for (row = 0; row < border_bitmap->rows; ++row) {
                const unsigned char *src =
                    border_bitmap->buffer + row * border_bitmap->pitch;
                memcpy(item->border_bitmap.buffer + row * pitch,
                       src,
                       (unsigned int)pitch);
            }
        }
    }
}

static unsigned long gufont_fallback_stream_io(FT_Stream stream,
                                               unsigned long offset,
                                               unsigned char* buffer,
                                               unsigned long count)
{
	int result;

	if (stream == 0)
		return 0;

	if (count == 0)
		return 0;

	if (buffer == 0)
		return 0;

	if (offset > 0x7fffffffUL)
		return 0;

	result = ppa_io_read_exact_at32_retry(stream->descriptor.value,
	                                     (int)offset,
	                                     buffer,
	                                     (unsigned int)count);

	if (result < 0)
		return 0;

	return (unsigned long)result;
}

static void gufont_fallback_stream_close(FT_Stream stream)
{
	if (stream == 0)
		return;

	if (stream->descriptor.value >= 0) {
		sceIoClose(stream->descriptor.value);
		stream->descriptor.value = -1;
	}
}

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
static void gu_font_destroy_hb_fonts(void)
{
	int i;

	if (gu_font_primary_hb_font != 0) {
		hb_font_destroy(gu_font_primary_hb_font);
		gu_font_primary_hb_font = 0;
	}

	for (i = 0; i < gu_font_fallback_count; i++) {
		if (gu_font_fallbacks[i].hb_font != 0) {
			hb_font_destroy(gu_font_fallbacks[i].hb_font);
			gu_font_fallbacks[i].hb_font = 0;
		}
	}
}
#else
static void gu_font_destroy_hb_fonts(void)
{
}
#endif

void gu_font_notify_font_set_changed(void)
{
	gu_font_destroy_hb_fonts();

	gu_font_atlas_emit_summary();
	memset(&gu_font_atlas_stats, 0, sizeof(gu_font_atlas_stats));
	memset(cache_string, 0, sizeof(cache_string));
	gu_font_atlas_invalidate_lines();
	gu_font_atlas_reset(1);

	++gu_font_layout_generation;
	if (gu_font_layout_generation == 0U)
		gu_font_layout_generation = 1U;

	sbit_cache_done();
	sbit_cache_init();
}

void gu_font_clear_fallbacks(void)
{
	int i;

	for (i = 0; i < gu_font_fallback_count; i++) {
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
		if (gu_font_fallbacks[i].hb_font != 0) {
			hb_font_destroy(gu_font_fallbacks[i].hb_font);
			gu_font_fallbacks[i].hb_font = 0;
		}
#endif

		if (gu_font_fallbacks[i].face != 0) {
			FT_Done_Face(gu_font_fallbacks[i].face);
			gu_font_fallbacks[i].face = 0;
		}

		if (gu_font_fallbacks[i].stream.descriptor.value >= 0) {
			sceIoClose(gu_font_fallbacks[i].stream.descriptor.value);
			gu_font_fallbacks[i].stream.descriptor.value = -1;
		}

		memset(&gu_font_fallbacks[i], 0, sizeof(gu_font_fallbacks[i]));
		gu_font_fallbacks[i].stream.descriptor.value = -1;
	}

	gu_font_fallback_count = 0;

	gu_font_notify_font_set_changed();
}

static void gu_font_prepare_face(FT_Face f)
{
	if (f == 0)
		return;

	FT_Select_Charmap(f, FT_ENCODING_UNICODE);
	FT_Set_Pixel_Sizes(f, gufont_char_width, gufont_char_height);
}

char* gu_font_add_fallback_file(const char* name)
{
	GUFontFallbackFace *fallback;
	FT_Error error;
	int fd;
	int size;

	if (gu_font_initialized == 0)
		return "gu_font_add_fallback_file: font system not initialized";

	if (name == 0 || name[0] == '\0')
		return "gu_font_add_fallback_file: invalid filename";

	if (gu_font_fallback_count >= GU_FONT_MAX_FALLBACK_FACES)
		return "gu_font_add_fallback_file: too many fallback fonts";

	fd = ppa_io_open_read_retry(name, PSP_O_RDONLY, 0777);
	if (fd < 0)
		return "gu_font_add_fallback_file: can't open font file";

	size = sceIoLseek32(fd, 0, PSP_SEEK_END);
	if (size <= 0) {
		sceIoClose(fd);
		return "gu_font_add_fallback_file: invalid font file size";
	}

	sceIoLseek32(fd, 0, PSP_SEEK_SET);

	fallback = &gu_font_fallbacks[gu_font_fallback_count];
	memset(fallback, 0, sizeof(*fallback));
	fallback->stream.descriptor.value = -1;

	fallback->stream.descriptor.value = fd;
	fallback->stream.base = 0;
	fallback->stream.size = size;
	fallback->stream.pos = 0;
	fallback->stream.read = gufont_fallback_stream_io;
	fallback->stream.close = gufont_fallback_stream_close;

	fallback->open_args.flags = FT_OPEN_STREAM;
	fallback->open_args.stream = &fallback->stream;

	error = FT_Open_Face(library, &fallback->open_args, 0, &fallback->face);
	if (error) {
		sceIoClose(fd);
		memset(fallback, 0, sizeof(*fallback));
		fallback->stream.descriptor.value = -1;
		return "gu_font_add_fallback_file: FT_Open_Face failed";
	}

	gu_font_prepare_face(fallback->face);

	strncpy(fallback->filename, name, sizeof(fallback->filename) - 1);
	fallback->loaded = 1;

	gu_font_fallback_count++;

	gu_font_notify_font_set_changed();

	return 0;
}

static FT_Face gu_font_face_by_index(int face_index)
{
	if (face_index == 0)
		return face;

	if (face_index > 0 &&
	    face_index <= gu_font_fallback_count)
		return gu_font_fallbacks[face_index - 1].face;

	return 0;
}

static FT_Face gu_font_find_face_for_codepoint(unsigned long ucs_code,
                                               int *out_face_index,
                                               FT_UInt *out_glyph_index)
{
	FT_UInt glyph_index;
	int i;

	if (out_face_index != 0)
		*out_face_index = 0;

	if (out_glyph_index != 0)
		*out_glyph_index = 0;

	if (face != 0) {
		glyph_index = FT_Get_Char_Index(face, ucs_code);

		if (glyph_index != 0) {
			if (out_face_index != 0)
				*out_face_index = 0;

			if (out_glyph_index != 0)
				*out_glyph_index = glyph_index;

			return face;
		}
	}

	for (i = 0; i < gu_font_fallback_count; i++) {
		if (gu_font_fallbacks[i].face == 0)
			continue;

		glyph_index = FT_Get_Char_Index(gu_font_fallbacks[i].face,
		                                ucs_code);

		if (glyph_index != 0) {
			if (out_face_index != 0)
				*out_face_index = i + 1;

			if (out_glyph_index != 0)
				*out_glyph_index = glyph_index;

			return gu_font_fallbacks[i].face;
		}
	}

	return face;
}

int gu_font_any_face_has_unicode_charmap(void)
{
	int i;

	if (face != 0) {
		if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0)
			return 1;
	}

	for (i = 0; i < gu_font_fallback_count; i++) {
		if (gu_font_fallbacks[i].face == 0)
			continue;

		if (FT_Select_Charmap(gu_font_fallbacks[i].face,
		                      FT_ENCODING_UNICODE) == 0)
			return 1;
	}

	return 0;
}

static SBit_HashItem *gu_font_ensure_glyph_index(int face_index,
                                                   unsigned long cache_codepoint,
                                                   FT_UInt glyph_index)
{
    FT_Face draw_face;
    SBit_HashItem* cache_item;
    FT_Error error;
    FT_Glyph glyph = 0;
    FT_BitmapGlyph bit;
    FT_GlyphSlot slot;
    FT_Bitmap empty_border;

    if (glyph_index == 0)
        return 0;

    draw_face = gu_font_face_by_index(face_index);
    if (draw_face == 0)
        return 0;

    cache_item =
        sbit_cache_find(face_index,
                        cache_codepoint,
                        glyph_index,
                        gufont_pixel_size,
                        gufont_embolden_enable);
    if (cache_item != 0)
        return cache_item;

    error = FT_Load_Glyph(draw_face,
                          glyph_index,
                          FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
    if (error)
        return 0;

    slot = draw_face->glyph;

    if (gufont_embolden_enable)
        FT_GlyphSlot_Embolden(slot);

    if (gu_font_saver_shadow) {
        /* Reuse fill coverage for the dark shadow at draw time. Avoid
         * FreeType stroking, a second glyph bitmap and a second atlas upload. */
        error = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
        if (error)
            return 0;
        memset(&empty_border, 0, sizeof(empty_border));
        sbit_cache_add(face_index, cache_codepoint, glyph_index,
                       gufont_pixel_size, gufont_embolden_enable,
                       &slot->bitmap, slot->bitmap_left, slot->bitmap_top,
                       &empty_border, 0, 0,
                       slot->advance.x, slot->advance.y);
        return sbit_cache_find(face_index, cache_codepoint, glyph_index,
                               gufont_pixel_size, gufont_embolden_enable);
    }

    error = FT_Get_Glyph(slot, &glyph);
    if (error)
        return 0;

    error = FT_Glyph_Stroke(&glyph, stroker, 1);
    if (error) {
        FT_Done_Glyph(glyph);
        return 0;
    }

    error = FT_Glyph_To_Bitmap(&glyph,
                               FT_RENDER_MODE_NORMAL,
                               0,
                               1);
    if (error) {
        FT_Done_Glyph(glyph);
        return 0;
    }

    error = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
    if (error) {
        FT_Done_Glyph(glyph);
        return 0;
    }

    bit = (FT_BitmapGlyph)glyph;

    sbit_cache_add(face_index,
                   cache_codepoint,
                   glyph_index,
                   gufont_pixel_size,
                   gufont_embolden_enable,
                   &slot->bitmap,
                   slot->bitmap_left,
                   slot->bitmap_top,
                   &bit->bitmap,
                   bit->left,
                   bit->top,
                   slot->advance.x,
                   slot->advance.y);

    FT_Done_Glyph(glyph);

    return sbit_cache_find(face_index,
                           cache_codepoint,
                           glyph_index,
                           gufont_pixel_size,
                           gufont_embolden_enable);
}

static void gu_font_capture_glyph(SBit_HashItem *item,
                                  unsigned long cache_codepoint,
                                  FT_UInt glyph_index,
                                  int pen_x,
                                  int pen_y,
                                  int x_offset,
                                  int y_offset)
{
    GUFontAtlasPlacement *placement;

    if (gu_font_capture_line == 0 || item == 0)
        return;

    if (gu_font_capture_line->glyph_count >=
        PPA_FONT_ATLAS_MAX_GLYPHS_PER_LINE) {
        gu_font_capture_line->overflow = 1;
        return;
    }

    placement =
        &gu_font_capture_line->glyphs[gu_font_capture_line->glyph_count++];
    memset(placement, 0, sizeof(*placement));
    placement->item = item;
    placement->cache_codepoint = cache_codepoint;
    placement->face_index = item->face_index;
    placement->glyph_index = glyph_index;
    placement->fill_x =
        (signed short)(pen_x + x_offset + item->bitmap.left);
    placement->fill_y =
        (signed short)(pen_y - y_offset - item->bitmap.top);
    placement->border_x =
        (signed short)(pen_x + x_offset + item->border_bitmap.left);
    placement->border_y =
        (signed short)(pen_y - y_offset - item->border_bitmap.top);
}

static void gu_font_draw_glyph_index(int face_index,
                                     unsigned long cache_codepoint,
                                     FT_UInt glyph_index,
                                     int pen_x,
                                     int pen_y,
                                     int x_offset,
                                     int y_offset,
                                     int width,
                                     int height)
{
    SBit_HashItem *cache_item =
        gu_font_ensure_glyph_index(face_index,
                                   cache_codepoint,
                                   glyph_index);

    if (cache_item == 0)
        return;

    if (gu_font_capture_line != 0) {
        gu_font_capture_glyph(cache_item,
                              cache_codepoint,
                              glyph_index,
                              pen_x,
                              pen_y,
                              x_offset,
                              y_offset);
        return;
    }

    if (sub_8888 != 0) {
        draw_cachedbitmap(sub_8888,
                          &cache_item->bitmap,
                          pen_x + x_offset + cache_item->bitmap.left,
                          pen_y - y_offset - cache_item->bitmap.top,
                          width,
                          height,
                          gufont_color);
    }

    if (border_sub_8888 != 0) {
        draw_cachedbitmap(border_sub_8888,
                          gu_font_saver_shadow ? &cache_item->bitmap :
                              &cache_item->border_bitmap,
                          pen_x + x_offset + (gu_font_saver_shadow ?
                              cache_item->bitmap.left + 1 : cache_item->border_bitmap.left),
                          pen_y - y_offset - (gu_font_saver_shadow ?
                              cache_item->bitmap.top - 1 : cache_item->border_bitmap.top),
                          width,
                          height,
                          gufont_border_color);
    }
}

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ

#define GU_FONT_HB_MAX_CHARS 1024
#define GU_FONT_HB_MAX_RUNS 128

typedef struct GUFontHBRun_ {
	int start;
	int end;
	int level;
	int face_index;
	hb_script_t script;
} GUFontHBRun;

static int gu_font_hb_is_format_control_cp(unsigned long cp)
{
	if (cp == 0x00AD)
		return 1;

	if (cp == 0x061C)
		return 1;

	if (cp == 0x200B)
		return 1;

	if (cp == 0x200E || cp == 0x200F)
		return 1;

	if (cp >= 0x202A && cp <= 0x202E)
		return 1;

	if (cp >= 0x2066 && cp <= 0x2069)
		return 1;

	if (cp == 0xFEFF)
		return 1;

	return 0;
}

static hb_font_t *gu_font_hb_font_for_face_index(int face_index)
{
	FT_Face f;

	f = gu_font_face_by_index(face_index);
	if (f == 0)
		return 0;

	if (face_index == 0) {
		if (gu_font_primary_hb_font == 0) {
			gu_font_primary_hb_font = hb_ft_font_create_referenced(f);
			if (gu_font_primary_hb_font != 0) {
				hb_ft_font_set_load_flags(gu_font_primary_hb_font,
				                          FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
			}
		}

		return gu_font_primary_hb_font;
	}

	if (face_index > 0 &&
	    face_index <= gu_font_fallback_count) {
		GUFontFallbackFace *fallback =
			&gu_font_fallbacks[face_index - 1];

		if (fallback->hb_font == 0) {
			fallback->hb_font = hb_ft_font_create_referenced(fallback->face);
			if (fallback->hb_font != 0) {
				hb_ft_font_set_load_flags(fallback->hb_font,
				                          FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
			}
		}

		return fallback->hb_font;
	}

	return 0;
}

static int gu_font_hb_is_rtl_cp(unsigned long cp)
{
	if (cp >= 0x0590 && cp <= 0x08FF)
		return 1;

	if (cp >= 0xFB1D && cp <= 0xFDFF)
		return 1;

	if (cp >= 0xFE70 && cp <= 0xFEFF)
		return 1;

	return 0;
}

static int gu_font_hb_is_strong_ltr_cp(unsigned long cp)
{
	if ((cp >= 'A' && cp <= 'Z') ||
	    (cp >= 'a' && cp <= 'z'))
		return 1;

	if (cp >= 0x0370 && cp <= 0x03FF)
		return 1;

	if (cp >= 0x0400 && cp <= 0x052F)
		return 1;

	if (cp >= 0x0900 && cp <= 0x0DFF)
		return 1;

	if (cp >= 0x0E00 && cp <= 0x0E7F)
		return 1;

	if (cp >= 0x1100 && cp <= 0x11FF)
		return 1;

	if (cp >= 0x2E80 && cp <= 0x9FFF)
		return 1;

	if (cp >= 0xAC00 && cp <= 0xD7AF)
		return 1;

	return 0;
}

static int gu_font_hb_is_neutral_cp(unsigned long cp)
{
	if (cp == ' ' || cp == '\t')
		return 1;

	if (cp >= '0' && cp <= '9')
		return 1;

	if (cp == '.' || cp == ',' || cp == ':' || cp == ';' ||
	    cp == '!' || cp == '?' || cp == '-' || cp == '_' ||
	    cp == '\'' || cp == '"' || cp == '(' || cp == ')' ||
	    cp == '[' || cp == ']' || cp == '{' || cp == '}' ||
	    cp == '<' || cp == '>' || cp == '/')
		return 1;

	return 0;
}

static hb_script_t gu_font_hb_script_for_cp(unsigned long cp)
{
	if (cp >= 0x0600 && cp <= 0x08FF)
		return HB_SCRIPT_ARABIC;

	if (cp >= 0xFB50 && cp <= 0xFEFF)
		return HB_SCRIPT_ARABIC;

	if (cp >= 0x0590 && cp <= 0x05FF)
		return HB_SCRIPT_HEBREW;

	if (cp >= 0x0900 && cp <= 0x097F)
		return HB_SCRIPT_DEVANAGARI;

	if (cp >= 0x0980 && cp <= 0x09FF)
		return HB_SCRIPT_BENGALI;

	if (cp >= 0x0E00 && cp <= 0x0E7F)
		return HB_SCRIPT_THAI;

	if (cp >= 0xAC00 && cp <= 0xD7AF)
		return HB_SCRIPT_HANGUL;

	if (cp >= 0x1100 && cp <= 0x11FF)
		return HB_SCRIPT_HANGUL;

	if (cp >= 0x3040 && cp <= 0x309F)
		return HB_SCRIPT_HIRAGANA;

	if (cp >= 0x30A0 && cp <= 0x30FF)
		return HB_SCRIPT_KATAKANA;

	if (cp >= 0x2E80 && cp <= 0x9FFF)
		return HB_SCRIPT_HAN;

	if (cp >= 0x0400 && cp <= 0x052F)
		return HB_SCRIPT_CYRILLIC;

	if (cp >= 0x0370 && cp <= 0x03FF)
		return HB_SCRIPT_GREEK;

	if ((cp >= 'A' && cp <= 'Z') ||
	    (cp >= 'a' && cp <= 'z'))
		return HB_SCRIPT_LATIN;

	return HB_SCRIPT_COMMON;
}

static int gu_font_hb_face_index_for_cp(unsigned long cp, int preferred_face)
{
	int face_index = preferred_face;
	FT_UInt glyph_index = 0;

	if (cp == ' ' || cp == '\t' || gu_font_hb_is_neutral_cp(cp))
		return preferred_face;

	gu_font_find_face_for_codepoint(cp, &face_index, &glyph_index);

	if (glyph_index == 0)
		return preferred_face;

	return face_index;
}

static int gu_font_hb_decode_utf8_line(char *s,
                                       hb_codepoint_t *out,
                                       int max_out)
{
	char *p;
	int count = 0;
	int style = 0;

	if (s == 0 || out == 0 || max_out <= 0)
		return 0;

	p = s;

	while (*p != '\0' && *p != '\n' && count < max_out) {
		unsigned char ch = (unsigned char)*p;

		if (ch < 32) {
			p++;
			continue;
		}

		if (gu_font_parse_bbcode(&p, &style) != 0) {
			p++;
			continue;
		}

		if (ch == '\t') {
			out[count++] = ' ';
			p++;
			continue;
		}

		{
			unsigned long cp = get_next_utf8(&p);

			if (cp == 0)
				break;

			if (gu_font_hb_is_format_control_cp(cp))
				continue;

			out[count++] = (hb_codepoint_t)cp;
		}
	}

	return count;
}

static int gu_font_hb_first_strong_rtl(const hb_codepoint_t *cps, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		unsigned long cp = cps[i];

		if (gu_font_hb_is_rtl_cp(cp))
			return 1;

		if (gu_font_hb_is_strong_ltr_cp(cp))
			return 0;
	}

	return 0;
}

static void gu_font_hb_build_levels(const hb_codepoint_t *cps,
                                    int count,
                                    int *levels)
{
	int i;
	int base_rtl;

	if (levels == 0)
		return;

	for (i = 0; i < count; i++)
		levels[i] = 0;

	if (count <= 0)
		return;

#if defined(PPA_USE_FRIBIDI) && PPA_USE_FRIBIDI
	{
		FriBidiCharType types[GU_FONT_HB_MAX_CHARS];
		FriBidiLevel fb_levels[GU_FONT_HB_MAX_CHARS];
		FriBidiParType base_dir = FRIBIDI_PAR_ON;

		if (count > GU_FONT_HB_MAX_CHARS)
			count = GU_FONT_HB_MAX_CHARS;

		fribidi_get_bidi_types((const FriBidiChar *)cps,
		                       count,
		                       types);

		if (fribidi_get_par_embedding_levels(types,
		                                     count,
		                                     &base_dir,
		                                     fb_levels)) {
			for (i = 0; i < count; i++)
				levels[i] = fb_levels[i];

			return;
		}
	}
#endif

	base_rtl = gu_font_hb_first_strong_rtl(cps, count);

	for (i = 0; i < count; i++) {
		unsigned long cp = cps[i];

		if (gu_font_hb_is_rtl_cp(cp))
			levels[i] = 1;
		else if (gu_font_hb_is_neutral_cp(cp))
			levels[i] = base_rtl ? 1 : 0;
		else
			levels[i] = 0;
	}
}

static int gu_font_hb_make_runs(const hb_codepoint_t *cps,
                                int count,
                                GUFontHBRun *runs,
                                int max_runs)
{
	int levels[GU_FONT_HB_MAX_CHARS];
	int run_count = 0;
	int i;
	int start;
	int level;
	int face_index;
	int preferred_face;
	hb_script_t script;

	if (cps == 0 || runs == 0 || count <= 0 || max_runs <= 0)
		return 0;

	if (count > GU_FONT_HB_MAX_CHARS)
		count = GU_FONT_HB_MAX_CHARS;

	gu_font_hb_build_levels(cps, count, levels);

	preferred_face = 0;
	start = 0;
	level = levels[0];
	face_index = gu_font_hb_face_index_for_cp(cps[0], preferred_face);
	preferred_face = face_index;
	script = gu_font_hb_script_for_cp(cps[0]);

	if (script == HB_SCRIPT_COMMON)
		script = level & 1 ? HB_SCRIPT_ARABIC : HB_SCRIPT_LATIN;

	for (i = 1; i < count; i++) {
		int next_level;
		int next_face;
		hb_script_t next_script;
		int split = 0;

		next_level = levels[i];
		next_face = gu_font_hb_face_index_for_cp(cps[i], preferred_face);
		next_script = gu_font_hb_script_for_cp(cps[i]);

		if (next_script == HB_SCRIPT_COMMON)
			next_script = script;

		if (next_level != level)
			split = 1;

		if (next_face != face_index && !gu_font_hb_is_neutral_cp(cps[i]))
			split = 1;

		if (next_script != script &&
		    next_script != HB_SCRIPT_COMMON &&
		    script != HB_SCRIPT_COMMON)
			split = 1;

		if (split) {
			if (run_count < max_runs) {
				runs[run_count].start = start;
				runs[run_count].end = i;
				runs[run_count].level = level;
				runs[run_count].face_index = face_index;
				runs[run_count].script = script;
				run_count++;
			}

			start = i;
			level = next_level;
			face_index = next_face;
			script = next_script;
		}

		if (!gu_font_hb_is_neutral_cp(cps[i]))
			preferred_face = next_face;
	}

	if (run_count < max_runs) {
		runs[run_count].start = start;
		runs[run_count].end = count;
		runs[run_count].level = level;
		runs[run_count].face_index = face_index;
		runs[run_count].script = script;
		run_count++;
	}

	return run_count;
}

static void gu_font_hb_reverse_runs(GUFontHBRun *runs, int start, int end)
{
	while (start < end) {
		GUFontHBRun tmp = runs[start];
		runs[start] = runs[end];
		runs[end] = tmp;
		start++;
		end--;
	}
}

static void gu_font_hb_reorder_runs_visual(GUFontHBRun *runs, int run_count)
{
	int i;
	int max_level = 0;
	int min_odd_level = 255;
	int level;

	if (runs == 0 || run_count <= 1)
		return;

	for (i = 0; i < run_count; i++) {
		if (runs[i].level > max_level)
			max_level = runs[i].level;

		if ((runs[i].level & 1) && runs[i].level < min_odd_level)
			min_odd_level = runs[i].level;
	}

	if (min_odd_level == 255)
		return;

	for (level = max_level; level >= min_odd_level; level--) {
		i = 0;

		while (i < run_count) {
			int start;

			while (i < run_count && runs[i].level < level)
				i++;

			start = i;

			while (i < run_count && runs[i].level >= level)
				i++;

			if (i - start > 1)
				gu_font_hb_reverse_runs(runs, start, i - 1);
		}
	}
}

static int gu_font_hb_shape_run_advance(const hb_codepoint_t *cps,
                                        const GUFontHBRun *run)
{
	hb_font_t *hb_font;
	hb_buffer_t *buffer;
	hb_glyph_position_t *positions;
	unsigned int glyph_count = 0;
	int i;
	int advance = 0;

	if (cps == 0 || run == 0 || run->end <= run->start)
		return 0;

	hb_font = gu_font_hb_font_for_face_index(run->face_index);
	if (hb_font == 0)
		return 0;

	buffer = hb_buffer_create();
	if (buffer == 0)
		return 0;

	hb_buffer_add_utf32(buffer,
	                    cps + run->start,
	                    run->end - run->start,
	                    0,
	                    run->end - run->start);

	hb_buffer_set_direction(buffer,
	                        (run->level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
	hb_buffer_set_script(buffer, run->script);
	hb_buffer_set_language(buffer, hb_language_from_string("und", -1));

	hb_shape(hb_font, buffer, 0, 0);

	positions = hb_buffer_get_glyph_positions(buffer, &glyph_count);

	for (i = 0; i < (int)glyph_count; i++)
		advance += positions[i].x_advance;

	hb_buffer_destroy(buffer);

	return advance;
}

static int gu_font_hb_line_width_get(char *s)
{
	hb_codepoint_t cps[GU_FONT_HB_MAX_CHARS];
	GUFontHBRun runs[GU_FONT_HB_MAX_RUNS];
	int count;
	int run_count;
	int i;
	int advance = 0;

	count = gu_font_hb_decode_utf8_line(s, cps, GU_FONT_HB_MAX_CHARS);
	if (count <= 0)
		return 8;

	run_count =
		gu_font_hb_make_runs(cps,
		                     count,
		                     runs,
		                     GU_FONT_HB_MAX_RUNS);

	for (i = 0; i < run_count; i++)
		advance += gu_font_hb_shape_run_advance(cps, &runs[i]);

	return (advance >> 6) + 8;
}

static int gu_font_hb_width_get(char *s, int flag)
{
    typedef struct GUFontHBWidthCache_ {
        int valid;
        int width;
        unsigned int generation;
        unsigned int lru;
        char text[1024];
    } GUFontHBWidthCache;

    static GUFontHBWidthCache cache[4];
    static unsigned int lru_tick = 1U;
    char *p;
    int width = 0;
    int line_width;
    char line[GU_FONT_HB_MAX_CHARS * 4];
    int line_pos = 0;
    GUFontHBWidthCache *victim = 0;
    unsigned int oldest = 0xffffffffU;
    int i;

    if (s == 0)
        return 0;

    if (flag == 0 && strlen(s) < sizeof(cache[0].text)) {
        for (i = 0; i < 4; ++i) {
            if (cache[i].valid &&
                cache[i].generation == gu_font_layout_generation &&
                strcmp(cache[i].text, s) == 0) {
                cache[i].lru = ++lru_tick;
                return cache[i].width;
            }
        }
    }

    p = s;

    while (1) {
        if (*p == '\0' || *p == '\n') {
            line[line_pos] = '\0';

            line_width = gu_font_hb_line_width_get(line);
            if (line_width > width)
                width = line_width;

            line_pos = 0;

            if (*p == '\0')
                break;

            p++;
            continue;
        }

        if (line_pos + 1 < (int)sizeof(line))
            line[line_pos++] = *p;

        p++;
    }

    if (flag == 0 && strlen(s) < sizeof(cache[0].text)) {
        for (i = 0; i < 4; ++i) {
            if (!cache[i].valid) {
                victim = &cache[i];
                break;
            }
            if (cache[i].lru < oldest) {
                oldest = cache[i].lru;
                victim = &cache[i];
            }
        }

        if (victim != 0) {
            memset(victim, 0, sizeof(*victim));
            victim->valid = 1;
            victim->width = width;
            victim->generation = gu_font_layout_generation;
            victim->lru = ++lru_tick;
            strncpy(victim->text, s, sizeof(victim->text) - 1);
            victim->text[sizeof(victim->text) - 1] = '\0';
        }
    }

    return width;
}

static void gu_font_hb_render_run(const hb_codepoint_t *cps,
                                  const GUFontHBRun *run,
                                  int *pen_x,
                                  int *pen_y,
                                  int width,
                                  int height)
{
	hb_font_t *hb_font;
	hb_buffer_t *buffer;
	hb_glyph_info_t *infos;
	hb_glyph_position_t *positions;
	unsigned int glyph_count = 0;
	int i;

	if (cps == 0 || run == 0 || pen_x == 0 || pen_y == 0)
		return;

	if (run->end <= run->start)
		return;

	hb_font = gu_font_hb_font_for_face_index(run->face_index);
	if (hb_font == 0)
		return;

	buffer = hb_buffer_create();
	if (buffer == 0)
		return;

	hb_buffer_add_utf32(buffer,
	                    cps + run->start,
	                    run->end - run->start,
	                    0,
	                    run->end - run->start);

	hb_buffer_set_direction(buffer,
	                        (run->level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
	hb_buffer_set_script(buffer, run->script);
	hb_buffer_set_language(buffer, hb_language_from_string("und", -1));

	hb_shape(hb_font, buffer, 0, 0);

	infos = hb_buffer_get_glyph_infos(buffer, &glyph_count);
	positions = hb_buffer_get_glyph_positions(buffer, &glyph_count);

	for (i = 0; i < (int)glyph_count; i++) {
		FT_UInt glyph_index = infos[i].codepoint;
		int x_offset = positions[i].x_offset >> 6;
		int y_offset = positions[i].y_offset >> 6;

		gu_font_draw_glyph_index(run->face_index,
		                         0,
		                         glyph_index,
		                         *pen_x,
		                         *pen_y,
		                         x_offset,
		                         y_offset,
		                         width,
		                         height);

		*pen_x += positions[i].x_advance >> 6;
		*pen_y -= positions[i].y_advance >> 6;
	}

	hb_buffer_destroy(buffer);
}

static void gu_font_hb_render_line(int x,
                                   int y,
                                   char *s,
                                   int width,
                                   int height)
{
	hb_codepoint_t cps[GU_FONT_HB_MAX_CHARS];
	GUFontHBRun runs[GU_FONT_HB_MAX_RUNS];
	int count;
	int run_count;
	int i;
	int pen_x = x;
	int pen_y = y;

	count = gu_font_hb_decode_utf8_line(s, cps, GU_FONT_HB_MAX_CHARS);
	if (count <= 0)
		return;

	run_count =
		gu_font_hb_make_runs(cps,
		                     count,
		                     runs,
		                     GU_FONT_HB_MAX_RUNS);

	gu_font_hb_reorder_runs_visual(runs, run_count);

	for (i = 0; i < run_count; i++) {
		gu_font_hb_render_run(cps,
		                      &runs[i],
		                      &pen_x,
		                      &pen_y,
		                      width,
		                      height);
	}
}

#endif

static void gu_font_unload_current(void)
{
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	if (gu_font_primary_hb_font != 0) {
		hb_font_destroy(gu_font_primary_hb_font);
		gu_font_primary_hb_font = 0;
	}
#endif

	if (stroker) {
		FT_Stroker_Done(stroker);
		stroker = 0;
	}

	if (face) {
		FT_Done_Face(face);
		face = 0;
	}

	if (!font_loaded_from_memory && font_stream.descriptor.value >= 0) {
		sceIoClose(font_stream.descriptor.value);
		font_stream.descriptor.value = -1;
	}

	if (font_memory_data) {
		free_64(font_memory_data);
		font_memory_data = 0;
		font_memory_size = 0;
	}

	font_loaded_from_memory = 0;
	memset(font_filename, 0, sizeof(font_filename));

	gu_font_notify_font_set_changed();
}

char* gu_font_init()
	{
	if (gu_font_initialized==1) return(0);
	
	FT_Error error;

	error = FT_Init_FreeType( &library );              /* initialize library */
  	if(error)
		return("FT_Init_FreeType failed");
	
	memset(cache_string, 0, 2048);

	font_stream.descriptor.value = -1;
	font_memory_data = 0;
	font_memory_size = 0;
	font_loaded_from_memory = 0;
	
	sbit_cache_init();
	gu_font_atlas_invalidate_lines();
	gu_font_atlas_reset(1);
	memset(&gu_font_atlas_stats, 0, sizeof(gu_font_atlas_stats));

	{
		int i;

		for (i = 0; i < GU_FONT_MAX_FALLBACK_FACES; i++) {
			memset(&gu_font_fallbacks[i], 0, sizeof(gu_font_fallbacks[i]));
			gu_font_fallbacks[i].stream.descriptor.value = -1;
		}
	}
	
	font_stream.read =  gufont_font_stream_io;
	font_stream.close = gufont_font_stream_close;
	
	open_args.flags = FT_OPEN_STREAM;
	open_args.stream = &font_stream;
	
	gu_font_initialized = 1;
	return(0);
	}

void gu_font_close()
{
	if (gu_font_initialized == 0)
		return;

	gu_font_clear_fallbacks();

	if (stroker) {
		FT_Stroker_Done(stroker);
		stroker = 0;
	}

	if (face) {
		FT_Done_Face(face);
		face = 0;
	}

	FT_Done_FreeType(library);
	library = 0;

	sbit_cache_done();

	gu_font_atlas_emit_summary();
	if (sub_8888 != 0) {
		free_64(sub_8888);
		sub_8888 = 0;
	}
	if (border_sub_8888 != 0) {
		free_64(border_sub_8888);
		border_sub_8888 = 0;
	}
	gu_font_atlas_invalidate_lines();
	gu_font_atlas_reset(1);

	gu_font_initialized = 0;
}
	
char* gu_font_load(char* name)
{
	FT_Error error;

	if (gu_font_initialized == 0)
		return "initialized : fail";

	if (name == 0 || name[0] == '\0')
		return "gu_font_load: empty font name";

	if (!font_loaded_from_memory && strcmp(font_filename, name) == 0)
		return 0;

	gu_font_unload_current();

	font_stream.descriptor.value = sceIoOpen(name, PSP_O_RDONLY, 0777);
	if (font_stream.descriptor.value < 0) {
		face = NULL;
		return "gu_font_load: sceIoOpen failed";
	}

	font_stream.base = 0;
	font_stream.size = sceIoLseek32(font_stream.descriptor.value, 0, PSP_SEEK_END);
	font_stream.pos = sceIoLseek32(font_stream.descriptor.value, 0, PSP_SEEK_SET);

	open_args.flags = FT_OPEN_STREAM;
	open_args.stream = &font_stream;

	error = FT_Open_Face(library, &open_args, 0, &face);
	if (error) {
		gu_font_unload_current();
		return "gu_font_load: FT_Open_Face failed";
	}

	error = FT_Stroker_New(library, &stroker);
	if (error) {
		gu_font_unload_current();
		return "gu_font_load: FT_Stroker_New failed";
	}

	font_loaded_from_memory = 0;
	strncpy(font_filename, name, sizeof(font_filename) - 1);

	gu_font_border_enable(1);
	gu_font_border_set(1.2);
	gu_font_pixelsize_set(16);
	gu_font_color_set(0xffffffff);
	gu_font_border_color_set(0xff000000);

	return 0;
}

char* gu_font_load_memory(const char* label,
                          const unsigned char* data,
                          unsigned int size)
{
	FT_Error error;

	if (gu_font_initialized == 0)
		return "initialized : fail";

	if (data == 0 || size == 0)
		return "gu_font_load_memory: empty font";

	if (size > MATROSKA_MAX_ATTACHMENT_SIZE)
		return "gu_font_load_memory: font too large";

	gu_font_unload_current();

	font_memory_data = (unsigned char *)malloc_64(size);
	if (font_memory_data == 0)
		return "gu_font_load_memory: malloc failed";

	memcpy(font_memory_data, data, size);
	font_memory_size = size;

	error = FT_New_Memory_Face(library,
	                           font_memory_data,
	                           (FT_Long)font_memory_size,
	                           0,
	                           &face);
	if (error) {
		gu_font_unload_current();
		return "gu_font_load_memory: FT_New_Memory_Face failed";
	}

	error = FT_Stroker_New(library, &stroker);
	if (error) {
		gu_font_unload_current();
		return "gu_font_load_memory: FT_Stroker_New failed";
	}

	font_loaded_from_memory = 1;

	if (label && label[0])
		strncpy(font_filename, label, sizeof(font_filename) - 1);
	else
		strncpy(font_filename, "mkv embedded font", sizeof(font_filename) - 1);

	gu_font_border_enable(1);
	gu_font_border_set(1.2);
	gu_font_pixelsize_set(16);
	gu_font_color_set(0xffffffff);
	gu_font_border_color_set(0xff000000);

	return 0;
}

void gu_font_on_suspend()
{
	if (font_loaded_from_memory)
		return;

	if (font_stream.descriptor.value >= 0) {
		sceIoClose(font_stream.descriptor.value);
		font_stream.descriptor.value = -1;
	}
}

void gu_font_on_resume()
{
	/* The GE context/texture cache may have been reconstructed while the atlas
	 * remained in main RAM. Defer the flush until the next active GU list. */
	gu_font_atlas_force_texflush = 1;

	if (font_loaded_from_memory)
		return;

	if (font_filename[0] == '\0')
		return;

	font_stream.descriptor.value =
		ppa_io_open_read_retry(font_filename, PSP_O_RDONLY, 0777);

	if (font_stream.descriptor.value >= 0)
		sceIoLseek32(font_stream.descriptor.value, font_stream.pos, PSP_SEEK_SET);
}

FT_Face gu_font_getface() 
	{
	return(face);
	}

void gu_font_pixelsize_set(int size)
{
	int i;

	gufont_pixel_size = size;
	gufont_char_width = size;
	gufont_char_height = size;

	if (face != 0)
		FT_Set_Pixel_Sizes(face, gufont_char_width, gufont_char_height);

	for (i = 0; i < gu_font_fallback_count; i++) {
		if (gu_font_fallbacks[i].face != 0)
			FT_Set_Pixel_Sizes(gu_font_fallbacks[i].face,
			                   gufont_char_width,
			                   gufont_char_height);
	}

	gu_font_notify_font_set_changed();
}

void gu_font_scale_set(float asc_scale, float multcode_scale)
	{
	if (gufont_asc_scale == asc_scale &&
	    gufont_multcode_scale == multcode_scale)
		return;
	gufont_asc_scale = asc_scale;
	gufont_multcode_scale = multcode_scale;
	gu_font_notify_font_set_changed();
	}

void gu_font_border_enable( int enable )
	{
	//if (face==0) return;
	if (gufont_border_enable==enable) return;
	gufont_border_enable = enable;
	}
	
void gu_font_border_set(float border)
	{
	if (gufont_border_size == border)
		return;
	if (stroker != 0) {
		FT_Stroker_Set( stroker,
			(int)(64 * border),
			FT_STROKER_LINECAP_ROUND,
			FT_STROKER_LINEJOIN_ROUND,
			0 );
	}
	gufont_border_size = border;
	gu_font_notify_font_set_changed();
	}

void gu_font_border_color_set( unsigned int color )
	{
	//if (face==0) return;
	gufont_border_color = 0xff000000 | color;
	if (gufont_border_enable==0) return;

	}

void gu_font_color_set( unsigned int color )
	{
	//if (face==0) return;
	gufont_color = 0xff000000 | color;
	
	}
unsigned int gu_font_color_get(void)
{
	return gufont_color;
}

unsigned int gu_font_border_color_get(void)
{
	return gufont_border_color;
}

int gu_font_border_enable_get(void)
{
	return (int)gufont_border_enable;
}

void gu_font_embolden_enable( int enable )
	{
	if (gufont_embolden_enable == enable) return;
	gufont_embolden_enable = enable;
	gu_font_notify_font_set_changed();
	}

void gu_font_align_set(unsigned int align) 
	{
	gufont_align = align;
	}

unsigned int gu_font_align_get()
	{
	return gufont_align;
	}

void gu_font_distance_set(unsigned int distance) 
	{
	gufont_distance = distance;
	}

unsigned int gu_font_distance_get()
	{
	return gufont_distance;
	}

int gu_font_line_width_get( char* s )
	{
	if (s==0) return 0;
	char* c = s;
	int x = 0;
	int style = 0;
	int lastcharstyle = 0;

	while (*c!='\0' && *c!='\n')
		{
		if (*c==' ')
			{
			x += CHARWIDTH('t');
			}
		else if ((unsigned char)*c>32)
			{
			if (gu_font_parse_bbcode( &c, &style )==0)
				{
				x += CHARWIDTH2((unsigned char)*c,style);
				lastcharstyle = style;
				}
			}
		c++;
		}
	if (lastcharstyle!=0) x+=4;
	x+=8;
	return (x);
	}

int gu_font_width_get( char* s, int flag )
	{
	if (s==0 ) return 0;
	char* c = s;
	int x = 0;
	int width = 0;
	if (flag == 0) flag--;
	int style=0;
	int lastcharstyle=0;
	
	while (*c!='\0' && flag!=0)
		{
		if (*c==' ')
			{
			flag--;
			x += CHARWIDTH('t');
			}
		else if (*c=='\n')
			{
			if (lastcharstyle!=0) x+=4;
			x+=8;
			if (x>width) width=x;
			x = 0;
			}
		else if ((unsigned char)*c>32)
			{
			if (gu_font_parse_bbcode( &c, &style )==0)
				{
				x += CHARWIDTH2((unsigned char)*c,style);
				lastcharstyle = style;
				}
			}
		c++;
		}
	if (lastcharstyle!=0) x+=4;
	x+=8;
	return (x>width?x:width);
	}

int gu_font_utf8_line_width_get(char* s)
{
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	return gu_font_hb_line_width_get(s);
#else
	char* c;
	int x = 0;
	int style = 0;
	int lastcharstyle = 0;

	if (s == 0)
		return 0;

	c = s;

	while (*c != '\0' && *c != '\n') {
		if (*c == ' ') {
			x += CHARWIDTH('t');
			c++;
		}
		else if ((unsigned char)*c > 32) {
			if (gu_font_parse_bbcode(&c, &style) != 0) {
				c++;
			}
			else {
				unsigned long ucs = get_next_utf8(&c);
				if (ucs == 0)
					break;

				x += CHARWIDTH2(ucs, style);
				lastcharstyle = style;
			}
		}
		else {
			c++;
		}
	}

	if (lastcharstyle != 0)
		x += 4;

	x += 8;
	return x;
#endif
}

int gu_font_utf8_width_get(char* s, int flag)
{
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	return gu_font_hb_width_get(s, flag);
#else
	char* c;
	int x = 0;
	int width = 0;
	int style = 0;
	int lastcharstyle = 0;

	if (s == 0)
		return 0;

	c = s;

	if (flag == 0)
		flag--;

	while (*c != '\0' && flag != 0) {
		unsigned char ch = (unsigned char)*c;

		if (ch == ' ') {
			flag--;
			x += CHARWIDTH('t');
			c++;
		}
		else if (ch == '\n') {
			if (lastcharstyle != 0)
				x += 4;

			x += 8;

			if (x > width)
				width = x;

			x = 0;
			c++;
		}
		else if (ch > 32) {
			if (gu_font_parse_bbcode(&c, &style) != 0) {
				c++;
			}
			else {
				unsigned long ucs = get_next_utf8(&c);
				if (ucs == 0)
					break;

				x += CHARWIDTH2(ucs, style);
				lastcharstyle = style;
			}
		}
		else {
			c++;
		}
	}

	if (lastcharstyle != 0)
		x += 4;

	x += 8;

	return (x > width ? x : width);
#endif
}

int gu_font_utf16le_line_width_get( short* s )
	{
	if (s==0 ) return 0;
	short* c = s;
	int x = 0;
	int style = 0;
	int lastcharstyle = 0;

	while (*c!=0 && *c!='\n')
		{
		if (*c==' ')
			{
			x += CHARWIDTH('t');
			}
		else if ((unsigned short)*c>32)
			{
			if (gu_font_utf16le_parse_bbcode( &c, &style )==0)
				{
				unsigned short ucs = get_next_utf16le(&c);
				x += CHARWIDTH2(ucs,style);
				lastcharstyle = style;
				}
			}
		c++;
		}
	if (lastcharstyle!=0) x+=4;
	x+=8;
	return (x);
	}
	
int gu_font_utf16le_width_get( short* s, int flag )
	{
	if (s==0 ) return 0;
	short* c = s;
	int x = 0;
	int width = 0;
	if (flag == 0) flag--;
	int style=0;
	int lastcharstyle=0;
	unsigned short ch;
	
	while ((ch=(unsigned short)*c)!=0 && flag!=0)
		{
		if (ch==' ')
			{
			flag--;
			x += CHARWIDTH('t');
			}
		else if (ch=='\n')
			{
			if (lastcharstyle!=0) x+=4;
			x+=8;
			if (x>width) width=x;
			x = 0;
			}
		else if (ch>32)
			{
			if (gu_font_utf16le_parse_bbcode( &c, &style )==0)
				{
				unsigned short ucs = ch;
				x += CHARWIDTH2(ucs,style);
				lastcharstyle = style;
				}
			}
		c++;
		}
	if (lastcharstyle!=0) x+=4;
	x+=8;
	return (x>width?x:width);
	}

int gu_font_utf16be_line_width_get( short* s )
	{
	if (s==0 ) return 0;
	short* c = s;
	int x = 0;
	int style = 0;
	int lastcharstyle = 0;
	
	while (*c!=0 && SWAPBYTES(*c)!='\n')
		{
		if (SWAPBYTES(*c)==' ')
			{
			x += CHARWIDTH('t');
			}
		else if ((unsigned short)SWAPBYTES(*c)>32)
			{
			if (gu_font_utf16be_parse_bbcode( &c, &style )==0)
				{
				unsigned short ucs = get_next_utf16be(&c);
				x += CHARWIDTH2(ucs,style);
				lastcharstyle = style;
				}
			}
		c++;
		}
	if (lastcharstyle!=0) x+=4;
	x+=8;
	return (x);
	}

int gu_font_utf16be_width_get( short* s, int flag )
	{
	if (s==0 ) return 0;
	short* c = s;
	int x = 0;
	int width = 0;
	if (flag == 0) flag--;
	int style=0;
	int lastcharstyle=0;
	
	unsigned short ch;
	while ((ch=(unsigned short)SWAPBYTES((*c)))!=0 && flag!=0)
		{
		if (ch==' ')
			{
			flag--;
			x += CHARWIDTH('t');
			}
		else if (ch=='\n')
			{
			if (lastcharstyle!=0) x+=4;
			x+=8;
			if (x>width) width=x;
			x = 0;
			}
		else if (ch>32)
			{
			if (gu_font_utf16be_parse_bbcode( &c, &style )==0)
				{
				unsigned short ucs = ch;
				x += CHARWIDTH2(ucs,style);
				lastcharstyle = style;
				}
			}
		c++;
		}
	if (lastcharstyle!=0) x+=4;
	x+=8;
	return (x>width?x:width);
	}

int gu_font_height_get( char* s )
	{
	if (s == 0 ) return 0;
	char* c = s;
	int height = gufont_char_height;//FONTHEIGHT;
	
	while (*c!='\0')
		{
		if (*c=='\n')
			{
			height += gufont_char_height;//FONTHEIGHT;
			}
		c++;
		}
	return (height);
	}

int gu_font_utf16be_height_get( short* s )
	{
	if (s == 0 ) return 0;
	short* c = s;
	int height = gufont_char_height;//FONTHEIGHT;
	
	while (*c!=0)
		{
		if (*c=='\n')
			{
			height += gufont_char_height;//FONTHEIGHT;
			}
		c++;
		}
	return (height);
	}

int gu_font_utf16le_height_get( short* s )
	{
	if (s == 0 ) return 0;
	short* c = s;
	int height = gufont_char_height;//FONTHEIGHT;
	
	while (*c!=0)
		{
		if ((*c>>8)=='\n')
			{
			height += gufont_char_height;//FONTHEIGHT;
			}
		c++;
		}
	return (height);
	}

inline int gu_font_height()
	{
	return gufont_char_height;//FONTHEIGHT;
	}

void render_string(int x, int y, char* s, int flags)
{
	int (*gu_font_line_width)(char*);
	int (*gu_font_width)(char* s, int flag);
	int max_width;
	int x_x;
	int y_y;
	int width = SUB_SCREEN_WIDTH;
	int height = SUB_SCREEN_HEIGHT;
	int style = 0;
	FT_Bool use_kerning = 0;
	FT_UInt previous = 0;
	int previous_face_index = -1;

	if (s == 0)
		return;

	if (IsSet(flags, FLAG_UTF8)) {
		gu_font_line_width = gu_font_utf8_line_width_get;
		gu_font_width = gu_font_utf8_width_get;
	}
	else {
		gu_font_line_width = gu_font_line_width_get;
		gu_font_width = gu_font_width_get;
	}

	max_width = gu_font_width(s, 0);

	if (IsSet(flags, FLAG_ALIGN_CENTER)) {
		x_x = x + ((max_width - gu_font_line_width(s)) / 2);
	}
	else if (IsSet(flags, FLAG_ALIGN_RIGHT)) {
		x_x = x + (max_width - gu_font_line_width(s));
	}
	else {
		x_x = x;
	}

	y_y = y;

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	if (IsSet(flags, FLAG_UTF8)) {
		char *line_start = s;
		char line[GU_FONT_HB_MAX_CHARS * 4];
		int line_pos = 0;

		while (1) {
			if (*line_start == '\0' || *line_start == '\n') {
				line[line_pos] = '\0';

				if (IsSet(flags, FLAG_ALIGN_CENTER)) {
					x_x = x + ((max_width - gu_font_hb_line_width_get(line)) / 2);
				}
				else if (IsSet(flags, FLAG_ALIGN_RIGHT)) {
					x_x = x + (max_width - gu_font_hb_line_width_get(line));
				}
				else {
					x_x = x;
				}

				gu_font_hb_render_line(x_x,
				                       y_y,
				                       line,
				                       width,
				                       height);

				line_pos = 0;

				if (*line_start == '\0')
					break;

				y_y += gufont_char_height + 2;
				line_start++;
				continue;
			}

			if (line_pos + 1 < (int)sizeof(line))
				line[line_pos++] = *line_start;

			line_start++;
		}

		return;
	}
#endif

	use_kerning = FT_HAS_KERNING(gu_font_getface());
	previous = 0;
	previous_face_index = -1;

	while (*s != '\0') {
		unsigned long ucs;
		int face_index = 0;
		FT_UInt glyph_index = 0;
		FT_Face draw_face;

		if (*s == '\n') {
			s++;

			if (IsSet(flags, FLAG_ALIGN_CENTER)) {
				x_x = x + ((max_width - gu_font_line_width(s)) / 2);
			}
			else if (IsSet(flags, FLAG_ALIGN_RIGHT)) {
				x_x = x + (max_width - gu_font_line_width(s));
			}
			else {
				x_x = x;
			}

			y_y += gufont_char_height + 2;
			previous = 0;
			previous_face_index = -1;
			continue;
		}

		if ((unsigned char)*s < 32) {
			s++;
			continue;
		}

		if (gu_font_parse_bbcode(&s, &style) != 0) {
			s++;
			continue;
		}

		if (IsSet(flags, FLAG_UTF8)) {
			ucs = get_next_utf8(&s);
			if (ucs == 0)
				break;
		}
		else {
			ucs = (unsigned char)*s;
			s++;
		}

		draw_face =
			gu_font_find_face_for_codepoint(ucs,
			                                &face_index,
			                                &glyph_index);

		if (draw_face == 0 || glyph_index == 0)
			continue;

		if (use_kerning &&
		    previous != 0 &&
		    previous_face_index == face_index &&
		    FT_HAS_KERNING(draw_face)) {
			FT_Vector delta;

			FT_Get_Kerning(draw_face,
			               previous,
			               glyph_index,
			               FT_KERNING_DEFAULT,
			               &delta);

			x_x += delta.x >> 6;
		}

		gu_font_draw_glyph_index(face_index,
		                         ucs,
		                         glyph_index,
		                         x_x,
		                         y_y,
		                         0,
		                         0,
		                         width,
		                         height);

		{
			SBit_HashItem *cache_item =
				sbit_cache_find(face_index,
				                ucs,
				                glyph_index,
				                gufont_pixel_size,
				                gufont_embolden_enable);

			if (cache_item != 0)
				x_x += cache_item->xadvance >> 6;
			else
				x_x += draw_face->glyph->advance.x >> 6;
		}

		previous = glyph_index;
		previous_face_index = face_index;
	}
}

static void gu_font_atlas_emit_summary(void)
{
}

static void gu_font_atlas_invalidate_lines(void)
{
    memset(gu_font_line_cache, 0, sizeof(gu_font_line_cache));
    gu_font_capture_line = 0;
    gu_font_line_lru_tick = 1U;
}

static void gu_font_atlas_reset(int clear_pixels)
{
    gu_font_fill_packer.x = GU_FONT_ATLAS_PADDING;
    gu_font_fill_packer.y = GU_FONT_ATLAS_PADDING;
    gu_font_fill_packer.row_height = 0;
    gu_font_fill_packer.y_base = 0;

    gu_font_border_packer.x = GU_FONT_ATLAS_PADDING;
    gu_font_border_packer.y =
        GU_FONT_ATLAS_PLANE_HEIGHT + GU_FONT_ATLAS_PADDING;
    gu_font_border_packer.row_height = 0;
    gu_font_border_packer.y_base = GU_FONT_ATLAS_PLANE_HEIGHT;

    ++gu_font_atlas_generation;
    if (gu_font_atlas_generation == 0U)
        gu_font_atlas_generation = 1U;

    if (clear_pixels)
        memset(gu_font_atlas, 0, sizeof(gu_font_atlas));

    gu_font_atlas_force_texflush = 1;
}

static int gu_font_legacy_buffers_ensure(void)
{
    if (sub_8888 == 0) {
        sub_8888 = (unsigned char *)malloc_64(DRAW_BUFFER_SIZE);
        if (sub_8888 == 0)
            return 0;
        memset(sub_8888, 0, DRAW_BUFFER_SIZE);
    }

    if (border_sub_8888 == 0) {
        border_sub_8888 = (unsigned char *)malloc_64(DRAW_BUFFER_SIZE);
        if (border_sub_8888 == 0)
            return 0;
        memset(border_sub_8888, 0, DRAW_BUFFER_SIZE);
    }

    return 1;
}

static int gu_font_atlas_pack_rect(GUFontAtlasPacker *packer,
                                   int width,
                                   int height,
                                   unsigned short *out_x,
                                   unsigned short *out_y)
{
    int packed_width;
    int packed_height;
    int plane_end;

    if (out_x == 0 || out_y == 0 || packer == 0)
        return 0;

    if (width <= 0 || height <= 0) {
        *out_x = GU_FONT_ATLAS_INVALID_COORD;
        *out_y = GU_FONT_ATLAS_INVALID_COORD;
        return 1;
    }

    packed_width = width + GU_FONT_ATLAS_PADDING * 2;
    packed_height = height + GU_FONT_ATLAS_PADDING * 2;
    plane_end = packer->y_base + GU_FONT_ATLAS_PLANE_HEIGHT;

    if (packed_width > GU_FONT_ATLAS_WIDTH ||
        packed_height > GU_FONT_ATLAS_PLANE_HEIGHT)
        return 0;

    if ((int)packer->x + packed_width > GU_FONT_ATLAS_WIDTH) {
        packer->x = GU_FONT_ATLAS_PADDING;
        packer->y = (unsigned short)(packer->y + packer->row_height);
        packer->row_height = 0;
    }

    if ((int)packer->y + packed_height > plane_end)
        return 0;

    *out_x = (unsigned short)(packer->x + GU_FONT_ATLAS_PADDING);
    *out_y = (unsigned short)(packer->y + GU_FONT_ATLAS_PADDING);
    packer->x = (unsigned short)(packer->x + packed_width);
    if (packed_height > packer->row_height)
        packer->row_height = (unsigned short)packed_height;

    return 1;
}

static const unsigned char *gu_font_bitmap_row(const Cache_Bitmap *bitmap,
                                                int y,
                                                int *out_pitch)
{
    int pitch;

    if (bitmap == 0 || bitmap->buffer == 0 || y < 0 || y >= bitmap->height)
        return 0;

    pitch = bitmap->pitch;
    if (pitch < 0) {
        pitch = -pitch;
        if (out_pitch != 0)
            *out_pitch = pitch;
        return bitmap->buffer + (bitmap->height - 1 - y) * pitch;
    }

    if (out_pitch != 0)
        *out_pitch = pitch;
    return bitmap->buffer + y * pitch;
}

static unsigned char gu_font_bitmap_coverage(const Cache_Bitmap *bitmap,
                                              int x,
                                              int y)
{
    const unsigned char *row;
    int pitch = 0;
    unsigned int value;

    if (bitmap == 0 || x < 0 || x >= bitmap->width ||
        y < 0 || y >= bitmap->height)
        return 0;

    row = gu_font_bitmap_row(bitmap, y, &pitch);
    if (row == 0 || pitch <= 0)
        return 0;

    if (bitmap->format == FT_PIXEL_MODE_MONO)
        return (row[x >> 3] & (0x80U >> (x & 7))) ? 255U : 0U;

    if (bitmap->format != FT_PIXEL_MODE_GRAY)
        return 0;

    value = row[x];
    if (bitmap->max_grays > 0 && bitmap->max_grays != 255)
        value = (value * 255U + (unsigned int)bitmap->max_grays / 2U) /
                (unsigned int)bitmap->max_grays;

    if (value > 255U)
        value = 255U;
    return (unsigned char)value;
}

static void gu_font_atlas_copy_bitmap(const Cache_Bitmap *bitmap,
                                      unsigned short atlas_x,
                                      unsigned short atlas_y,
                                      unsigned char *stage,
                                      unsigned int stage_bytes,
                                      int *dirty_min_y,
                                      int *dirty_max_y)
{
    unsigned int needed;
    int x;
    int y;

    if (bitmap == 0 || bitmap->buffer == 0 ||
        atlas_x == GU_FONT_ATLAS_INVALID_COORD ||
        atlas_y == GU_FONT_ATLAS_INVALID_COORD ||
        bitmap->width <= 0 || bitmap->height <= 0)
        return;

    needed = (unsigned int)bitmap->width * (unsigned int)bitmap->height;

    if (stage != 0 && needed <= stage_bytes) {
        unsigned char *dst = stage;

        for (y = 0; y < bitmap->height; ++y) {
            for (x = 0; x < bitmap->width; ++x)
                dst[y * bitmap->width + x] =
                    gu_font_bitmap_coverage(bitmap, x, y);
        }

        for (y = 0; y < bitmap->height; ++y) {
            memcpy(gu_font_atlas +
                       ((unsigned int)atlas_y + (unsigned int)y) *
                           GU_FONT_ATLAS_WIDTH + atlas_x,
                   dst + y * bitmap->width,
                   (unsigned int)bitmap->width);
        }

        ++gu_font_atlas_stats.scratchpad_stages;
    }
    else {
        for (y = 0; y < bitmap->height; ++y) {
            unsigned char *dst =
                gu_font_atlas +
                ((unsigned int)atlas_y + (unsigned int)y) *
                    GU_FONT_ATLAS_WIDTH + atlas_x;

            for (x = 0; x < bitmap->width; ++x)
                dst[x] = gu_font_bitmap_coverage(bitmap, x, y);
        }
    }

    if (dirty_min_y != 0 && atlas_y < *dirty_min_y)
        *dirty_min_y = atlas_y;
    if (dirty_max_y != 0 &&
        (int)atlas_y + bitmap->height - 1 > *dirty_max_y)
        *dirty_max_y = (int)atlas_y + bitmap->height - 1;

    ++gu_font_atlas_stats.glyph_uploads;
    gu_font_atlas_stats.upload_bytes += needed;
}

static int gu_font_atlas_pack_item(SBit_HashItem *item,
                                   unsigned char *stage,
                                   unsigned int stage_bytes,
                                   int *dirty_min_y,
                                   int *dirty_max_y)
{
    GUFontAtlasPacker fill_before;
    GUFontAtlasPacker border_before;
    unsigned short fill_x;
    unsigned short fill_y;
    unsigned short border_x;
    unsigned short border_y;

    if (item == 0 || item->glyph_index == 0)
        return 0;

    if (item->atlas_generation == gu_font_atlas_generation)
        return 1;

    fill_before = gu_font_fill_packer;
    border_before = gu_font_border_packer;

    if (!gu_font_atlas_pack_rect(&gu_font_fill_packer,
                                 item->bitmap.width,
                                 item->bitmap.height,
                                 &fill_x,
                                 &fill_y) ||
        !gu_font_atlas_pack_rect(&gu_font_border_packer,
                                 item->border_bitmap.width,
                                 item->border_bitmap.height,
                                 &border_x,
                                 &border_y)) {
        gu_font_fill_packer = fill_before;
        gu_font_border_packer = border_before;
        return 0;
    }

    gu_font_atlas_copy_bitmap(&item->bitmap,
                              fill_x,
                              fill_y,
                              stage,
                              stage_bytes,
                              dirty_min_y,
                              dirty_max_y);
    gu_font_atlas_copy_bitmap(&item->border_bitmap,
                              border_x,
                              border_y,
                              stage,
                              stage_bytes,
                              dirty_min_y,
                              dirty_max_y);

    item->atlas_fill_x = fill_x;
    item->atlas_fill_y = fill_y;
    item->atlas_border_x = border_x;
    item->atlas_border_y = border_y;
    item->atlas_generation = gu_font_atlas_generation;
    return 1;
}

static int gu_font_atlas_validate_placement(const GUFontAtlasPlacement *placement)
{
    const SBit_HashItem *item;

    if (placement == 0 || placement->item == 0)
        return 0;

    item = placement->item;
    return item->glyph_index == (int)placement->glyph_index &&
           item->ucs_code == placement->cache_codepoint &&
           item->face_index == placement->face_index &&
           item->size == (int)gufont_pixel_size &&
           item->embolden == (int)gufont_embolden_enable;
}

static int gu_font_atlas_prepare_line(GUFontAtlasLine *line)
{
    struct ppa_scratchpad_lease lease;
    unsigned char *stage = 0;
    unsigned int stage_bytes = 0;
    int dirty_min_y = GU_FONT_ATLAS_HEIGHT;
    int dirty_max_y = -1;
    int attempt;
    int i;
    int needs_upload = 0;

    if (line == 0 || !line->valid || line->overflow)
        return 0;

    /* The steady-state subtitle frame must not touch scratchpad ownership or
     * cache maintenance. Only a line introducing a non-resident glyph enters
     * the upload path. */
    for (i = 0; i < line->glyph_count; ++i) {
        if (!gu_font_atlas_validate_placement(&line->glyphs[i]))
            return 0;
        if (line->glyphs[i].item->atlas_generation !=
            gu_font_atlas_generation)
            needs_upload = 1;
    }

    if (!needs_upload)
        return 1;

    memset(&lease, 0, sizeof(lease));

#if PPA_FONT_ATLAS_USE_SCRATCHPAD && PPA_ENABLE_SCRATCHPAD_ACCEL
    if (ppa_scratchpad_acquire(PPA_SCRATCHPAD_PARTITION_VIDEO, &lease)) {
        stage = (unsigned char *)lease.base;
        stage_bytes = lease.bytes;
    }
#endif

    for (attempt = 0; attempt < 2; ++attempt) {
        int packed = 1;

        dirty_min_y = GU_FONT_ATLAS_HEIGHT;
        dirty_max_y = -1;

        for (i = 0; i < line->glyph_count; ++i) {
            GUFontAtlasPlacement *placement = &line->glyphs[i];

            if (!gu_font_atlas_validate_placement(placement)) {
                packed = 0;
                break;
            }

            if (!gu_font_atlas_pack_item(placement->item,
                                         stage,
                                         stage_bytes,
                                         &dirty_min_y,
                                         &dirty_max_y)) {
                packed = 0;
                break;
            }
        }

        if (packed) {
            if (dirty_max_y >= dirty_min_y) {
                unsigned int first =
                    (unsigned int)dirty_min_y * GU_FONT_ATLAS_WIDTH;
                unsigned int bytes =
                    (unsigned int)(dirty_max_y - dirty_min_y + 1) *
                    GU_FONT_ATLAS_WIDTH;

                ppa_cache_cpu_wrote_ge_will_read(gu_font_atlas + first,
                                                  bytes);
                sceGuTexFlush();
            }

            if (lease.base != 0)
                ppa_scratchpad_release(&lease);
            return 1;
        }

        if (attempt == 0) {
            ++gu_font_atlas_stats.atlas_resets;
            /* Restart packing without clearing 128 KiB. Old texels are never
             * referenced after the generation bump, and every sampled glyph
             * rectangle is overwritten before submission. */
            gu_font_atlas_reset(0);
        }
    }

    if (lease.base != 0)
        ppa_scratchpad_release(&lease);
    return 0;
}

static GUFontAtlasLine *gu_font_atlas_find_line(const char *text,
                                                int x,
                                                int flags)
{
    GUFontAtlasLine *victim = 0;
    unsigned int oldest = 0xffffffffU;
    int i;

    if (text == 0 || strlen(text) >= sizeof(gu_font_line_cache[0].text))
        return 0;

    if (!IsSet(flags, FLAG_NOCACHE)) {
        for (i = 0; i < PPA_FONT_ATLAS_LINE_CACHE_COUNT; ++i) {
            GUFontAtlasLine *line = &gu_font_line_cache[i];

            if (line->valid &&
                line->font_generation == gu_font_layout_generation &&
                line->x == x && line->flags == flags &&
                strcmp(line->text, text) == 0) {
                line->lru_tick = ++gu_font_line_lru_tick;
                ++gu_font_atlas_stats.line_hits;
                return line;
            }
        }
    }

    for (i = 0; i < PPA_FONT_ATLAS_LINE_CACHE_COUNT; ++i) {
        GUFontAtlasLine *line = &gu_font_line_cache[i];

        if (!line->valid) {
            victim = line;
            break;
        }

        if (line->lru_tick < oldest) {
            oldest = line->lru_tick;
            victim = line;
        }
    }

    if (victim == 0)
        return 0;

    memset(victim, 0, sizeof(*victim));
    victim->x = x;
    victim->flags = flags;
    victim->font_generation = gu_font_layout_generation;
    victim->lru_tick = ++gu_font_line_lru_tick;
    strncpy(victim->text, text, sizeof(victim->text) - 1);
    victim->text[sizeof(victim->text) - 1] = '\0';

    gu_font_capture_line = victim;
    render_string(x, gu_font_height(), victim->text, flags);
    gu_font_capture_line = 0;

    victim->valid = 1;
    if (victim->overflow) {
        victim->fallback_only = 1;
        victim->fallback_atlas_generation = 0xffffffffU;
    }

    ++gu_font_atlas_stats.line_misses;
    return victim;
}

static void gu_font_atlas_prepare_clut(unsigned int color,
                                       unsigned int *clut,
                                       unsigned int *cached_color,
                                       int *valid)
{
    unsigned int rgb;
    unsigned int alpha;
    unsigned int i;

    if (clut == 0 || cached_color == 0 || valid == 0)
        return;

    if (*valid && *cached_color == color)
        return;

    rgb = color & 0x00ffffffU;
    alpha = (color >> 24) & 0xffU;

    for (i = 0; i < 256U; ++i) {
        unsigned int coverage_alpha = (i * alpha + 127U) / 255U;
        clut[i] = rgb | (coverage_alpha << 24);
    }
    clut[0] = 0U;

    *cached_color = color;
    *valid = 1;
    ppa_cache_cpu_wrote_ge_will_read(clut, 256U * sizeof(unsigned int));
    sceGuTexFlush();
}

static void gu_font_atlas_load_texture(unsigned int *clut)
{
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD,
                   GU_SRC_ALPHA,
                   GU_ONE_MINUS_SRC_ALPHA,
                   0,
                   0);
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, clut);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0,
                  GU_FONT_ATLAS_WIDTH,
                  GU_FONT_ATLAS_HEIGHT,
                  GU_FONT_ATLAS_WIDTH,
                  gu_font_atlas);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

static int gu_font_atlas_append_sprite(struct vertex_struct *vertices,
                                       int vertex_count,
                                       const GUFontAtlasPlacement *placement,
                                       int border,
                                       int print_y,
                                       int output_inversion)
{
    const Cache_Bitmap *bitmap;
    int atlas_x;
    int atlas_y;
    int dst_x;
    int dst_y;
    int width;
    int height;
    float x0;
    float y0;
    float x1;
    float y1;
    int base_y;

    if (vertices == 0 || placement == 0 || placement->item == 0)
        return vertex_count;

    if (border && !gu_font_saver_shadow) {
        bitmap = &placement->item->border_bitmap;
        atlas_x = placement->item->atlas_border_x;
        atlas_y = placement->item->atlas_border_y;
        dst_x = placement->border_x;
        dst_y = placement->border_y;
    }
    else {
        bitmap = &placement->item->bitmap;
        atlas_x = placement->item->atlas_fill_x;
        atlas_y = placement->item->atlas_fill_y;
        dst_x = placement->fill_x;
        dst_y = placement->fill_y;
        if (border) {
            ++dst_x;
            ++dst_y;
        }
    }

    width = bitmap->width;
    height = bitmap->height;

    if (atlas_x == (int)GU_FONT_ATLAS_INVALID_COORD ||
        atlas_y == (int)GU_FONT_ATLAS_INVALID_COORD ||
        width <= 0 || height <= 0)
        return vertex_count;

    if (dst_x < 0) {
        atlas_x -= dst_x;
        width += dst_x;
        dst_x = 0;
    }
    if (dst_y < 0) {
        atlas_y -= dst_y;
        height += dst_y;
        dst_y = 0;
    }
    if (dst_x + width > SUB_SCREEN_WIDTH)
        width = SUB_SCREEN_WIDTH - dst_x;
    if (dst_y + height > SUB_SCREEN_HEIGHT)
        height = SUB_SCREEN_HEIGHT - dst_y;

    if (width <= 0 || height <= 0)
        return vertex_count;

    base_y = gufont_output_y + print_y * gufont_output_height / 272;
    x0 = (float)gufont_output_x +
         (float)dst_x * (float)gufont_output_sub_width /
             (float)SUB_SCREEN_WIDTH;
    y0 = (float)base_y +
         (float)dst_y * (float)gufont_output_sub_height /
             (float)SUB_SCREEN_HEIGHT;
    x1 = (float)gufont_output_x +
         (float)(dst_x + width) * (float)gufont_output_sub_width /
             (float)SUB_SCREEN_WIDTH;
    y1 = (float)base_y +
         (float)(dst_y + height) * (float)gufont_output_sub_height /
             (float)SUB_SCREEN_HEIGHT;

    vertices[vertex_count].texture_x = (short)atlas_x;
    vertices[vertex_count].texture_y = (short)atlas_y;
    vertices[vertex_count].vertex_x =
        output_inversion ? 480.0f - x0 : x0;
    vertices[vertex_count].vertex_y =
        output_inversion ? 272.0f - y0 : y0;
    vertices[vertex_count].vertex_z = 0.0f;
    ++vertex_count;

    vertices[vertex_count].texture_x = (short)(atlas_x + width);
    vertices[vertex_count].texture_y = (short)(atlas_y + height);
    vertices[vertex_count].vertex_x =
        output_inversion ? 480.0f - x1 : x1;
    vertices[vertex_count].vertex_y =
        output_inversion ? 272.0f - y1 : y1;
    vertices[vertex_count].vertex_z = 0.0f;
    ++vertex_count;

    return vertex_count;
}

static void gu_font_atlas_draw_pass(const GUFontAtlasLine *line,
                                    int border,
                                    int print_y,
                                    int output_inversion)
{
    int first = 0;

    if (line == 0 || line->glyph_count <= 0)
        return;

    while (first < line->glyph_count) {
        int limit = line->glyph_count - first;
        int vertex_count = 0;
        int i;
        struct vertex_struct *vertices;

        if (limit > PPA_FONT_ATLAS_BATCH_GLYPHS)
            limit = PPA_FONT_ATLAS_BATCH_GLYPHS;

        vertices = (struct vertex_struct *)
            sceGuGetMemory((unsigned int)(limit * 2) *
                           sizeof(struct vertex_struct));
        if (vertices == 0)
            return;

        for (i = 0; i < limit; ++i) {
            vertex_count =
                gu_font_atlas_append_sprite(vertices,
                                            vertex_count,
                                            &line->glyphs[first + i],
                                            border,
                                            print_y,
                                            output_inversion);
        }

        if (vertex_count > 0) {
            sceGuDrawArray(GU_SPRITES,
                           GU_TEXTURE_16BIT |
                               GU_VERTEX_32BITF |
                               GU_TRANSFORM_2D,
                           vertex_count,
                           0,
                           vertices);
            ++gu_font_atlas_stats.ge_batches;
        }

        first += limit;
    }
}

static int gu_font_print_atlas(int x,
                               int y,
                               int flags,
                               char *text,
                               int output_inversion)
{
#if PPA_ENABLE_GE_FONT_ATLAS
    static int fill_clut_valid;
    static int border_clut_valid;
    GUFontAtlasLine *line;
    int status;

    if (text == 0 || text[0] == '\0')
        return 1;

    if (IsSet(flags, FLAG_UTF16BE) || IsSet(flags, FLAG_UTF16LE))
        return 0;

    line = gu_font_atlas_find_line(text, x, flags);
    if (line == 0)
        return 0;

    if (line->fallback_only) {
        if (line->overflow ||
            line->fallback_atlas_generation == gu_font_atlas_generation)
            return 0;
        line->fallback_only = 0;
    }

    if (!gu_font_atlas_prepare_line(line)) {
        line->fallback_only = 1;
        line->fallback_atlas_generation = gu_font_atlas_generation;
        return 0;
    }

    if (gu_font_atlas_force_texflush) {
        sceGuTexFlush();
        gu_font_atlas_force_texflush = 0;
    }

    gu_font_atlas_prepare_clut(gufont_color,
                               gu_font_fill_clut,
                               &gu_font_fill_clut_color,
                               &fill_clut_valid);
    gu_font_atlas_prepare_clut(gufont_border_color,
                               gu_font_border_clut,
                               &gu_font_border_clut_color,
                               &border_clut_valid);

    status = sceGuGetAllStatus();
    sceGuEnable(GU_TEXTURE_2D);

    if (gufont_border_enable) {
        gu_font_atlas_load_texture(gu_font_border_clut);
        gu_font_atlas_draw_pass(line, 1, y, output_inversion);
    }

    gu_font_atlas_load_texture(gu_font_fill_clut);
    gu_font_atlas_draw_pass(line, 0, y, output_inversion);

    sceGuSetAllStatus(status);
    ++gu_font_atlas_stats.draw_calls;
    return 1;
#else
    (void)x;
    (void)y;
    (void)flags;
    (void)text;
    (void)output_inversion;
    return 0;
#endif
}

static void load_subtitle_texture(void *image)
	{
	sceGuEnable(GU_BLEND);
	//sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xffffff, 0xffffff);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, SUB_SCREEN_TEXTURE_WIDTH, SUB_SCREEN_HEIGHT, SUB_SCREEN_TEXTURE_WIDTH, image);
	//sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	}

static void gu_font_draw_sprite(struct texture_subdivision_struct *t)
	{
	struct vertex_struct *v = sceGuGetMemory(2 * sizeof(struct vertex_struct));

	v[0].texture_x = t->output_texture_x_start;
	v[0].texture_y = t->output_texture_y_start;
	v[0].vertex_x  = (int) t->output_vertex_x_start;
	v[0].vertex_y  = t->output_vertex_y_start;
	v[0].vertex_z  = 0.0;

	v[1].texture_x = t->output_texture_x_end;
	v[1].texture_y = t->output_texture_y_end;
	v[1].vertex_x  = (int) t->output_vertex_x_end;
	v[1].vertex_y  = t->output_vertex_y_end;
	v[1].vertex_z  = 0.0;

	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
	}

static void gu_font_draw_sprite_180(struct texture_subdivision_struct *t, int w, int h)
	{
	struct vertex_struct *v = sceGuGetMemory(2 * sizeof(struct vertex_struct));

	v[0].texture_x = t->output_texture_x_start;
	v[0].texture_y = t->output_texture_y_start;
	v[0].vertex_x  = (w) - (int) t->output_vertex_x_start;
	v[0].vertex_y  = (h) - t->output_vertex_y_start;
	v[0].vertex_z  = 0.0;

	v[1].texture_x = t->output_texture_x_end;
	v[1].texture_y = t->output_texture_y_end;
	v[1].vertex_x  = (w) - (int) t->output_vertex_x_end;
	v[1].vertex_y  = (h) - t->output_vertex_y_end;
	v[1].vertex_z  = 0.0;

	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
	}

void gu_font_printf( int x, int y, int flags, int output_inversion, char* fmt, ... )
	{
		
	va_list         ap;
	char   p[512];

	va_start( ap,fmt );
	vsnprintf( p,512,fmt,ap );
	va_end( ap );

	gu_font_print(x, y, flags, p, output_inversion );
	}

static void gu_font_print_legacy(int x,
                                 int y,
                                 int flags,
                                 char *s,
                                 int output_inversion)
{
    static int cached_x = -32768;
    static int cached_flags = -1;
    static unsigned int cached_color;
    static unsigned int cached_border_color;
    static int cached_border_enable = -1;
    int status;
    struct texture_subdivision_struct texture_subdivision;

    if (!gu_font_legacy_buffers_ensure())
        return;

    if (IsSet(flags, FLAG_UTF16BE) || IsSet(flags, FLAG_UTF16LE))
        return;

    if (strcmp(s, cache_string) != 0 ||
        cached_x != x || cached_flags != flags ||
        cached_color != gufont_color ||
        cached_border_color != gufont_border_color ||
        cached_border_enable != (int)gufont_border_enable) {
        memset(sub_8888, 0, DRAW_BUFFER_SIZE);
        memset(border_sub_8888, 0, DRAW_BUFFER_SIZE);
        memset(cache_string, 0, sizeof(cache_string));
        strncpy(cache_string, s, sizeof(cache_string) - 1);
        cache_string[sizeof(cache_string) - 1] = '\0';
        cached_x = x;
        cached_flags = flags;
        cached_color = gufont_color;
        cached_border_color = gufont_border_color;
        cached_border_enable = (int)gufont_border_enable;

        render_string(x, gu_font_height(), cache_string, flags);

        ppa_cache_cpu_wrote_ge_will_read(sub_8888, DRAW_BUFFER_SIZE);
        ppa_cache_cpu_wrote_ge_will_read(border_sub_8888,
                                         DRAW_BUFFER_SIZE);
        sceGuTexFlush();
    }

    status = sceGuGetAllStatus();
    sceGuEnable(GU_TEXTURE_2D);

    if (gufont_border_enable) {
        load_subtitle_texture(border_sub_8888);
        texture_subdivision_constructor(
            &texture_subdivision,
            SUB_SCREEN_WIDTH,
            SUB_SCREEN_HEIGHT,
            16,
            gufont_output_sub_width,
            gufont_output_sub_height,
            gufont_output_x,
            gufont_output_y + y * gufont_output_height / 272);
        do {
            texture_subdivision_get(&texture_subdivision);
            if (output_inversion)
                gu_font_draw_sprite_180(&texture_subdivision, 480, 272);
            else
                gu_font_draw_sprite(&texture_subdivision);
        } while (texture_subdivision.output_last == 0);
    }

    load_subtitle_texture(sub_8888);
    texture_subdivision_constructor(
        &texture_subdivision,
        SUB_SCREEN_WIDTH,
        SUB_SCREEN_HEIGHT,
        16,
        gufont_output_sub_width,
        gufont_output_sub_height,
        gufont_output_x,
        gufont_output_y + y * gufont_output_height / 272);
    do {
        texture_subdivision_get(&texture_subdivision);
        if (output_inversion)
            gu_font_draw_sprite_180(&texture_subdivision, 480, 272);
        else
            gu_font_draw_sprite(&texture_subdivision);
    } while (texture_subdivision.output_last == 0);

    sceGuSetAllStatus(status);
}

void gu_font_print(int x,
                   int y,
                   int flags,
                   char *s,
                   int output_inversion)
{
    int saver_shadow;
    if (face == NULL || s == 0)
        return;

    saver_shadow = cpu_clock_get_extreme_battery_saver();
    if (saver_shadow != gu_font_saver_shadow) {
        /* The cache owns fill/outline coverage as one generation. Never
         * reuse a saver fill-only entry after normal rendering is restored. */
        gu_font_saver_shadow = saver_shadow;
        gu_font_notify_font_set_changed();
    }

    if (!IsSet(gufont_haveflags, GU_FONT_HAS_UNICODE_CHARMAP)) {
        flags &= ~FLAG_UTF8;
        flags &= ~FLAG_UTF16BE;
        flags &= ~FLAG_UTF16LE;
    }

    if (gu_font_print_atlas(x, y, flags, s, output_inversion))
        return;

    ++gu_font_atlas_stats.legacy_fallbacks;
    gu_font_print_legacy(x, y, flags, s, output_inversion);
}

void gu_font_output_set(int x, int y, int w, int h)
	{
	gufont_output_width = w;
	gufont_output_height = h;
	gufont_output_sub_width = w*SUB_SCREEN_WIDTH/480;
	gufont_output_sub_height = h*SUB_SCREEN_HEIGHT/272;
	gufont_output_x = x;
	gufont_output_y = y;
	}
