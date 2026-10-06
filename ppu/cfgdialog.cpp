#include <new>
//cfgdialog.cpp
#include <strings.h>
#ifndef stricmp
#define stricmp strcasecmp
#endif
#ifndef strnicmp
#define strnicmp strncasecmp
#endif
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <pspkernel.h>
#include <pspdisplay.h>
#include "common/ctrl.h"
#include "common/ppu_controls.h"
#include "common/libminiconv.h"
#include "common/directory.h"
#include "common/graphics.h"
#include "common/libi18n.h"

#include "mod/cpu_clock.h"
#include "mod/subtitle_charset.h"
#include "mod/gu_font.h"
#include "mod/subtitle_preferences.h"
#include "media/VideoPipeline.h"

#include "cfgdialog.h"
#include "config.h"
#include "skin.h"
#include "videomode.h"
#include "ui_i18n.h"

#define PPA_CONFIGDLG_ALPHA	0xD0
#define PPA_CONFIGDLG_HLALPHA	0xD0
#define PPA_CONFIGDLG_BG_COLOR 0x000000
#define PPA_CONFIGDLG_BG_HLCOLOR 0x000000
#define PPA_CONFIGDLG_LABEL_COLOR 0xFFFFFF
#define PPA_CONFIGDLG_LABEL_HLCOLOR 0x00FFFF
#define PPA_CONFIGDLG_VALUE_COLOR 0x00FF00
#define PPA_CONFIGDLG_VALUE_EDCOLOR 0x0000FF

ConfigItem::ConfigItem(ConfigDialog* dialog, Image* drawImage){
	focus = false;
	editing = false;
	this->dialog = dialog;
	this->drawImage = drawImage;
	mainFont = dialog != NULL ? dialog->getFont() : NULL;
	if (mainFont == NULL && FtFontManager::getInstance() != NULL)
		mainFont = FtFontManager::getInstance()->getMainFont();
	fontSize = dialog != NULL ? dialog->getFontSize()
	                          : (mainFont != NULL ? mainFont->getPixelSize() : 12);
	if (mainFont != NULL)
		mainFont->setPixelSize(fontSize);
	Skin* skin = Skin::getInstance();
	alpha = skin->getAlphaValue("skin/config_dialog/dialog/highlight_alpha", PPA_CONFIGDLG_HLALPHA);
	bgHlColor = skin->getColorValue("skin/config_dialog/dialog/background_highlight_color", PPA_CONFIGDLG_BG_HLCOLOR);
	labelColor = skin->getColorValue("skin/config_dialog/dialog/label_color", PPA_CONFIGDLG_LABEL_COLOR);
	labelHlColor = skin->getColorValue("skin/config_dialog/dialog/label_highlight_color", PPA_CONFIGDLG_LABEL_HLCOLOR);
	valueColor = skin->getColorValue("skin/config_dialog/dialog/value_color", PPA_CONFIGDLG_VALUE_COLOR);
	valueEdColor = skin->getColorValue("skin/config_dialog/dialog/value_edit_color", PPA_CONFIGDLG_VALUE_EDCOLOR);
};

ConfigItem::~ConfigItem() {
	dialog = NULL;
	drawImage = NULL;
	mainFont = NULL;
};

void ConfigItem::setFocus(bool focus) {
	this->focus = focus;
};

const char* ConfigItem::tr(const char* id, const char* fallback) const {
	return dialog != NULL ? dialog->text(id, fallback)
	                      : (fallback != NULL ? fallback : "");
}

int ConfigItem::textWidth(const char* value) const {
	if (mainFont == NULL || value == NULL)
		return 0;
	mainFont->setPixelSize(fontSize);
	return mainFont->measureShapedString(value);
}
/********************************************************************************
 *                      CpuSpeed                                                *
 ********************************************************************************/
class CpuSpeedConfigItem : public ConfigItem {
private:
	enum { ValueCount = 7 };
	int values[ValueCount];
	const char* label;
	int currentValue, newValue;
public:
	CpuSpeedConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h);
	virtual void enterEditStatus();
};

CpuSpeedConfigItem::CpuSpeedConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.cpu_speed", "CPU Speed");

	values[0] = 66;
	values[1] = 111;
	values[2] = 133;
	values[3] = 166;
	values[4] = 222;
	values[5] = 266;
	values[6] = 333;

	Config* config = Config::getInstance();
	const int speed = config->getIntegerValue("config/cpu/speed", 66);

	/* Old configurations may contain a frequency no longer shown in the
	 * menu.  Select the nearest supported point rather than silently forcing
	 * the clock upward. */
	currentValue = 0;
	int nearestDistance = abs(speed - values[0]);
	for (int i = 1; i < ValueCount; ++i) {
		const int distance = abs(speed - values[i]);
		if (distance < nearestDistance) {
			currentValue = i;
			nearestDistance = distance;
		}
	}

	newValue = currentValue;
}

void CpuSpeedConfigItem::paint(int x, int y, int w, int h) {
	if (focus) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage,
		                             x + 1,
		                             y + fontSize - 1,
		                             w - 2,
		                             h - 2,
		                             labelHlColor,
		                             label);
	}
	else {
		mainFont->printStringToImage(drawImage,
		                             x + 1,
		                             y + fontSize - 1,
		                             w - 2,
		                             h - 2,
		                             labelColor,
		                             label);
	}

	int x1 = x + 1 + textWidth(label) + fontSize / 2;

	char valueString[4];
	memset(valueString, 0, sizeof(valueString));

	if (editing) {
		snprintf(valueString, sizeof(valueString), "%3d", values[newValue]);
		mainFont->printStringToImage(drawImage,
		                             x1,
		                             y + fontSize - 1,
		                             w - 2 - x1 + x,
		                             h - 2,
		                             valueEdColor,
		                             valueString);
	}
	else {
		snprintf(valueString, sizeof(valueString), "%3d", values[currentValue]);
		mainFont->printStringToImage(drawImage,
		                             x1,
		                             y + fontSize - 1,
		                             w - 2 - x1 + x,
		                             h - 2,
		                             valueColor,
		                             valueString);
	}
}

void CpuSpeedConfigItem::enterEditStatus() {
	if (focus == false)
		return;

	editing = true;

	while (true) {
		u32 key = ctrl_wait(50000);

		if (key & ppu_controls_cancel()) {
			newValue = currentValue;
			break;
		}
		else if (key & ppu_controls_confirm()) {
			Config* config = Config::getInstance();
			config->setIntegerValue("config/cpu/speed", values[newValue]);
			cpu_clock_set_cpu_speed(values[newValue]);
			currentValue = newValue;
			break;
		}
		else if ((key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP)) {
			newValue = (newValue > 0 ? (newValue - 1) : 0);
		}
		else if ((key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN)) {
			newValue = (newValue < ValueCount - 1 ? (newValue + 1) : ValueCount - 1);
		}

		dialog->paint();
	}

	editing = false;
}

/********************************************************************************
 *                      AutoCpuClock                                             *
 ********************************************************************************/
class AutoCpuClockConfigItem : public ConfigItem {
private:
	const char* label;
	bool currentValue, newValue;
public:
	AutoCpuClockConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h);
	virtual void enterEditStatus();
};

AutoCpuClockConfigItem::AutoCpuClockConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.auto_cpu_clock", "Auto CPU Clock");

	Config* config = Config::getInstance();
	currentValue = config->getBooleanValue("config/cpu/auto_clock", true);
	newValue = currentValue;
}

void AutoCpuClockConfigItem::paint(int x, int y, int w, int h) {
	if (focus) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x + 1, y + fontSize - 1, w - 2, h - 2, labelHlColor, label);
	}
	else {
		mainFont->printStringToImage(drawImage, x + 1, y + fontSize - 1, w - 2, h - 2, labelColor, label);
	}

	int x1 = x + 1 + textWidth(label) + fontSize / 2;

	char valueString[64];
	memset(valueString, 0, sizeof(valueString));

	if (editing) {
		if (newValue)
			snprintf(valueString, sizeof(valueString), "%s", tr("config.on", "On"));
		else
			snprintf(valueString, sizeof(valueString), "%s", tr("config.off", "Off"));

		mainFont->printStringToImage(drawImage, x1, y + fontSize - 1, w - 2 - x1 + x, h - 2, valueEdColor, valueString);
	}
	else {
		if (currentValue)
			snprintf(valueString, sizeof(valueString), "%s", tr("config.on", "On"));
		else
			snprintf(valueString, sizeof(valueString), "%s", tr("config.off", "Off"));

		mainFont->printStringToImage(drawImage, x1, y + fontSize - 1, w - 2 - x1 + x, h - 2, valueColor, valueString);
	}
}

void AutoCpuClockConfigItem::enterEditStatus() {
	if (focus == false)
		return;

	editing = true;

	while (true) {
		u32 key = ctrl_wait(50000);

		if (key & ppu_controls_cancel()) {
			newValue = currentValue;
			break;
		}
		else if (key & ppu_controls_confirm()) {
			Config* config = Config::getInstance();
			config->setBooleanValue("config/cpu/auto_clock", newValue);
			cpu_clock_set_auto_enabled(newValue ? 1 : 0);
			currentValue = newValue;
			break;
		}
		else if ((key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP)) {
			newValue = false;
		}
		else if ((key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN)) {
			newValue = true;
		}

		dialog->paint();
	}

	editing = false;
}

/********************************************************************************
 *                      SubCharset                                              *
 ********************************************************************************/
class SubCharsetConfigItem : public ConfigItem {
private:
	int count;
	const char* label; 
	int currentValue, newValue;
public:
	SubCharsetConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

 /********************************************************************************
 *                      SubPreferredLanguage                                     *
 ********************************************************************************/
class SubPreferredLanguageConfigItem : public ConfigItem {
private:
	typedef struct {
		const char *name;
		const char *code;
	} LanguageValue;

	static const LanguageValue values[];

	int count;
	const char* label;
	int currentValue, newValue;

	int findLanguageIndex(const char *code);
public:
	SubPreferredLanguageConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h);
	virtual void enterEditStatus();
};

const SubPreferredLanguageConfigItem::LanguageValue
SubPreferredLanguageConfigItem::values[] = {
	{"English", "en"},
	{"Spanish", "es"},
	{"French", "fr"},
	{"German", "de"},
	{"Italian", "it"},
	{"Portuguese", "pt"},
	{"Polish", "pl"},
	{"Turkish", "tr"},
	{"Swedish", "sv"},
	{"Danish", "da"},
	{"Finnish", "fi"},
	{"Dutch", "nl"},
	{"Norwegian", "no"},
	{"Russian", "ru"},
	{"Greek", "el"},
	{"Hebrew", "he"},
	{"Arabic", "ar"},
	{"Romanian", "ro"},
	{"Indonesian", "id"},
	{"Vietnamese", "vi"},
	{"Thai", "th"},
	{"Korean", "ko"},
	{"Chinese", "zh"},
	{"Japanese", "ja"},
	{"Hindi", "hi"},
	{"Bengali", "bn"},
	{"Urdu", "ur"},
	{0, 0}
};

int SubPreferredLanguageConfigItem::findLanguageIndex(const char *code)
{
	int i;

	if (code == 0 || code[0] == '\0')
		return 0;

	for (i = 0; i < count; i++) {
		if (stricmp(values[i].code, code) == 0)
			return i;
	}

	/*
	 * Accept common ISO-639-2 codes in config.xml too.
	 */
	if (stricmp(code, "eng") == 0)
		return findLanguageIndex("en");
	if (stricmp(code, "spa") == 0)
		return findLanguageIndex("es");
	if (stricmp(code, "fre") == 0 || stricmp(code, "fra") == 0)
		return findLanguageIndex("fr");
	if (stricmp(code, "ger") == 0 || stricmp(code, "deu") == 0)
		return findLanguageIndex("de");
	if (stricmp(code, "ita") == 0)
		return findLanguageIndex("it");
	if (stricmp(code, "por") == 0)
		return findLanguageIndex("pt");
	if (stricmp(code, "pol") == 0)
		return findLanguageIndex("pl");
	if (stricmp(code, "tur") == 0)
		return findLanguageIndex("tr");
	if (stricmp(code, "swe") == 0)
		return findLanguageIndex("sv");
	if (stricmp(code, "dan") == 0)
		return findLanguageIndex("da");
	if (stricmp(code, "fin") == 0)
		return findLanguageIndex("fi");
	if (stricmp(code, "dut") == 0 || stricmp(code, "nld") == 0)
		return findLanguageIndex("nl");
	if (stricmp(code, "nor") == 0)
		return findLanguageIndex("no");
	if (stricmp(code, "rus") == 0)
		return findLanguageIndex("ru");
	if (stricmp(code, "gre") == 0 || stricmp(code, "ell") == 0)
		return findLanguageIndex("el");
	if (stricmp(code, "heb") == 0)
		return findLanguageIndex("he");
	if (stricmp(code, "ara") == 0)
		return findLanguageIndex("ar");
	if (stricmp(code, "rum") == 0 || stricmp(code, "ron") == 0)
		return findLanguageIndex("ro");
	if (stricmp(code, "ind") == 0)
		return findLanguageIndex("id");
	if (stricmp(code, "vie") == 0)
		return findLanguageIndex("vi");
	if (stricmp(code, "tha") == 0)
		return findLanguageIndex("th");
	if (stricmp(code, "kor") == 0)
		return findLanguageIndex("ko");
	if (stricmp(code, "chi") == 0 || stricmp(code, "zho") == 0)
		return findLanguageIndex("zh");
	if (stricmp(code, "jpn") == 0)
		return findLanguageIndex("ja");
	if (stricmp(code, "hin") == 0)
		return findLanguageIndex("hi");
	if (stricmp(code, "ben") == 0)
		return findLanguageIndex("bn");
	if (stricmp(code, "urd") == 0)
		return findLanguageIndex("ur");

	return 0;
}

SubPreferredLanguageConfigItem::SubPreferredLanguageConfigItem(ConfigDialog* dialog,
                                                               Image* drawImage)
	: ConfigItem(dialog, drawImage)
{
	int i;

	label = tr("config.preferred_subtitle_language", "Subtitle Language");

	count = 0;
	while (values[count].name != 0)
		count++;

	Config* config = Config::getInstance();
	const char* configValue =
		config->getStringValue("config/subtitles/preferred_language", "en");

	currentValue = findLanguageIndex(configValue);
	newValue = currentValue;

	/*
	 * Ensure old config.xml files get a real default value once the user
	 * opens the config dialog.
	 */
	config->setStringValue("config/subtitles/preferred_language",
	                       values[currentValue].code);

	subtitle_preferences_set_preferred_language(values[currentValue].code);

	(void)i;
}

void SubPreferredLanguageConfigItem::paint(int x, int y, int w, int h)
{
	if (focus) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage,
		                             x + 1,
		                             y + fontSize - 1,
		                             w - 2,
		                             h - 2,
		                             labelHlColor,
		                             label);
	}
	else {
		mainFont->printStringToImage(drawImage,
		                             x + 1,
		                             y + fontSize - 1,
		                             w - 2,
		                             h - 2,
		                             labelColor,
		                             label);
	}

	int x1 = x + 1 + textWidth(label) + fontSize / 2;

	if (editing) {
		mainFont->printStringToImage(drawImage,
		                             x1,
		                             y + fontSize - 1,
		                             w - 2 - x1 + x,
		                             h - 2,
		                             valueEdColor,
		                             dialog->languageName(values[newValue].code, values[newValue].name));
	}
	else {
		mainFont->printStringToImage(drawImage,
		                             x1,
		                             y + fontSize - 1,
		                             w - 2 - x1 + x,
		                             h - 2,
		                             valueColor,
		                             dialog->languageName(values[currentValue].code, values[currentValue].name));
	}
}

void SubPreferredLanguageConfigItem::enterEditStatus()
{
	if (focus == false)
		return;

	editing = true;

	while (true) {
		u32 key = ctrl_wait(50000);

		if (key & ppu_controls_cancel()) {
			newValue = currentValue;
			break;
		}
		else if (key & ppu_controls_confirm()) {
			Config* config = Config::getInstance();

			config->setStringValue("config/subtitles/preferred_language",
			                       values[newValue].code);

			subtitle_preferences_set_preferred_language(values[newValue].code);

			currentValue = newValue;
			break;
		}
		else if ((key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP)) {
			newValue = (newValue > 0 ? (newValue - 1) : 0);
		}
		else if ((key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN)) {
			newValue = (newValue < (count - 1) ? (newValue + 1) : (count - 1));
		}

		dialog->paint();
	}

	editing = false;
}

SubCharsetConfigItem::SubCharsetConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.subtitle_charset", "Subtitle Encoding");

	count = miniConvGetConvCount();

	Config* config = Config::getInstance();
	const char* configValue = config->getStringValue("config/subtitles/charset/value", "UTF-8");
	int i;
	for( i = 0; i< count; i++)
		if ( stricmp(miniConvGetConvCharset(i), configValue) == 0 ) {
			currentValue = i;
			break;
		}
	if (i == count)
		currentValue = 0;
	newValue = currentValue;
};

void SubCharsetConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	if ( editing )
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueEdColor, miniConvGetConvCharset(newValue));
	else
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, miniConvGetConvCharset(currentValue));
};

void SubCharsetConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			newValue = currentValue;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			Config* config = Config::getInstance();
			config->setStringValue("config/subtitles/charset/value", miniConvGetConvCharset(newValue));
			miniConvSetDefaultSubtitleConv( miniConvGetConvCharset(newValue) );

			currentValue = newValue;
			break;
		}
		else if ( (key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP) ) {
			newValue = (newValue > 0 ? (newValue-1) : 0);
		}
		else if ( (key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN) ) {
			newValue = (newValue < (count-1) ? (newValue+1) : (count-1));
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      SubFontSize                                             *
 ********************************************************************************/
class SubFontSizeConfigItem : public ConfigItem {
private:
	const char* label; 
	int currentValue, newValue;
public:
	SubFontSizeConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

SubFontSizeConfigItem::SubFontSizeConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.subtitle_font_size", "Subtitle Size");

	Config* config = Config::getInstance();
	currentValue = config->getIntegerValue("config/subtitles/font/size", 16);
	currentValue = (currentValue <= 0 || currentValue > 48)?16:currentValue;
	newValue = currentValue;
};

void SubFontSizeConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	char valueString[64];
	memset(valueString, 0, 64);
	if ( editing ) {
		snprintf(valueString, sizeof(valueString), "%d", newValue);
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueEdColor, valueString);
	}
	else {
		snprintf(valueString, sizeof(valueString), "%d", currentValue);
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, valueString);
	}
};

void SubFontSizeConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			newValue = currentValue;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			Config* config = Config::getInstance();
			config->setIntegerValue("config/subtitles/font/size", newValue);
			gu_font_pixelsize_set(newValue);
			currentValue = newValue;
			break;
		}
		else if ( (key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP) ) {
			newValue = (newValue > 1 ? (newValue-1) : 1);
		}
		else if ( (key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN) ) {
			newValue = (newValue < 48 ? (newValue+1) : 48);
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      SubEmbolden                                             *
 ********************************************************************************/
class SubEmboldenConfigItem : public ConfigItem {
private:
	const char* label; 
	bool currentValue, newValue;
public:
	SubEmboldenConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

SubEmboldenConfigItem::SubEmboldenConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.subtitle_embolden", "Bold Subtitles");

	Config* config = Config::getInstance();
	currentValue = config->getBooleanValue("config/subtitles/font/embolden", false);
	newValue = currentValue;
};

void SubEmboldenConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	char valueString[64];
	memset(valueString, 0, 64);
	if ( editing ) {
		if ( newValue )
			snprintf(valueString, sizeof(valueString), "%s", tr("config.yes", "Yes"));
		else
			snprintf(valueString, sizeof(valueString), "%s", tr("config.no", "No"));
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueEdColor, valueString);
	}
	else {
		if ( currentValue )
			snprintf(valueString, sizeof(valueString), "%s", tr("config.yes", "Yes"));
		else
			snprintf(valueString, sizeof(valueString), "%s", tr("config.no", "No"));
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, valueString);
	}
};

void SubEmboldenConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			newValue = currentValue;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			Config* config = Config::getInstance();
			config->setBooleanValue("config/subtitles/font/embolden", newValue);
			if ( newValue )
				gu_font_embolden_enable(1);
			else
				gu_font_embolden_enable(0);
			currentValue = newValue;
			break;
		}
		else if ( (key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP) ) {
			newValue = true;
		}
		else if ( (key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN) ) {
			newValue = false;
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      SubAlign                                                *
 ********************************************************************************/
class SubAlignConfigItem : public ConfigItem {
private:
	const char* values[2]; 
	const char* label; 
	int currentValue, newValue;
public:
	SubAlignConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

SubAlignConfigItem::SubAlignConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.subtitle_position", "Subtitle Position");
	values[0] = tr("config.subtitle_position_top", "Top");
	values[1] = tr("config.subtitle_position_bottom", "Bottom");
	
	Config* config = Config::getInstance();
	const char* configValue = config->getStringValue("config/subtitles/position/align", "bottom");
	if ( stricmp("top", configValue) == 0 )
		currentValue = 0;
	else
		currentValue = 1;
	newValue = currentValue;
};

void SubAlignConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	if ( editing )
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueEdColor, values[newValue]);
	else
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, values[currentValue]);
	
};

void SubAlignConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			newValue = currentValue;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			Config* config = Config::getInstance();
			config->setStringValue("config/subtitles/position/align", ((newValue==0)?"TOP":"BOTTOM"));
			gu_font_align_set( newValue );
			currentValue = newValue;
			break;
		}
		else if ( (key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP) ) {
			newValue = (newValue > 0 ? (newValue-1) : 0);
		}
		else if ( (key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN) ) {
			newValue = (newValue < 1 ? (newValue+1) : 1);
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      SubDistance                                             *
 ********************************************************************************/
class SubDistanceConfigItem : public ConfigItem {
private:
	const char* label; 
	int currentValue, newValue;
public:
	SubDistanceConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

SubDistanceConfigItem::SubDistanceConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.subtitle_distance", "Subtitle Margin");

	Config* config = Config::getInstance();
	currentValue = config->getIntegerValue("config/subtitles/position/distance", 16);
	currentValue = (currentValue <= 0 || currentValue > 48)?16:currentValue;
	newValue = currentValue;
};

void SubDistanceConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	char valueString[64];
	memset(valueString, 0, 64);
	if ( editing ) {
		snprintf(valueString, sizeof(valueString), "%d", newValue);
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueEdColor, valueString);
	}
	else {
		snprintf(valueString, sizeof(valueString), "%d", currentValue);
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, valueString);
	}
	
};

void SubDistanceConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			newValue = currentValue;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			Config* config = Config::getInstance();
			config->setIntegerValue("config/subtitles/position/distance", newValue);
			gu_font_distance_set(newValue);
			currentValue = newValue;
			break;
		}
		else if ( (key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP) ) {
			newValue = (newValue > 1 ? (newValue-1) : 1);
		}
		else if ( (key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN) ) {
			newValue = (newValue < 48 ? (newValue+1) : 48);
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      FilesystemCharset                                       *
 ********************************************************************************/
class FilesystemCharsetConfigItem : public ConfigItem {
private:
	int count;
	const char* label; 
	int currentValue, newValue;
public:
	FilesystemCharsetConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

FilesystemCharsetConfigItem::FilesystemCharsetConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.filesystem_charset", "File Encoding");
	count = miniConvGetConvCount();
	
	Config* config = Config::getInstance();
	const char* configValue = config->getStringValue("config/filesystem/charset/value", "UTF-8");
	int i;
	for( i = 0; i< count; i++)
		if ( stricmp(miniConvGetConvCharset(i), configValue) == 0 ) {
			currentValue = i;
			break;
		}
	if (i == count)
		currentValue = 0;
	newValue = currentValue;
};

void FilesystemCharsetConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	if ( editing )
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueEdColor, miniConvGetConvCharset(newValue));
	else
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, miniConvGetConvCharset(currentValue));
};

void FilesystemCharsetConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			newValue = currentValue;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			Config* config = Config::getInstance();
			config->setStringValue("config/filesystem/charset/value", miniConvGetConvCharset(newValue));
			miniConvSetFileSystemConv( miniConvGetConvCharset(newValue) );
			currentValue = newValue;
			break;
		}
		else if ( (key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP) ) {
			newValue = (newValue > 0 ? (newValue-1) : 0);
		}
		else if ( (key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN) ) {
			newValue = (newValue < (count-1) ? (newValue+1) : (count-1));
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      PlayMode                                                *
 ********************************************************************************/
class PlayModeConfigItem : public ConfigItem {
private:
	const char* values[3]; 
	const char* label; 
	int currentValue, newValue;
public:
	PlayModeConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

PlayModeConfigItem::PlayModeConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.play_mode", "Play Mode");
	values[0] = tr("config.play_single", "Single");
	values[1] = tr("config.play_group", "Folder");
	values[2] = tr("config.play_all", "All");
	
	Config* config = Config::getInstance();
	const char* configValue = config->getStringValue("config/player/play_mode", "group");
	if ( stricmp("SINGLE", configValue) == 0 ) 
		currentValue = 0;
	else if ( stricmp("ALL", configValue) == 0 ) 
		currentValue = 2;
	else
		currentValue = 1;
	newValue = currentValue;
};

void PlayModeConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	if ( editing )
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueEdColor, values[newValue]);
	else
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, values[currentValue]);
	
};

void PlayModeConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			newValue = currentValue;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			Config* config = Config::getInstance();
			if ( newValue == 0 )
				config->setStringValue("config/player/play_mode", "SINGLE");
			else if( newValue == 1 )
				config->setStringValue("config/player/play_mode", "GROUP");
			else
				config->setStringValue("config/player/play_mode", "ALL");
			currentValue = newValue;
			break;
		}
		else if ( (key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP) ) {
			newValue = (newValue > 0 ? (newValue-1) : 0);
		}
		else if ( (key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN) ) {
			newValue = (newValue < 2 ? (newValue+1) : 2);
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      TVAspectRatio                                                *
 ********************************************************************************/
class TVAspectRatioConfigItem : public ConfigItem {
private:
	const char* values[2]; 
	const char* label; 
	int currentValue, newValue;
public:
	TVAspectRatioConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

TVAspectRatioConfigItem::TVAspectRatioConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.tv_aspect_ratio", "TV Aspect Ratio");
	values[0] = "16:9";
	values[1] = "4:3";
		
	Config* config = Config::getInstance();
	const char* configValue = config->getStringValue("config/tvout/aspect_ratio", "16:9");
	int i;
	for( i = 0; i< 2; i++)
		if ( stricmp(values[i], configValue) == 0 ) {
			currentValue = i;
			break;
		}
	if (i == 2)
		currentValue = 0;
	newValue = currentValue;
};

void TVAspectRatioConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	if ( editing )
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueEdColor, values[newValue]);
	else
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, values[currentValue]);
	
};

void TVAspectRatioConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			newValue = currentValue;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			if ( newValue != currentValue) {
				Config* config = Config::getInstance();
				config->setStringValue("config/tvout/aspect_ratio", values[newValue]);
				VideoMode::setTVAspectRatio(newValue);
				setGraphicsTVAspectRatio(newValue);
				setGraphicsTVOutScreen();
			}
			currentValue = newValue = VideoMode::getTVAspectRatio();
			break;
		}
		else if ( (key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP) ) {
			newValue = (newValue > 0 ? (newValue-1) : 0);
		}
		else if ( (key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN) ) {
			newValue = (newValue < 1 ? (newValue+1) : 1);
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      TV OverScan                                              *
 ********************************************************************************/
class TVOverScanConfigItem : public ConfigItem {
private:
	const char* label; 
	char currentValue[16+1];
	u8 newValue[4];
	int valueIndex;
public:
	TVOverScanConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

TVOverScanConfigItem::TVOverScanConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.tv_overscan", "TV Overscan (L.T.R.B)");

	Config* config = Config::getInstance();
	snprintf(currentValue, sizeof(currentValue), "%s",
	         config->getStringValue("config/tvout/over_scan", "8.0.8.0"));
	int a0, a1, a2, a3;
	a0 = a1 = a2 = a3 = 0;
	sscanf(currentValue, "%9d.%9d.%9d.%9d", &a0, &a1, &a2, &a3);
	newValue[0] = (u8)a0;
	newValue[1] = (u8)a1;
	newValue[2] = (u8)a2;
	newValue[3] = (u8)a3;
	snprintf(currentValue, sizeof(currentValue), "%d.%d.%d.%d", newValue[0], newValue[1], newValue[2], newValue[3]);
};

void TVOverScanConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	if ( editing ) {
		char s0[4], s1[4], s2[4], s3[4];
		memset(s0, 0, 4);
		snprintf(s0, sizeof(s0), "%d", newValue[0]);
		memset(s1, 0, 4);
		snprintf(s1, sizeof(s1), "%d", newValue[1]);
		memset(s2, 0, 4);
		snprintf(s2, sizeof(s2), "%d", newValue[2]);
		memset(s3, 0, 4);
		snprintf(s3, sizeof(s3), "%d", newValue[3]);
		int sx0, sx1, sx2, sx3;
		sx0 = x1;
		sx1 = sx0 + textWidth(s0) + fontSize / 2;
		sx2 = sx1 + textWidth(s1) + fontSize / 2;
		sx3 = sx2 + textWidth(s2) + fontSize / 2;
		if ( valueIndex == 0 )
			mainFont->printStringToImage(drawImage, sx0, y+fontSize-1, w-2-sx0+x, h-2, valueEdColor, s0);
		else
			mainFont->printStringToImage(drawImage, sx0, y+fontSize-1, w-2-sx0+x, h-2, valueColor, s0);
		
		mainFont->printStringToImage(drawImage, sx1-fontSize / 2, y+fontSize-1, fontSize / 2, h-2, valueColor, ".");
		
		if ( valueIndex == 1 )
			mainFont->printStringToImage(drawImage, sx1, y+fontSize-1, w-2-sx1+x, h-2, valueEdColor, s1);
		else
			mainFont->printStringToImage(drawImage, sx1, y+fontSize-1, w-2-sx1+x, h-2, valueColor, s1);
		
		mainFont->printStringToImage(drawImage, sx2-fontSize / 2, y+fontSize-1, fontSize / 2, h-2, valueColor, ".");
		
		if ( valueIndex == 2 )
			mainFont->printStringToImage(drawImage, sx2, y+fontSize-1, w-2-sx2+x, h-2, valueEdColor, s2);
		else
			mainFont->printStringToImage(drawImage, sx2, y+fontSize-1, w-2-sx2+x, h-2, valueColor, s2);
		
		mainFont->printStringToImage(drawImage, sx3-fontSize / 2, y+fontSize-1, fontSize / 2, h-2, valueColor, ".");
		
		if ( valueIndex == 3 )
			mainFont->printStringToImage(drawImage, sx3, y+fontSize-1, w-2-sx3+x, h-2, valueEdColor, s3);
		else
			mainFont->printStringToImage(drawImage, sx3, y+fontSize-1, w-2-sx3+x, h-2, valueColor, s3);
	}
	else {
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, currentValue);
	}
};

void TVOverScanConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	valueIndex = 0;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			int a0, a1, a2, a3;
			a0 = a1 = a2 = a3 = 0;
			sscanf(currentValue, "%9d.%9d.%9d.%9d", &a0, &a1, &a2, &a3);
			newValue[0] = (u8)a0;
			newValue[1] = (u8)a1;
			newValue[2] = (u8)a2;
			newValue[3] = (u8)a3;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			char valueString[16+1];
			memset(valueString, 0, 16+1);
			snprintf(valueString, sizeof(valueString), "%d.%d.%d.%d", newValue[0], newValue[1], newValue[2], newValue[3]);
			Config* config = Config::getInstance();
			config->setStringValue("config/tvout/over_scan", valueString);
			snprintf(currentValue, sizeof(currentValue), "%s", valueString);
			VideoMode::setTVOverScan(newValue[0], newValue[1], newValue[2], newValue[3]);
			setGraphicsTVOverScan(newValue[0], newValue[1], newValue[2], newValue[3]);
			setGraphicsTVOutScreen();
			break;
		}
		else if ( key & PSP_CTRL_LEFT) {
			if ( valueIndex > 0 ) valueIndex--;
		}
		else if ( key & PSP_CTRL_RIGHT) {
			if ( valueIndex < 3 ) valueIndex++;
		}
		else if ( key & PSP_CTRL_UP ) {
			if ( newValue[valueIndex] > 0 ) newValue[valueIndex]--;
		}
		else if ( key & PSP_CTRL_DOWN ) {
			if ( newValue[valueIndex] < 50 ) newValue[valueIndex]++;
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      VideoMode                                                *
 ********************************************************************************/
class VideoModeConfigItem : public ConfigItem {
private:
	const char* values[4]; 
	const char* label; 
	int currentValue, newValue;
public:
	VideoModeConfigItem(ConfigDialog* dialog, Image* drawImage);
	virtual void paint(int x, int y, int w, int h) ;
	virtual void enterEditStatus();
};

VideoModeConfigItem::VideoModeConfigItem(ConfigDialog* dialog, Image* drawImage) : ConfigItem(dialog, drawImage) {
	label = tr("config.video_mode", "Video Output");
	values[0] = tr("config.video_psp_lcd", "PSP LCD");
	values[1] = tr("config.video_composite", "Composite");
	values[2] = tr("config.video_component_interlace", "Component Interlaced");
	values[3] = tr("config.video_component_progressive", "Component Progressive");
	currentValue = 0;
	currentValue = VideoMode::getVideoMode();
	newValue = currentValue;
};

void VideoModeConfigItem::paint(int x, int y, int w, int h) {
	if ( focus ) {
		fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelHlColor, label );  
	}
	else
		mainFont->printStringToImage(drawImage, x+1, y+fontSize-1, w-2, h-2, labelColor, label );
	
	int x1 = x + 1 + textWidth(label) + fontSize / 2;
	
	if ( editing )
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueEdColor, values[newValue]);
	else
		mainFont->printStringToImage(drawImage, x1, y+fontSize-1, w-2-x1+x, h-2, valueColor, values[currentValue]);
	
};

void VideoModeConfigItem::enterEditStatus() {
	if ( focus == false) 
		return;
	editing = true;
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & ppu_controls_cancel() ) {
			newValue = currentValue;
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			if ( newValue != currentValue) {
				if ( VideoMode::setVideoMode(newValue) == newValue ) {
					setGraphicsVideoMode(VideoMode::getVideoMode());
				}
			}
			currentValue = newValue = VideoMode::getVideoMode();
			break;
		}
		else if ( (key & PSP_CTRL_LEFT) || (key & PSP_CTRL_UP) ) {
			newValue = (newValue > 0 ? (newValue-1) : 0);
		}
		else if ( (key & PSP_CTRL_RIGHT) || (key & PSP_CTRL_DOWN) ) {
			newValue = (newValue < 3 ? (newValue+1) : 3);
		}
		dialog->paint();
	};
	editing = false;
};

/********************************************************************************
 *                      VideoPipeline Boolean Options                           *
 ********************************************************************************/
class VideoPipelineConfigItem : public ConfigItem {
private:
    const char *label;
    const char *path;
    bool currentValue;
    bool newValue;
    bool reloadPipeline;
public:
    VideoPipelineConfigItem(ConfigDialog *dialog, Image *drawImage,
                            const char *labelText, const char *configPath, bool reload = true)
        : ConfigItem(dialog, drawImage), label(labelText), path(configPath),
          currentValue(false), newValue(false), reloadPipeline(reload) {
        Config *config = Config::getInstance();
        currentValue = config ? config->getBooleanValue(path, false) : false;
        newValue = currentValue;
    }

    virtual void paint(int x, int y, int w, int h) {
        Color activeLabel = focus ? labelHlColor : labelColor;
        Color activeValue = editing ? valueEdColor : valueColor;
        bool value = editing ? newValue : currentValue;
        if (focus)
            fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
        mainFont->printStringToImage(drawImage, x + 1, y + fontSize - 1,
                                     w - 2, h - 2, activeLabel, label);
        int x1 = x + 1 + textWidth(label) + fontSize / 2;
        mainFont->printStringToImage(drawImage, x1, y + fontSize - 1,
                                     w - 2 - x1 + x, h - 2, activeValue,
                                     value ? tr("config.on", "On")
                                           : tr("config.off", "Off"));
    }

    virtual void enterEditStatus() {
        if (!focus)
            return;
        editing = true;
        newValue = currentValue;
        while (true) {
            u32 key = ctrl_wait(50000);
            if (key & ppu_controls_cancel()) {
                newValue = currentValue;
                break;
            }
            if (key & ppu_controls_confirm()) {
                Config *config = Config::getInstance();
                if (config && config->setBooleanValue(path, newValue)) {
                    currentValue = newValue;
                    if (reloadPipeline) ppa_video_pipeline_reload_config();
                }
                break;
            }
            if (key & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT |
                       PSP_CTRL_UP | PSP_CTRL_DOWN))
                newValue = !newValue;
            dialog->paint();
        }
        editing = false;
    }
};

class ControlsTypeConfigItem : public ConfigItem {
    int currentValue, newValue;
    const char *label;
public:
    ControlsTypeConfigItem(ConfigDialog *dialog, Image *image)
        : ConfigItem(dialog, image), currentValue(ppu_controls_get_type()),
          newValue(currentValue), label(tr("config.controls_type", "Controls Type")) {}
    virtual void paint(int x, int y, int w, int h) {
        if (focus) fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
        mainFont->printStringToImage(drawImage, x + 1, y + fontSize - 1,
            w - 2, h - 2, focus ? labelHlColor : labelColor, label);
        int x1 = x + 1 + textWidth(label) + fontSize / 2;
        int value = editing ? newValue : currentValue;
        mainFont->printStringToImage(drawImage, x1, y + fontSize - 1,
            w - 2 - x1 + x, h - 2, editing ? valueEdColor : valueColor,
            value == PPU_CONTROLS_ASIA ? tr("config.controls_asia", "Asia") :
                                       tr("config.controls_international", "International"));
    }
    virtual void enterEditStatus() {
        if (!focus) return;
        editing = true;
        newValue = currentValue;
        while (true) {
            u32 key = ctrl_wait(50000);
            if (key & ppu_controls_cancel()) break;
            if (key & ppu_controls_confirm()) {
                Config *config = Config::getInstance();
                if (config && config->setStringValue("config/player/controls_type",
                        newValue == PPU_CONTROLS_ASIA ? "Asia" : "International")) {
                    currentValue = newValue;
                    ppu_controls_set_type(currentValue);
                }
                break;
            }
            if (key & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_UP | PSP_CTRL_DOWN))
                newValue = newValue == PPU_CONTROLS_ASIA ? PPU_CONTROLS_INTERNATIONAL : PPU_CONTROLS_ASIA;
            dialog->paint();
        }
        editing = false;
    }
};

/* The timer is session policy, not a video-pipeline setting. */
class SleepTimerConfigItem : public ConfigItem {
    int currentValue, newValue;
    const char *label;
public:
    SleepTimerConfigItem(ConfigDialog *dialog, Image *drawImage)
        : ConfigItem(dialog, drawImage), currentValue(0), newValue(0),
          label(tr("config.sleep_timer", "Sleep Timer")) {
        Config *c = Config::getInstance();
        currentValue = c ? c->getIntegerValue("config/player/sleep_timer_minutes", 0) : 0;
        if (currentValue < 0) currentValue = 0;
        if (currentValue > 720) currentValue = 720;
        currentValue -= currentValue % 15;
        newValue = currentValue;
    }
    virtual void paint(int x, int y, int w, int h) {
        char value[80];
        int minutes = editing ? newValue : currentValue;
        if (minutes == 0) snprintf(value, sizeof(value), "%s", tr("config.off", "Off"));
        else snprintf(value, sizeof(value), "%d %s", minutes, tr("config.minutes", "min"));
        if (focus) fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
        mainFont->printStringToImage(drawImage, x + 1, y + fontSize - 1,
            w - 2, h - 2, focus ? labelHlColor : labelColor, label);
        int x1 = x + 1 + textWidth(label) + fontSize / 2;
        mainFont->printStringToImage(drawImage, x1, y + fontSize - 1,
            w - 2 - x1 + x, h - 2, editing ? valueEdColor : valueColor, value);
    }
    virtual void enterEditStatus() {
        if (!focus) return;
        editing = true; newValue = currentValue;
        while (true) {
            u32 key = ctrl_wait(50000);
            if (key & ppu_controls_cancel()) break;
            if (key & ppu_controls_confirm()) {
                Config *c = Config::getInstance();
                if (c && c->setIntegerValue("config/player/sleep_timer_minutes", newValue))
                    currentValue = newValue;
                break;
            }
            if (key & (PSP_CTRL_RIGHT | PSP_CTRL_UP))
                newValue = newValue >= 720 ? 0 : newValue + 15;
            else if (key & (PSP_CTRL_LEFT | PSP_CTRL_DOWN))
                newValue = newValue <= 0 ? 720 : newValue - 15;
            dialog->paint();
        }
        editing = false;
    }
};

/********************************************************************************
 *                      Interface language                                      *
 ********************************************************************************/
class UiLanguageConfigItem : public ConfigItem {
private:
	const char* label;
	int currentValue;
	int newValue;

	int findLanguage(const char* id) const {
		int i;
		for (i = 0; i < UiI18n::languageCount(); ++i) {
			if (id != NULL && stricmp(UiI18n::languageIdAt(i), id) == 0)
				return i;
		}
		return 0;
	}
public:
	UiLanguageConfigItem(ConfigDialog* dialog, Image* drawImage)
		: ConfigItem(dialog, drawImage), label(tr("config.interface_language", "Interface Language")),
		  currentValue(0), newValue(0) {
		Config* config = Config::getInstance();
		currentValue = findLanguage(config
			? config->getStringValue("config/windows/ui/language", "auto")
			: "auto");
		newValue = currentValue;
	}

	virtual void paint(int x, int y, int w, int h) {
		Color activeLabel = focus ? labelHlColor : labelColor;
		Color activeValue = editing ? valueEdColor : valueColor;
		int value = editing ? newValue : currentValue;
		int x1;
		if (focus)
			fillImageRect(drawImage, (alpha << 24) | bgHlColor, x, y, w, h);
		mainFont->printStringToImage(drawImage, x + 1, y + fontSize - 1,
		                             w - 2, h - 2, activeLabel, label);
		x1 = x + 1 + textWidth(label) + fontSize / 2;
		mainFont->printStringToImage(drawImage, x1, y + fontSize - 1,
		                             w - 2 - x1 + x, h - 2, activeValue,
		                             dialog->languageName(UiI18n::languageIdAt(value),
		                                              UiI18n::languageNameAt(value)));
	}

	virtual void enterEditStatus() {
		if (!focus)
			return;
		editing = true;
		newValue = currentValue;
		while (true) {
			u32 key = ctrl_wait(50000);
			if (key & ppu_controls_cancel()) {
				newValue = currentValue;
				break;
			}
			if (key & ppu_controls_confirm()) {
				Config* config = Config::getInstance();
				if (config != NULL &&
				    config->setStringValue("config/windows/ui/language",
				                           UiI18n::languageIdAt(newValue)))
					currentValue = newValue;
				break;
			}
			if (key & (PSP_CTRL_LEFT | PSP_CTRL_UP)) {
				--newValue;
				if (newValue < 0)
					newValue = UiI18n::languageCount() - 1;
			}
			else if (key & (PSP_CTRL_RIGHT | PSP_CTRL_DOWN)) {
				++newValue;
				if (newValue >= UiI18n::languageCount())
					newValue = 0;
			}
			dialog->paint();
		}
		editing = false;
	}
};

#define CONFIG_DIALOG_X		60
#define CONFIG_DIALOG_Y		4	
#define CONFIG_DIALOG_W		360
#define CONFIG_DIALOG_H		264
#define CONFIG_DIALOG_R		6
#define CONFIG_DIALOG_ITEM_X    90
#define CONFIG_DIALOG_ITEM_Y	24
#define CONFIG_DIALOG_ITEM_W	300
#define CONFIG_DIALOG_ITEM_H	238

ConfigDialog::ConfigDialog(Image* mainWindow, Image* mainDrawImage, UiI18n* uiI18n) {
	this->uiI18n = uiI18n;
	mainFont = (uiI18n != NULL && uiI18n->isReady())
	         ? uiI18n->getFont() : NULL;
	if (mainFont == NULL && FtFontManager::getInstance() != NULL)
		mainFont = FtFontManager::getInstance()->getMainFont();
	fontSize = Config::getInstance() != NULL
	         ? Config::getInstance()->getIntegerValue("config/windows/font/size", 12)
	         : 12;
	if (fontSize < 8)
		fontSize = 8;
	if (fontSize > 14)
		fontSize = 14;
	if (mainFont != NULL)
		mainFont->setPixelSize(fontSize);

	itemBottom = itemTop = itemCurrent = itemCount = 0;
	itemCount = static_cast<int>(items.size());
	items.assign(static_cast<PConfigItem>(NULL));
	
//	screenSnapshot = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	this->mainWindow = mainWindow;
	this->mainDrawImage = mainDrawImage;
	drawImage = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
};

ConfigDialog::~ConfigDialog() {
//	freeImage(screenSnapshot);
	mainWindow = NULL;
	mainDrawImage = NULL;
	freeImage(drawImage);
	int i;
	for(i = 0; i < itemCount; i++) 
		if (items[i])
			delete items[i];
	
	mainFont = NULL;
	uiI18n = NULL;
};

const char* ConfigDialog::text(const char* id, const char* fallback) const {
	if (uiI18n != NULL && uiI18n->isReady())
		return uiI18n->getText(id, fallback);
	return fallback != NULL ? fallback : "";
}

const char* ConfigDialog::languageName(const char* id, const char* fallback) const {
	if (uiI18n != NULL && uiI18n->isReady())
		return uiI18n->getLanguageName(id, fallback);
	return fallback != NULL ? fallback : "";
}

FtFont* ConfigDialog::getFont() const {
	return mainFont;
}

int ConfigDialog::getFontSize() const {
	return fontSize;
}

bool ConfigDialog::init() {
//	if ( screenSnapshot == NULL )
//		return false;
	if (drawImage == NULL)
		return false;
		
//	makeScreenSnapshot(screenSnapshot);
	
	/* Keep Interface Language at the top of the popup so it is visible
	 * immediately, including on skins/font sizes that show fewer rows. */
	items[0] = new (std::nothrow) UiLanguageConfigItem(this, this->drawImage);
	items[1] = new (std::nothrow) ControlsTypeConfigItem(this, this->drawImage);
	items[2] = new (std::nothrow) CpuSpeedConfigItem(this, this->drawImage);
	items[3] = new (std::nothrow) SubCharsetConfigItem(this, this->drawImage);
	items[4] = new (std::nothrow) SubPreferredLanguageConfigItem(this, this->drawImage);
	items[5] = new (std::nothrow) SubFontSizeConfigItem(this, this->drawImage);
	items[6] = new (std::nothrow) SubEmboldenConfigItem(this, this->drawImage);
	items[7] = new (std::nothrow) SubAlignConfigItem(this, this->drawImage);
	items[8] = new (std::nothrow) SubDistanceConfigItem(this, this->drawImage);
	items[9] = new (std::nothrow) FilesystemCharsetConfigItem(this, this->drawImage);
	items[10] = new (std::nothrow) PlayModeConfigItem(this, this->drawImage);
	items[11] = new (std::nothrow) TVAspectRatioConfigItem(this, this->drawImage);
	items[12] = new (std::nothrow) TVOverScanConfigItem(this, this->drawImage);
	items[13] = new (std::nothrow) VideoModeConfigItem(this, this->drawImage);
	items[14] = new (std::nothrow) AutoCpuClockConfigItem(this, this->drawImage);
	items[15] = new (std::nothrow) VideoPipelineConfigItem(this, this->drawImage, text("config.extreme_battery_saver", "Battery Saver"), "config/player/extreme_battery_saver");
	items[16] = new (std::nothrow) VideoPipelineConfigItem(this, this->drawImage, text("config.optimize_psp_screen", "Fit PSP Screen"), "config/video_pipeline/optimize_psp_screen");
	items[17] = new (std::nothrow) SleepTimerConfigItem(this, this->drawImage);
	items[18] = new (std::nothrow) VideoPipelineConfigItem(this, this->drawImage,
	    text("config.playback_health", "Playback Health"), "config/player/playback_health", false);
	items[19] = new (std::nothrow) VideoPipelineConfigItem(this, this->drawImage,
	    text("config.audio_only", "Audio Only Mode"), "config/player/audio_only", false);
	for (int i = 0; i < itemCount; ++i)
		if (!items[i]) return false;
	
	if (mainFont == NULL)
		return false;
	mainFont->setPixelSize(fontSize);

	itemBottom = CONFIG_DIALOG_ITEM_H / ( fontSize + 2 );
	
	items[itemCurrent]->setFocus(true);
	
	Skin* skin = Skin::getInstance();
	alpha = skin->getAlphaValue("skin/config_dialog/dialog/alpha", PPA_CONFIGDLG_ALPHA);
	bgColor = skin->getColorValue("skin/config_dialog/dialog/background_color", PPA_CONFIGDLG_BG_COLOR);
	labelColor = skin->getColorValue("skin/config_dialog/dialog/label_color", PPA_CONFIGDLG_LABEL_COLOR);
	
	title = text("config.title", "Configuration");
	help = "";
	return true;
};

void ConfigDialog::execute() {
    enum ctrl_context previousContext = ctrl_set_context(CTRL_CONTEXT_MENU);
	/* The Square press that opens this modal can produce a queued repeat while
	 * the dialog allocates items and shapes multilingual labels.  Wait for
	 * release and start from an empty queue so Square reliably opens the menu
	 * instead of immediately acting as its close command. */
	ctrl_flush();
	while (ctrl_read_cont() & PSP_CTRL_SQUARE)
		sceKernelDelayThread(1000);
	ctrl_flush();
	paint();

	while( true ) {
		u32 key = ctrl_wait(50000);
		if (( key & PSP_CTRL_TRIANGLE ) || (key & PSP_CTRL_SQUARE)) {
			break;
		}
		else if ( key & ppu_controls_confirm() ) {
			items[itemCurrent]->enterEditStatus();
		}
		else if( (key & PSP_CTRL_UP) || (key & CTRL_BACK) ) {
			items[itemCurrent]->setFocus(false);
			if (itemCurrent != 0){
				itemCurrent--;
			}
			else {
				itemCurrent = itemCount - 1;
			}
			items[itemCurrent]->setFocus(true);
		}
		else if( (key & PSP_CTRL_DOWN) || (key & CTRL_FORWARD) ){
			items[itemCurrent]->setFocus(false);
			if (itemCurrent + 1 < itemCount) {
				itemCurrent++;
			}
			else 
				itemCurrent = 0;
			items[itemCurrent]->setFocus(true);
		}
		else if (key & PSP_CTRL_LTRIGGER ) {
			items[itemCurrent]->setFocus(false);
			itemCurrent = 0;
			items[itemCurrent]->setFocus(true);
		}
		else if (key & PSP_CTRL_RTRIGGER ) {
			items[itemCurrent]->setFocus(false);
			itemCurrent = itemCount - 1;
			items[itemCurrent]->setFocus(true);
		}
		paint();
	};
    ctrl_set_context(previousContext);
};

void ConfigDialog::paint() {
	
	clearImage(drawImage, 0);
	
//	fillImageRect(drawImage, (alpha << 24) | bgColor, CONFIG_DIALOG_X, CONFIG_DIALOG_Y, CONFIG_DIALOG_W, CONFIG_DIALOG_H);
	fillImageEllipse(drawImage, (alpha << 24) | bgColor, CONFIG_DIALOG_X, CONFIG_DIALOG_Y, CONFIG_DIALOG_W, CONFIG_DIALOG_H, CONFIG_DIALOG_R);
	
	int titleX = CONFIG_DIALOG_X + (CONFIG_DIALOG_W - mainFont->measureShapedString(title))/2;
	mainFont->printStringToImage(drawImage, titleX, CONFIG_DIALOG_Y+2+fontSize-1, CONFIG_DIALOG_W-titleX+CONFIG_DIALOG_X, fontSize+2, labelColor, title);
	
	int helpX = CONFIG_DIALOG_X + (CONFIG_DIALOG_W - mainFont->measureShapedString(help))/2;
	mainFont->printStringToImage(drawImage, helpX, CONFIG_DIALOG_Y+CONFIG_DIALOG_H-4-fontSize+fontSize-1, CONFIG_DIALOG_W-helpX+CONFIG_DIALOG_X, fontSize+2, labelColor, help);
	
	if ( itemCurrent < itemTop ) {
		itemTop = itemCurrent;
	}
	else if (itemCurrent - itemTop >= itemBottom ){
		itemTop = itemCurrent - itemBottom + 1;
	}
	
	int i;
	for(i=0;i<itemBottom;i++) {
		if( itemTop + i < itemCount) {
			items[itemTop + i]->paint(CONFIG_DIALOG_ITEM_X,
							CONFIG_DIALOG_ITEM_Y + i * (fontSize + 2),
							CONFIG_DIALOG_ITEM_W,
							fontSize + 2);
		}
	}
	
	guStart();	
	clearScreen();	
//	blitImageToScreen(0, 0, screenSnapshot->imageWidth, screenSnapshot->imageHeight, screenSnapshot, 0, 0);
	blitImageToScreen(0, 0, mainWindow->imageWidth, mainWindow->imageHeight, mainWindow, 0, 0);
	blitAlphaImageToScreen(0, 0, mainDrawImage->imageWidth, mainDrawImage->imageHeight, mainDrawImage, 0, 0);
	blitAlphaImageToScreen(0, 0, drawImage->imageWidth, drawImage->imageHeight, drawImage, 0, 0);
	flipScreen();
};

