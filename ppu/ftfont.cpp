#include <new>
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
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "ftfont.h"
#include "common/ppa_hardware_profile.h"

#include FT_MODULE_H
#include FT_OUTLINE_H

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>
#endif

#if defined(PPA_USE_FRIBIDI) && PPA_USE_FRIBIDI
#include <fribidi/fribidi.h>
#endif

#define SBIT_HASH_SIZE 256
#define FTFONT_MAX_LINE_BYTES 512
#define FTFONT_MAX_CODEPOINTS 256
#define FTFONT_MAX_RUNS 64
#define FTFONT_SHAPED_CACHE_BASE 0x200000UL
#define FTFONT_FALLBACK_SLOTS 3
#define FTFONT_CACHE_BYTES (64U * 1024U)
#define FTFONT_MAX_PIXEL_SIZE 128

static int ftfont_text_length(const char* s)
{
	int length = 0;
	if (s != NULL)
		while (length < FTFONT_MAX_LINE_BYTES - 1 && s[length] != 0)
			++length;
	return length;
}

typedef struct Cache_Bitmap_ {
	int width;
	int height;
	int left;
	int top;
	char format;
	short max_grays;
	int pitch;
	unsigned char* buffer;
} Cache_Bitmap;

typedef struct SBit_HashItem_ {
	unsigned long ucs_code;
	int glyph_index;
	int size;
	bool anti_alias;
	bool embolden;
	int xadvance;
	int yadvance;
	Cache_Bitmap bitmap;
} SBit_HashItem;

static int get_next_utf8_char(unsigned long* ucs, const char* s, int n)
{
	const unsigned char *src = (const unsigned char *)s;
	unsigned char c;
	int extra;
	unsigned long minimum;
	unsigned long result;

	if (ucs == NULL || s == NULL || n <= 0)
		return 0;

	c = *src++;
	n--;

	if ((c & 0x80U) == 0) {
		result = c;
		extra = 0;
		minimum = 0;
	}
	else if ((c & 0xE0U) == 0xC0U) {
		result = c & 0x1FU;
		extra = 1;
		minimum = 0x80UL;
	}
	else if ((c & 0xF0U) == 0xE0U) {
		result = c & 0x0FU;
		extra = 2;
		minimum = 0x800UL;
	}
	else if ((c & 0xF8U) == 0xF0U) {
		result = c & 0x07U;
		extra = 3;
		minimum = 0x10000UL;
	}
	else {
		return -1;
	}

	if (extra > n)
		return -1;

	while (extra-- > 0) {
		c = *src++;
		if ((c & 0xC0U) != 0x80U)
			return -1;
		result = (result << 6) | (c & 0x3FU);
	}

	if (result < minimum || result > 0x10FFFFUL ||
	    (result >= 0xD800UL && result <= 0xDFFFUL))
		return -1;

	*ucs = result;
	return (int)((const char *)src - s);
}

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
typedef struct FtShapeRun_ {
	int start;
	int end;
	int level;
	hb_script_t script;
} FtShapeRun;

static int ftfont_is_rtl_cp(unsigned long cp)
{
	if (cp >= 0x0590UL && cp <= 0x08FFUL)
		return 1;
	if (cp >= 0xFB1DUL && cp <= 0xFDFFUL)
		return 1;
	if (cp >= 0xFE70UL && cp <= 0xFEFFUL)
		return 1;
	return 0;
}

static int ftfont_is_neutral_cp(unsigned long cp)
{
	if (cp == ' ' || cp == '\t')
		return 1;
	if (cp >= '0' && cp <= '9')
		return 1;
	if (cp == '.' || cp == ',' || cp == ':' || cp == ';' ||
	    cp == '!' || cp == '?' || cp == '-' || cp == '_' ||
	    cp == '\'' || cp == '"' || cp == '(' || cp == ')' ||
	    cp == '[' || cp == ']' || cp == '{' || cp == '}' ||
	    cp == '<' || cp == '>' || cp == '/' || cp == '+')
		return 1;
	return 0;
}

static hb_script_t ftfont_script_for_cp(unsigned long cp)
{
	if ((cp >= 0x0600UL && cp <= 0x08FFUL) ||
	    (cp >= 0xFB50UL && cp <= 0xFEFFUL))
		return HB_SCRIPT_ARABIC;
	if (cp >= 0x0590UL && cp <= 0x05FFUL)
		return HB_SCRIPT_HEBREW;
	if (cp >= 0x0900UL && cp <= 0x097FUL)
		return HB_SCRIPT_DEVANAGARI;
	if (cp >= 0x0980UL && cp <= 0x09FFUL)
		return HB_SCRIPT_BENGALI;
	if (cp >= 0x0C00UL && cp <= 0x0C7FUL)
		return HB_SCRIPT_TELUGU;
	if (cp >= 0x0E00UL && cp <= 0x0E7FUL)
		return HB_SCRIPT_THAI;
	if ((cp >= 0x1100UL && cp <= 0x11FFUL) ||
	    (cp >= 0x3130UL && cp <= 0x318FUL) ||
	    (cp >= 0xAC00UL && cp <= 0xD7AFUL))
		return HB_SCRIPT_HANGUL;
	if ((cp >= 0x3040UL && cp <= 0x309FUL))
		return HB_SCRIPT_HIRAGANA;
	if (cp >= 0x30A0UL && cp <= 0x30FFUL)
		return HB_SCRIPT_KATAKANA;
	if ((cp >= 0x2E80UL && cp <= 0x9FFFUL) ||
	    (cp >= 0xF900UL && cp <= 0xFAFFUL) ||
	    (cp >= 0x20000UL && cp <= 0x2FA1FUL))
		return HB_SCRIPT_HAN;
	if (cp >= 0x0400UL && cp <= 0x052FUL)
		return HB_SCRIPT_CYRILLIC;
	if (cp >= 0x0370UL && cp <= 0x03FFUL)
		return HB_SCRIPT_GREEK;
	if ((cp >= 'A' && cp <= 'Z') ||
	    (cp >= 'a' && cp <= 'z') ||
	    (cp >= 0x00C0UL && cp <= 0x024FUL) ||
	    (cp >= 0x1E00UL && cp <= 0x1EFFUL))
		return HB_SCRIPT_LATIN;
	return HB_SCRIPT_COMMON;
}

static int ftfont_first_strong_rtl(const hb_codepoint_t* cps, int count)
{
	int i;
	for (i = 0; i < count; ++i) {
		unsigned long cp = cps[i];
		if (ftfont_is_rtl_cp(cp))
			return 1;
		if (!ftfont_is_neutral_cp(cp))
			return 0;
	}
	return 0;
}

static void ftfont_build_levels(const hb_codepoint_t* cps,
	                            int count,
	                            int* levels)
{
	int i;
	int base_rtl;

	if (levels == NULL)
		return;
	for (i = 0; i < count; ++i)
		levels[i] = 0;
	if (cps == NULL || count <= 0)
		return;

#if defined(PPA_USE_FRIBIDI) && PPA_USE_FRIBIDI
	{
		FriBidiCharType types[FTFONT_MAX_CODEPOINTS];
		FriBidiLevel fb_levels[FTFONT_MAX_CODEPOINTS];
		FriBidiParType base_dir = FRIBIDI_PAR_ON;
		int limited = count > FTFONT_MAX_CODEPOINTS
		            ? FTFONT_MAX_CODEPOINTS : count;

		fribidi_get_bidi_types((const FriBidiChar *)cps,
		                       limited,
		                       types);
		if (fribidi_get_par_embedding_levels(types,
		                                     limited,
		                                     &base_dir,
		                                     fb_levels)) {
			for (i = 0; i < limited; ++i)
				levels[i] = fb_levels[i];
			return;
		}
	}
#endif

	base_rtl = ftfont_first_strong_rtl(cps, count);
	for (i = 0; i < count; ++i) {
		if (ftfont_is_rtl_cp(cps[i]))
			levels[i] = 1;
		else if (ftfont_is_neutral_cp(cps[i]))
			levels[i] = base_rtl ? 1 : 0;
	}
}

static int ftfont_decode_utf8_line(const char* s,
	                               hb_codepoint_t* out,
	                               int max_out)
{
	const char* p;
	int remaining;
	int count = 0;

	if (s == NULL || out == NULL || max_out <= 0)
		return 0;
	p = s;
	remaining = ftfont_text_length(s);

	while (remaining > 0 && *p != 0 && count < max_out) {
		unsigned long cp = 0;
		int consumed = get_next_utf8_char(&cp, p, remaining);
		if (consumed <= 0) {
			cp = '?';
			consumed = 1;
		}
		out[count++] = (hb_codepoint_t)cp;
		p += consumed;
		remaining -= consumed;
	}
	return count;
}

static int ftfont_make_runs(const hb_codepoint_t* cps,
	                        int count,
	                        FtShapeRun* runs,
	                        int max_runs)
{
	int levels[FTFONT_MAX_CODEPOINTS];
	int run_count = 0;
	int start;
	int level;
	hb_script_t script;
	int i;

	if (cps == NULL || runs == NULL || count <= 0 || max_runs <= 0)
		return 0;
	if (count > FTFONT_MAX_CODEPOINTS)
		count = FTFONT_MAX_CODEPOINTS;

	ftfont_build_levels(cps, count, levels);
	start = 0;
	level = levels[0];
	script = ftfont_script_for_cp(cps[0]);
	if (script == HB_SCRIPT_COMMON)
		script = (level & 1) ? HB_SCRIPT_ARABIC : HB_SCRIPT_LATIN;

	for (i = 1; i < count; ++i) {
		int next_level = levels[i];
		hb_script_t next_script = ftfont_script_for_cp(cps[i]);
		int split = 0;

		if (next_script == HB_SCRIPT_COMMON)
			next_script = script;
		if (next_level != level)
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
				runs[run_count].script = script;
				++run_count;
			}
			start = i;
			level = next_level;
			script = next_script;
		}
	}

	if (run_count < max_runs) {
		runs[run_count].start = start;
		runs[run_count].end = count;
		runs[run_count].level = level;
		runs[run_count].script = script;
		++run_count;
	}
	return run_count;
}

static void ftfont_reverse_runs(FtShapeRun* runs, int start, int end)
{
	while (start < end) {
		FtShapeRun temp = runs[start];
		runs[start] = runs[end];
		runs[end] = temp;
		++start;
		--end;
	}
}

static void ftfont_reorder_runs_visual(FtShapeRun* runs, int run_count)
{
	int i;
	int max_level = 0;
	int min_odd_level = 255;
	int level;

	if (runs == NULL || run_count <= 1)
		return;
	for (i = 0; i < run_count; ++i) {
		if (runs[i].level > max_level)
			max_level = runs[i].level;
		if ((runs[i].level & 1) && runs[i].level < min_odd_level)
			min_odd_level = runs[i].level;
	}
	if (min_odd_level == 255)
		return;

	for (level = max_level; level >= min_odd_level; --level) {
		i = 0;
		while (i < run_count) {
			int start;
			while (i < run_count && runs[i].level < level)
				++i;
			start = i;
			while (i < run_count && runs[i].level >= level)
				++i;
			if (i - start > 1)
				ftfont_reverse_runs(runs, start, i - 1);
		}
	}
}
#endif

/* Face, stream, glyph cache and fallback faces all have one owner. */
class FtFontImpl : public FtFont, private boost::noncopyable {
private:
	FT_Library library;
	FT_Face face;
	char* fontName;
	FT_Open_Args openArgs;
	FT_StreamRec fontStream;
	bool antiAlias;
	bool embolden;
	bool streamBackedFace;
	int pixelSize;
	int coverageFloor;
	int coverageCeiling;
	int coverageLevels;
	FT_Pos verticalEmboldenStrength;
	bool strongGridFit;
	bool desaturateColor;

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	hb_font_t* hbFont;
#endif

	SBit_HashItem sbitHashRoot[SBIT_HASH_SIZE];
	int cacheSize;
	int cachePop;
	size_t cacheBytes;
	/* Faces stay streamed; the three most recently needed scripts are kept.
	 * A render run never retains a fallback pointer across another lookup, so
	 * eviction is safe even for a filename containing more than three scripts. */
	FtFontImpl* fallbackFonts[FTFONT_FALLBACK_SLOTS];
	int fallbackIds[FTFONT_FALLBACK_SLOTS];
	unsigned int unavailableFallbacks;
	bool allowFallbacks;

	FtFontImpl(FT_Library libraryValue);
	bool loadFace(const char* filename);
	FtFontImpl* fallbackForId(int id);
	FtFontImpl* fontForCodepoint(unsigned long cp);
	bool supportsCodepoint(unsigned long cp) const;

	SBit_HashItem* sbitCacheFind(unsigned long cacheCode);
	void sbitCacheAdd(unsigned long cacheCode,
	                  int glyphIndex,
	                  FT_Bitmap* bitmap,
	                  int left,
	                  int top,
	                  int xadvance,
	                  int yadvance);

	void drawCachedBitmapClipped(Image* image,
	                             Cache_Bitmap* sbt,
	                             FT_Int x,
	                             FT_Int y,
	                             int clipLeft,
	                             int clipTop,
	                             int clipRight,
	                             int clipBottom,
	                             Color color);
	void drawBitmapClipped(Image* image,
	                       FT_Bitmap* bitmap,
	                       FT_Int x,
	                       FT_Int y,
	                       int clipLeft,
	                       int clipTop,
	                       int clipRight,
	                       int clipBottom,
	                       Color color);
	void drawGlyphIndexClipped(Image* image,
	                           FT_UInt glyphIndex,
	                           int penX,
	                           int baselineY,
	                           int xOffset,
	                           int yOffset,
	                           int clipLeft,
	                           int clipTop,
	                           int clipRight,
	                           int clipBottom,
	                           Color color);

	FT_Int32 glyphLoadFlags() const;
	FT_Render_Mode glyphRenderMode() const;
	void prepareGlyphSlot(FT_GlyphSlot slot);
	unsigned int sharpenCoverage(unsigned int alpha) const;

	int fontAscender() const;
	int fontDescender() const;
	int fontLineHeight() const;
	int measureShapedLine(const char* s);
	bool measureCjkInkBounds(const char* s, int* top, int* bottom);
	void renderShapedLine(Image* image,
	                      int x,
	                      int baselineY,
	                      const char* s,
	                      int clipLeft,
	                      int clipTop,
	                      int clipRight,
	                      int clipBottom,
	                      Color color);

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	hb_font_t* getHbFont();
	int shapeRunAdvance(const hb_codepoint_t* cps,
	                    const FtShapeRun* run);
	int shapeRunAdvanceSingle(const hb_codepoint_t* cps,
	                          const FtShapeRun* run);
	void renderShapeRun(Image* image,
	                    const hb_codepoint_t* cps,
	                    const FtShapeRun* run,
	                    int* penX,
	                    int* penY,
	                    int clipLeft,
	                    int clipTop,
	                    int clipRight,
	                    int clipBottom,
	                    Color color);
	void renderShapeRunSingle(Image* image,
	                          const hb_codepoint_t* cps,
	                          const FtShapeRun* run,
	                          int* penX, int* penY,
	                          int clipLeft, int clipTop,
	                          int clipRight, int clipBottom,
	                          Color color);
#endif

public:
	static FtFont* loadFont(FT_Library libraryValue, const char* filename);
	virtual ~FtFontImpl();
	virtual void setAntiAlias(bool enable);
	virtual void setEmbolden(bool enable);
	virtual void setPixelSize(int size);
	virtual int getPixelSize();
	virtual void printStringToImage(Image* image,
	                                int x,
	                                int y,
	                                int width,
	                                int height,
	                                Color color,
	                                const char* s);
	virtual void drawStringInRect(Image* image,
	                              int x,
	                              int y,
	                              int width,
	                              int height,
	                              Color color,
	                              const char* s,
	                              FtTextAlign align,
	                              int preferredPixelSize,
	                              int minimumPixelSize);
	virtual int measureShapedString(const char* s);
	virtual void onSuspend();
	virtual void onResume();
	virtual void releaseFallbackFonts();
};

static unsigned long ftfont_stream_io(FT_Stream stream,
	                                  unsigned long offset,
	                                  unsigned char* buffer,
	                                  unsigned long count)
{
	if (stream == NULL || stream->descriptor.value < 0)
		return count == 0 ? 1 : 0;
	/* FreeType commonly performs long sequential table reads.  Avoid a PSP
	 * I/O seek syscall for every callback when the requested offset is already
	 * the stream position. Reject offsets outside the streamed file. */
	if (offset > stream->size || offset > INT_MAX)
		return count == 0 ? 1 : 0;
	if (stream->pos != offset) {
		if (sceIoLseek32(stream->descriptor.value, offset, PSP_SEEK_SET) < 0)
			return count == 0 ? 1 : 0;
		stream->pos = offset;
	}
	if (count == 0)
		return 0;
	if (buffer == NULL)
		return 0;
	if (count > stream->size - offset)
		count = stream->size - offset;
	{
		int readBytes = sceIoRead(stream->descriptor.value, buffer, count);
		if (readBytes < 0)
			return 0;
		stream->pos = offset + (unsigned long)readBytes;
		return (unsigned long)readBytes;
	}
}

static void ftfont_stream_close(FT_Stream stream)
{
	if (stream != NULL && stream->descriptor.value >= 0) {
		sceIoClose(stream->descriptor.value);
		stream->descriptor.value = -1;
	}
}

FtFontImpl::FtFontImpl(FT_Library libraryValue)
	: library(libraryValue),
	  face(NULL),
	  fontName(NULL),
	  antiAlias(true),
	  embolden(false),
	  streamBackedFace(false),
	  pixelSize(12),
	  coverageFloor(40),
	  coverageCeiling(216),
	  coverageLevels(16),
	  verticalEmboldenStrength(0),
	  strongGridFit(false),
	  desaturateColor(false),
	  cacheSize(0),
	  cachePop(0),
	  cacheBytes(0),
	  unavailableFallbacks(0),
	  allowFallbacks(true)
{
	int i;
	memset(&fontStream, 0, sizeof(fontStream));
	fontStream.descriptor.value = -1;
	fontStream.read = ftfont_stream_io;
	fontStream.close = ftfont_stream_close;
	memset(&openArgs, 0, sizeof(openArgs));
	openArgs.flags = FT_OPEN_STREAM;
	openArgs.stream = &fontStream;
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	hbFont = NULL;
#endif
	/*
	 * UI text is always rasterized from vector outlines and composited by the
	 * GE with nearest-neighbour sampling.  The PSP GE cannot perform correct
	 * per-channel LCD coverage blending over an arbitrary skin texture, so
	 * grayscale coverage is the only fringe-free baseline across all panels.
	 *
	 * The model profiles instead tune three things that are effective on this
	 * fixed 480x272 target:
	 *   - full-pixel grid fitting for the slow 01g and scanlined Brite panels;
	 *   - a narrow, quantized coverage transfer function (no low-alpha halo);
	 *   - sub-pixel outline expansion on Y only for Brite horizontal strokes.
	 */
	switch (ppa_hardware_family()) {
		case PPA_PSP_FAMILY_1000: /* 01g: crisp edges reduce motion smear. */
			coverageFloor = 64;
			coverageCeiling = 192;
			coverageLevels = 8;
			strongGridFit = true;
			break;
		case PPA_PSP_FAMILY_2000: /* 02g: retain a little more edge gradation. */
			coverageFloor = 40;
			coverageCeiling = 216;
			coverageLevels = 16;
			break;
		case PPA_PSP_FAMILY_3000: /* 03g/04g/07g/09g Brite-family LCDs. */
			coverageFloor = 72;
			coverageCeiling = 184;
			coverageLevels = 8;
			verticalEmboldenStrength = 24; /* 3/8 pixel in 26.6 units. */
			strongGridFit = true;
			desaturateColor = true;
			break;
		default:
			coverageFloor = 48;
			coverageCeiling = 208;
			coverageLevels = 16;
			break;
	}
	for (i = 0; i < SBIT_HASH_SIZE; ++i)
		memset(&sbitHashRoot[i], 0, sizeof(SBit_HashItem));
	for (i = 0; i < FTFONT_FALLBACK_SLOTS; ++i) {
		fallbackFonts[i] = NULL;
		fallbackIds[i] = -1;
	}
}

FtFontImpl::~FtFontImpl()
{
	int i;
	releaseFallbackFonts();
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	if (hbFont != NULL) {
		hb_font_destroy(hbFont);
		hbFont = NULL;
	}
#endif
	for (i = 0; i < SBIT_HASH_SIZE; ++i) {
		if (sbitHashRoot[i].bitmap.buffer != NULL) {
			free_64(sbitHashRoot[i].bitmap.buffer);
			sbitHashRoot[i].bitmap.buffer = NULL;
		}
	}
	if (face != NULL) {
		FT_Done_Face(face);
		face = NULL;
	}
	if (fontStream.descriptor.value >= 0) {
		sceIoClose(fontStream.descriptor.value);
		fontStream.descriptor.value = -1;
	}
	if (fontName != NULL) {
		free_64(fontName);
		fontName = NULL;
	}
}

bool FtFontImpl::loadFace(const char* filename)
{
	FT_Error error;
	int fd;
	int size;

	if (filename == NULL || filename[0] == 0)
		return false;

	fontName = (char*)malloc_64(strlen(filename) + 1U);
	if (fontName == NULL)
		return false;
	strcpy(fontName, filename);

	fd = sceIoOpen(filename, PSP_O_RDONLY, 0777);
	if (fd < 0) {
		face = NULL;
		return false;
	}
	fontStream.descriptor.value = fd;
	fontStream.base = NULL;
	size = sceIoLseek32(fd, 0, PSP_SEEK_END);
	if (size <= 0) {
		sceIoClose(fd);
		fontStream.descriptor.value = -1;
		return false;
	}
	fontStream.size = (unsigned long)size;
	fontStream.pos = 0;
	if (sceIoLseek32(fd, 0, PSP_SEEK_SET) < 0) {
		sceIoClose(fd);
		fontStream.descriptor.value = -1;
		return false;
	}

	error = FT_Open_Face(library, &openArgs, 0, &face);
	streamBackedFace = (error == 0);
	if (error != 0) {
		/* Some PSP FreeType builds reject externally supplied streams for
		 * particular TTC/OTF faces even though their normal pathname loader
		 * accepts the same file.  Close our stream and retry through the
		 * library's native file path before reporting a startup failure. */
		if (fontStream.descriptor.value >= 0)
			sceIoClose(fontStream.descriptor.value);
		fontStream.descriptor.value = -1;
		streamBackedFace = false;
		face = NULL;
		error = FT_New_Face(library, filename, 0, &face);
		if (error != 0) {
			face = NULL;
			return false;
		}
	}
	FT_Select_Charmap(face, FT_ENCODING_UNICODE);
	setPixelSize(pixelSize);
	return true;
}

void FtFontImpl::onSuspend()
{
	int i;
	for (i = 0; i < FTFONT_FALLBACK_SLOTS; ++i)
		if (fallbackFonts[i] != NULL)
			fallbackFonts[i]->onSuspend();
	if (streamBackedFace && fontStream.descriptor.value >= 0) {
		sceIoClose(fontStream.descriptor.value);
		fontStream.descriptor.value = -1;
	}
}

void FtFontImpl::onResume()
{
	int i;
	for (i = 0; i < FTFONT_FALLBACK_SLOTS; ++i)
		if (fallbackFonts[i] != NULL)
			fallbackFonts[i]->onResume();
	if (!streamBackedFace || fontStream.descriptor.value >= 0 || fontName == NULL)
		return;
	fontStream.descriptor.value = sceIoOpen(fontName, PSP_O_RDONLY, 0777);
	if (fontStream.descriptor.value >= 0)
		sceIoLseek32(fontStream.descriptor.value,
		             fontStream.pos,
		             PSP_SEEK_SET);
}

void FtFontImpl::releaseFallbackFonts()
{
	int i;
	for (i = 0; i < FTFONT_FALLBACK_SLOTS; ++i) {
		delete fallbackFonts[i];
		fallbackFonts[i] = NULL;
		fallbackIds[i] = -1;
	}
	/* A removed/reinserted card may have gained a missing font. */
	unavailableFallbacks = 0;
}

static const char* const ftfont_fallback_names[] = {
	"DejaVuSans.ttf", "NotoSansDevanagari-Regular.ttf",
	"NotoSansBengali-Regular.ttf", "NotoSansTelugu-Regular.ttf",
	"NotoSansThai-Regular.ttf", "NotoSansHebrew-Regular.ttf",
	"NotoSansArabic-Regular.ttf", "NotoSansSC-Regular.otf",
	"NotoSansJP-Regular.otf", "NotoSansKR-Regular.otf",
	"wqy-microhei.ttc"
};

bool FtFontImpl::supportsCodepoint(unsigned long cp) const
{
	/* Join controls/variation selectors are shaping instructions, not reasons
	 * to split an Indic or Arabic cluster across fonts. */
	if (cp == 0x200CUL || cp == 0x200DUL ||
	    (cp >= 0xFE00UL && cp <= 0xFE0FUL) ||
	    (cp >= 0xE0100UL && cp <= 0xE01EFUL))
		return true;
	return face != NULL && FT_Get_Char_Index(face, cp) != 0;
}

FtFontImpl* FtFontImpl::fallbackForId(int id)
{
	FtFontImpl* selected = NULL;
	int i;
	char path[1024];
	const char* slash;
	size_t prefix;
	int count = (int)(sizeof(ftfont_fallback_names) /
	                  sizeof(ftfont_fallback_names[0]));
	if (!allowFallbacks || fontName == NULL || id < 0 || id >= count ||
	    (unavailableFallbacks & (1U << id)) != 0)
		return NULL;
	for (i = 0; i < FTFONT_FALLBACK_SLOTS; ++i) {
		if (fallbackIds[i] == id) {
			selected = fallbackFonts[i];
			break;
		}
	}
	if (selected == NULL) {
		static const char* const roots[] = {
			"PPA3xx/fonts/", "extra/fonts/", "fonts/", "interface/"
		};
		size_t applicationPrefix = 0;
		bool knownLayout = false;
		int root;
		slash = strrchr(fontName, '/');
		prefix = slash != NULL ? (size_t)(slash - fontName + 1) : 0;
		if (prefix + strlen(ftfont_fallback_names[id]) >= sizeof(path))
			return NULL;
		for (root = 0; root < 4; ++root) {
			size_t length = strlen(roots[root]);
			if (prefix >= length &&
			    memcmp(fontName + prefix - length, roots[root], length) == 0) {
				applicationPrefix = prefix - length;
				knownLayout = true;
				break;
			}
		}
		/* Evict before opening a replacement to bound peak face memory. */
		i = FTFONT_FALLBACK_SLOTS - 1;
		delete fallbackFonts[i];
		fallbackFonts[i] = NULL;
		fallbackIds[i] = -1;
		/* Assets normally share a directory. Preserve fallback coverage when
		 * a legacy interface/ primary coexists with source/release font roots. */
		for (root = -1; root < (knownLayout ? 4 : 0); ++root) {
			if (root < 0) {
				memcpy(path, fontName, prefix);
				strcpy(path + prefix, ftfont_fallback_names[id]);
			}
			else {
				size_t rootLength = strlen(roots[root]);
				if (prefix == applicationPrefix + rootLength &&
				    memcmp(fontName + applicationPrefix, roots[root], rootLength) == 0)
					continue;
				if (applicationPrefix + rootLength +
				    strlen(ftfont_fallback_names[id]) >= sizeof(path))
					continue;
				memcpy(path, fontName, applicationPrefix);
				strcpy(path + applicationPrefix, roots[root]);
				strcat(path, ftfont_fallback_names[id]);
			}
			selected = new (std::nothrow) FtFontImpl(library);
			if (selected == NULL)
				return NULL;
			selected->allowFallbacks = false;
			if (selected->loadFace(path))
				break;
			delete selected;
			selected = NULL;
		}
		if (selected == NULL) {
			unavailableFallbacks |= 1U << id;
			return NULL;
		}
	}
	/* Move to the front. Only the owning font performs fallback selection. */
	for (; i > 0; --i) {
		fallbackFonts[i] = fallbackFonts[i - 1];
		fallbackIds[i] = fallbackIds[i - 1];
	}
	fallbackFonts[0] = selected;
	fallbackIds[0] = id;
	selected->setPixelSize(pixelSize);
	selected->setAntiAlias(antiAlias);
	selected->setEmbolden(embolden);
	return selected;
}

FtFontImpl* FtFontImpl::fontForCodepoint(unsigned long cp)
{
	int id = 0;
	FtFontImpl* selected;
	if (supportsCodepoint(cp) || !allowFallbacks)
		return this;
	if ((cp >= 0x0900UL && cp <= 0x097FUL) ||
	    (cp >= 0xA8E0UL && cp <= 0xA8FFUL) ||
	    (cp >= 0x1CD0UL && cp <= 0x1CFFUL)) id = 1;
	else if (cp >= 0x0980UL && cp <= 0x09FFUL) id = 2;
	else if (cp >= 0x0C00UL && cp <= 0x0C7FUL) id = 3;
	else if (cp >= 0x0E00UL && cp <= 0x0E7FUL) id = 4;
	else if ((cp >= 0x0590UL && cp <= 0x05FFUL) ||
	         (cp >= 0xFB1DUL && cp <= 0xFB4FUL)) id = 5;
	else if ((cp >= 0x0600UL && cp <= 0x08FFUL) ||
	         (cp >= 0xFB50UL && cp <= 0xFEFFUL)) id = 6;
	else if ((cp >= 0x1100UL && cp <= 0x11FFUL) ||
	         (cp >= 0x3130UL && cp <= 0x318FUL) ||
	         (cp >= 0xAC00UL && cp <= 0xD7AFUL)) id = 9;
	else if (cp >= 0x3040UL && cp <= 0x30FFUL) id = 8;
	else if ((cp >= 0x2E80UL && cp <= 0x9FFFUL) ||
	         (cp >= 0xF900UL && cp <= 0xFAFFUL) ||
	         (cp >= 0xFF00UL && cp <= 0xFFEFUL) ||
	         (cp >= 0x20000UL && cp <= 0x2FA1FUL)) id = 7;
	selected = fallbackForId(id);
	if (selected != NULL && selected->supportsCodepoint(cp))
		return selected;
	if (id == 7 || id == 8) {
		selected = fallbackForId(10);
		if (selected != NULL && selected->supportsCodepoint(cp))
			return selected;
	}
	if (id != 0) {
		selected = fallbackForId(0);
		if (selected != NULL && selected->supportsCodepoint(cp))
			return selected;
	}
	/* Missing scripts remain a visible replacement; paths are never changed. */
	if (supportsCodepoint('?'))
		return this;
	selected = fallbackForId(0);
	return selected != NULL ? selected : this;
}

SBit_HashItem* FtFontImpl::sbitCacheFind(unsigned long cacheCode)
{
	int i;
	for (i = 0; i < cacheSize; ++i) {
		if (sbitHashRoot[i].ucs_code == cacheCode &&
		    sbitHashRoot[i].size == pixelSize &&
		    sbitHashRoot[i].anti_alias == antiAlias &&
		    sbitHashRoot[i].embolden == embolden)
			return &sbitHashRoot[i];
	}
	return NULL;
}

static bool ftfont_bitmap_safe(const FT_Bitmap* bitmap)
{
	unsigned int pitch;
	unsigned int rowBytes;
	if (bitmap == NULL || bitmap->pitch == INT_MIN ||
	    bitmap->width > 512U || bitmap->rows > 512U)
		return false;
	if (bitmap->width == 0 || bitmap->rows == 0)
		return true;
	if (bitmap->buffer == NULL ||
	    (bitmap->pixel_mode != FT_PIXEL_MODE_MONO &&
	     bitmap->pixel_mode != FT_PIXEL_MODE_GRAY))
		return false;
	pitch = bitmap->pitch < 0 ? (unsigned int)-bitmap->pitch :
	                            (unsigned int)bitmap->pitch;
	rowBytes = bitmap->pixel_mode == FT_PIXEL_MODE_MONO ?
	           (bitmap->width + 7U) / 8U : bitmap->width;
	return pitch >= rowBytes && pitch <= FTFONT_CACHE_BYTES / bitmap->rows;
}

void FtFontImpl::sbitCacheAdd(unsigned long cacheCode,
                             int glyphIndex,
                             FT_Bitmap* bitmap,
                             int left,
                             int top,
                             int xadvance,
                             int yadvance)
{
	int addIndex;
	int pitch;
	size_t bytes;
	unsigned char* copy = NULL;
	SBit_HashItem* item;
	unsigned int row;

	if (!ftfont_bitmap_safe(bitmap))
		return;
	pitch = bitmap->pitch < 0 ? -bitmap->pitch : bitmap->pitch;
	bytes = bitmap->width != 0 && bitmap->rows != 0 ?
	        (size_t)pitch * bitmap->rows : 0;
	if (bytes != 0) {
		copy = (unsigned char*)malloc_64(bytes);
		if (copy == NULL)
			return; /* Never cache a missing bitmap as a successfully drawn glyph. */
		for (row = 0; row < bitmap->rows; ++row)
			memcpy(copy + (size_t)row * pitch,
			       bitmap->buffer + (int)row * bitmap->pitch, (size_t)pitch);
	}
	if (cacheSize < SBIT_HASH_SIZE)
		addIndex = cacheSize++;
	else {
		addIndex = cachePop;
		cachePop = (cachePop + 1) % SBIT_HASH_SIZE;
	}
	item = &sbitHashRoot[addIndex];
	if (item->bitmap.buffer != NULL) {
		cacheBytes -= (size_t)item->bitmap.pitch * item->bitmap.height;
		free_64(item->bitmap.buffer);
		item->bitmap.buffer = NULL;
	}
	while (cacheBytes + bytes > FTFONT_CACHE_BYTES) {
		SBit_HashItem* old = &sbitHashRoot[cachePop];
		cachePop = (cachePop + 1) % SBIT_HASH_SIZE;
		if (old->bitmap.buffer != NULL) {
			cacheBytes -= (size_t)old->bitmap.pitch * old->bitmap.height;
			free_64(old->bitmap.buffer);
		}
		memset(old, 0, sizeof(*old));
	}
	item->ucs_code = cacheCode;
	item->glyph_index = glyphIndex;
	item->size = pixelSize;
	item->anti_alias = antiAlias;
	item->embolden = embolden;
	item->xadvance = xadvance;
	item->yadvance = yadvance;
	item->bitmap.width = bitmap->width;
	item->bitmap.height = bitmap->rows;
	item->bitmap.pitch = pitch;
	item->bitmap.left = left;
	item->bitmap.top = top;
	item->bitmap.format = bitmap->pixel_mode;
	item->bitmap.max_grays = bitmap->num_grays > 0 ? bitmap->num_grays - 1 : 0;
	item->bitmap.buffer = copy;
	cacheBytes += bytes;
}

static unsigned int ftfont_bitmap_alpha(const Cache_Bitmap* bitmap,
                                        int pitch,
                                        int x,
                                        int y)
{
	unsigned int alpha = 0;
	if (bitmap == NULL || bitmap->buffer == NULL ||
	    x < 0 || x >= bitmap->width || y < 0 || y >= bitmap->height)
		return 0;
	if (bitmap->format == FT_PIXEL_MODE_MONO) {
		if (bitmap->buffer[y * pitch + x / 8] & (0x80U >> (x % 8)))
			alpha = 255;
	}
	else if (bitmap->format == FT_PIXEL_MODE_GRAY) {
		alpha = bitmap->buffer[y * pitch + x];
		if (bitmap->max_grays > 0 && bitmap->max_grays != 255)
			alpha = (alpha * 255U) / (unsigned int)bitmap->max_grays;
	}
	return alpha;
}

FT_Int32 FtFontImpl::glyphLoadFlags() const
{
	FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP;

	if (!antiAlias)
		return flags | FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME;

	/*
	 * FT_LOAD_TARGET_MONO requests aggressive integer grid fitting without
	 * forcing a 1-bit output bitmap; FT_Render_Glyph still produces grayscale
	 * coverage.  This gives tiny UI glyphs stable stems while preserving
	 * language-specific curves and joins.
	 */
	if (strongGridFit && pixelSize <= 16)
		return flags | FT_LOAD_TARGET_MONO;
	return flags | FT_LOAD_TARGET_NORMAL;
}

FT_Render_Mode FtFontImpl::glyphRenderMode() const
{
	return antiAlias ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO;
}

void FtFontImpl::prepareGlyphSlot(FT_GlyphSlot slot)
{
	if (slot == NULL)
		return;

	if (embolden)
		FT_GlyphSlot_Embolden(slot);

	/*
	 * The Brite LCD's dark row gaps can swallow one-pixel horizontal strokes.
	 * Expand the vector outline on Y before rasterization rather than copying
	 * neighboring alpha rows afterwards.  The old post-raster dilation created
	 * the visible blur/halo reported on real PSP-3000 hardware.
	 *
	 * X strength remains zero, so advances and letter spacing do not need to be
	 * changed.  The small 3/8-pixel Y expansion is deliberately below the full
	 * one-pixel proposal, which was too heavy at 9-12px UI sizes.
	 */
	if (antiAlias &&
	    verticalEmboldenStrength > 0 &&
	    pixelSize <= 18 &&
	    slot->format == FT_GLYPH_FORMAT_OUTLINE)
		FT_Outline_EmboldenXY(&slot->outline, 0, verticalEmboldenStrength);
}

unsigned int FtFontImpl::sharpenCoverage(unsigned int alpha) const
{
	unsigned int span;
	unsigned int mapped;

	if (!antiAlias || alpha == 0 || alpha == 255)
		return alpha;
	if (alpha <= (unsigned int)coverageFloor)
		return 0;
	if (alpha >= (unsigned int)coverageCeiling)
		return 255;

	span = (unsigned int)(coverageCeiling - coverageFloor);
	if (span == 0)
		return alpha >= (unsigned int)coverageCeiling ? 255U : 0U;

	mapped = ((alpha - (unsigned int)coverageFloor) * 255U + span / 2U) / span;

	/*
	 * Quantize edge coverage to the same practical precision as a 4-bit alpha
	 * atlas.  This suppresses muddy near-transparent pixels while keeping
	 * enough levels for diagonals, CJK curves, and connected Arabic scripts.
	 */
	if (coverageLevels > 1) {
		unsigned int steps = (unsigned int)(coverageLevels - 1);
		unsigned int index = (mapped * steps + 127U) / 255U;
		mapped = (index * 255U + steps / 2U) / steps;
	}
	return mapped;
}

void FtFontImpl::drawCachedBitmapClipped(Image* image,
	                                    Cache_Bitmap* sbt,
	                                    FT_Int x,
	                                    FT_Int y,
	                                    int clipLeft,
	                                    int clipTop,
	                                    int clipRight,
	                                    int clipBottom,
	                                    Color color)
{
	FT_Int i;
	FT_Int j;
	int pitch;
	unsigned int red = R(color);
	unsigned int green = G(color);
	unsigned int blue = B(color);

	if (image == NULL || sbt == NULL || sbt->buffer == NULL)
		return;
	pitch = sbt->pitch;

	/* The Brite panel's wider gamut exaggerates saturated UI colors.  A
	 * gentle 25% luma mix preserves hue while avoiding neon fringes. */
	if (desaturateColor) {
		unsigned int luma = (77U * red + 150U * green + 29U * blue) >> 8;
		red = (3U * red + luma) >> 2;
		green = (3U * green + luma) >> 2;
		blue = (3U * blue + luma) >> 2;
	}

	for (j = 0; j < sbt->height; ++j) {
		for (i = 0; i < sbt->width; ++i) {
			int px = x + i;
			int py = y + j;
			unsigned int alpha;
			Color pixel;

			if (px < clipLeft || px >= clipRight ||
			    py < clipTop || py >= clipBottom ||
			    px < 0 || px >= image->imageWidth ||
			    py < 0 || py >= image->imageHeight)
				continue;

			alpha = sharpenCoverage(ftfont_bitmap_alpha(sbt, pitch, i, j));

			if (alpha == 0)
				continue;
			pixel = ((Color)alpha << 24) |
			        ((Color)blue << 16) |
			        ((Color)green << 8) |
			        (Color)red;
			putPixelToImage(image, pixel, px, py);
		}
	}
}

void FtFontImpl::drawBitmapClipped(Image* image,
	                              FT_Bitmap* bitmap,
	                              FT_Int x,
	                              FT_Int y,
	                              int clipLeft,
	                              int clipTop,
	                              int clipRight,
	                              int clipBottom,
	                              Color color)
{
	Cache_Bitmap temp;
	if (!ftfont_bitmap_safe(bitmap))
		return;
	memset(&temp, 0, sizeof(temp));
	temp.width = bitmap->width;
	temp.height = bitmap->rows;
	temp.pitch = bitmap->pitch;
	temp.format = bitmap->pixel_mode;
	temp.max_grays = bitmap->num_grays > 0 ? bitmap->num_grays - 1 : 0;
	temp.buffer = bitmap->buffer;
	drawCachedBitmapClipped(image,
	                        &temp,
	                        x,
	                        y,
	                        clipLeft,
	                        clipTop,
	                        clipRight,
	                        clipBottom,
	                        color);
}

void FtFontImpl::drawGlyphIndexClipped(Image* image,
	                                  FT_UInt glyphIndex,
	                                  int penX,
	                                  int baselineY,
	                                  int xOffset,
	                                  int yOffset,
	                                  int clipLeft,
	                                  int clipTop,
	                                  int clipRight,
	                                  int clipBottom,
	                                  Color color)
{
	unsigned long cacheCode;
	SBit_HashItem* cache;
	FT_Error error;
	FT_GlyphSlot slot;

	if (image == NULL || face == NULL || glyphIndex == 0)
		return;
	cacheCode = FTFONT_SHAPED_CACHE_BASE + (unsigned long)glyphIndex;
	cache = sbitCacheFind(cacheCode);
	if (cache != NULL) {
		drawCachedBitmapClipped(image,
		                        &cache->bitmap,
		                        penX + xOffset + cache->bitmap.left,
		                        baselineY - yOffset - cache->bitmap.top,
		                        clipLeft,
		                        clipTop,
		                        clipRight,
		                        clipBottom,
		                        color);
		return;
	}

	error = FT_Load_Glyph(face, glyphIndex, glyphLoadFlags());
	if (error != 0)
		return;
	slot = face->glyph;
	prepareGlyphSlot(slot);
	error = FT_Render_Glyph(slot, glyphRenderMode());
	if (error != 0)
		return;

	drawBitmapClipped(image,
	                  &slot->bitmap,
	                  penX + xOffset + slot->bitmap_left,
	                  baselineY - yOffset - slot->bitmap_top,
	                  clipLeft,
	                  clipTop,
	                  clipRight,
	                  clipBottom,
	                  color);
	sbitCacheAdd(cacheCode,
	             glyphIndex,
	             &slot->bitmap,
	             slot->bitmap_left,
	             slot->bitmap_top,
	             slot->advance.x,
	             slot->advance.y);
}

FtFont* FtFontImpl::loadFont(FT_Library libraryValue, const char* filename)
{
	FtFontImpl* object = new (std::nothrow) FtFontImpl(libraryValue);
	if (object == NULL)
		return NULL;
	if (!object->loadFace(filename)) {
		delete object;
		return NULL;
	}
	return object;
}

void FtFontImpl::setAntiAlias(bool enable)
{
	if (antiAlias == enable)
		return;
	antiAlias = enable;
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	if (hbFont != NULL) {
		hb_font_destroy(hbFont);
		hbFont = NULL;
	}
#endif
}

void FtFontImpl::setEmbolden(bool enable)
{
	embolden = enable;
}

void FtFontImpl::setPixelSize(int size)
{
	if (size < 1)
		size = 1;
	if (size > FTFONT_MAX_PIXEL_SIZE)
		size = FTFONT_MAX_PIXEL_SIZE;
	if (pixelSize == size && face != NULL && face->size != NULL &&
	    face->size->metrics.y_ppem == (FT_UShort)size)
		return;
	pixelSize = size;
	if (face != NULL)
		FT_Set_Pixel_Sizes(face, pixelSize, pixelSize);
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	if (hbFont != NULL) {
		hb_font_destroy(hbFont);
		hbFont = NULL;
	}
#endif
}

int FtFontImpl::getPixelSize()
{
	return pixelSize;
}

void FtFontImpl::printStringToImage(Image* image,
	                               int x,
	                               int y,
	                               int width,
	                               int height,
	                               Color color,
	                               const char* s)
{
	int clipLeft;
	int clipTop;
	int clipRight;
	int clipBottom;
	int stringLength;
	const char* p;
	int n = 0;
	FtFontImpl* glyphFont = this;
	FT_UInt previous = 0;

	if (image == NULL || face == NULL || s == NULL || width <= 0 || height <= 0)
		return;
	if (x < -image->imageWidth || x >= image->imageWidth ||
	    y < -image->imageHeight || y > image->imageHeight + pixelSize)
		return;
	if (width > image->imageWidth) width = image->imageWidth;
	if (height > image->imageHeight) height = image->imageHeight;
	clipLeft = x;
	clipTop = y - pixelSize;
	clipRight = x + width;
	clipBottom = clipTop + height;
	if (clipLeft < 0)
		clipLeft = 0;
	if (clipTop < 0)
		clipTop = 0;
	if (clipRight > image->imageWidth)
		clipRight = image->imageWidth;
	if (clipBottom > image->imageHeight)
		clipBottom = image->imageHeight;

	stringLength = ftfont_text_length(s);
	if (stringLength <= 0)
		return;
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	/* Keep the fast legacy path for the overwhelmingly common ASCII browser
	 * names.  Non-ASCII text goes through HarfBuzz/FriBidi so the translated
	 * config menu and multilingual filenames receive correct shaping. */
	{
		const unsigned char* scan = (const unsigned char*)s;
		while (scan < (const unsigned char*)s + stringLength &&
		       *scan != 0 && *scan < 0x80U)
			++scan;
		if (*scan != 0) {
			renderShapedLine(image, x, y, s,
			                 clipLeft, clipTop, clipRight, clipBottom, color);
			return;
		}
	}
#endif
	p = s;

	while (n < stringLength) {
		unsigned long ucs = 0;
		int consumed = get_next_utf8_char(&ucs, p, stringLength - n);
		FT_UInt glyphIndex;
		SBit_HashItem* cache;

		if (consumed <= 0) {
			ucs = '?';
			consumed = 1;
		}
		p += consumed;
		n += consumed;

		if (!glyphFont->supportsCodepoint(ucs)) {
			previous = 0;
			glyphFont = fontForCodepoint(ucs);
		}
		glyphIndex = FT_Get_Char_Index(glyphFont->face, ucs);
		if (glyphIndex == 0)
			glyphIndex = FT_Get_Char_Index(glyphFont->face, '?');
		cache = glyphFont->sbitCacheFind(ucs);

		if (FT_HAS_KERNING(glyphFont->face) && previous != 0 && glyphIndex != 0) {
			FT_Vector delta;
			FT_Get_Kerning(glyphFont->face,
			              previous,
			              glyphIndex,
			              FT_KERNING_DEFAULT,
			              &delta);
			x += delta.x >> 6;
		}

		if (cache != NULL && cache->glyph_index == (int)glyphIndex) {
			glyphFont->drawCachedBitmapClipped(image,
			                        &cache->bitmap,
			                        x + cache->bitmap.left,
			                        y - cache->bitmap.top,
			                        clipLeft,
			                        clipTop,
			                        clipRight,
			                        clipBottom,
			                        color);
			x += cache->xadvance >> 6;
		}
		else {
			FT_Error error = FT_Load_Glyph(glyphFont->face,
			                               glyphIndex,
			                               glyphFont->glyphLoadFlags());
			FT_GlyphSlot slot;
			if (error != 0)
				continue;
			slot = glyphFont->face->glyph;
			glyphFont->prepareGlyphSlot(slot);
			error = FT_Render_Glyph(slot, glyphFont->glyphRenderMode());
			if (error != 0)
				continue;
			glyphFont->drawBitmapClipped(image,
			                  &slot->bitmap,
			                  x + slot->bitmap_left,
			                  y - slot->bitmap_top,
			                  clipLeft,
			                  clipTop,
			                  clipRight,
			                  clipBottom,
			                  color);
			x += slot->advance.x >> 6;
			glyphFont->sbitCacheAdd(ucs,
			             glyphIndex,
			             &slot->bitmap,
			             slot->bitmap_left,
			             slot->bitmap_top,
			             slot->advance.x,
			             slot->advance.y);
		}
		previous = glyphIndex;
	}
}

int FtFontImpl::fontAscender() const
{
	if (face == NULL || face->size == NULL)
		return pixelSize;
	return (int)((face->size->metrics.ascender + 63) >> 6);
}

int FtFontImpl::fontDescender() const
{
	int value;
	if (face == NULL || face->size == NULL)
		return 2;
	value = (int)(-(face->size->metrics.descender >> 6));
	return value < 0 ? 0 : value;
}

int FtFontImpl::fontLineHeight() const
{
	int height;
	if (face == NULL || face->size == NULL)
		return pixelSize + 2;
	height = (int)((face->size->metrics.height + 63) >> 6);
	if (height < fontAscender() + fontDescender())
		height = fontAscender() + fontDescender();
	return height > 0 ? height : pixelSize + 2;
}

bool FtFontImpl::measureCjkInkBounds(const char* s, int* top, int* bottom)
{
	const char* p = s;
	int remaining = ftfont_text_length(s);
	bool hasCjk = false;
	bool hasInk = false;
	int inkTop = 0;
	int inkBottom = 0;

	if (face == NULL || top == NULL || bottom == NULL)
		return false;
	/* Skip glyph loads entirely for the common non-CJK UI strings. */
	while (remaining > 0) {
		unsigned long cp = 0;
		int consumed = get_next_utf8_char(&cp, p, remaining);
		if (consumed <= 0) return false;
		p += consumed;
		remaining -= consumed;
		if ((cp >= 0x2e80UL && cp <= 0x9fffUL) ||
		    (cp >= 0xac00UL && cp <= 0xd7afUL) ||
		    (cp >= 0xf900UL && cp <= 0xfaffUL) ||
		    (cp >= 0x20000UL && cp <= 0x2fa1fUL)) {
			hasCjk = true;
			break;
		}
	}
	if (!hasCjk)
		return false;
	p = s;
	remaining = ftfont_text_length(s);
	while (remaining > 0) {
		unsigned long cp = 0;
		int consumed = get_next_utf8_char(&cp, p, remaining);
		FT_UInt glyph;
		int glyphTop;
		int glyphBottom;
		if (consumed <= 0)
			return false;
		p += consumed;
		remaining -= consumed;
		FtFontImpl* selected = fontForCodepoint(cp);
		glyph = FT_Get_Char_Index(selected->face, cp);
		if (glyph == 0 || FT_Load_Glyph(selected->face, glyph, selected->glyphLoadFlags()) != 0)
			continue;
		selected->prepareGlyphSlot(selected->face->glyph);
		if (selected->face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
			FT_BBox box;
			FT_Outline_Get_CBox(&selected->face->glyph->outline, &box);
			glyphTop = (int)((box.yMax + 63) >> 6);
			glyphBottom = (int)(box.yMin >> 6);
		}
		else {
			glyphTop = (int)((selected->face->glyph->metrics.horiBearingY + 63) >> 6);
			glyphBottom = (int)((selected->face->glyph->metrics.horiBearingY -
			                     selected->face->glyph->metrics.height) >> 6);
		}
		if (!hasInk || glyphTop > inkTop) inkTop = glyphTop;
		if (!hasInk || glyphBottom < inkBottom) inkBottom = glyphBottom;
		hasInk = true;
	}
	if (!hasInk)
		return false;
	*top = inkTop;
	*bottom = inkBottom;
	return true;
}

#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
hb_font_t* FtFontImpl::getHbFont()
{
	if (face == NULL)
		return NULL;
	if (hbFont == NULL) {
		hbFont = hb_ft_font_create_referenced(face);
		if (hbFont != NULL)
			hb_ft_font_set_load_flags(hbFont, glyphLoadFlags());
	}
	return hbFont;
}

/* Keep complete covered script segments together for HarfBuzz; splitting
 * every character would break Indic conjuncts and Arabic contextual forms.
 * Face selection is performed separately during measure and render, so no
 * pointers into the bounded LRU escape the currently processed segment. */
int FtFontImpl::shapeRunAdvance(const hb_codepoint_t* cps,
                               const FtShapeRun* run)
{
	int width = 0;
	int start;
	if (cps == NULL || run == NULL) return 0;
	start = run->start;
	while (start < run->end) {
		FtShapeRun part = *run;
		FtFontImpl* selected = fontForCodepoint(cps[start]);
		part.start = start;
		part.end = start + 1;
		while (part.end < run->end && selected->supportsCodepoint(cps[part.end]))
			++part.end;
		width += selected->shapeRunAdvanceSingle(cps, &part);
		start = part.end;
	}
	return width;
}

void FtFontImpl::renderShapeRun(Image* image,
                              const hb_codepoint_t* cps,
                              const FtShapeRun* run,
                              int* penX, int* penY,
                              int clipLeft, int clipTop,
                              int clipRight, int clipBottom,
                              Color color)
{
	int boundaries[FTFONT_MAX_CODEPOINTS + 1];
	int count = 0;
	int cursor;
	int i;
	if (cps == NULL || run == NULL || run->start < 0 ||
	    run->end > FTFONT_MAX_CODEPOINTS) return;
	cursor = run->start;
	boundaries[0] = cursor;
	/* Partition in logical order, just as measurement does. Render the
	 * resulting segments in visual order without retaining evictable faces. */
	while (cursor < run->end && count < FTFONT_MAX_CODEPOINTS) {
		FtFontImpl* selected = fontForCodepoint(cps[cursor]);
		++cursor;
		while (cursor < run->end && selected->supportsCodepoint(cps[cursor]))
			++cursor;
		boundaries[++count] = cursor;
	}
	for (i = 0; i < count; ++i) {
		int index = (run->level & 1) ? count - i - 1 : i;
		FtShapeRun part = *run;
		part.start = boundaries[index];
		part.end = boundaries[index + 1];
		FtFontImpl* selected = fontForCodepoint(cps[part.start]);
		selected->renderShapeRunSingle(image, cps, &part, penX, penY,
		                              clipLeft, clipTop, clipRight, clipBottom, color);
	}
}

int FtFontImpl::shapeRunAdvanceSingle(const hb_codepoint_t* cps,
	                           const FtShapeRun* run)
{
	hb_font_t* font;
	hb_buffer_t* buffer;
	hb_glyph_position_t* positions;
	unsigned int glyphCount = 0;
	int advance = 0;
	unsigned int i;

	if (cps == NULL || run == NULL || run->end <= run->start)
		return 0;
	font = getHbFont();
	if (font == NULL)
		return 0;
	buffer = hb_buffer_create();
	if (buffer == NULL)
		return 0;

	hb_buffer_add_utf32(buffer,
	                    cps + run->start,
	                    run->end - run->start,
	                    0,
	                    run->end - run->start);
	hb_buffer_set_direction(buffer,
	                        (run->level & 1)
	                            ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
	hb_buffer_set_script(buffer, run->script);
	hb_buffer_set_language(buffer, hb_language_from_string("und", -1));
	hb_shape(font, buffer, NULL, 0);
	positions = hb_buffer_get_glyph_positions(buffer, &glyphCount);
	for (i = 0; i < glyphCount; ++i)
		advance += positions[i].x_advance;
	hb_buffer_destroy(buffer);
	return (advance + 32) >> 6;
}

void FtFontImpl::renderShapeRunSingle(Image* image,
	                           const hb_codepoint_t* cps,
	                           const FtShapeRun* run,
	                           int* penX,
	                           int* penY,
	                           int clipLeft,
	                           int clipTop,
	                           int clipRight,
	                           int clipBottom,
	                           Color color)
{
	hb_font_t* font;
	hb_buffer_t* buffer;
	hb_glyph_info_t* infos;
	hb_glyph_position_t* positions;
	unsigned int glyphCount = 0;
	unsigned int i;

	if (image == NULL || cps == NULL || run == NULL ||
	    penX == NULL || penY == NULL || run->end <= run->start)
		return;
	font = getHbFont();
	if (font == NULL)
		return;
	buffer = hb_buffer_create();
	if (buffer == NULL)
		return;

	hb_buffer_add_utf32(buffer,
	                    cps + run->start,
	                    run->end - run->start,
	                    0,
	                    run->end - run->start);
	hb_buffer_set_direction(buffer,
	                        (run->level & 1)
	                            ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
	hb_buffer_set_script(buffer, run->script);
	hb_buffer_set_language(buffer, hb_language_from_string("und", -1));
	hb_shape(font, buffer, NULL, 0);
	infos = hb_buffer_get_glyph_infos(buffer, &glyphCount);
	positions = hb_buffer_get_glyph_positions(buffer, &glyphCount);

	for (i = 0; i < glyphCount; ++i) {
		FT_UInt glyph = infos[i].codepoint;
		/* HarfBuzz may emit a zero-width .notdef for an invisible join/control
		 * character. Do not turn that shaping instruction into a visible '?'. */
		if (glyph == 0 && (positions[i].x_advance != 0 || positions[i].y_advance != 0))
			glyph = FT_Get_Char_Index(face, '?');
		drawGlyphIndexClipped(image,
		                      glyph,
		                      *penX,
		                      *penY,
		                      positions[i].x_offset >> 6,
		                      positions[i].y_offset >> 6,
		                      clipLeft,
		                      clipTop,
		                      clipRight,
		                      clipBottom,
		                      color);
		*penX += positions[i].x_advance >> 6;
		*penY -= positions[i].y_advance >> 6;
	}
	hb_buffer_destroy(buffer);
}
#endif

int FtFontImpl::measureShapedLine(const char* s)
{
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	hb_codepoint_t cps[FTFONT_MAX_CODEPOINTS];
	FtShapeRun runs[FTFONT_MAX_RUNS];
	int count;
	int runCount;
	int width = 0;
	int i;

	if (s == NULL || s[0] == 0)
		return 0;
	count = ftfont_decode_utf8_line(s, cps, FTFONT_MAX_CODEPOINTS);
	if (count <= 0)
		return 0;
	runCount = ftfont_make_runs(cps, count, runs, FTFONT_MAX_RUNS);
	ftfont_reorder_runs_visual(runs, runCount);
	for (i = 0; i < runCount; ++i)
		width += shapeRunAdvance(cps, &runs[i]);
	return width;
#else
	int width = 0;
	const char* p = s;
	int remaining = ftfont_text_length(s);
	while (remaining > 0) {
		unsigned long cp;
		int consumed = get_next_utf8_char(&cp, p, remaining);
		FT_UInt glyph;
		if (consumed <= 0) {
			cp = '?';
			consumed = 1;
		}
		FtFontImpl* selected = fontForCodepoint(cp);
		glyph = FT_Get_Char_Index(selected->face, cp);
		if (glyph == 0) glyph = FT_Get_Char_Index(selected->face, '?');
		if (FT_Load_Glyph(selected->face, glyph, selected->glyphLoadFlags()) == 0)
			width += selected->face->glyph->advance.x >> 6;
		p += consumed;
		remaining -= consumed;
	}
	return width;
#endif
}

void FtFontImpl::renderShapedLine(Image* image,
	                             int x,
	                             int baselineY,
	                             const char* s,
	                             int clipLeft,
	                             int clipTop,
	                             int clipRight,
	                             int clipBottom,
	                             Color color)
{
#if defined(PPA_USE_HARFBUZZ) && PPA_USE_HARFBUZZ
	hb_codepoint_t cps[FTFONT_MAX_CODEPOINTS];
	FtShapeRun runs[FTFONT_MAX_RUNS];
	int count;
	int runCount;
	int penX = x;
	int penY = baselineY;
	int i;

	count = ftfont_decode_utf8_line(s, cps, FTFONT_MAX_CODEPOINTS);
	if (count <= 0)
		return;
	runCount = ftfont_make_runs(cps, count, runs, FTFONT_MAX_RUNS);
	ftfont_reorder_runs_visual(runs, runCount);
	for (i = 0; i < runCount; ++i)
		renderShapeRun(image,
		               cps,
		               &runs[i],
		               &penX,
		               &penY,
		               clipLeft,
		               clipTop,
		               clipRight,
		               clipBottom,
		               color);
#else
	printStringToImage(image,
	                   x,
	                   baselineY,
	                   clipRight - x,
	                   clipBottom - clipTop,
	                   color,
	                   s);
#endif
}

int FtFontImpl::measureShapedString(const char* s)
{
	char line[FTFONT_MAX_LINE_BYTES];
	const char* p;
	int maxWidth = 0;
	if (s == NULL)
		return 0;
	p = s;
	while (*p != 0) {
		const char* newline = strchr(p, '\n');
		size_t length = newline != NULL
		              ? (size_t)(newline - p) : strlen(p);
		int width;
		if (length >= sizeof(line))
			length = sizeof(line) - 1U;
		memcpy(line, p, length);
		line[length] = 0;
		width = measureShapedLine(line);
		if (width > maxWidth)
			maxWidth = width;
		if (newline == NULL)
			break;
		p = newline + 1;
	}
	return maxWidth;
}

void FtFontImpl::drawStringInRect(Image* image,
	                             int x,
	                             int y,
	                             int width,
	                             int height,
	                             Color color,
	                             const char* s,
	                             FtTextAlign align,
	                             int preferredPixelSize,
	                             int minimumPixelSize)
{
	char lines[4][FTFONT_MAX_LINE_BYTES];
	int lineWidths[4];
	int inkTops[4];
	int inkBottoms[4];
	int lineCount = 0;
	bool cjkInkFit = false;
	const char* p;
	int chosenSize;
	int size;
	int lineHeight;
	int ascender;
	int totalHeight;
	int baseline;
	int i;
	int clipLeft;
	int clipTop;
	int clipRight;
	int clipBottom;

	if (image == NULL || face == NULL || s == NULL || s[0] == 0 ||
	    width <= 0 || height <= 0)
		return;
	if (x < -image->imageWidth || x >= image->imageWidth ||
	    y < -image->imageHeight || y >= image->imageHeight)
		return;
	if (width > image->imageWidth) width = image->imageWidth;
	if (height > image->imageHeight) height = image->imageHeight;

	p = s;
	while (lineCount < 4) {
		const char* newline = strchr(p, '\n');
		size_t length = newline != NULL
		              ? (size_t)(newline - p) : strlen(p);
		if (length >= FTFONT_MAX_LINE_BYTES)
			length = FTFONT_MAX_LINE_BYTES - 1U;
		memcpy(lines[lineCount], p, length);
		lines[lineCount][length] = 0;
		++lineCount;
		if (newline == NULL)
			break;
		p = newline + 1;
	}

	if (preferredPixelSize < 1)
		preferredPixelSize = height;
	if (preferredPixelSize > FTFONT_MAX_PIXEL_SIZE)
		preferredPixelSize = FTFONT_MAX_PIXEL_SIZE;
	if (minimumPixelSize < 1)
		minimumPixelSize = 1;
	if (minimumPixelSize > preferredPixelSize)
		minimumPixelSize = preferredPixelSize;

	chosenSize = minimumPixelSize;
	/* CJK fonts can reserve much more line spacing than their visible glyphs.
	 * Fit the actual outlines for these short UI labels; keep the existing
	 * typographic line-height layout for other scripts. */
	setPixelSize(preferredPixelSize);
	for (i = 0; i < lineCount; ++i) {
		int top, bottom;
		if (measureCjkInkBounds(lines[i], &top, &bottom)) {
			cjkInkFit = true;
			break;
		}
	}
	for (size = preferredPixelSize; size >= minimumPixelSize; --size) {
		int fits = 1;
		setPixelSize(size);
		if (cjkInkFit) {
			int inkHeight = 0;
			for (i = 0; i < lineCount; ++i) {
				if (!measureCjkInkBounds(lines[i], &inkTops[i], &inkBottoms[i])) {
					inkTops[i] = fontAscender();
					inkBottoms[i] = -fontDescender();
				}
				inkHeight += inkTops[i] - inkBottoms[i];
			}
			if (inkHeight + lineCount - 1 > height)
				fits = 0;
		}
		else if (fontLineHeight() * lineCount > height)
			fits = 0;
		for (i = 0; i < lineCount; ++i) {
			lineWidths[i] = measureShapedLine(lines[i]);
			if (lineWidths[i] > width)
				fits = 0;
		}
		if (fits) {
			chosenSize = size;
			break;
		}
	}

	setPixelSize(chosenSize);
	lineHeight = fontLineHeight();
	ascender = fontAscender();
	totalHeight = lineHeight * lineCount;
	if (cjkInkFit) {
		totalHeight = lineCount - 1;
		for (i = 0; i < lineCount; ++i) {
			if (!measureCjkInkBounds(lines[i], &inkTops[i], &inkBottoms[i])) {
				inkTops[i] = ascender;
				inkBottoms[i] = -fontDescender();
			}
			totalHeight += inkTops[i] - inkBottoms[i];
		}
	}
	for (i = 0; i < lineCount; ++i)
		lineWidths[i] = measureShapedLine(lines[i]);

	clipLeft = x < 0 ? 0 : x;
	clipTop = y < 0 ? 0 : y;
	clipRight = x + width;
	clipBottom = y + height;
	if (clipRight > image->imageWidth)
		clipRight = image->imageWidth;
	if (clipBottom > image->imageHeight)
		clipBottom = image->imageHeight;
	if (clipRight <= clipLeft || clipBottom <= clipTop)
		return;

	baseline = y + (height - totalHeight) / 2 +
	           (cjkInkFit ? inkTops[0] : ascender);
	for (i = 0; i < lineCount; ++i) {
		int lineX = x;
		if (align == FT_TEXT_ALIGN_CENTER)
			lineX = x + (width - lineWidths[i]) / 2;
		else if (align == FT_TEXT_ALIGN_RIGHT)
			lineX = x + width - lineWidths[i];
		renderShapedLine(image,
		                  lineX,
		                  baseline,
		                  lines[i],
		                  clipLeft,
		                  clipTop,
		                  clipRight,
		                  clipBottom,
		                  color);
		if (cjkInkFit && i + 1 < lineCount)
			baseline += -inkBottoms[i] + 1 + inkTops[i + 1];
		else
			baseline += lineHeight;
	}
}

FtFontManager* FtFontManager::instance = NULL;

FtFontManager::FtFontManager()
	: library(NULL), mainFont(NULL)
{
}

FtFontManager::~FtFontManager()
{
	if (mainFont != NULL) {
		delete mainFont;
		mainFont = NULL;
	}
	if (library != NULL) {
		FT_Done_FreeType(library);
		library = NULL;
	}
}

bool FtFontManager::init()
{
	FT_Error error;
	FT_UInt interpreterVersion = 35;

	error = FT_Init_FreeType(&library);
	if (error != 0)
		return false;

	/*
	 * Interpreter v35 performs full-pixel TrueType grid fitting.  Version 40
	 * is designed for ClearType/minimal subpixel hinting and is visibly softer
	 * at this application's fixed 9-12px UI sizes.  CFF and unhinted outlines
	 * continue through FreeType's normal hinter.
	 */
	error = FT_Property_Set(library,
	                        "truetype",
	                        "interpreter-version",
	                        &interpreterVersion);
	return true;
}

FtFontManager* FtFontManager::getInstance()
{
	if (instance != NULL)
		return instance;
	instance = new (std::nothrow) FtFontManager();
	if (instance == NULL)
		return NULL;
	if (!instance->init()) {
		delete instance;
		instance = NULL;
	}
	return instance;
}

void FtFontManager::freeFtFontManager()
{
	if (instance != NULL) {
		delete instance;
		instance = NULL;
	}
}

bool FtFontManager::loadMainFont(const char* filename)
{
	unloadMainFont();
	mainFont = FtFontImpl::loadFont(library, filename);
	return mainFont != NULL;
}

void FtFontManager::unloadMainFont()
{
	if (mainFont != NULL) {
		delete mainFont;
		mainFont = NULL;
	}
}

FtFont* FtFontManager::getMainFont()
{
	return mainFont;
}

FtFont* FtFontManager::createFont(const char* filename)
{
	if (library == NULL || filename == NULL)
		return NULL;
	return FtFontImpl::loadFont(library, filename);
}
