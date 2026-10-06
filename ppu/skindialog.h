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
 
#ifndef __PPA_SKIN_DIALOG_H__
#define __PPA_SKIN_DIALOG_H__

#include "common/graphics.h"
#include "common/directory.h"
#include "ftfont.h"
#include "common/ppu_boost.hpp"
#include <boost/core/noncopyable.hpp>

class SkinDialog : private boost::noncopyable {
private:
	char skinPath[512];
	char skinShortPath[512];
	char* skinName;
	directory_item_struct *skinItems;
//	Image* screenSnapshot;
	Image* mainWindow;
	Image* mainDrawImage;
	Image* drawImage;
	int itemBottom, itemTop, itemCurrent, itemCount;
	FtFont* mainFont;
	int fontSize;
	Color alpha, bgColor, labelColor;
	const char* title;
	char help[256];
public:
	
	SkinDialog(Image* mainWindow, Image* mainDrawImage);
	~SkinDialog();
	bool init(const char* skinPath, char* skinName);
	bool execute();
	void paint();
};

#endif
