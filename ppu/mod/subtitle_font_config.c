#include <stdio.h>
#include <string.h>
#include "subtitle_font_config.h"
#include "gu_font.h"

/* Configured at startup before any playback threads are created. */
static char subtitle_font_directory[256];

void subtitle_font_config_set_directory(const char *directory)
{
    int length;
    subtitle_font_directory[0] = 0;
    if (directory == 0 || directory[0] == 0)
        return;
    length = snprintf(subtitle_font_directory, sizeof(subtitle_font_directory),
                      "%s%s", directory,
                      directory[strlen(directory) - 1U] == '/' ? "" : "/");
    if (length < 0 || (unsigned int)length >= sizeof(subtitle_font_directory))
        subtitle_font_directory[0] = 0;
}

static int ppa_lang_eq2(const char *lang, const char *a, const char *b, const char *c)
{
	char tmp[16];
	int i;

	if (lang == 0 || lang[0] == '\0')
		return 0;

	memset(tmp, 0, sizeof(tmp));

	for (i = 0; i < (int)sizeof(tmp) - 1 && lang[i] != '\0'; i++) {
		char ch = lang[i];

		if (ch == '-' || ch == '_')
			break;

		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch + ('a' - 'A'));

		tmp[i] = ch;
	}

	tmp[i] = '\0';

	if (a != 0 && strcmp(tmp, a) == 0)
		return 1;

	if (b != 0 && strcmp(tmp, b) == 0)
		return 1;

	if (c != 0 && strcmp(tmp, c) == 0)
		return 1;

	return 0;
}

static int ppa_try_add_font_at_prefix(const char *prefix, const char *name)
{
	char path[256];
	char *error;
	int length;

	if (prefix == 0 || name == 0)
		return 0;

	length = snprintf(path, sizeof(path), "%s%s", prefix, name);
	if (length < 0 || (unsigned int)length >= sizeof(path))
		return 0;

	error = gu_font_add_fallback_file(path);
	if (error == 0) {
		return 1;
	}

	return 0;
}

static int ppa_try_add_font(const char *name)
{
	static const char *prefixes[] = {
		"ms0:/PSP/GAME/PPA3xx/fonts/",
		"ef0:/PSP/GAME/PPA3xx/fonts/",
		"ms1:/PSP/GAME/PPA3xx/fonts/",
		"ms0:/PSP/PMPLAYER/FONT/",
		"ef0:/PSP/PMPLAYER/FONT/",
		"ms1:/PSP/PMPLAYER/FONT/",
		0
	};

	int i;

	if (subtitle_font_directory[0] != 0 &&
	    ppa_try_add_font_at_prefix(subtitle_font_directory, name))
		return 1;

	for (i = 0; prefixes[i] != 0; i++) {
		if (ppa_try_add_font_at_prefix(prefixes[i], name))
			return 1;
	}

	return 0;
}

void subtitle_font_config_clear(void)
{
	gu_font_clear_fallbacks();
}

void subtitle_font_config_load_core(void)
{
	/*
	 * Small fallback for Greek/Cyrillic/Hebrew/basic Arabic symbols.
	 * This is safe to load for all subtitle tracks.
	 */
	ppa_try_add_font("ppa-fallback-core.ttf");
}

void subtitle_font_config_load_for_track(const mkvinfo_track_t *track)
{
	const char *lang;

	gu_font_clear_fallbacks();

	subtitle_font_config_load_core();

	if (track == 0)
		return;

	lang = track->language_ietf[0] ? track->language_ietf : track->language;

	if (ppa_lang_eq2(lang, "ar", "ara", 0)) {
		ppa_try_add_font("ppa-fallback-arabic.ttf");
		return;
	}

	if (ppa_lang_eq2(lang, "ur", "urd", 0)) {
		ppa_try_add_font("ppa-fallback-urdu.ttf");
		ppa_try_add_font("ppa-fallback-arabic.ttf");
		return;
	}

	if (ppa_lang_eq2(lang, "he", "heb", 0)) {
		ppa_try_add_font("ppa-fallback-hebrew.ttf");
		return;
	}

	if (ppa_lang_eq2(lang, "hi", "hin", 0) ||
	    ppa_lang_eq2(lang, "mr", "mar", 0) ||
	    ppa_lang_eq2(lang, "ne", "nep", 0)) {
		ppa_try_add_font("ppa-fallback-devanagari.ttf");
		return;
	}

	if (ppa_lang_eq2(lang, "bn", "ben", 0)) {
		ppa_try_add_font("ppa-fallback-bengali.ttf");
		return;
	}

	if (ppa_lang_eq2(lang, "th", "tha", 0)) {
		ppa_try_add_font("ppa-fallback-thai.ttf");
		return;
	}

	if (ppa_lang_eq2(lang, "zh", "chi", "zho")) {
		ppa_try_add_font("ppa-fallback-sc.otf");
		return;
	}

	if (ppa_lang_eq2(lang, "ja", "jpn", 0)) {
		ppa_try_add_font("ppa-fallback-jp.otf");
		return;
	}

	if (ppa_lang_eq2(lang, "ko", "kor", 0)) {
		ppa_try_add_font("ppa-fallback-kr.otf");
		return;
	}

	if (ppa_lang_eq2(lang, "ru", "rus", 0) ||
	    ppa_lang_eq2(lang, "uk", "ukr", 0) ||
	    ppa_lang_eq2(lang, "bg", "bul", 0) ||
	    ppa_lang_eq2(lang, "el", "gre", "ell")) {
		return;
	}
}