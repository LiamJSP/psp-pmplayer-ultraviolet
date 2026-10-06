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
 
#ifndef __PPA_FTFONT_H__
#define __PPA_FTFONT_H__

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_STROKER_H
#include FT_SYNTHESIS_H

#include "common/graphics.h"
#include "common/mem64.h"
#include "common/ppu_boost.hpp"
#include <boost/core/noncopyable.hpp>

enum FtTextAlign {
	FT_TEXT_ALIGN_LEFT = 0,
	FT_TEXT_ALIGN_CENTER = 1,
	FT_TEXT_ALIGN_RIGHT = 2
};

class FtFont {
public:
	virtual ~FtFont(){};
	virtual void setAntiAlias(bool enable) = 0;
	virtual void setEmbolden(bool enable) = 0;
	virtual void setPixelSize(int size) = 0;
	virtual int getPixelSize() = 0;
	virtual void printStringToImage(Image* image, int x, int y, int width, int height, Color color, const char* s) = 0;

	/* Draw a UTF-8 string inside a hard clip rectangle.  The renderer applies
	 * FriBidi paragraph levels and HarfBuzz shaping, supports explicit '\n'
	 * line breaks, and reduces the pixel size until every line fits or the
	 * supplied minimum is reached. */
	virtual void drawStringInRect(Image* image,
	                             int x,
	                             int y,
	                             int width,
	                             int height,
	                             Color color,
	                             const char* s,
	                             FtTextAlign align,
	                             int preferredPixelSize,
	                             int minimumPixelSize) = 0;

	virtual int measureShapedString(const char* s) = 0;
	virtual void onSuspend() = 0;
	virtual void onResume() = 0;
	/* Drop browser-only fallback faces and their glyph caches before playback.
	 * The primary UI face remains available for localized playback overlays. */
	virtual void releaseFallbackFonts() = 0;
};

class FtFontManager : private boost::noncopyable {
private:
	static FtFontManager* instance;
	
	FT_Library library;
	FtFont* mainFont;
	
	FtFontManager();
	~FtFontManager();
	bool init();
public:
	static FtFontManager* getInstance();
	static void freeFtFontManager();
	bool loadMainFont(const char* filename);
	void unloadMainFont();
	FtFont* getMainFont();

	/* Independent streamed face used by skin/UI overlays.  Ownership is
	 * returned to the caller; delete it before freeFtFontManager(). */
	FtFont* createFont(const char* filename);
};

#endif
