/* 
 *	Copyright (C) 2009 cooleyes
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
 
#ifndef __LIB_I18N_H__
#define __LIB_I18N_H__

#ifdef __cplusplus
extern "C" {
#endif

#define I18N_MSG_OK 0
#define I18N_MSG_CANCEL 1
#define I18N_MSG_YES 2
#define I18N_MSG_NO 3
#define I18N_MSG_ON 4
#define I18N_MSG_OFF 5
#define I18N_MSG_NOT_USED 6

#define I18N_MSG_CFG_CPU_SPEED 7
#define I18N_MSG_CFG_SUBTITLES_CHARSET 8
#define I18N_MSG_CFG_SUBTITLES_FONTSIZE 9
#define I18N_MSG_CFG_SUBTITLES_EMBOLDEN 10
#define I18N_MSG_CFG_SUBTITLES_POSITION_ALIGN 11
#define I18N_MSG_CFG_SUBTITLES_POSITION_ALIGN_TOP 12
#define I18N_MSG_CFG_SUBTITLES_POSITION_ALIGN_BOTTOM 13
#define I18N_MSG_CFG_SUBTITLES_POSITION_DISTANCE 14
#define I18N_MSG_CFG_FILESYSTEM_CHARSET 15
/* 16..21 are reserved to preserve the legacy language-module ABI. */
#define I18N_MSG_CFG_PLAY_MODE 22
#define I18N_MSG_CFG_PLAY_MODE_SINGLE 23
#define I18N_MSG_CFG_PLAY_MODE_GROUP 24
#define I18N_MSG_CFG_PLAY_MODE_ALL 25
#define I18N_MSG_CFG_TV_AR 26
#define I18N_MSG_CFG_TV_OVERSCAN 27
#define I18N_MSG_CFG_VIDEO_MODE 28
#define I18N_MSG_CFG_VIDEO_MODE_LCD 29
#define I18N_MSG_CFG_VIDEO_MODE_COMPOSITE 30
#define I18N_MSG_CFG_VIDEO_MODE_COMPONENT_INTERLACE 31
#define I18N_MSG_CFG_VIDEO_MODE_COMPONENT_PROGRESSIVE 32
#define I18N_MSG_CFG_TITLE 33

#define I18N_MSG_SKIN_TITLE 34

#define I18N_MSG_DLG_DELETE_TITLE 35
#define I18N_MSG_DLG_QUIT_TITLE 36

char* i18nGetText(int entry);

#ifdef __cplusplus
}
#endif

#endif
