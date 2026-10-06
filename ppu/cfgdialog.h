/* cfgdialog.h
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
 
#ifndef __PPA_CONFIG_DIALOG_H__
#define __PPA_CONFIG_DIALOG_H__

#include "common/graphics.h"
#include "ftfont.h"
#include "common/ppu_boost.hpp"
#include <boost/array.hpp>
#include <boost/core/noncopyable.hpp>

class ConfigDialog;
class UiI18n;

class ConfigItem {
protected:
	bool focus, editing;
	ConfigDialog* dialog;
	Image* drawImage;
	FtFont* mainFont;
	int fontSize;
	Color alpha, bgHlColor, labelColor, labelHlColor, valueColor, valueEdColor;
public:
	ConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual ~ConfigItem();
	virtual void paint(int x, int y, int w, int h) = 0;
	virtual void enterEditStatus() = 0;
	void setFocus(bool focus);
	const char* tr(const char* id, const char* fallback) const;
	int textWidth(const char* value) const;
};

typedef ConfigItem* PConfigItem;

class ConfigDialog : private boost::noncopyable {
private:
	//Image* screenSnapshot;
	Image* mainWindow;
	Image* mainDrawImage;
	Image* drawImage;
	/* The menu always has 20 entries. Inline pointer storage avoids a second
	 * allocation; each pointed-to ConfigItem keeps its existing ownership. */
	enum { ItemCapacity = 20 };
	boost::array<PConfigItem, ItemCapacity> items;
	int itemBottom, itemTop, itemCurrent, itemCount;
	FtFont* mainFont;
	int fontSize;
	Color alpha, bgColor, labelColor;
	const char* title;
	const char* help;
	UiI18n* uiI18n;
public:
	
	ConfigDialog(Image* mainWindow, Image* mainDrawImage, UiI18n* uiI18n);
	~ConfigDialog();
	bool init();
	void execute();
	void paint();
	const char* text(const char* id, const char* fallback) const;
	const char* languageName(const char* id, const char* fallback) const;
	FtFont* getFont() const;
	int getFontSize() const;
};

#endif
