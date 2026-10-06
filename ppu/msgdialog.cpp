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
#include "common/libi18n.h"

#include "msgdialog.h"
#include "config.h"
#include "skin.h"

#define PPA_MESSAGEDLG_ALPHA	0xD0
#define PPA_MESSAGEDLG_HLALPHA	0xD0
#define PPA_MESSAGEDLG_BG_COLOR 0x000000
#define PPA_MESSAGEDLG_BG_HLCOLOR 0x000000
#define PPA_MESSAGEDLG_LABEL_COLOR 0xFFFFFF
#define PPA_MESSAGEDLG_LABEL_HLCOLOR 0x00FFFF
#define PPA_MESSAGEDLG_VALUE_COLOR 0x00FF00
#define PPA_MESSAGEDLG_VALUE_EDCOLOR 0x0000FF

/********************************************************************************
 *                      MessageDialog                                            *
 ********************************************************************************/

#define MESSAGE_DIALOG_X		120
#define MESSAGE_DIALOG_Y		76	
#define MESSAGE_DIALOG_W		240
#define MESSAGE_DIALOG_H		120
#define MESSAGE_DIALOG_R		6

//static char triangleChar[] = {0xE2, 0x96, 0xB3, 0x00};
static char circleChar[] = {(char)0xE2, (char)0x97, (char)0x8B, 0x00};
static char crossChar[] = {(char)0xC3, (char)0x97, 0x00};
//static char squareChar[] = {0xE2, 0x96, 0xA1, 0x00};

MessageDialog::MessageDialog(Image* mainWindow, Image* mainDrawImage) {
//	screenSnapshot = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	this->mainWindow = mainWindow;
	this->mainDrawImage = mainDrawImage;
	drawImage = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
};

MessageDialog::~MessageDialog() {
//	freeImage(screenSnapshot);
	freeImage(drawImage);
	mainWindow = NULL;
	mainDrawImage = NULL;
	mainFont = NULL;
};

bool MessageDialog::init(const char* title, u32 type) {
//	if ( screenSnapshot == NULL )
//		return false;
	if ( drawImage == NULL )
		return false;
		
//	makeScreenSnapshot(screenSnapshot);
	
	mainFont = FtFontManager::getInstance()->getMainFont();
	fontSize = (Config::getInstance())->getIntegerValue("config/windows/font/size",12);
	
	Skin* skin = Skin::getInstance();
	alpha = skin->getAlphaValue("skin/message_dialog/dialog/alpha", PPA_MESSAGEDLG_ALPHA);
	bgColor = skin->getColorValue("skin/message_dialog/dialog/background_color", PPA_MESSAGEDLG_BG_COLOR);
	labelColor = skin->getColorValue("skin/message_dialog/dialog/label_color", PPA_MESSAGEDLG_LABEL_COLOR);
	
	memset(messageTitle, 0, 512);
	if (title) 
		strncpy(messageTitle, title, 512-1);
	
	messageType = type;
	
	memset(keyTips, 0, 512);
	int i;
	for(i=0; i<4; i++) {
		keys[i] = 0;
		results[i] = MESSAGE_RESULT_NONE;
	}
	if ( messageType == MESSAGE_TYPE_ANY ) {
		snprintf(keyTips, sizeof(keyTips), "%s", "Press any key...");
		keys[0] = 0xFFFFFFFF;
	}
	else if ( messageType == MESSAGE_TYPE_OK_CANCEL ) {
		snprintf(keyTips, sizeof(keyTips), "%s %s    %s %s", ppu_controls_confirm_glyph(), i18nGetText(I18N_MSG_OK), ppu_controls_cancel_glyph(), i18nGetText(I18N_MSG_CANCEL));
		keys[0] = ppu_controls_confirm();
		keys[1] = ppu_controls_cancel();
		results[0] = MESSAGE_RESULT_OK;
		results[1] = MESSAGE_RESULT_CANCEL;
	}
	else if ( messageType == MESSAGE_TYPE_YES_NO ) {
		snprintf(keyTips, sizeof(keyTips), "%s %s    %s %s", ppu_controls_confirm_glyph(), i18nGetText(I18N_MSG_YES), ppu_controls_cancel_glyph(), i18nGetText(I18N_MSG_NO));
		keys[0] = ppu_controls_confirm();
		keys[1] = ppu_controls_cancel();
		results[0] = MESSAGE_RESULT_YES;
		results[1] = MESSAGE_RESULT_NO;
	}
	
	return true;
};

u32 MessageDialog::execute() {
    enum ctrl_context previousContext = ctrl_set_context(CTRL_CONTEXT_MENU);
	while( true ) {
		u32 key = ctrl_wait(50000);
		int i;
		for( i=0; i<4; i++ ) {
			if ( key & keys[i] )
				{ ctrl_set_context(previousContext); return results[i]; }
		}
		paint();
	};
};

void MessageDialog::paint() {
	
	clearImage(drawImage, 0);
	
	//fillImageRect(drawImage, (alpha << 24) | bgColor, MESSAGE_DIALOG_X, MESSAGE_DIALOG_Y, MESSAGE_DIALOG_W, MESSAGE_DIALOG_H);
	fillImageEllipse(drawImage, (alpha << 24) | bgColor, MESSAGE_DIALOG_X, MESSAGE_DIALOG_Y, MESSAGE_DIALOG_W, MESSAGE_DIALOG_H, MESSAGE_DIALOG_R);
	
	int titleX = MESSAGE_DIALOG_X + 2*fontSize;
	mainFont->printStringToImage(drawImage, titleX, MESSAGE_DIALOG_Y+MESSAGE_DIALOG_H/3, MESSAGE_DIALOG_W-titleX+MESSAGE_DIALOG_X, fontSize+2, labelColor, messageTitle);
	
	int tipsX = MESSAGE_DIALOG_X + (MESSAGE_DIALOG_W - strlen(keyTips)*fontSize/2)/2;
	mainFont->printStringToImage(drawImage, tipsX, MESSAGE_DIALOG_Y+MESSAGE_DIALOG_H-4-2*fontSize+fontSize-1, MESSAGE_DIALOG_W-tipsX+MESSAGE_DIALOG_X, fontSize+2, labelColor, keyTips);
	
	guStart();
	clearScreen();		
//	blitImageToScreen(0, 0, screenSnapshot->imageWidth, screenSnapshot->imageHeight, screenSnapshot, 0, 0);
	blitImageToScreen(0, 0, mainWindow->imageWidth, mainWindow->imageHeight, mainWindow, 0, 0);
	blitAlphaImageToScreen(0, 0, mainDrawImage->imageWidth, mainDrawImage->imageHeight, mainDrawImage, 0, 0);
	blitAlphaImageToScreen(0, 0, drawImage->imageWidth, drawImage->imageHeight, drawImage, 0, 0);
	flipScreen();
};

