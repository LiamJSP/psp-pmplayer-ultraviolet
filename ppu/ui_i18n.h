/*
 * Runtime skin text localization for the PSP browser and controller-help UI.
 *
 * The visual skin remains a PNG, while all functional labels are loaded from
 * ui_strings.xml and placed using the JSON clip maps supplied with the skin.
 */
#ifndef __PPA_UI_I18N_H__
#define __PPA_UI_I18N_H__

#include "common/graphics.h"
#include "common/ppu_boost.hpp"
#include <boost/core/noncopyable.hpp>

class FtFont;

class UiI18n : private boost::noncopyable {
private:
	class Impl;
	Impl* impl;

public:
	UiI18n();
	~UiI18n();

	bool load(const char* applicationPath, const char* skinPath);
	bool isReady() const;
	bool isRtl() const;
	const char* getLanguageId() const;
	const char* getText(const char* id, const char* fallback) const;
	const char* getLanguageName(const char* id, const char* fallback) const;
	FtFont* getFont() const;

	/* Non-owning cached overlays. They are rasterized once (or restored from
	 * ms0:) and should be submitted directly to the GE instead of CPU-merging
	 * a 480x272 sparse bitmap into every browser frame. */
	Image* getMainStaticOverlay();
	Image* getHelpStaticOverlay();
	void prepareCachedOverlays();
	void releaseCachedOverlays();

	/* Resolve the active JSON clip after source/target rectangle selection. */
	bool getMainClipRect(const char* id, int* x, int* y,
	                     int* width, int* height) const;

	void renderMainLabels(Image* image, bool thumbnailUnavailable);
	void drawMainValue(Image* image, const char* clipId, const char* value);
	void renderHelp(Image* image);

	void onSuspend();
	void onResume();

	static int languageCount();
	static const char* languageIdAt(int index);
	static const char* languageNameAt(int index);
};

#endif
