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
#include "common/libi18n.h"

#include "skindialog.h"
#include "config.h"
#include "skin.h"

#define PPA_SKINDLG_ALPHA	0xD0
#define PPA_SKINDLG_HLALPHA	0xD0
#define PPA_SKINDLG_BG_COLOR 0x000000
#define PPA_SKINDLG_BG_HLCOLOR 0x000000
#define PPA_SKINDLG_LABEL_COLOR 0xFFFFFF
#define PPA_SKINDLG_LABEL_HLCOLOR 0x00FFFF
#define PPA_SKINDLG_VALUE_COLOR 0x00FF00
#define PPA_SKINDLG_VALUE_HLCOLOR 0x0000FF

#define SKIN_DIALOG_X		60
#define SKIN_DIALOG_Y		16	
#define SKIN_DIALOG_W		360
#define SKIN_DIALOG_H		240
#define SKIN_DIALOG_R		6
#define SKIN_DIALOG_ITEM_X    90
#define SKIN_DIALOG_ITEM_Y	36
#define SKIN_DIALOG_ITEM_W	300
#define SKIN_DIALOG_ITEM_H	200

//static char triangleChar[] = {0xE2, 0x96, 0xB3, 0x00};
static char circleChar[] = {(char)0xE2, (char)0x97, (char)0x8B, 0x00};
static char crossChar[] = {(char)0xC3, (char)0x97, 0x00};
//static char squareChar[] = {0xE2, 0x96, 0xA1, 0x00};

static file_type_ext_struct skinFilter[] = {
	{NULL, FS_UNKNOWN_FILE}
};

SkinDialog::SkinDialog(Image* mainWindow, Image* mainDrawImage) {
	
	skinItems = NULL;
	itemBottom = itemTop = itemCurrent = itemCount = 0;
//	screenSnapshot = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	this->mainWindow = mainWindow;
	this->mainDrawImage = mainDrawImage;
	drawImage = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
};

SkinDialog::~SkinDialog() {
	if ( skinItems ) {
		free(skinItems);
	}
//	freeImage(screenSnapshot);
	mainWindow = NULL;
	mainDrawImage = NULL;
	freeImage(drawImage);
	mainFont = NULL;
};

bool SkinDialog::init(const char* skinPath, char* skinName) {
//	if ( screenSnapshot == NULL )
//		return false;
	if ( drawImage == NULL )
		return false;
	memset(this->skinPath, 0, 512);
	memset(this->skinShortPath, 0, 512);
	strncpy(this->skinPath, skinPath, 511);
	strncpy(this->skinShortPath, skinPath, 511);
	this->skinName = skinName;
	
	itemCount = open_directory(this->skinPath, this->skinShortPath, 0, 0, skinFilter, &skinItems);
	if ( itemCount < 2) 
		return false;
	itemTop = itemCurrent = 1;	
//	makeScreenSnapshot(screenSnapshot);
	
	mainFont = FtFontManager::getInstance()->getMainFont();
	fontSize = (Config::getInstance())->getIntegerValue("config/windows/font/size",12);
	
	itemBottom = SKIN_DIALOG_ITEM_H / ( fontSize + 2 );
	
	Skin* skin = Skin::getInstance();
	alpha = skin->getAlphaValue("skin/config_dialog/dialog/alpha", PPA_SKINDLG_ALPHA);
	bgColor = skin->getColorValue("skin/config_dialog/dialog/background_color", PPA_SKINDLG_BG_COLOR);
	labelColor = skin->getColorValue("skin/config_dialog/dialog/label_color", PPA_SKINDLG_LABEL_COLOR);
	
	title = i18nGetText(I18N_MSG_SKIN_TITLE);
	snprintf(help, sizeof(help), "%s %s    %s %s", circleChar, i18nGetText(I18N_MSG_OK), crossChar, i18nGetText(I18N_MSG_CANCEL));
	return true;
};

bool SkinDialog::execute() {
	bool returnValue = false;
	while( true ) {
		//*/
		u32 key = ctrl_wait(50000);
		if ( key & PSP_CTRL_CROSS ) {
			break;
		}
		else if ( key & PSP_CTRL_CIRCLE ) {
			strcpy(skinName, skinItems[itemCurrent].shortname);
			returnValue = true;
			break;
		}
		else if( (key & PSP_CTRL_UP) || (key & CTRL_BACK) ) {
			if (itemCurrent != 1){
				itemCurrent--;
			}
			else {
				itemCurrent = itemCount - 1;
			}
		}
		else if( (key & PSP_CTRL_DOWN) || (key & CTRL_FORWARD) ){
			if (itemCurrent + 1 < itemCount) {
				itemCurrent++;
			}
			else 
				itemCurrent = 1;
		}
		else if (key & PSP_CTRL_LTRIGGER ) {
			itemCurrent = 1;
		}
		else if (key & PSP_CTRL_RTRIGGER ) {
			itemCurrent = itemCount - 1;
		}
		//*/
		paint();
	};
	return returnValue;
};

void SkinDialog::paint() {
	
	clearImage(drawImage, 0);
	
	//fillImageRect(drawImage, (alpha << 24) | bgColor, SKIN_DIALOG_X, SKIN_DIALOG_Y, SKIN_DIALOG_W, SKIN_DIALOG_H);
	fillImageEllipse(drawImage, (alpha << 24) | bgColor, SKIN_DIALOG_X, SKIN_DIALOG_Y, SKIN_DIALOG_W, SKIN_DIALOG_H, SKIN_DIALOG_R);
	
	int titleX = SKIN_DIALOG_X + (SKIN_DIALOG_W - strlen(title)*fontSize/2)/2;
	mainFont->printStringToImage(drawImage, titleX, SKIN_DIALOG_Y+2+fontSize-1, SKIN_DIALOG_W-titleX+SKIN_DIALOG_X, fontSize+2, labelColor, title);
	
	int helpX = SKIN_DIALOG_X + (SKIN_DIALOG_W - strlen(help)*fontSize/2)/2;
	mainFont->printStringToImage(drawImage, helpX, SKIN_DIALOG_Y+SKIN_DIALOG_H-4-fontSize+fontSize-1, SKIN_DIALOG_W-helpX+SKIN_DIALOG_X, fontSize+2, labelColor, help);
	
	if ( itemCurrent < itemTop ) {
		itemTop = itemCurrent;
	}
	else if (itemCurrent - itemTop >= itemBottom ){
		itemTop = itemCurrent - itemBottom + 1;
	}
	
	int i;
	for(i=0;i<itemBottom;i++) {
		if( itemTop + i < itemCount) {
			if ( itemTop + i == itemCurrent)
				mainFont->printStringToImage(drawImage, SKIN_DIALOG_ITEM_X+1, SKIN_DIALOG_ITEM_Y + i*(fontSize+2)+fontSize-1,
					SKIN_DIALOG_ITEM_W-2, fontSize , PPA_SKINDLG_VALUE_HLCOLOR, skinItems[itemTop + i].longname);
			else
				mainFont->printStringToImage(drawImage, SKIN_DIALOG_ITEM_X+1, SKIN_DIALOG_ITEM_Y + i*(fontSize+2)+fontSize-1,
					SKIN_DIALOG_ITEM_W-2, fontSize , PPA_SKINDLG_VALUE_COLOR, skinItems[itemTop + i].longname);
			//items[i]->paint(SKIN_DIALOG_ITEM_X, SKIN_DIALOG_ITEM_Y + i*(fontSize+2), SKIN_DIALOG_ITEM_W, fontSize + 2 );
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

