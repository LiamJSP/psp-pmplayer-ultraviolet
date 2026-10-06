#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <map>
#include <string>
#include <vector>

#include <pspiofilemgr.h>
#include <psputility_sysparam.h>

#include "ui_i18n.h"
#include <boost/static_assert.hpp>
#include <boost/type_traits/is_pod.hpp>
#include <stddef.h>
#include "common/ppu_controls.h"
#include "config.h"
#include "skin.h"
#include "ftfont.h"
#include "tinyxml/tinyxml.h"

#ifndef PPA_ENABLE_THUMBNAILS
#define PPA_ENABLE_THUMBNAILS 0
#endif

namespace {

struct UiLanguageDescriptor {
	const char* id;
	const char* name;
};

static const UiLanguageDescriptor kLanguages[] = {
	{ "auto",  "Auto (PSP system)" },
	{ "en",    "English" },
	{ "zh-CN", "Mandarin Chinese" },
	{ "hi",    "Hindi" },
	{ "es",    "Spanish" },
	{ "ar",    "Modern Standard Arabic" },
	{ "fr",    "French" },
	{ "bn",    "Bengali" },
	{ "pt",    "Portuguese" },
	{ "id",    "Indonesian" },
	{ "ur",    "Urdu" },
	{ "ru",    "Russian" },
	{ "de",    "Standard German" },
	{ "ja",    "Japanese" },
	{ "pcm",   "Nigerian Pidgin" },
	{ "arz",   "Egyptian Arabic" },
	{ "mr",    "Marathi" },
	{ "vi",    "Vietnamese" },
	{ "te",    "Telugu" }
};

static const int kLanguageCount =
	(int)(sizeof(kLanguages) / sizeof(kLanguages[0]));

struct UiClip {
	std::string id;
	int x;
	int y;
	int width;
	int height;
};

struct UiBuiltinClip {
	const char* id;
	int sourceX;
	int sourceY;
	int sourceWidth;
	int sourceHeight;
	int targetX;
	int targetY;
	int targetWidth;
	int targetHeight;
};

static const UiBuiltinClip kMainBuiltinClips[] = {
	{ "file_list_header",       252,  33,  58, 11, 224,  32,  80, 10 },
	{ "information_header",     393,  33,  77, 11, 367,  32,  90, 10 },
	{ "thumbnail_label",        328,  55,  58, 11, 324,  49,  66, 10 },
	{ "thumbnail_unavailable",  359, 107,  78, 12, 362,  97,  66, 12 },
	{ "total_time",             328, 174,  67, 12, 324, 158, 132, 10 },
	{ "aspect_ratio",           328, 193,  74, 12, 324, 171, 132, 10 },
	{ "frames_per_second",      328, 212,  57, 12, 324, 184, 132, 10 },
	{ "streams",                328, 231,  51, 12, 324, 197, 132, 10 },
	{ "subtitles",              328, 250,  58, 12, 324, 209, 132, 10 },
	{ "cancel_label",            27, 248,  42, 14,  43, 175, 105, 13 },
	{ "help_label",              83, 248,  29, 14, 187, 175, 109, 13 },
	{ "start_label",            137, 248,  36, 14,  43, 205, 105, 13 },
	{ "resume_label",           194, 248,  44, 14, 187, 205, 109, 13 },
	{ "delete_label",           259, 248,  44, 14, 118, 236, 109, 13 },
	{ "main_directory_list_area", 8, 46, 304, 198, 11, 44, 296, 115 }
};

static const UiBuiltinClip kHelpBuiltinClips[] = {
	{ "control_instruction",      8,  32, 122, 14,  14,  34, 135, 12 },
	{ "luminosity",             198,  56,  83, 15, 204,  53,  78, 13 },
	{ "forward_backward",        41,  73,  58, 25,  33,  71,  62, 23 },
	{ "pause",                  380,  79,  35, 14, 386,  79,  60, 11 },
	{ "exit",                   381,  98,  27, 14, 386, 101,  55, 11 },
	{ "show_info",              381, 117,  54, 14, 386, 118,  66, 12 },
	{ "volume",                  58, 171,  42, 14,  34, 166,  63, 12 },
	{ "aspect_ratio",           380, 171,  70, 14, 380, 166,  78, 11 },
	{ "aspect_ratio_values",    389, 187,  83, 14, 362, 179,  96, 12 },
	{ "audio_stream_switch",    339, 202, 116, 14, 340, 197, 118, 12 },
	{ "subtitles_switch",       144, 194, 124, 14, 149, 190, 133, 12 },
	{ "loop_toggle",            144, 213,  70, 14, 149, 207, 103, 12 },
	{ "fast_forward_rewind",    144, 231, 117, 14, 149, 224, 135, 12 },
	{ "zoom_in_out",            144, 248,  72, 14, 149, 242, 103, 12 },
	{ "close_prompt_text",      349, 244, 112, 18, 356, 236,  96, 14 }
};

static void uiExtractSkinName(const char* skinPath,
	                          char* result,
	                          size_t resultSize)
{
	const char* begin;
	const char* end;
	size_t length;
	if (result == NULL || resultSize == 0)
		return;
	result[0] = 0;
	if (skinPath == NULL || skinPath[0] == 0)
		return;
	end = skinPath + strlen(skinPath);
	while (end > skinPath && (end[-1] == '/' || end[-1] == '\\'))
		--end;
	begin = end;
	while (begin > skinPath && begin[-1] != '/' && begin[-1] != '\\' &&
	       begin[-1] != ':')
		--begin;
	length = (size_t)(end - begin);
	if (length >= resultSize)
		length = resultSize - 1U;
	memcpy(result, begin, length);
	result[length] = 0;
}

static void uiLoadBuiltinClips(const UiBuiltinClip* builtin,
	                           size_t count,
	                           bool sourceRects,
	                           std::vector<UiClip>* clips)
{
	size_t i;
	if (builtin == NULL || clips == NULL)
		return;
	clips->clear();
	for (i = 0; i < count; ++i) {
		UiClip clip;
		clip.id = builtin[i].id;
		if (sourceRects) {
			clip.x = builtin[i].sourceX;
			clip.y = builtin[i].sourceY;
			clip.width = builtin[i].sourceWidth;
			clip.height = builtin[i].sourceHeight;
		}
		else {
			clip.x = builtin[i].targetX;
			clip.y = builtin[i].targetY;
			clip.width = builtin[i].targetWidth;
			clip.height = builtin[i].targetHeight;
		}
		clips->push_back(clip);
	}
}

static char* uiReadWholeFile(const char* path, size_t* outSize)
{
	FILE* file;
	long length;
	char* data;
	if (outSize != NULL)
		*outSize = 0;
	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	length = ftell(file);
	if (length <= 0 || length > 1024L * 1024L) {
		fclose(file);
		return NULL;
	}
	if (fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	data = (char*)malloc((size_t)length + 1U);
	if (data == NULL) {
		fclose(file);
		return NULL;
	}
	if (fread(data, 1, (size_t)length, file) != (size_t)length) {
		free(data);
		fclose(file);
		return NULL;
	}
	data[length] = 0;
	fclose(file);
	if (outSize != NULL)
		*outSize = (size_t)length;
	return data;
}

static const char* uiSkipWhitespace(const char* p)
{
	while (p != NULL && *p != 0 && isspace((unsigned char)*p))
		++p;
	return p;
}

static const char* uiFindMatchingBrace(const char* openBrace)
{
	int depth = 0;
	bool inString = false;
	bool escaped = false;
	const char* p;
	if (openBrace == NULL || *openBrace != '{')
		return NULL;
	for (p = openBrace; *p != 0; ++p) {
		if (inString) {
			if (escaped)
				escaped = false;
			else if (*p == '\\')
				escaped = true;
			else if (*p == '"')
				inString = false;
			continue;
		}
		if (*p == '"') {
			inString = true;
			continue;
		}
		if (*p == '{')
			++depth;
		else if (*p == '}') {
			--depth;
			if (depth == 0)
				return p;
		}
	}
	return NULL;
}

static bool uiJsonStringAfter(const char* begin,
	                          const char* end,
	                          const char* key,
	                          std::string* value,
	                          const char** after)
{
	std::string quotedKey;
	const char* found;
	const char* p;
	const char* start;
	if (begin == NULL || end == NULL || key == NULL || value == NULL)
		return false;
	quotedKey = std::string("\"") + key + "\"";
	found = strstr(begin, quotedKey.c_str());
	if (found == NULL || found >= end)
		return false;
	p = found + quotedKey.length();
	p = uiSkipWhitespace(p);
	if (p == NULL || p >= end || *p != ':')
		return false;
	p = uiSkipWhitespace(p + 1);
	if (p == NULL || p >= end || *p != '"')
		return false;
	start = ++p;
	while (p < end && *p != 0) {
		if (*p == '"' && (p == start || p[-1] != '\\'))
			break;
		++p;
	}
	if (p >= end || *p != '"')
		return false;
	value->assign(start, (size_t)(p - start));
	if (after != NULL)
		*after = p + 1;
	return true;
}

static bool uiJsonIntInObject(const char* begin,
	                          const char* end,
	                          const char* key,
	                          int* value)
{
	std::string quotedKey;
	const char* found;
	const char* p;
	char* parseEnd;
	long parsed;
	if (begin == NULL || end == NULL || key == NULL || value == NULL)
		return false;
	quotedKey = std::string("\"") + key + "\"";
	found = strstr(begin, quotedKey.c_str());
	if (found == NULL || found >= end)
		return false;
	p = uiSkipWhitespace(found + quotedKey.length());
	if (p == NULL || p >= end || *p != ':')
		return false;
	p = uiSkipWhitespace(p + 1);
	if (p == NULL || p >= end)
		return false;
	parsed = strtol(p, &parseEnd, 10);
	if (parseEnd == p || parseEnd > end)
		return false;
	*value = (int)parsed;
	return true;
}

static bool uiLoadClipMap(const char* filename,
	                      const char* rectKey,
	                      std::vector<UiClip>* clips)
{
	char* json;
	size_t jsonSize = 0;
	const char* cursor;
	const char* jsonEnd;
	std::string quotedRect;
	if (filename == NULL || rectKey == NULL || clips == NULL)
		return false;
	clips->clear();
	json = uiReadWholeFile(filename, &jsonSize);
	if (json == NULL)
		return false;
	cursor = json;
	jsonEnd = json + jsonSize;
	quotedRect = std::string("\"") + rectKey + "\"";

	while (cursor < jsonEnd) {
		const char* idKey = strstr(cursor, "\"id\"");
		const char* nextId;
		const char* rectKeyPos;
		const char* colon;
		const char* rectOpen;
		const char* rectClose;
		std::string id;
		UiClip clip;
		if (idKey == NULL || idKey >= jsonEnd)
			break;
		nextId = strstr(idKey + 4, "\"id\"");
		if (nextId == NULL)
			nextId = jsonEnd;
		if (!uiJsonStringAfter(idKey, nextId, "id", &id, NULL)) {
			cursor = idKey + 4;
			continue;
		}
		rectKeyPos = strstr(idKey, quotedRect.c_str());
		if (rectKeyPos == NULL || rectKeyPos >= nextId) {
			cursor = nextId;
			continue;
		}
		colon = strchr(rectKeyPos + quotedRect.length(), ':');
		if (colon == NULL || colon >= nextId) {
			cursor = nextId;
			continue;
		}
		rectOpen = strchr(colon + 1, '{');
		if (rectOpen == NULL || rectOpen >= nextId) {
			cursor = nextId;
			continue;
		}
		rectClose = uiFindMatchingBrace(rectOpen);
		if (rectClose == NULL || rectClose > nextId) {
			cursor = nextId;
			continue;
		}
		clip.id = id;
		if (uiJsonIntInObject(rectOpen, rectClose, "x", &clip.x) &&
		    uiJsonIntInObject(rectOpen, rectClose, "y", &clip.y) &&
		    uiJsonIntInObject(rectOpen, rectClose, "width", &clip.width) &&
		    uiJsonIntInObject(rectOpen, rectClose, "height", &clip.height) &&
		    clip.width > 0 && clip.height > 0)
			clips->push_back(clip);
		cursor = nextId;
	}
	free(json);
	return !clips->empty();
}

static std::string uiDecodeText(const char* value)
{
	std::string result;
	const char* p = value ? value : "";
	while (*p != 0) {
		if (p[0] == '\\' && p[1] == 'n') {
			result.push_back('\n');
			p += 2;
		}
		else {
			result.push_back(*p++);
		}
	}
	return result;
}

static const char* uiSystemLanguageId()
{
	int language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
	if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
	                                &language) < 0)
		return "en";
	switch (language) {
		case PSP_SYSTEMPARAM_LANGUAGE_JAPANESE: return "ja";
		case PSP_SYSTEMPARAM_LANGUAGE_FRENCH: return "fr";
		case PSP_SYSTEMPARAM_LANGUAGE_SPANISH: return "es";
		case PSP_SYSTEMPARAM_LANGUAGE_GERMAN: return "de";
		case PSP_SYSTEMPARAM_LANGUAGE_PORTUGUESE: return "pt";
		case PSP_SYSTEMPARAM_LANGUAGE_RUSSIAN: return "ru";
		case PSP_SYSTEMPARAM_LANGUAGE_CHINESE_SIMPLIFIED: return "zh-CN";
		case PSP_SYSTEMPARAM_LANGUAGE_CHINESE_TRADITIONAL: return "zh-CN";
		default: return "en";
	}
}

static bool uiIsKnownLanguage(const char* id)
{
	int i;
	if (id == NULL)
		return false;
	for (i = 1; i < kLanguageCount; ++i) {
		if (strcmp(kLanguages[i].id, id) == 0)
			return true;
	}
	return false;
}

static FtTextAlign uiMainAlign(const char* id)
{
	if (id == NULL)
		return FT_TEXT_ALIGN_LEFT;
	if (strcmp(id, "file_list_header") == 0 ||
	    strcmp(id, "information_header") == 0)
		return FT_TEXT_ALIGN_RIGHT;
	if (strcmp(id, "thumbnail_unavailable") == 0)
		return FT_TEXT_ALIGN_CENTER;
	return FT_TEXT_ALIGN_LEFT;
}

static FtTextAlign uiHelpAlign(const char* id)
{
	if (id == NULL)
		return FT_TEXT_ALIGN_LEFT;
	if (strcmp(id, "luminosity") == 0 ||
	    strcmp(id, "aspect_ratio_values") == 0)
		return FT_TEXT_ALIGN_CENTER;
	if (strcmp(id, "forward_backward") == 0 ||
	    strcmp(id, "volume") == 0)
		return FT_TEXT_ALIGN_RIGHT;
	return FT_TEXT_ALIGN_LEFT;
}

static bool uiIsMetadataClip(const char* id)
{
	return id != NULL &&
	       (strcmp(id, "total_time") == 0 ||
	        strcmp(id, "aspect_ratio") == 0 ||
	        strcmp(id, "frames_per_second") == 0 ||
	        strcmp(id, "streams") == 0 ||
	        strcmp(id, "subtitles") == 0);
}

#if !PPA_ENABLE_THUMBNAILS
static bool uiIsThumbnailClip(const char* id)
{
	return id != NULL &&
	       (strcmp(id, "thumbnail_label") == 0 ||
	        strcmp(id, "thumbnail_unavailable") == 0);
}
#endif

static bool uiIsButtonLabelClip(const char* id)
{
	return id != NULL &&
	       (strcmp(id, "cancel_label") == 0 ||
	        strcmp(id, "help_label") == 0 ||
	        strcmp(id, "start_label") == 0 ||
	        strcmp(id, "resume_label") == 0 ||
	        strcmp(id, "delete_label") == 0 ||
	        strcmp(id, "close_prompt_text") == 0);
}

#define UI_OVERLAY_CACHE_VERSION 5U
#define UI_OVERLAY_CACHE_MAGIC   "PPAUI04"

struct UiOverlayCacheHeader {
	char magic[8];
	u32 version;
	u32 signature;
	u32 imageWidth;
	u32 imageHeight;
	u32 textureWidth;
	u32 imageBytes;
	u32 mainChecksum;
	u32 helpChecksum;
};

/* This POD is read/written as an existing native PSP cache record. Freeze
 * its current layout; a deliberate format change must also version the cache. */
BOOST_STATIC_ASSERT_MSG(boost::is_pod<UiOverlayCacheHeader>::value,
    "Overlay cache header must remain safe for raw-byte I/O");
BOOST_STATIC_ASSERT_MSG(sizeof(UiOverlayCacheHeader) == 40,
    "Overlay cache header layout changed; review cache versioning");
BOOST_STATIC_ASSERT_MSG(offsetof(UiOverlayCacheHeader, version) == 8 &&
    offsetof(UiOverlayCacheHeader, helpChecksum) == 36,
    "Overlay cache field offsets changed");

static u32 uiHashBytes(u32 hash, const void* bytes, size_t size)
{
	const unsigned char* p = (const unsigned char*)bytes;
	while (size-- != 0) {
		hash ^= *p++;
		hash *= 16777619U;
	}
	return hash;
}

static u32 uiHashString(u32 hash, const char* value)
{
	if (value != NULL)
		hash = uiHashBytes(hash, value, strlen(value));
	/* Keep adjacent strings unambiguous. */
	{
		const unsigned char separator = 0xff;
		hash = uiHashBytes(hash, &separator, 1);
	}
	return hash;
}

static bool uiReadExact(SceUID fd, void* buffer, unsigned int bytes)
{
	unsigned char* out = (unsigned char*)buffer;
	unsigned int done = 0;
	while (done < bytes) {
		int got = sceIoRead(fd, out + done, bytes - done);
		if (got <= 0)
			return false;
		done += (unsigned int)got;
	}
	return true;
}

static bool uiWriteExact(SceUID fd, const void* buffer, unsigned int bytes)
{
	const unsigned char* in = (const unsigned char*)buffer;
	unsigned int done = 0;
	while (done < bytes) {
		int wrote = sceIoWrite(fd, in + done, bytes - done);
		if (wrote <= 0)
			return false;
		done += (unsigned int)wrote;
	}
	return true;
}

} /* namespace */

class UiI18n::Impl : private boost::noncopyable {
public:
	bool ready;
	bool rtl;
	bool sourceRects;
	std::string languageId;
	std::string applicationPath;
	std::string skinPath;
	std::string fontFile;
	std::string loadedFontPath;
	std::map<std::string, std::string> strings;
	std::vector<UiClip> mainClips;
	std::vector<UiClip> helpClips;
	FtFont* font;
	Image* mainStaticOverlay;
	Image* helpStaticOverlay;
	bool overlayCacheTried;
	Color color;
	int mainFontSize;
	int mainMinFontSize;
	int helpFontSize;
	int helpMinFontSize;
	int offsetX;
	int mainOffsetY;
	int helpOffsetY;
	int buttonOffsetY;

	Impl()
		: ready(false), rtl(false), sourceRects(false), font(NULL),
		  mainStaticOverlay(NULL), helpStaticOverlay(NULL),
		  overlayCacheTried(false), color(0xFFFFFF),
		  mainFontSize(10), mainMinFontSize(6),
		  helpFontSize(11), helpMinFontSize(6),
		  offsetX(6), mainOffsetY(5), helpOffsetY(6), buttonOffsetY(2)
	{
	}

	~Impl()
	{
		if (mainStaticOverlay != NULL) {
			freeImage(mainStaticOverlay);
			mainStaticOverlay = NULL;
		}
		if (helpStaticOverlay != NULL) {
			freeImage(helpStaticOverlay);
			helpStaticOverlay = NULL;
		}
		if (font != NULL) {
			delete font;
			font = NULL;
		}
	}

	void reset()
	{
		ready = false;
		rtl = false;
		sourceRects = false;
		languageId = "en";
		fontFile = "DejaVuSans.ttf";
		loadedFontPath.clear();
		strings.clear();
		mainClips.clear();
		helpClips.clear();
		offsetX = 6;
		mainOffsetY = 5;
		helpOffsetY = 6;
		buttonOffsetY = 2;
		overlayCacheTried = false;
		if (mainStaticOverlay != NULL) {
			freeImage(mainStaticOverlay);
			mainStaticOverlay = NULL;
		}
		if (helpStaticOverlay != NULL) {
			freeImage(helpStaticOverlay);
			helpStaticOverlay = NULL;
		}
		if (font != NULL) {
			delete font;
			font = NULL;
		}
		seedEnglishFallbacks();
	}

	void seedEnglishFallbacks()
	{
		strings["main.file_list_header"] = "FILE LIST";
		strings["main.information_header"] = "INFORMATION";
		strings["main.thumbnail_label"] = "THUMBNAIL";
		strings["main.thumbnail_unavailable"] = "UNAVAILABLE";
		strings["main.total_time"] = "TOTAL TIME";
		strings["main.aspect_ratio"] = "ASPECT RATIO";
		strings["main.frames_per_second"] = "FRAMES/S";
		strings["main.streams"] = "STREAMS";
		strings["main.subtitles"] = "SUBTITLES";
		strings["main.cancel_label"] = "CONFIG";
		strings["main.help_label"] = "HELP";
		strings["main.start_label"] = "START";
		strings["main.resume_label"] = "RESUME";
		strings["main.delete_label"] = "DELETE";

		strings["help.control_instruction"] = "Control instruction";
		strings["help.luminosity"] = "luminosity +";
		strings["help.forward_backward"] = "forward /\nbackward";
		strings["help.pause"] = "pause";
		strings["help.exit"] = "exit";
		strings["help.show_info"] = "show info";
		strings["help.volume"] = "volume";
		strings["help.aspect_ratio"] = "aspect_ratio";
		strings["help.aspect_ratio_values"] = "4:3/16:9/2.35:1";
		strings["help.audio_stream_switch"] = "audio stream switch";
		strings["help.subtitles_switch"] = "subtitles switch/off";
		strings["help.loop_toggle"] = "loop on/off";
		strings["help.fast_forward_rewind"] = "fast forward/rewind";
		strings["help.zoom_in_out"] = "zoom in/out";
		strings["help.close_prompt_text"] = "press to close";
	}

	const UiClip* findClip(const std::vector<UiClip>& clips,
	                       const char* id) const
	{
		size_t i;
		if (id == NULL)
			return NULL;
		for (i = 0; i < clips.size(); ++i) {
			if (clips[i].id == id)
				return &clips[i];
		}
		return NULL;
	}

	const char* rawText(const char* id) const
	{
		std::map<std::string, std::string>::const_iterator found;
		if (id == NULL)
			return "";
		found = strings.find(id);
		return found != strings.end() ? found->second.c_str() : "";
	}

	const char* text(const char* screen, const char* id) const
	{
		std::string key;
		if (screen == NULL || id == NULL)
			return "";
        if (!strcmp(screen, "main") && ppu_controls_get_type() == PPU_CONTROLS_INTERNATIONAL) {
            if (!strcmp(id, "start_label")) id = "resume_label";
            else if (!strcmp(id, "resume_label")) id = "start_label";
        }
		key = std::string(screen) + "." + id;
		return rawText(key.c_str());
	}

	int mainRightEdge() const
	{
		int right = 0;
		size_t i;
		for (i = 0; i < mainClips.size(); ++i) {
			int clipRight = mainClips[i].x + mainClips[i].width;
			if (clipRight > right)
				right = clipRight;
		}
		return right;
	}

	bool loadLanguage(TiXmlElement* language, bool replace)
	{
		TiXmlElement* textElement;
		const char* id;
		if (language == NULL)
			return false;
		if (replace) {
			const char* rtlValue = language->Attribute("rtl");
			const char* fontValue = language->Attribute("font");
			rtl = rtlValue != NULL && strcasecmp(rtlValue, "true") == 0;
			fontFile = fontValue != NULL ? fontValue : "DejaVuSans.ttf";
		}
		for (textElement = language->FirstChildElement("text");
		     textElement != NULL;
		     textElement = textElement->NextSiblingElement("text")) {
			const char* value;
			id = textElement->Attribute("id");
			value = textElement->Attribute("value");
			if (id != NULL && value != NULL)
				strings[id] = uiDecodeText(value);
		}
		return true;
	}

	bool loadTranslations(const char* filename, const char* selectedId)
	{
		TiXmlDocument document(filename);
		TiXmlElement* root;
		TiXmlElement* language;
		TiXmlElement* english = NULL;
		TiXmlElement* selected = NULL;
		if (!document.LoadFile())
			return false;
		root = document.RootElement();
		if (root == NULL)
			return false;
		for (language = root->FirstChildElement("language");
		     language != NULL;
		     language = language->NextSiblingElement("language")) {
			const char* id = language->Attribute("id");
			if (id == NULL)
				continue;
			if (strcmp(id, "en") == 0)
				english = language;
			if (strcmp(id, selectedId) == 0)
				selected = language;
		}
		if (english == NULL)
			return false;
		loadLanguage(english, true);
		if (selected != NULL && selected != english)
			loadLanguage(selected, true);
		else if (selected == NULL)
			languageId = "en";
		return true;
	}

	void drawClip(Image* image,
	              const UiClip& clip,
	              const char* value,
	              FtTextAlign align,
	              int preferredSize,
	              int minimumSize,
	              bool helpScreen = false)
	{
		if (font == NULL || image == NULL || value == NULL || value[0] == 0)
			return;
		font->drawStringInRect(image,
		                       clip.x + offsetX,
		                       clip.y + (uiIsButtonLabelClip(clip.id.c_str())
		                               ? buttonOffsetY
		                               : (helpScreen ? helpOffsetY : mainOffsetY)),
		                       clip.width, clip.height,
		                       color, value, align,
		                       preferredSize, minimumSize);
	}

	void drawMetadataLabel(Image* image, const UiClip& clip, const char* value)
	{
		UiClip labelClip = clip;
		FtTextAlign align = FT_TEXT_ALIGN_LEFT;
		if (!sourceRects) {
			int valueWidth = clip.width * 38 / 100;
			if (valueWidth < 36)
				valueWidth = clip.width / 3;
			labelClip.width = clip.width - valueWidth - 2;
			if (rtl)
				labelClip.x = clip.x + valueWidth + 2;
		}
		if (rtl)
			align = FT_TEXT_ALIGN_RIGHT;
		drawClip(image, labelClip, value, align,
		         mainFontSize, mainMinFontSize);
	}

	bool ensureMainStaticOverlay()
	{
		size_t i;
		if (mainStaticOverlay != NULL)
			return true;
		mainStaticOverlay = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
		if (mainStaticOverlay == NULL)
			return false;
		clearImage(mainStaticOverlay, 0);
		for (i = 0; i < mainClips.size(); ++i) {
			const UiClip& clip = mainClips[i];
			const char* value;
			if (clip.id == "thumbnail_unavailable")
				continue;
#if !PPA_ENABLE_THUMBNAILS
			if (uiIsThumbnailClip(clip.id.c_str()))
				continue;
#endif
			value = text("main", clip.id.c_str());
			if (uiIsMetadataClip(clip.id.c_str()))
				drawMetadataLabel(mainStaticOverlay, clip, value);
			else
				drawClip(mainStaticOverlay, clip, value,
				         uiMainAlign(clip.id.c_str()),
				         mainFontSize, mainMinFontSize);
		}
		return true;
	}

	bool ensureHelpStaticOverlay()
	{
		size_t i;
		if (helpStaticOverlay != NULL)
			return true;
		helpStaticOverlay = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
		if (helpStaticOverlay == NULL)
			return false;
		clearImage(helpStaticOverlay, 0);
		for (i = 0; i < helpClips.size(); ++i) {
			const UiClip& clip = helpClips[i];
			drawClip(helpStaticOverlay, clip,
			         text("help", clip.id.c_str()),
			         uiHelpAlign(clip.id.c_str()),
			         helpFontSize, helpMinFontSize, true);
		}
		return true;
	}

	u32 overlaySignature() const
	{
		u32 hash = 2166136261U;
        int controlsType = ppu_controls_get_type();
        hash = uiHashBytes(hash, &controlsType, sizeof(controlsType));
		size_t i;
		std::map<std::string, std::string>::const_iterator it;
		hash = uiHashBytes(hash, &color, sizeof(color));
		hash = uiHashBytes(hash, &mainFontSize, sizeof(mainFontSize));
		hash = uiHashBytes(hash, &mainMinFontSize, sizeof(mainMinFontSize));
		hash = uiHashBytes(hash, &helpFontSize, sizeof(helpFontSize));
		hash = uiHashBytes(hash, &helpMinFontSize, sizeof(helpMinFontSize));
		hash = uiHashBytes(hash, &offsetX, sizeof(offsetX));
		hash = uiHashBytes(hash, &mainOffsetY, sizeof(mainOffsetY));
		hash = uiHashBytes(hash, &helpOffsetY, sizeof(helpOffsetY));
		hash = uiHashBytes(hash, &buttonOffsetY, sizeof(buttonOffsetY));
		{
			const u32 thumbnailFeature = PPA_ENABLE_THUMBNAILS ? 1U : 0U;
			hash = uiHashBytes(hash, &thumbnailFeature, sizeof(thumbnailFeature));
		}
		hash = uiHashString(hash, languageId.c_str());
		hash = uiHashString(hash, loadedFontPath.c_str());
		for (i = 0; i < mainClips.size(); ++i) {
			hash = uiHashString(hash, mainClips[i].id.c_str());
			hash = uiHashBytes(hash, &mainClips[i].x, sizeof(int) * 4);
		}
		for (i = 0; i < helpClips.size(); ++i) {
			hash = uiHashString(hash, helpClips[i].id.c_str());
			hash = uiHashBytes(hash, &helpClips[i].x, sizeof(int) * 4);
		}
		for (it = strings.begin(); it != strings.end(); ++it) {
			hash = uiHashString(hash, it->first.c_str());
			hash = uiHashString(hash, it->second.c_str());
		}
		{
			SceIoStat stat;
			memset(&stat, 0, sizeof(stat));
			if (!loadedFontPath.empty() &&
			    sceIoGetstat(loadedFontPath.c_str(), &stat) >= 0) {
				/* SceIoStat timestamp members vary across PSPSDK/M33 SDK trees.
				 * File size is portable and the loaded path plus all UI inputs are
				 * already part of this signature. */
				hash = uiHashBytes(hash, &stat.st_size, sizeof(stat.st_size));
			}
		}
		return hash;
	}

	bool cachePaths(char* directory, size_t directorySize,
	                char* file, size_t fileSize,
	                char* temporary, size_t temporarySize) const
	{
		if (applicationPath.compare(0, 4, "ms0:") != 0)
			return false;
		if (snprintf(directory, directorySize, "%scache",
		             applicationPath.c_str()) >= (int)directorySize ||
		    snprintf(file, fileSize, "%s/ppa_ui_overlay_v4.bin", directory) >=
		             (int)fileSize ||
		    snprintf(temporary, temporarySize, "%s.tmp", file) >=
		             (int)temporarySize)
			return false;
		return true;
	}

	bool loadOverlayCache()
	{
		char directory[512];
		char file[640];
		char temporary[672];
		UiOverlayCacheHeader header;
		Image* mainImage = NULL;
		Image* helpImage = NULL;
		SceUID fd;
		u32 bytes;
		bool ok = false;
		if (!cachePaths(directory, sizeof(directory), file, sizeof(file),
		                temporary, sizeof(temporary)))
			return false;
		fd = sceIoOpen(file, PSP_O_RDONLY, 0);
		if (fd < 0)
			return false;
		memset(&header, 0, sizeof(header));
		if (!uiReadExact(fd, &header, sizeof(header)) ||
		    memcmp(header.magic, UI_OVERLAY_CACHE_MAGIC, 8) != 0 ||
		    header.version != UI_OVERLAY_CACHE_VERSION ||
		    header.signature != overlaySignature() ||
		    header.imageWidth != PSP_SCREEN_WIDTH ||
		    header.imageHeight != PSP_SCREEN_HEIGHT ||
		    header.textureWidth != PSP_SCREEN_TEXTURE_WIDTH)
			goto done;
		bytes = PSP_SCREEN_TEXTURE_WIDTH * PSP_SCREEN_HEIGHT * sizeof(Color);
		if (header.imageBytes != bytes)
			goto done;
		mainImage = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
		helpImage = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
		if (mainImage == NULL || helpImage == NULL ||
		    !uiReadExact(fd, mainImage->data, bytes) ||
		    !uiReadExact(fd, helpImage->data, bytes) ||
		    uiHashBytes(2166136261U, mainImage->data, bytes) !=
		        header.mainChecksum ||
		    uiHashBytes(2166136261U, helpImage->data, bytes) !=
		        header.helpChecksum)
			goto done;
		mainImage->dirty = 1;
		helpImage->dirty = 1;
		mainStaticOverlay = mainImage;
		helpStaticOverlay = helpImage;
		mainImage = NULL;
		helpImage = NULL;
		ok = true;
	done:
		sceIoClose(fd);
		if (mainImage != NULL) freeImage(mainImage);
		if (helpImage != NULL) freeImage(helpImage);
		
		return ok;
	}

	void saveOverlayCache() const
	{
		char directory[512];
		char file[640];
		char temporary[672];
		UiOverlayCacheHeader header;
		SceUID fd;
		u32 bytes;
		bool ok;
		if (mainStaticOverlay == NULL || helpStaticOverlay == NULL ||
		    !cachePaths(directory, sizeof(directory), file, sizeof(file),
		                temporary, sizeof(temporary)))
			return;
		(void)sceIoMkdir(directory, 0777);
		fd = sceIoOpen(temporary, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
		if (fd < 0)
			return;
		bytes = PSP_SCREEN_TEXTURE_WIDTH * PSP_SCREEN_HEIGHT * sizeof(Color);
		memset(&header, 0, sizeof(header));
		memcpy(header.magic, UI_OVERLAY_CACHE_MAGIC, 8);
		header.version = UI_OVERLAY_CACHE_VERSION;
		header.signature = overlaySignature();
		header.imageWidth = PSP_SCREEN_WIDTH;
		header.imageHeight = PSP_SCREEN_HEIGHT;
		header.textureWidth = PSP_SCREEN_TEXTURE_WIDTH;
		header.imageBytes = bytes;
		header.mainChecksum = uiHashBytes(2166136261U,
		                                  mainStaticOverlay->data, bytes);
		header.helpChecksum = uiHashBytes(2166136261U,
		                                  helpStaticOverlay->data, bytes);
		ok = uiWriteExact(fd, &header, sizeof(header)) &&
		     uiWriteExact(fd, mainStaticOverlay->data, bytes) &&
		     uiWriteExact(fd, helpStaticOverlay->data, bytes);
		sceIoClose(fd);
		if (!ok) {
			(void)sceIoRemove(temporary);
			return;
		}
		/* Cache replacement is deliberately isolated from user/media data. */
		(void)sceIoRemove(file);
		if (sceIoRename(temporary, file) < 0) {
			(void)sceIoRemove(temporary);
			return;
		}
	}

	void prepareOverlays()
	{
		bool restored = false;
		if (!ready)
			return;
		if (!overlayCacheTried) {
			overlayCacheTried = true;
			restored = loadOverlayCache();
		}
		if (!restored && (mainStaticOverlay == NULL || helpStaticOverlay == NULL)) {
			bool hadMain = mainStaticOverlay != NULL;
			bool hadHelp = helpStaticOverlay != NULL;
			if (ensureMainStaticOverlay() && ensureHelpStaticOverlay() &&
			    (!hadMain || !hadHelp))
				saveOverlayCache();
		}
	}
};

UiI18n::UiI18n()
	: impl(new (std::nothrow) Impl())
{
}

UiI18n::~UiI18n()
{
	delete impl;
	impl = NULL;
}

bool UiI18n::load(const char* applicationPath, const char* skinPath)
{
	Skin* skin = Skin::getInstance();
	Config* config = Config::getInstance();
	const char* configuredLanguage;
	const char* selectedLanguage;
	const char* translationRelative;
	const char* mainMapRelative;
	const char* helpMapRelative;
	const char* mainRect;
	const char* helpRect;
	char skinName[128];
	char pathCandidates[8][1024];
	char fontPath[1024];
	int candidateCount;
	int candidateIndex;
	int baseOffsetY;
	bool translationsLoaded = false;
	bool mainMapLoaded = false;
	bool helpMapLoaded = false;
	FtFontManager* manager;

	if (impl == NULL || skin == NULL || config == NULL ||
	    applicationPath == NULL || skinPath == NULL) {
		return false;
	}
	impl->reset();
	impl->applicationPath = applicationPath;
	impl->skinPath = skinPath;
	uiExtractSkinName(skinPath, skinName, sizeof(skinName));
	if (skinName[0] == 0)
		strncpy(skinName, "default", sizeof(skinName) - 1U);
	skinName[sizeof(skinName) - 1U] = 0;

	/* Older source-tree/runtime skins predate the <ui> element.  Treat the
	 * two textless-capable bundled skins as enabled by default.  Other legacy
	 * skins remain opt-in so compiled emergency clips never duplicate their
	 * baked labels.  An explicit enabled="false" always disables the overlay. */
	{
		bool bundledTextlessSkin = strcmp(skinName, "default") == 0 ||
		                           strcmp(skinName, "normal-en") == 0;
		if (!skin->getBooleanValue("skin/ui/enabled", bundledTextlessSkin)) {
			return false;
		}
	}

	configuredLanguage = config->getStringValue("config/windows/ui/language", "auto");
	selectedLanguage = (configuredLanguage == NULL ||
	                    strcmp(configuredLanguage, "auto") == 0)
	                 ? uiSystemLanguageId() : configuredLanguage;
	if (!uiIsKnownLanguage(selectedLanguage))
		selectedLanguage = "en";
	impl->languageId = selectedLanguage;

	translationRelative = skin->getStringValue("skin/ui/translations", "ui/ui_strings.xml");
	mainMapRelative = skin->getStringValue("skin/ui/main_menu_clips", "main_menu_clips.json");
	helpMapRelative = skin->getStringValue("skin/ui/help_screen_clips", "help_screen_clips.json");
	mainRect = skin->getStringValue("skin/ui/main_menu_rect", "target_rect_480x272");
	helpRect = skin->getStringValue("skin/ui/help_screen_rect", "target_rect_480x272");
	impl->sourceRects = strcmp(mainRect, "source_rect") == 0;

	/* PSPLink launches the PRX from the source directory, while release builds
	 * place assets in PPA3xx/.  Try both layouts, plus extra/ for a freshly
	 * extracted source tree. */
	candidateCount = 0;
	snprintf(pathCandidates[candidateCount++], 1024, "%s%s",
	         applicationPath, translationRelative);
	snprintf(pathCandidates[candidateCount++], 1024, "%sPPA3xx/%s",
	         applicationPath, translationRelative);
	snprintf(pathCandidates[candidateCount++], 1024, "%sextra/%s",
	         applicationPath, translationRelative);
	snprintf(pathCandidates[candidateCount++], 1024, "%s%s",
	         skinPath, translationRelative);
	for (candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
		if (impl->loadTranslations(pathCandidates[candidateIndex], selectedLanguage)) {
			translationsLoaded = true;
			break;
		}
	}
	if (!translationsLoaded) {
		impl->languageId = "en";
		impl->rtl = false;
		impl->fontFile = "DejaVuSans.ttf";
	}

	candidateCount = 0;
	snprintf(pathCandidates[candidateCount++], 1024, "%s%s",
	         skinPath, mainMapRelative);
	snprintf(pathCandidates[candidateCount++], 1024, "%sskins/%s/%s",
	         applicationPath, skinName, mainMapRelative);
	snprintf(pathCandidates[candidateCount++], 1024, "%sPPA3xx/skins/%s/%s",
	         applicationPath, skinName, mainMapRelative);
	snprintf(pathCandidates[candidateCount++], 1024, "%sextra/skins/%s/%s",
	         applicationPath, skinName, mainMapRelative);
	for (candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
		if (uiLoadClipMap(pathCandidates[candidateIndex], mainRect,
		                  &impl->mainClips)) {
			mainMapLoaded = true;
			break;
		}
	}
	if (!mainMapLoaded) {
		uiLoadBuiltinClips(kMainBuiltinClips,
		                   sizeof(kMainBuiltinClips) / sizeof(kMainBuiltinClips[0]),
		                   impl->sourceRects,
		                   &impl->mainClips);
	}

	candidateCount = 0;
	snprintf(pathCandidates[candidateCount++], 1024, "%s%s",
	         skinPath, helpMapRelative);
	snprintf(pathCandidates[candidateCount++], 1024, "%sskins/%s/%s",
	         applicationPath, skinName, helpMapRelative);
	snprintf(pathCandidates[candidateCount++], 1024, "%sPPA3xx/skins/%s/%s",
	         applicationPath, skinName, helpMapRelative);
	snprintf(pathCandidates[candidateCount++], 1024, "%sextra/skins/%s/%s",
	         applicationPath, skinName, helpMapRelative);
	for (candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
		if (uiLoadClipMap(pathCandidates[candidateIndex], helpRect,
		                  &impl->helpClips)) {
			helpMapLoaded = true;
			break;
		}
	}
	if (!helpMapLoaded) {
		uiLoadBuiltinClips(kHelpBuiltinClips,
		                   sizeof(kHelpBuiltinClips) / sizeof(kHelpBuiltinClips[0]),
		                   strcmp(helpRect, "source_rect") == 0,
		                   &impl->helpClips);
	}

	impl->color = skin->getColorValue("skin/ui/color", 0xD7E3E5);
	impl->mainFontSize = skin->getIntegerValue("skin/ui/main_font_size", 10);
	impl->mainMinFontSize = skin->getIntegerValue("skin/ui/main_min_font_size", 5);
	impl->helpFontSize = skin->getIntegerValue("skin/ui/help_font_size", 11);
	impl->helpMinFontSize = skin->getIntegerValue("skin/ui/help_min_font_size", 5);
	impl->offsetX = skin->getIntegerValue("skin/ui/text_offset_x", 6);
	baseOffsetY = skin->getIntegerValue("skin/ui/text_offset_y", 6);
	impl->mainOffsetY =
		skin->getIntegerValue("skin/ui/main_text_offset_y", baseOffsetY - 1);
	impl->helpOffsetY =
		skin->getIntegerValue("skin/ui/help_text_offset_y", baseOffsetY);
	impl->buttonOffsetY =
		skin->getIntegerValue("skin/ui/button_text_offset_y", baseOffsetY - 4);
	if (impl->offsetX < -32) impl->offsetX = -32;
	if (impl->offsetX > 32) impl->offsetX = 32;
	if (impl->mainOffsetY < -32) impl->mainOffsetY = -32;
	if (impl->mainOffsetY > 32) impl->mainOffsetY = 32;
	if (impl->helpOffsetY < -32) impl->helpOffsetY = -32;
	if (impl->helpOffsetY > 32) impl->helpOffsetY = 32;
	if (impl->buttonOffsetY < -32) impl->buttonOffsetY = -32;
	if (impl->buttonOffsetY > 32) impl->buttonOffsetY = 32;

	manager = FtFontManager::getInstance();
	if (manager == NULL) {
		return false;
	}

	/* Search every source/release asset layout for the language-specific face,
	 * then repeat with broad emergency faces. */
	{
		static const char* roots[] = {
			"fonts/",
			"PPA3xx/fonts/",
			"extra/fonts/",
			"interface/",
			NULL
		};
		const char* faces[6];
		int faceIndex;
		int rootIndex;
		faces[0] = impl->fontFile.c_str();
		faces[1] = "DejaVuSans.ttf";
		faces[2] = "wqy-microhei.ttc";
		faces[3] = "mainfont.ttf";
		faces[4] = "04B_25__.TTF";
		faces[5] = NULL;

		for (faceIndex = 0; faces[faceIndex] != NULL && impl->font == NULL;
		     ++faceIndex) {
			for (rootIndex = 0; roots[rootIndex] != NULL && impl->font == NULL;
			     ++rootIndex) {
				snprintf(fontPath, sizeof(fontPath), "%s%s%s",
				         applicationPath, roots[rootIndex], faces[faceIndex]);
				impl->font = manager->createFont(fontPath);
				if (impl->font != NULL) {
					impl->loadedFontPath = fontPath;
				}
			}
		}
	}
	if (impl->font == NULL) {
		return false;
	}

	impl->font->setAntiAlias(true);
	impl->font->setEmbolden(false);
	impl->font->setPixelSize(impl->mainFontSize);
	impl->ready = true;
	/* Front-load shaping/rasterization while the boot screen is already up.
	 * Later browser/help paints become two small GE submissions. */
	impl->prepareOverlays();
	return true;
}

bool UiI18n::isReady() const
{
	return impl != NULL && impl->ready;
}

bool UiI18n::isRtl() const
{
	return impl != NULL && impl->rtl;
}

const char* UiI18n::getLanguageId() const
{
	return impl != NULL ? impl->languageId.c_str() : "en";
}

const char* UiI18n::getText(const char* id, const char* fallback) const
{
	const char* value;
	if (fallback == NULL)
		fallback = "";
	if (!isReady() || id == NULL)
		return fallback;
	value = impl->rawText(id);
	return (value != NULL && value[0] != 0) ? value : fallback;
}

const char* UiI18n::getLanguageName(const char* id, const char* fallback) const
{
	std::string key;
	if (id == NULL)
		return fallback != NULL ? fallback : "";
	/* Subtitle metadata uses the generic zh code, while the interface uses
	 * the more precise zh-CN identifier.  Both resolve to the same label. */
	if (strcmp(id, "zh") == 0)
		id = "zh-CN";
	key = std::string("language.") + id;
	return getText(key.c_str(), fallback);
}

FtFont* UiI18n::getFont() const
{
	return isReady() ? impl->font : NULL;
}

Image* UiI18n::getMainStaticOverlay()
{
	if (!isReady())
		return NULL;
	impl->prepareOverlays();
	return impl->mainStaticOverlay;
}

Image* UiI18n::getHelpStaticOverlay()
{
	if (!isReady())
		return NULL;
	impl->prepareOverlays();
	return impl->helpStaticOverlay;
}

void UiI18n::prepareCachedOverlays()
{
	if (isReady())
		impl->prepareOverlays();
}

void UiI18n::releaseCachedOverlays()
{
	if (impl == NULL)
		return;
	/* Playback keeps the language face for its UI, but browser-only script
	 * faces and glyph bitmaps must not compete with decoder working memory. */
	if (impl->font != NULL)
		impl->font->releaseFallbackFonts();
	if (impl->mainStaticOverlay != NULL) {
		freeImage(impl->mainStaticOverlay);
		impl->mainStaticOverlay = NULL;
	}
	if (impl->helpStaticOverlay != NULL) {
		freeImage(impl->helpStaticOverlay);
		impl->helpStaticOverlay = NULL;
	}
	impl->overlayCacheTried = false;
}

bool UiI18n::getMainClipRect(const char* id, int* x, int* y,
                             int* width, int* height) const
{
	const UiClip* clip;
	if (!isReady() || id == NULL)
		return false;
	clip = impl->findClip(impl->mainClips, id);
	if (clip == NULL || clip->width <= 0 || clip->height <= 0)
		return false;
	if (x != NULL) *x = clip->x;
	if (y != NULL) *y = clip->y;
	if (width != NULL) *width = clip->width;
	if (height != NULL) *height = clip->height;
	return true;
}

void UiI18n::renderMainLabels(Image* image, bool thumbnailUnavailable)
{
#if PPA_ENABLE_THUMBNAILS
	const UiClip* unavailableClip;
	if (!isReady() || image == NULL)
		return;

	/* Static labels are submitted as a separate GE layer by PpuPlayer::paint.
	 * This function intentionally emits only state-dependent text. */
	if (!thumbnailUnavailable)
		return;
	unavailableClip = impl->findClip(impl->mainClips, "thumbnail_unavailable");
	if (unavailableClip != NULL)
		impl->drawClip(image, *unavailableClip,
		               impl->text("main", "thumbnail_unavailable"),
		               FT_TEXT_ALIGN_CENTER,
		               impl->mainFontSize, impl->mainMinFontSize);
#else
	(void)image;
	(void)thumbnailUnavailable;
#endif
}

void UiI18n::drawMainValue(Image* image, const char* clipId, const char* value)
{
	const UiClip* clip;
	UiClip valueClip;
	FtTextAlign align;
	if (!isReady() || image == NULL || clipId == NULL || value == NULL)
		return;
	clip = impl->findClip(impl->mainClips, clipId);
	if (clip == NULL)
		return;
	valueClip = *clip;
	if (impl->sourceRects) {
		/* The supplied source rectangles cover the label only; metadata
		 * values occupy the remaining fixed information-panel space. */
		valueClip.x = clip->x + clip->width + 2;
		valueClip.width = impl->mainRightEdge() - valueClip.x;
		align = FT_TEXT_ALIGN_RIGHT;
	}
	else {
		int valueWidth = clip->width * 38 / 100;
		if (valueWidth < 36)
			valueWidth = clip->width / 3;
		valueClip.width = valueWidth;
		if (!impl->rtl) {
			valueClip.x = clip->x + clip->width - valueWidth;
			align = FT_TEXT_ALIGN_RIGHT;
		}
		else {
			valueClip.x = clip->x;
			align = FT_TEXT_ALIGN_LEFT;
		}
	}
	if (valueClip.width <= 0)
		return;
	impl->drawClip(image, valueClip, value, align,
	               impl->mainFontSize, impl->mainMinFontSize);
}

void UiI18n::renderHelp(Image* image)
{
	size_t i;
	if (!isReady() || image == NULL)
		return;
	/* Compatibility path for callers that need a CPU-owned composite. The
	 * normal help screen uses getHelpStaticOverlay() and a direct GE blit. */
	for (i = 0; i < impl->helpClips.size(); ++i) {
		const UiClip& clip = impl->helpClips[i];
		impl->drawClip(image, clip, impl->text("help", clip.id.c_str()),
		               uiHelpAlign(clip.id.c_str()), impl->helpFontSize,
		               impl->helpMinFontSize, true);
	}
}

void UiI18n::onSuspend()
{
	if (impl != NULL && impl->font != NULL)
		impl->font->onSuspend();
}

void UiI18n::onResume()
{
	if (impl != NULL && impl->font != NULL)
		impl->font->onResume();
}

int UiI18n::languageCount()
{
	return kLanguageCount;
}

const char* UiI18n::languageIdAt(int index)
{
	if (index < 0 || index >= kLanguageCount)
		return "en";
	return kLanguages[index].id;
}

const char* UiI18n::languageNameAt(int index)
{
	if (index < 0 || index >= kLanguageCount)
		return "English";
	return kLanguages[index].name;
}
