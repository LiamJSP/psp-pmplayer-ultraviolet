/* player.h
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

#ifndef __PPA_PLAYER_H__
#define __PPA_PLAYER_H__

#include<time.h>
#include "common/graphics.h"
#include "common/directory.h"
#include "common/m33sdk.h"
#include "common/ppu_boost.hpp"
#include <boost/core/noncopyable.hpp>

class UiI18n;
class BrowserTextCache;
class FtFont;

/* Owns worker lifetimes, images and directory results; never duplicate it. */
class PpuPlayer : private boost::noncopyable {

private:
	int pspType;
	char applicationPath[256];
	char fontPath[1024];
	char skinPath[1024];
	Image* drawImage;
	UiI18n* uiI18n;
	BrowserTextCache* browserTextCache;
	void paintBrowserText(FtFont* font, int x, int y, int width, int height,
	                      Color color, const char* text);
	/* Launch-only texture; the browser/UI thread owns all GE submissions and
	 * releases it after the last synchronized fade frame, before playback. */
	Image* startupCredit;
	u64 startupCreditVisibleSinceUs;
	u64 startupCreditFadeStartedUs;
	unsigned int startupCreditOpacity;
	void beginStartupCredit();
	bool serviceStartupCredit();
	void releaseStartupCredit();
	void presentBrowserFrame();
	
	int textPixelSize;
	
	int fileListBoxLeft, fileListBoxTop, fileListBoxWidth, fileListBoxHeight;
	int fileItemBottom, fileItemTop, fileItemCurrent, fileItemCount;
	int fileShowHidden, fileShowUnknown;
	char filePath[512], fileShortPath[512];
	int filesystemMode; /* 0 = ms0 FAT, 1 = ms1 NTFS */
	void switchFilesystemMode();
	directory_item_struct *fileItems;
	directory_item_struct *attachmentItems;
	Color fileListTextColor, fileListHLTextColor, fileListHLBackgroundColor, fileListScrollbarColor, fileListCursorColor;
	int fileListCursorWidth;
	
	bool dateVisible, timeVisible;
	int dateLeft, dateTop, dateWidth, dateHeight;
	int timeLeft, timeTop, timeWidth, timeHeight;
	
	bool batteryStatusVisible, batteryLifeVisible;
	int batteryStatusLeft, batteryStatusTop, batteryStatusWidth, batteryStatusHeight;
	int batteryLifeLeft, batteryLifeTop, batteryLifeWidth, batteryLifeHeight;
	Image* batteryPercent10;
	Image* batteryPercent33;
	Image* batteryPercent66;
	Image* batteryPercent100;
	Image* batteryCharging;
	
	bool filmInformationReload;
	bool filmAspectRatioVisible, filmFpsVisible, filmTotalTimeVisible, filmAudioStreamsVisible, filmSubtitlesVisible;
	int filmAspectRatioLeft, filmAspectRatioTop, filmAspectRatioWidth, filmAspectRatioHeight;
	int filmFpsLeft, filmFpsTop, filmFpsWidth, filmFpsHeight;
	int filmTotalTimeLeft, filmTotalTimeTop, filmTotalTimeWidth, filmTotalTimeHeight;
	int filmAudioStreamsLeft, filmAudioStreamsTop, filmAudioStreamsWidth, filmAudioStreamsHeight;
	int filmSubtitlesLeft, filmSubtitlesTop, filmSubtitlesWidth, filmSubtitlesHeight;
	u32 filmTotalFrames, filmWidth, filmHeight, filmScale, filmRate, filmAudioStreams, filmSubtitles;

	bool filmPreviewVisible, filmPreviewReload;
	int filmPreviewLeft, filmPreviewTop, filmPreviewWidth, filmPreviewHeight;
	Image*  filmPreviewImage;
	bool filmPreviewUnavailable;
	
	bool filmReloadEnable;
	
	bool isSuspended;
	volatile unsigned int powerEventFlags;
	time_t activeTime;
	int idleSecond;
	
	void saveConfig();
	
	void initSkinData();
	
	void initFilmInformation();

	bool listDirectory();
	bool requestDirectory(const char* path, const char* shortPath,
	                      const char* restoreName);
	bool commitDirectoryResult();
	bool applyDirectoryItems(const char* path, const char* shortPath,
	                         const char* restoreName,
	                         directory_item_struct* items,
	                         int itemCount,
	                         bool cacheHit);
	void paintFileListBox();
	void paintDateTime();
	void paintBattery();
	void paintFilmPreview();
	void paintFilmInformation();
	void paintLoading();
	
	void showPadHelp();
	
	void deleteSelectMovie();
	
	int getSelectMovieSubtitles();
	
	void fillSelectMovieInfo();
	
	void playMovie(bool resume);
	void processPowerEvents();

public:
	PpuPlayer();
	~PpuPlayer();
	
	int init(char* ppaPath);
	void run();
	void paint();
	
	void requestSuspendFromCallback();
	void requestResumeFromCallback();
	void enterSuspendMode();
	void leaveSuspendMode();
	
};

#endif
