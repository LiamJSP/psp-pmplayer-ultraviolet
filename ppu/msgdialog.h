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
 
#ifndef __PPA_MESSAGE_DIALOG_H__
#define __PPA_MESSAGE_DIALOG_H__

#include "common/graphics.h"
#include "ftfont.h"
#include "common/ppu_boost.hpp"
#include <boost/core/noncopyable.hpp>

#define MESSAGE_TYPE_ANY	0x00000000
#define MESSAGE_TYPE_OK_CANCEL  0x00000001
#define MESSAGE_TYPE_YES_NO	0x00000002

#define MESSAGE_RESULT_NONE	0x00000000
#define MESSAGE_RESULT_OK	0x00000001
#define MESSAGE_RESULT_CANCEL	0x00000002
#define MESSAGE_RESULT_YES	0x00000003
#define MESSAGE_RESULT_NO	0x00000004

class MessageDialog : private boost::noncopyable {
private:
//	Image* screenSnapshot;
	Image* mainWindow;
	Image* mainDrawImage;
	Image* drawImage;
	FtFont* mainFont;
	int fontSize;
	Color alpha, bgColor, labelColor;
	char messageTitle[512];
	u32 messageType;
	char keyTips[512];
	u32 keys[4];
	u32 results[4];
public:
	MessageDialog(Image* mainWindow, Image* mainDrawImage);
	~MessageDialog();
	bool init(const char* title, u32 type);
	u32 execute();
	void paint();

};

#endif
