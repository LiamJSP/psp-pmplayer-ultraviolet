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

#include "version.h"
#include "versiondialog.h"
#include "config.h"
#include "skin.h"

#define PPA_VERSIONDLG_ALPHA	0xD0
#define PPA_VERSIONDLG_HLALPHA	0xD0
#define PPA_VERSIONDLG_BG_COLOR 0x000000
#define PPA_VERSIONDLG_BG_HLCOLOR 0x000000
#define PPA_VERSIONDLG_LABEL_COLOR 0xFFFFFF
#define PPA_VERSIONDLG_LABEL_HLCOLOR 0x00FFFF
#define PPA_VERSIONDLG_VALUE_COLOR 0x00FF00
#define PPA_VERSIONDLG_VALUE_EDCOLOR 0x0000FF

/********************************************************************************
 *                      VersionDialog                                            *
 ********************************************************************************/

#define VERSION_DIALOG_X		90
#define VERSION_DIALOG_Y		76	
#define VERSION_DIALOG_W		300
#define VERSION_DIALOG_H		120
#define VERSION_DIALOG_R		6

VersionDialog::VersionDialog(Image* mainWindow, Image* mainDrawImage) {
//	screenSnapshot = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	this->mainWindow = mainWindow;
	this->mainDrawImage = mainDrawImage;
	drawImage = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
};

VersionDialog::~VersionDialog() {
//	freeImage(screenSnapshot);
	mainWindow = NULL;
	mainDrawImage = NULL;
	freeImage(drawImage);
	mainFont = NULL;
};

bool VersionDialog::init() {
//	if ( screenSnapshot == NULL )
//		return false;
	if ( drawImage == NULL )
		return false;
		
//	makeScreenSnapshot(screenSnapshot);
	
	mainFont = FtFontManager::getInstance()->getMainFont();
	fontSize = (Config::getInstance())->getIntegerValue("config/windows/font/size",12);
	
	Skin* skin = Skin::getInstance();
	alpha = skin->getAlphaValue("skin/message_dialog/dialog/alpha", PPA_VERSIONDLG_ALPHA);
	bgColor = skin->getColorValue("skin/message_dialog/dialog/background_color", PPA_VERSIONDLG_BG_COLOR);
	labelColor = skin->getColorValue("skin/message_dialog/dialog/label_color", PPA_VERSIONDLG_LABEL_COLOR);
	
	return true;
};

void VersionDialog::execute() {
    enum ctrl_context previousContext = ctrl_set_context(CTRL_CONTEXT_MENU);
	while( true ) {
		u32 key = ctrl_wait(50000);
		if ( key & PSP_CTRL_TRIANGLE ) {
            ctrl_set_context(previousContext);
            return;
        }
		paint();
	};
};

void VersionDialog::paint() {
	
	clearImage(drawImage, 0);
	
	//fillImageRect(drawImage, (alpha << 24) | bgColor, VERSION_DIALOG_X, VERSION_DIALOG_Y, VERSION_DIALOG_W, VERSION_DIALOG_H);
	fillImageEllipse(drawImage, (alpha << 24) | bgColor, VERSION_DIALOG_X, VERSION_DIALOG_Y, VERSION_DIALOG_W, VERSION_DIALOG_H, VERSION_DIALOG_R);
	
	int titleX = VERSION_DIALOG_X + fontSize;
	mainFont->printStringToImage(drawImage, titleX, VERSION_DIALOG_Y+fontSize+5, VERSION_DIALOG_W-titleX+VERSION_DIALOG_X, fontSize+2, labelColor, PPU_NAME);
	mainFont->printStringToImage(drawImage, titleX, VERSION_DIALOG_Y+(fontSize+5)*2, VERSION_DIALOG_W-titleX+VERSION_DIALOG_X, fontSize+2, labelColor, PPU_VERSION);
	mainFont->printStringToImage(drawImage, titleX, VERSION_DIALOG_Y+(fontSize+5)*3, VERSION_DIALOG_W-titleX+VERSION_DIALOG_X, fontSize+2, labelColor, PPU_COPYRIGHT);
	
	guStart();
	clearScreen();		
//	blitImageToScreen(0, 0, screenSnapshot->imageWidth, screenSnapshot->imageHeight, screenSnapshot, 0, 0);
	blitImageToScreen(0, 0, mainWindow->imageWidth, mainWindow->imageHeight, mainWindow, 0, 0);
	blitAlphaImageToScreen(0, 0, mainDrawImage->imageWidth, mainDrawImage->imageHeight, mainDrawImage, 0, 0);
	blitAlphaImageToScreen(0, 0, drawImage->imageWidth, drawImage->imageHeight, drawImage, 0, 0);
	flipScreen();
};

